#include "internal.hpp"

namespace stellac {

std::vector<std::string> split_scopes(const std::string &s) {
	std::vector<std::string> segs;
	std::string				 cur;
	for (size_t i = 0; i < s.size(); ++i) {
		if (s[i] == '.') {
			segs.push_back(cur);
			cur.clear();
		} else {
			cur += s[i];
		}
	}
	segs.push_back(cur);
	return segs;
}

Symbol *lookup_named_type(ModuleResolver &r, Sema &sema, Module &m,
						  const std::string &single, const std::string &full) {
	(void)r;
	std::vector<std::string> segs = full.find('.') == std::string::npos
												? std::vector<std::string>{single}
												: split_scopes(full);
	return sema.resolve_path(segs, m);
}

std::vector<std::string> split_scopes(const std::string &s);

bool eval_generic_const_expr(const Expr &e, const std::unordered_map<std::string, int64_t> &values,
							 int64_t &out) {
	switch (e.kind) {
	case ExprKind::IntLit:
	case ExprKind::CharLit:
		out = signed_bits(e.int_val, 64);
		return true;
	case ExprKind::Ident:
		if (e.path.size() == 1) {
			auto it = values.find(e.path[0]);
			if (it != values.end()) {
				out = it->second;
				return true;
			}
		}
		return false;
	case ExprKind::Unary: {
		if (!e.lhs) {
			return false;
		}
		int64_t v = 0;
		if (!eval_generic_const_expr(*e.lhs, values, v)) {
			return false;
		}
		if (e.str_val == "-") {
			// As with an ordinary signed literal, the unary minus makes the
			// otherwise out-of-range 2^63 magnitude representable as INT64_MIN.
			if (e.lhs->kind == ExprKind::IntLit && e.lhs->int_val == (uint64_t{1} << 63)) {
				out = INT64_MIN;
				return true;
			}
			if (!checked_sub(0, v, out)) {
				return false;
			}
		} else if (e.str_val == "~") {
			out = ~v;
		} else {
			return false;
		}
		return true;
	}
	case ExprKind::Binary: {
		if (!e.lhs || !e.rhs) {
			return false;
		}
		int64_t l = 0, r = 0;
		if (!eval_generic_const_expr(*e.lhs, values, l) ||
			!eval_generic_const_expr(*e.rhs, values, r)) {
			return false;
		}
		if (e.str_val == "+") {
			return checked_add(l, r, out);
		} else if (e.str_val == "-") {
			return checked_sub(l, r, out);
		} else if (e.str_val == "*") {
			return checked_mul(l, r, out);
		} else if (e.str_val == "/") {
			return checked_div(l, r, out);
		} else if (e.str_val == "%") {
			return checked_mod(l, r, out);
		} else if (e.str_val == "<<") {
			if (r < 0 || r >= 64) {
				return false;
			}
			uint64_t bits = static_cast<uint64_t>(l) << static_cast<unsigned>(r);
			out = signed_bits(bits, 64);
		} else if (e.str_val == ">>") {
			if (r < 0 || r >= 64) {
				return false;
			}
			out = arithmetic_shift_right(l, static_cast<unsigned>(r));
		} else if (e.str_val == "&") {
			out = l & r;
		} else if (e.str_val == "|") {
			out = l | r;
		} else if (e.str_val == "^") {
			out = l ^ r;
		} else {
			return false;
		}
		return true;
	}
	default:
		return false;
	}
}

bool Sema::eval_comptime_int_expr(const Expr &e, Module &m, int64_t &out_signed,
								   uint64_t &out_unsigned, bool &is_unsigned) {
	switch (e.kind) {
	case ExprKind::IntLit:
	case ExprKind::CharLit:
		out_unsigned = e.int_val;
		out_signed	 = static_cast<int64_t>(e.int_val);
		is_unsigned	 = true;
		return true;

	case ExprKind::Ident: {
		if (generic_context_ && e.path.size() == 1) {
			auto it = generic_context_->generic_const_subst.find(e.path[0]);
			if (it != generic_context_->generic_const_subst.end()) {
				out_signed	 = it->second;
				out_unsigned = static_cast<uint64_t>(it->second);
				is_unsigned	 = false;
				return true;
			}
		}

		if (current_scope_ && e.path.size() == 1) {
			if (Symbol *ls = current_scope_->lookup(e.path[0])) {
				if (ls->kind == SymKind::Const && ls->has_const_value && !ls->const_value_is_float) {
					if (ls->usage) {
						*ls->usage = true;
					}
					if (ls->type && ls->type->kind == TypeKind::UInt) {
						is_unsigned	 = true;
						out_unsigned = ls->const_int_val;
						out_signed	 = static_cast<int64_t>(ls->const_int_val);
					} else {
						is_unsigned	 = false;
						int width	 = (ls->type && ls->type->width > 0) ? ls->type->width : 64;
						out_signed	 = signed_bits(ls->const_int_val, width);
						out_unsigned = static_cast<uint64_t>(out_signed);
					}
					return true;
				}
			}
		}

		Module &ident_module = e.binding_module ? *e.binding_module : m;

		if (e.path.size() >= 2) {
			std::vector<std::string> head(e.path.begin(), e.path.end() - 1);
			Symbol					*en = resolve_path(head, ident_module);
			if (en && en->kind == SymKind::Enum && en->en) {
				const std::string &mem = e.path.back();
				for (auto &mb : en->en->members) {
					if (mb.name == mem) {
						if (en->usage) {
							*en->usage = true;
						}
						out_signed	 = mb.resolved;
						out_unsigned = static_cast<uint64_t>(mb.resolved);
						is_unsigned	 = (en->type && en->type->base && en->type->base->kind == TypeKind::UInt);
						return true;
					}
				}
			}
		}

		Symbol *sym = resolve_path(e.path, ident_module);
		if (sym && sym->kind == SymKind::Const) {
			if (!sym->has_const_value) {
				ensure_const_value(*sym);
			}
			if (sym->has_const_value && !sym->const_value_is_float) {
				if (sym->usage) {
					*sym->usage = true;
				}
				if (sym->type && sym->type->kind == TypeKind::UInt) {
					is_unsigned	 = true;
					out_unsigned = sym->const_int_val;
					out_signed	 = static_cast<int64_t>(sym->const_int_val);
				} else {
					is_unsigned	 = false;
					int width	 = (sym->type && sym->type->width > 0) ? sym->type->width : 64;
					out_signed	 = signed_bits(sym->const_int_val, width);
					out_unsigned = static_cast<uint64_t>(out_signed);
				}
				return true;
			}
		}
		return false;
	}

	case ExprKind::Member: {
		Module &member_module = e.binding_module ? *e.binding_module : m;
		std::vector<std::string> path;
		if (!flatten_member_path(e, path) || path.size() < 2) {
			return false;
		}

		std::vector<std::string> head(path.begin(), path.end() - 1);
		Symbol *en = resolve_path(head, member_module);
		if (en && en->kind == SymKind::Enum && en->en) {
			const std::string &mem = path.back();
			for (auto &mb : en->en->members) {
				if (mb.name == mem) {
					if (en->usage) {
						*en->usage = true;
					}
					is_unsigned	 = (en->type && en->type->base && en->type->base->kind == TypeKind::UInt);
					out_signed	 = mb.resolved;
					out_unsigned = static_cast<uint64_t>(mb.resolved);
					return true;
				}
			}
		}

		Symbol *sym = resolve_path(path, member_module);
		if (!sym && en &&
			(en->kind == SymKind::Struct || en->kind == SymKind::Union ||
			 en->kind == SymKind::Enum || en->kind == SymKind::Mod) &&
			member_module.fpath == en->owner_module->fpath) {
			std::vector<std::string> scoped;
			if (member_module.lexical_path.size() > member_module.mod_path.size()) {
				scoped.insert(scoped.end(), member_module.lexical_path.begin() +
					member_module.mod_path.size(), member_module.lexical_path.end());
			}
			scoped.insert(scoped.end(), path.begin(), path.end());
			auto table = module_syms_.find(member_module.fpath);
			if (table != module_syms_.end()) {
				auto it = table->second.find(join_path(scoped));
				if (it != table->second.end()) sym = &it->second;
			}
		}

		if (sym && sym->kind == SymKind::Const) {
			if (!sym->has_const_value) {
				ensure_const_value(*sym);
			}
			if (sym->has_const_value && !sym->const_value_is_float) {
				if (sym->usage) {
					*sym->usage = true;
				}
				if (sym->type && sym->type->kind == TypeKind::UInt) {
					is_unsigned	 = true;
					out_unsigned = sym->const_int_val;
					out_signed	 = static_cast<int64_t>(sym->const_int_val);
				} else {
					is_unsigned	 = false;
					int width	 = (sym->type && sym->type->width > 0) ? sym->type->width : 64;
					out_signed	 = signed_bits(sym->const_int_val, width);
					out_unsigned = static_cast<uint64_t>(out_signed);
				}
				return true;
			}
		}
		return false;
	}

	case ExprKind::Unary: {
		if (!e.lhs) {
			return false;
		}
		int64_t	 sv			  = 0;
		uint64_t uv			  = 0;
		bool	 sub_unsigned = false;
		if (!eval_comptime_int_expr(*e.lhs, m, sv, uv, sub_unsigned)) {
			return false;
		}
		if (e.str_val == "-") {
			if (sub_unsigned) {
				if (uv == 0) {
					out_signed	 = 0;
					out_unsigned = 0;
					is_unsigned	 = true;
					return true;
				}
				if (uv == static_cast<uint64_t>(1) << 63) {
					out_signed	 = INT64_MIN;
					out_unsigned = static_cast<uint64_t>(INT64_MIN);
					is_unsigned	 = false;
					return true;
				}
				if (uv > static_cast<uint64_t>(INT64_MAX)) {
					return false;
				}
				out_signed	 = -static_cast<int64_t>(uv);
				out_unsigned = static_cast<uint64_t>(out_signed);
				is_unsigned	 = false;
				return true;
			}
			if (e.lhs->kind == ExprKind::IntLit && e.lhs->int_val == (uint64_t{1} << 63)) {
				out_signed	 = INT64_MIN;
				out_unsigned = static_cast<uint64_t>(INT64_MIN);
				is_unsigned	 = false;
				return true;
			}
			if (!checked_sub(0, sv, out_signed)) {
				return false;
			}
			out_unsigned = static_cast<uint64_t>(out_signed);
			is_unsigned	 = false;
			return true;
		} else if (e.str_val == "~") {
			if (sub_unsigned) {
				out_unsigned = ~uv;
				out_signed	 = static_cast<int64_t>(out_unsigned);
				is_unsigned	 = true;
			} else {
				out_signed	 = ~sv;
				out_unsigned = static_cast<uint64_t>(out_signed);
				is_unsigned	 = false;
			}
			return true;
		}
		return false;
	}

	case ExprKind::Binary: {
		if (!e.lhs || !e.rhs) {
			return false;
		}
		int64_t	 sl			 = 0, sr = 0;
		uint64_t ul			 = 0, ur = 0;
		bool	 un_l		 = false, un_r = false;
		if (!eval_comptime_int_expr(*e.lhs, m, sl, ul, un_l) ||
			!eval_comptime_int_expr(*e.rhs, m, sr, ur, un_r)) {
			return false;
		}
		bool op_unsigned = un_l || un_r;
		if (op_unsigned) {
			uint64_t l = un_l ? ul : static_cast<uint64_t>(sl);
			uint64_t r = un_r ? ur : static_cast<uint64_t>(sr);
			if (e.str_val == "+") {
				if (__builtin_add_overflow(l, r, &out_unsigned)) return false;
			} else if (e.str_val == "-") {
				if (l < r) return false;
				out_unsigned = l - r;
			} else if (e.str_val == "*") {
				if (__builtin_mul_overflow(l, r, &out_unsigned)) return false;
			} else if (e.str_val == "/") {
				if (r == 0) return false;
				out_unsigned = l / r;
			} else if (e.str_val == "%") {
				if (r == 0) return false;
				out_unsigned = l % r;
			} else if (e.str_val == "<<") {
				if (r >= 64) return false;
				out_unsigned = l << r;
			} else if (e.str_val == ">>") {
				if (r >= 64) return false;
				out_unsigned = l >> r;
			} else if (e.str_val == "&") {
				out_unsigned = l & r;
			} else if (e.str_val == "|") {
				out_unsigned = l | r;
			} else if (e.str_val == "^") {
				out_unsigned = l ^ r;
			} else {
				return false;
			}
			out_signed	= static_cast<int64_t>(out_unsigned);
			is_unsigned = true;
			return true;
		} else {
			if (e.str_val == "+") {
				if (!checked_add(sl, sr, out_signed)) return false;
			} else if (e.str_val == "-") {
				if (!checked_sub(sl, sr, out_signed)) return false;
			} else if (e.str_val == "*") {
				if (!checked_mul(sl, sr, out_signed)) return false;
			} else if (e.str_val == "/") {
				if (!checked_div(sl, sr, out_signed)) return false;
			} else if (e.str_val == "%") {
				if (!checked_mod(sl, sr, out_signed)) return false;
			} else if (e.str_val == "<<") {
				if (sr < 0 || sr >= 64) return false;
				uint64_t bits = static_cast<uint64_t>(sl) << static_cast<unsigned>(sr);
				out_signed	  = signed_bits(bits, 64);
			} else if (e.str_val == ">>") {
				if (sr < 0 || sr >= 64) return false;
				out_signed = arithmetic_shift_right(sl, static_cast<unsigned>(sr));
			} else if (e.str_val == "&") {
				out_signed = sl & sr;
			} else if (e.str_val == "|") {
				out_signed = sl | sr;
			} else if (e.str_val == "^") {
				out_signed = sl ^ sr;
			} else {
				return false;
			}
			out_unsigned = static_cast<uint64_t>(out_signed);
			is_unsigned	 = false;
			return true;
		}
	}

	case ExprKind::Cast: {
		if (!e.lhs) return false;
		if (!eval_comptime_int_expr(*e.lhs, m, out_signed, out_unsigned, is_unsigned)) {
			return false;
		}
		if (e.cast_type) {
			Module &cast_module = e.binding_module ? *e.binding_module : m;
			StType *target_ty	= resolve_type(*e.cast_type, cast_module);
			target_ty			= underlying_type(target_ty);
			if (target_ty) {
				if (target_ty->kind == TypeKind::UInt) {
					is_unsigned = true;
					int width	= target_ty->width > 0 ? target_ty->width : 64;
					if (width < 64) {
						uint64_t mask = (uint64_t{1} << width) - 1;
						out_unsigned  = (is_unsigned ? out_unsigned : static_cast<uint64_t>(out_signed)) & mask;
					} else {
						out_unsigned = is_unsigned ? out_unsigned : static_cast<uint64_t>(out_signed);
					}
					out_signed = static_cast<int64_t>(out_unsigned);
				} else if (target_ty->kind == TypeKind::SInt) {
					is_unsigned	  = false;
					int		 width = target_ty->width > 0 ? target_ty->width : 64;
					uint64_t bits  = is_unsigned ? out_unsigned : static_cast<uint64_t>(out_signed);
					out_signed	  = signed_bits(bits, width);
					out_unsigned  = static_cast<uint64_t>(out_signed);
				}
			}
		}
		return true;
	}

	default:
		return false;
	}
}

bool Sema::eval_array_len_expr(const Expr &e, Module &m, uint64_t &out_value) {
	int64_t	 sval  = 0;
	uint64_t uval  = 0;
	bool	 is_un = false;
	if (!eval_comptime_int_expr(e, m, sval, uval, is_un)) {
		return false;
	}
	if (is_un) {
		out_value = uval;
		return true;
	}
	if (sval < 0) {
		return false;
	}
	out_value = static_cast<uint64_t>(sval);
	return true;
}

StType *Sema::resolve_type(TypeRef &tr, Module &m) {
	if (tr.kind == TypeRef::TypeOf) {
		if (!tr.typeof_expr) {
			return types_.void_ty();
		}
		// Build a minimal dummy FnDecl for check_expr context.  The
		// fn_sig must be set so that any code path reaching it from
		// inside the @of.type expression (e.g. a function-pointer lookup)
		// doesn't dereference a null pointer.
		FnDecl dummy_fn;
		dummy_fn.name	= "<@of.type>";
		dummy_fn.loc	= tr.loc;
		dummy_fn.fn_sig = types_.function_of({}, types_.void_ty(), false);
		ScopeStack dummy_scope;
		dummy_scope.enter();
		StType *t = check_expr(*tr.typeof_expr, dummy_fn, m, dummy_scope);
		return t ? t : types_.void_ty();
	}
	if (tr.kind == TypeRef::Pointer) {
		if (!tr.base) {
			return types_.pointer(types_.void_ty(), tr.is_mutable, tr.is_volatile);
		}
		StType *elem	= resolve_type(*tr.base, m);
		tr.sema_invalid = tr.base->sema_invalid;
		return types_.pointer(elem, tr.is_mutable, tr.is_volatile);
	}
	if (tr.kind == TypeRef::Slice) {
		StType *elem	= tr.base ? resolve_type(*tr.base, m) : types_.void_ty();
		tr.sema_invalid = !tr.base || (tr.base && tr.base->sema_invalid);
		return types_.slice(elem, tr.is_mutable, tr.is_volatile);
	}
	if (tr.kind == TypeRef::Array) {
		StType *elem	= tr.base ? resolve_type(*tr.base, m) : types_.void_ty();
		tr.sema_invalid = !tr.base || (tr.base && tr.base->sema_invalid);
		if (tr.array_len_inferred) {
			log::report(log::Level::Error, tr.loc,
						"array length cannot be inferred without an initializer");
			tr.sema_invalid		  = true;
			tr.array_len_inferred = false;
		}
		size_t len = tr.array_len;
		if (tr.array_len_expr) {
			uint64_t value = 0;
			if (!eval_array_len_expr(*tr.array_len_expr, m, value)) {
				log::report(log::Level::Error, tr.loc,
							"array length is not a valid non-negative compile-time constant");
				tr.sema_invalid = true;
				len = 0;
			} else if (!fits_uint_width(value, types_.pointer_width()) ||
				   value > std::numeric_limits<size_t>::max()) {
				log::report(log::Level::Error, tr.loc,
							"array length does not fit the target usize type");
				tr.sema_invalid = true;
				len = 0;
			} else {
				len = static_cast<size_t>(value);
				tr.array_len = len;
			}
		}
		return types_.array_of(elem, len);
	}
	if (tr.kind == TypeRef::Function) {
		validate_attribute_list(tr.fn_attrs, AttributeTarget::FunctionType, "function types");
		AbiKind abi = resolve_abi_attr(tr.fn_attrs);
		if (tr.fn_variadic && abi != AbiKind::C) {
			log::report(log::Level::Error, tr.loc,
						"variadic function types (...) are only supported with #[abi(\"C\")]");
		}
		std::vector<StType *> params;
		params.reserve(tr.fn_params.size());
		for (auto &p : tr.fn_params) {
			StType *pt = p ? resolve_type(*p, m) : types_.void_ty();
			if (abi == AbiKind::C && (!p || !p->sema_invalid) && !is_ffi_safe_value_type(pt)) {
				log::report(log::Level::Error, p ? p->loc : tr.loc,
							"#[abi(\"C\")] function parameter type '" + type_name_for_diag(pt) +
								"' is not FFI-safe (use #[repr(\"C\")] or pass a pointer)");
			}
			params.push_back(pt);
			tr.sema_invalid = tr.sema_invalid || (p && p->sema_invalid);
		}
		StType *ret		= tr.fn_ret ? resolve_type(*tr.fn_ret, m) : types_.void_ty();
		if (abi == AbiKind::C && (!tr.fn_ret || !tr.fn_ret->sema_invalid) &&
			!is_ffi_safe_value_type(ret)) {
			log::report(log::Level::Error, tr.fn_ret ? tr.fn_ret->loc : tr.loc,
						"#[abi(\"C\")] function return type '" + type_name_for_diag(ret) +
							"' is not FFI-safe (use #[repr(\"C\")] or return a pointer)");
		}
		tr.sema_invalid = tr.sema_invalid || (tr.fn_ret && tr.fn_ret->sema_invalid);
		return types_.function_of(std::move(params), ret, tr.fn_variadic, abi);
	}
	if (tr.kind == TypeRef::SelfType || tr.single == "Self") {
		if (active_self_type_) {
			return active_self_type_;
		}
		log::error(tr.loc, "'Self' is only allowed within trait definitions and impl blocks")
			.code("E_INVALID_SELF")
			.phase("sema")
			.emit();
		tr.sema_invalid = true;
		return types_.poison_ty();
	}
	// Named
	if (tr.single == "type" && tr.generic_args.empty()) {
		return types_.type_ty();
	}
	if (generic_context_ && tr.generic_args.empty()) {
		auto it = generic_context_->generic_type_subst.find(tr.single);
		if (it != generic_context_->generic_type_subst.end()) {
			return it->second;
		}
	}
	StType *p = prim_from_name(types_, tr.single);
	if (p) {
		return p;
	}
	if (is_generic_type_set_name(tr.single)) {
		log::report(log::Level::Error, tr.loc,
					"generic-only type-set name '" + tr.single +
						"' is only valid in a generic parameter declaration",
					"E_GENERIC_TYPE_SET", "sema");
		tr.sema_invalid = true;
		return types_.void_ty();
	}
	if (tr.name.find('.') != std::string::npos) {
		mark_import_use(split_scopes(tr.name), m);
	}
	Symbol *s = lookup_named_type(resolver_, *this, m, tr.single, tr.name);
	if (s) {
		if (s->kind == SymKind::Mod) {
			log::error(tr.loc, "'" + tr.name + "' is a module, not a type")
				.code("E_UNDEFINED_TYPE")
				.phase("semantic")
				.emit();
			tr.sema_invalid = true;
			return types_.poison_ty();
		}
		if (s->kind == SymKind::Struct && !s->st->generic_params.empty()) {
			return instantiate_generic_type(*s, tr, m);
		}
		if (s->kind == SymKind::Union && !s->un->generic_params.empty()) {
			return instantiate_generic_type(*s, tr, m);
		}
		if (s->kind == SymKind::TypeBinding && s->tb && !s->tb->generic_params.empty()) {
			return instantiate_generic_type(*s, tr, m);
		}
		if (s->kind == SymKind::Enum && s->en && !s->en->generic_params.empty()) {
			return instantiate_generic_type(*s, tr, m);
		}
		if (s->kind == SymKind::Struct) {
			return s->st->resolved;
		}
		if (s->kind == SymKind::Union) {
			return s->un->resolved;
		}
		if (s->kind == SymKind::TypeBinding && s->tb) {
			StType *result = resolve_type_binding(*s, m);
			if (s->tb->sema_invalid) {
				tr.sema_invalid = true;
			}
			return result;
		}
		if (s->kind == SymKind::Enum) {
			return s->type;
		}
	}
	if (Symbol *any_sym = resolve_path_any(split_scopes(tr.name.empty() ? tr.single : tr.name), m);
		any_sym && any_sym->kind == SymKind::Mod) {
		log::error(tr.loc, "'" + tr.name + "' is a module, not a type")
			.code("E_UNDEFINED_TYPE")
			.phase("semantic")
			.emit();
		tr.sema_invalid = true;
		return types_.poison_ty();
	}
	log::error(tr.loc, "unknown type '" + tr.name + "'")
		.code("E_UNDEFINED_TYPE")
		.phase("semantic")
		.emit();
	tr.sema_invalid = true;
	return types_.poison_ty();
}

StType *Sema::resolve_type_binding(Symbol &symbol, Module &m) {
	if (!symbol.tb) {
		return types_.void_ty();
	}
	TypeBindingDecl &binding = *symbol.tb;
	if (binding.resolved) {
		return binding.resolved;
	}
	if (binding.sema_invalid) {
		return types_.void_ty();
	}
	if (binding.resolving) {
		log::report(log::Level::Error, binding.loc,
					"cyclic type binding involving '" + binding.name + "'",
					"E_TYPE_BINDING_CYCLE", "sema");
		binding.sema_invalid = true;
		return types_.void_ty();
	}

	binding.resolving = true;
	Module &binding_module = symbol.owner_module ? *symbol.owner_module : m;
	StType *target = binding.target ? resolve_type(*binding.target, binding_module) : nullptr;
	binding.resolving = false;
	if (!target || (binding.target && binding.target->sema_invalid)) {
		binding.sema_invalid = true;
		target = types_.void_ty();
	}
	if (underlying_type(target) && underlying_type(target)->kind == TypeKind::VaList) {
		log::report(log::Level::Error, binding.loc,
					"@va.list cannot be hidden behind a user-defined type binding");
	}
	binding.resolved = types_.type_binding_of(symbol.mangled, target, binding.is_pub);
	symbol.type = binding.resolved;
	return binding.resolved;
}

StType *Sema::resolve_type_in_context(TypeRef &tr, Module &m, FnDecl *context) {
	FnDecl *saved = generic_context_;
	if (context && context->is_generic_instance) {
		generic_context_ = context;
	}
	StType *result	 = resolve_type(tr, m);
	generic_context_ = saved;
	return result;
}

void Sema::validate_generic_params(std::vector<GenericParam> &params) {
	std::unordered_set<std::string> names;
	for (auto &param : params) {
		if (!names.insert(param.name).second) {
			log::report(log::Level::Error, param.loc,
						"duplicate generic parameter '" + param.name + "'");
		}
		std::function<std::string(const TypeRef &)> find_set_name =
			[&](const TypeRef &type) -> std::string {
			if (type.kind == TypeRef::Named && is_generic_type_set_name(type.single)) {
				return type.single;
			}
			if (type.base) {
				if (std::string nested = find_set_name(*type.base); !nested.empty()) {
					return nested;
				}
			}
			for (const auto &nested : type.fn_params) {
				if (nested) {
					if (std::string found = find_set_name(*nested); !found.empty()) {
						return found;
					}
				}
			}
			return type.fn_ret ? find_set_name(*type.fn_ret) : std::string{};
		};
		if (param.const_type) {
			if (std::string set_name = find_set_name(*param.const_type); !set_name.empty()) {
				log::report(log::Level::Error, param.const_type->loc,
							"generic-only type-set name '" + set_name +
								"' is only valid in a generic parameter declaration",
							"E_GENERIC_TYPE_SET", "sema");
			}
		}
		if (param.kind == GenericParam::Kind::Const && !param.trait_bounds.empty()) {
			for (const auto &tb : param.trait_bounds) {
				if (tb) {
					log::report(log::Level::Error, tb->loc,
								"trait bounds are only valid on type parameter '" + param.name + "'");
				}
			}
			param.trait_bounds.clear();
		}
		if (param.type_set.empty()) {
			continue;
		}
		if (param.kind == GenericParam::Kind::Const) {
			for (const auto &member : param.type_set) {
				log::report(log::Level::Error, member.loc,
							"type-set constraints are only valid on type parameter '" + param.name +
								"'");
			}
			param.type_set.clear();
			continue;
		}

		std::vector<GenericParam::TypeSetMember> normalized;
		for (const auto &source : param.type_set) {
			GenericParam::TypeSetMember member = source;
			if (is_generic_type_set_name(member.name)) {
				if (member.name == "i") {
					member.kind = GenericParam::TypeSetMember::Kind::Sint;
				} else if (member.name == "u") {
					member.kind = GenericParam::TypeSetMember::Kind::Uint;
				} else {
					member.kind = GenericParam::TypeSetMember::Kind::Float;
				}
				normalized.push_back(std::move(member));
				continue;
			}
			StType *primitive =
				is_prim_type_name(member.name) ? prim_from_name(types_, member.name) : nullptr;
			StType *u	  = underlying_type(primitive);
			bool	valid = u && (u->kind == TypeKind::Void || u->kind == TypeKind::Bool ||
								  u->kind == TypeKind::UInt || u->kind == TypeKind::SInt ||
								  u->kind == TypeKind::Float);
			if (!valid) {
				log::report(log::Level::Error, member.loc,
							"unknown or non-primitive generic type-set member '" + member.name +
								"' for parameter '" + param.name + "'");
				continue;
			}
			member.kind		   = GenericParam::TypeSetMember::Kind::Exact;
			member.name		   = u->name;
			member.exact_type  = u->kind;
			member.exact_width = u->width;
			normalized.push_back(std::move(member));
		}

		bool has_sint = false, has_uint = false, has_float = false;
		for (const auto &member : normalized) {
			has_sint |= member.kind == GenericParam::TypeSetMember::Kind::Sint;
			has_uint |= member.kind == GenericParam::TypeSetMember::Kind::Uint;
			has_float |= member.kind == GenericParam::TypeSetMember::Kind::Float;
		}
		std::vector<GenericParam::TypeSetMember> result;
		std::unordered_set<std::string>			 seen;
		for (const auto &member : normalized) {
			bool covered = false;
			if (member.kind == GenericParam::TypeSetMember::Kind::Exact) {
				if ((member.exact_type == TypeKind::SInt && has_sint) ||
					(member.exact_type == TypeKind::UInt && has_uint) ||
					(member.exact_type == TypeKind::Float && has_float)) {
					covered = true;
				}
			}
			if (covered) {
				continue;
			}
			std::string key;
			switch (member.kind) {
			case GenericParam::TypeSetMember::Kind::Exact:
				key = "e:" + std::to_string(static_cast<int>(member.exact_type)) + ":" +
					  std::to_string(member.exact_width);
				break;
			case GenericParam::TypeSetMember::Kind::Sint:
				key = "c:i";
				break;
			case GenericParam::TypeSetMember::Kind::Uint:
				key = "c:u";
				break;
			case GenericParam::TypeSetMember::Kind::Float:
				key = "c:f";
				break;
			}
			if (seen.insert(key).second) {
				result.push_back(member);
			}
		}
		param.type_set = std::move(result);
	}
}

bool Sema::generic_type_allowed(const GenericParam &param, StType *supplied) const {
	StType *u = underlying_type(supplied);
	if (!u) {
		return false;
	}
	for (const auto &member : param.type_set) {
		bool match = false;
		switch (member.kind) {
		case GenericParam::TypeSetMember::Kind::Sint:
			match = u->kind == TypeKind::SInt;
			break;
		case GenericParam::TypeSetMember::Kind::Uint:
			match = u->kind == TypeKind::UInt;
			break;
		case GenericParam::TypeSetMember::Kind::Float:
			match = u->kind == TypeKind::Float;
			break;
		case GenericParam::TypeSetMember::Kind::Exact:
			match = u->kind == member.exact_type &&
					((u->kind == TypeKind::UInt || u->kind == TypeKind::SInt ||
					  u->kind == TypeKind::Float)
						 ? u->width == member.exact_width
						 : true);
			break;
		}
		if (match) {
			return true;
		}
	}
	return false;
}

std::string Sema::generic_constraint_text(const GenericParam &param) const {
	std::string result = "{";
	for (size_t i = 0; i < param.type_set.size(); ++i) {
		if (i) {
			result += ", ";
		}
		const auto &member = param.type_set[i];
		switch (member.kind) {
		case GenericParam::TypeSetMember::Kind::Exact:
			result += member.name;
			break;
		case GenericParam::TypeSetMember::Kind::Sint:
			result += "i";
			break;
		case GenericParam::TypeSetMember::Kind::Uint:
			result += "u";
			break;
		case GenericParam::TypeSetMember::Kind::Float:
			result += "f";
			break;
		}
	}
	return result + "}";
}

bool Sema::is_dead_branch(Expr &cond) {
	return !eval_define_cond(cond, opts_.flags);
}

} // namespace stellac
