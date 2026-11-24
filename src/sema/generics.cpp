#include "internal.hpp"

namespace stellac {

bool Sema::infer_generic_type(TypeRef &pattern, StType *actual, FnDecl &fn, Module &m) {
	if (!actual) {
		return false;
	}
	if (pattern.kind == TypeRef::Named) {
		if (pattern.generic_args.empty()) {
			for (const auto &p : fn.generic_params) {
				if (p.kind == GenericParam::Kind::Type && p.name == pattern.single) {
					auto it = fn.generic_type_subst.find(p.name);
					if (it != fn.generic_type_subst.end()) {
						return eq_type(it->second, actual);
					}
					fn.generic_type_subst.emplace(p.name, actual);
					return true;
				}
			}
		} else {
			StType *u = underlying_type(actual);
			Symbol *pat_sym = lookup_named_type(resolver_, *this, m, pattern.single, pattern.name);
			if (!pat_sym && fn.decl_module && fn.decl_module != &m) {
				pat_sym = lookup_named_type(resolver_, *this, *fn.decl_module, pattern.single,
											pattern.name);
			}
			if (u && u->generic_origin && pat_sym &&
				(u->generic_origin == pat_sym || u->generic_origin->mangled == pat_sym->mangled) &&
				pattern.generic_args.size() == u->generic_arg_is_const.size()) {
				for (size_t i = 0; i < pattern.generic_args.size(); ++i) {
					if (!u->generic_arg_is_const[i]) {
						if (pattern.generic_args[i].kind != GenericArg::Kind::Type ||
							!pattern.generic_args[i].type) {
							return false;
						}
						if (!infer_generic_type(*pattern.generic_args[i].type,
												u->generic_type_args[i], fn, m)) {
							return false;
						}
					} else {
						std::string cname;
						if (pattern.generic_args[i].kind == GenericArg::Kind::Type &&
							pattern.generic_args[i].type &&
							pattern.generic_args[i].type->kind == TypeRef::Named &&
							pattern.generic_args[i].type->generic_args.empty()) {
							cname = pattern.generic_args[i].type->single;
						} else if (pattern.generic_args[i].kind == GenericArg::Kind::Value &&
								   pattern.generic_args[i].value &&
								   pattern.generic_args[i].value->kind == ExprKind::Ident &&
								   pattern.generic_args[i].value->path.size() == 1) {
							cname = pattern.generic_args[i].value->path[0];
						}
						bool bound_param = false;
						if (!cname.empty()) {
							for (const auto &p : fn.generic_params) {
								if (p.kind == GenericParam::Kind::Const && p.name == cname) {
									auto it = fn.generic_const_subst.find(cname);
									if (it != fn.generic_const_subst.end() &&
										it->second != u->generic_const_args[i]) {
										return false;
									}
									fn.generic_const_subst[cname] = u->generic_const_args[i];
									bound_param = true;
									break;
								}
							}
						}
						if (!bound_param) {
							int64_t val = 0;
							if (pattern.generic_args[i].kind == GenericArg::Kind::Value &&
								pattern.generic_args[i].value &&
								eval_generic_const_expr(*pattern.generic_args[i].value,
														fn.generic_const_subst, val)) {
								if (val != u->generic_const_args[i]) {
									return false;
								}
							} else {
								return false;
							}
						}
					}
				}
				return true;
			}
			if (type_ref_contains_generic(pattern, fn)) {
				return false;
			}
		}
		// Concrete named types can occur inside a generic pattern, for example
		// the `i32` return type of a comparator `fn(*T, *T) i32`. They do not
		// bind a generic parameter, but they still need to match the actual type
		// for nested inference to succeed.
		StType *expected = resolve_type(pattern, m);
		return expected && eq_type(expected, actual);
	}
	StType *u = underlying_type(actual);
	if (pattern.kind == TypeRef::Pointer && u && u->kind == TypeKind::Pointer && pattern.base &&
		(!pattern.is_mutable || u->is_mutable) && (!u->is_volatile || pattern.is_volatile)) {
		return infer_generic_type(*pattern.base, u->base, fn, m);
	}
	if (pattern.kind == TypeRef::Slice && u && pattern.base &&
		(!pattern.is_mutable || u->is_mutable) && (!u->is_volatile || pattern.is_volatile)) {
		if (u->kind == TypeKind::Slice) {
			return infer_generic_type(*pattern.base, u->base, fn, m);
		}
		if (u->kind == TypeKind::Pointer && u->base && u->base->kind == TypeKind::Array) {
			return infer_generic_type(*pattern.base, u->base->base, fn, m);
		}
	}
	if (pattern.kind == TypeRef::Array && u && u->kind == TypeKind::Array && pattern.base) {
		if (pattern.array_len_expr && pattern.array_len_expr->kind == ExprKind::Ident &&
			pattern.array_len_expr->path.size() == 1) {
			const std::string &n = pattern.array_len_expr->path[0];
			for (const auto &p : fn.generic_params) {
				if (p.kind == GenericParam::Kind::Const && p.name == n) {
					if (u->array_len > static_cast<size_t>(INT64_MAX)) {
						return false;
					}
					const int64_t actual_length = static_cast<int64_t>(u->array_len);
					auto it = fn.generic_const_subst.find(n);
					if (it != fn.generic_const_subst.end() && it->second != actual_length) {
						return false;
					}
					fn.generic_const_subst[n] = actual_length;
				}
			}
		} else if (pattern.array_len != u->array_len) {
			return false;
		}
		return infer_generic_type(*pattern.base, u->base, fn, m);
	}
	if (pattern.kind == TypeRef::Function && u && u->kind == TypeKind::Function &&
		pattern.fn_params.size() == u->fn_params.size()) {
		for (size_t i = 0; i < pattern.fn_params.size(); ++i) {
			if (!infer_generic_type(*pattern.fn_params[i], u->fn_params[i], fn, m)) {
				return false;
			}
		}
		return pattern.fn_ret && infer_generic_type(*pattern.fn_ret, u->fn_ret, fn, m);
	}
	return false;
}

bool Sema::bind_generic_arguments(FnDecl &fn, Expr &call,
								  const std::vector<std::unique_ptr<Expr>> &args, Module &m,
								  FnDecl &caller) {
	fn.generic_type_subst.clear();
	fn.generic_const_subst.clear();
	fn.generic_const_string_subst.clear();
	if (expected_generic_result_ && fn.ret) {
		bool has_inferable_return = type_ref_contains_generic(*fn.ret, fn);
		if (has_inferable_return && !infer_generic_type(*fn.ret, expected_generic_result_, fn, m)) {
			return false;
		}
	}
	const std::vector<GenericArg> *explicit_args = call.lhs ? &call.lhs->generic_args : nullptr;
	const size_t type_prefix_args =
		(call.lhs && call.lhs->kind == ExprKind::Member && call.lhs->lhs)
			? call.lhs->lhs->generic_args.size()
			: 0;
	size_t explicit_offset = 0;
	size_t explicit_end	   = 0;
	if (explicit_args && !explicit_args->empty()) {
		if (explicit_args->size() == fn.generic_params.size()) {
			explicit_offset = 0;
			explicit_end	= fn.generic_params.size();
		} else if (fn.impl_generic_param_count > 0 &&
				   type_prefix_args == fn.impl_generic_param_count &&
				   explicit_args->size() == fn.impl_generic_param_count) {
			explicit_offset = 0;
			explicit_end	= fn.impl_generic_param_count;
		} else if (fn.impl_generic_param_count > 0 && type_prefix_args == 0 &&
				   explicit_args->size() == fn.generic_params.size() - fn.impl_generic_param_count) {
			explicit_offset = fn.impl_generic_param_count;
			explicit_end	= fn.generic_params.size();
		} else {
			return false;
		}
	}
	FnDecl *saved	 = generic_context_;
	// Explicit type arguments are resolved in the caller's module/context.
	generic_context_ = &caller;
	auto generic_const_fits = [&](const GenericParam &param, Expr &value) {
		if (!param.const_type) {
			return false;
		}
		auto resolved_value = clone_expr(value);
		Expr *candidate = resolved_value.get();
		const auto &caller_params = caller.generic_template
			? caller.generic_template->generic_params
			: caller.generic_params;
		std::function<bool(Expr &)> resolve_caller_constants = [&](Expr &expr) {
			if (expr.kind == ExprKind::Ident && expr.path.size() == 1 &&
				caller.generic_const_subst.count(expr.path[0])) {
				expr.int_val = static_cast<uint64_t>(
					caller.generic_const_subst.at(expr.path[0]));
				expr.has_const_value = true;
				for (const auto &caller_param : caller_params) {
					if (caller_param.kind == GenericParam::Kind::Const &&
						caller_param.name == expr.path[0] && caller_param.const_type) {
						expr.ty = resolve_type(*caller_param.const_type, m);
						break;
					}
				}
				return expr.ty != nullptr;
			}
			if (expr.lhs && !resolve_caller_constants(*expr.lhs)) return false;
			if (expr.rhs && !resolve_caller_constants(*expr.rhs)) return false;
			if (expr.condition && !resolve_caller_constants(*expr.condition)) return false;
			if (expr.then_expr && !resolve_caller_constants(*expr.then_expr)) return false;
			if (expr.else_expr && !resolve_caller_constants(*expr.else_expr)) return false;
			for (auto &arg : expr.args) if (arg && !resolve_caller_constants(*arg)) return false;
			return true;
		};
		if (!resolve_caller_constants(*candidate)) {
			return false;
		}
		FnDecl *value_context = generic_context_;
		generic_context_ = &fn;
		StType *value_type = resolve_type(*param.const_type, m);
		generic_context_ = value_context;
		return value_type && value_fits(value_type, *candidate) == FitResult::Fit;
	};
	for (size_t i = 0; i < fn.generic_params.size(); ++i) {
		const auto &p = fn.generic_params[i];
		if (explicit_args && !explicit_args->empty() && i >= explicit_offset && i < explicit_end) {
			const GenericArg &a = (*explicit_args)[i - explicit_offset];
			if (p.kind == GenericParam::Kind::Type) {
				if (a.kind != GenericArg::Kind::Type || !a.type) {
					generic_context_ = saved;
					return false;
				}
				fn.generic_type_subst[p.name] = resolve_type(*a.type, m);
			} else {
				int64_t v = 0;
				if (a.kind == GenericArg::Kind::Value && a.value) {
					const auto &visible_consts = generic_context_
													 ? generic_context_->generic_const_subst
													 : fn.generic_const_subst;
					if (!eval_generic_const_expr(*a.value, visible_consts, v)) {
						int64_t sval = 0;
						uint64_t uval = 0;
						bool is_un = false;
						if (eval_comptime_int_expr(*a.value, m, sval, uval, is_un)) {
							v = sval;
							if (!generic_const_fits(p, *a.value)) {
								generic_context_ = saved;
								return false;
							}
							fn.generic_const_subst[p.name] = v;
						} else if (a.value->kind == ExprKind::StringLit) {
							fn.generic_const_string_subst[p.name] = a.value->str_val;
						} else if (a.value->kind == ExprKind::Ident &&
							   a.value->path.size() == 1 &&
							   caller.generic_const_string_subst.count(a.value->path[0])) {
							fn.generic_const_string_subst[p.name] =
								caller.generic_const_string_subst.at(a.value->path[0]);
						} else {
							generic_context_ = saved;
							return false;
						}
					} else {
						if (!generic_const_fits(p, *a.value)) {
							generic_context_ = saved;
							return false;
						}
						fn.generic_const_subst[p.name] = v;
					}
				} else if (a.kind == GenericArg::Kind::Type && a.type &&
						   a.type->kind == TypeRef::Named) {
					Expr ident;
					ident.kind	  = ExprKind::Ident;
					ident.loc	  = a.type->loc;
					if (!a.type->name.empty()) {
						ident.path = split_dotted_path(a.type->name);
					} else {
						ident.path = {a.type->single};
					}
					ident.str_val = a.type->single;
					bool got_val  = false;
					if (eval_generic_const_expr(ident, caller.generic_const_subst, v)) {
						const auto &caller_params = caller.generic_template
							? caller.generic_template->generic_params
							: caller.generic_params;
						for (const auto &caller_param : caller_params) {
							if (caller_param.kind == GenericParam::Kind::Const &&
								caller_param.name == a.type->single && caller_param.const_type) {
								ident.ty = resolve_type(*caller_param.const_type, m);
								break;
							}
						}
						got_val = true;
					} else {
						int64_t  sval  = 0;
						uint64_t uval  = 0;
						bool	 is_un = false;
						if (eval_comptime_int_expr(ident, m, sval, uval, is_un)) {
							v = sval;
							Symbol *s = resolve_path(ident.path, m);
							if (s && s->type) {
								ident.ty = s->type;
							}
							got_val = true;
						}
					}
					if (!got_val) {
						generic_context_ = saved;
						return false;
					}
					ident.int_val = static_cast<uint64_t>(v);
					ident.has_const_value = true;
					if (!ident.ty || !generic_const_fits(p, ident)) {
						generic_context_ = saved;
						return false;
					}
					fn.generic_const_subst[p.name] = v;
				} else {
					generic_context_ = saved;
					return false;
				}
			}
		}
	}
	generic_context_ = saved;
	for (size_t i = 0; i < args.size() && i < fn.params.size(); ++i) {
		if (fn.params[i].is_comptime) {
			if (!args[i]->type_value || !eq_type(args[i]->ty, types_.type_ty())) {
				return false;
			}
			auto it = fn.generic_type_subst.find(fn.params[i].name);
			if (it != fn.generic_type_subst.end()) {
				if (!eq_type(it->second, args[i]->type_value)) {
					return false;
				}
			} else {
				fn.generic_type_subst[fn.params[i].name] = args[i]->type_value;
			}
			continue;
		}
		// Explicit type arguments contextually type literal arguments later,
		// after overload selection; their provisional default type must not
		// conflict with the explicit specialization.
		if (((explicit_args && !explicit_args->empty()) || expected_generic_result_) &&
			is_contextual_literal(*args[i]) && fn.params[i].ty &&
			(!type_ref_contains_unbound_generic(*fn.params[i].ty, fn) ||
			 (fn.is_macro && expected_generic_result_ &&
			  (!explicit_args || explicit_args->empty())))) {
			continue;
		}
		// Fixed parameters such as a buffer length do not participate in generic
		// substitution. Their ordinary overload/type compatibility is checked
		// below after specialization; attempting to infer them would reject a
		// representable contextual literal (for example `6` for `usize`).
		if (fn.params[i].ty && !type_ref_contains_generic(*fn.params[i].ty, fn)) {
			continue;
		}
		// A typed argument may bind T while a later literal argument still has
		// its provisional default integer type. Let overload selection apply the
		// contextual T type instead of rejecting the specialization here.
		if (is_contextual_literal(*args[i]) && fn.params[i].ty &&
			!type_ref_contains_unbound_generic(*fn.params[i].ty, fn)) {
			continue;
		}
		if (!fn.params[i].ty || !infer_generic_type(*fn.params[i].ty, args[i]->ty, fn, m)) {
			return false;
		}
	}
	// A value-producing macro may omit its return type: its result type is
	// inferred from `yield` expressions after expansion.  Contextual literals
	// are intentionally not used to bind a generic parameter, so use the
	// destination type when it identifies the one remaining type parameter.
	if (fn.is_macro && expected_generic_result_ &&
		underlying_type(expected_generic_result_)->kind != TypeKind::Void &&
		(!explicit_args || explicit_args->empty())) {
		std::string unbound;
		unsigned unbound_count = 0;
		for (const auto &p : fn.generic_params) {
			if (p.kind == GenericParam::Kind::Type && !fn.generic_type_subst.count(p.name)) {
				unbound = p.name;
				++unbound_count;
			}
		}
		if (unbound_count == 1) {
			fn.generic_type_subst[unbound] = expected_generic_result_;
		}
	}
	for (const auto &p : fn.generic_params) {
		if (p.kind == GenericParam::Kind::Type && !fn.generic_type_subst.count(p.name)) {
			return false;
		}
		if (p.kind == GenericParam::Kind::Const && !fn.generic_const_subst.count(p.name) &&
			!fn.generic_const_string_subst.count(p.name)) {
			return false;
		}
		if (p.kind == GenericParam::Kind::Const && p.const_type &&
			(!explicit_args || explicit_args->empty()) && fn.generic_const_subst.count(p.name)) {
			Expr inferred_value;
			inferred_value.kind = ExprKind::IntLit;
			inferred_value.int_val = static_cast<uint64_t>(fn.generic_const_subst.at(p.name));
			if (!generic_const_fits(p, inferred_value)) {
				return false;
			}
		}
		if (p.kind == GenericParam::Kind::Type && !p.type_set.empty()) {
			StType *supplied = fn.generic_type_subst[p.name];
			if (!generic_type_allowed(p, supplied)) {
				last_generic_constraint_error_ =
					"generic parameter '" + p.name + "' supplied type '" +
					type_name_for_diag(supplied) + "', allowed set " + generic_constraint_text(p);
				return false;
			}
		}
		if (p.kind == GenericParam::Kind::Type && !p.trait_bounds.empty()) {
			StType *supplied = fn.generic_type_subst[p.name];
			Module &decl_m = fn.decl_module ? *fn.decl_module : m;
			for (const auto &bound_ref : p.trait_bounds) {
				if (!bound_ref) continue;
				std::string tname = bound_ref->name.empty() ? bound_ref->single : bound_ref->name;
				Symbol *trait_sym = resolve_path(split_scopes(tname), decl_m);
				if (!trait_sym) {
					trait_sym = resolve_path_any(split_scopes(tname), decl_m);
				}
				if (!trait_sym || trait_sym->kind != SymKind::Trait || !trait_sym->trait_decl) {
					return false;
				}
				TraitDecl *trait = trait_sym->trait_decl;
				if (!type_implements_trait(supplied, trait)) {
					last_generic_constraint_error_ =
						"type '" + type_name_for_diag(supplied) + "' does not implement trait '" +
						trait->name + "'";
					return false;
				}
			}
		}
	}
	return true;
}

FnDecl *Sema::instantiate_generic_fn(FnDecl &fn, Expr &call,
									 const std::vector<std::unique_ptr<Expr>> &args, Module &m,
									 FnDecl &caller) {
	last_generic_constraint_error_.clear();
	if (fn.is_extern || has_attr(fn.attrs, "link_name")) {
		log::report(log::Level::Error, call.loc,
					"generic extern/export functions are not supported");
		return nullptr;
	}
	fn.used = true;
	if (!bind_generic_arguments(fn, call, args, m, caller)) {
		return nullptr;
	}
	std::string					  base_key = fn.mangled.empty() ? mangle(fn.full_path) : fn.mangled;
	std::vector<GenericMangleArg> mangle_args;
	mangle_args.reserve(fn.generic_params.size());
	for (const auto &p : fn.generic_params) {
		GenericMangleArg arg;
		if (p.kind == GenericParam::Kind::Type) {
			arg.kind = GenericMangleArg::Kind::Type;
			arg.type = fn.generic_type_subst[p.name];
		} else if (fn.generic_const_subst.count(p.name)) {
			arg.kind	= GenericMangleArg::Kind::ConstInt;
			arg.int_val = fn.generic_const_subst[p.name];
		} else {
			arg.kind	= GenericMangleArg::Kind::ConstString;
			arg.str_val = fn.generic_const_string_subst[p.name];
		}
		mangle_args.push_back(std::move(arg));
	}
	std::string key		  = mangle_generic(base_key, mangle_args, fn.is_macro);
	std::string cache_key = std::to_string(reinterpret_cast<uintptr_t>(&fn)) + "|" + key;
	if (auto it = generic_instance_cache_.find(cache_key); it != generic_instance_cache_.end()) {
		return it->second;
	}
	if (!generic_in_progress_.insert(cache_key).second) {
		log::report(log::Level::Error, call.loc,
					"recursive generic specialization cycle for '" + key + "'");
		return nullptr;
	}
	auto inst						 = std::make_unique<FnDecl>();
	inst->owner_path				 = fn.owner_path;
	inst->name						 = fn.name;
	inst->full_path					 = fn.full_path;
	inst->is_pub					 = fn.is_pub;
	inst->is_method					 = fn.is_method;
	inst->is_variadic				 = fn.is_variadic;
	inst->is_macro					 = fn.is_macro;
	inst->macro_returns_value			 = fn.macro_returns_value;
	inst->is_extern					 = false;
	inst->abi						 = fn.abi;
	inst->loc						 = call.loc;
	inst->attrs						 = fn.attrs;
	inst->generic_template			 = &fn;
	inst->is_generic_instance		 = true;
	inst->decl_module				 = fn.decl_module;
	inst->generic_type_subst		 = fn.generic_type_subst;
	inst->generic_const_subst		 = fn.generic_const_subst;
	inst->generic_const_string_subst = fn.generic_const_string_subst;
	for (const auto &p : fn.params) {
		Param np;
		np.name		   = p.name;
		np.loc		   = p.loc;
		np.attrs	   = p.attrs;
		np.is_comptime = p.is_comptime;
		np.is_mutable  = p.is_mutable;
		np.ty		   = p.ty ? clone_typeref(*p.ty) : nullptr;
		inst->params.push_back(std::move(np));
	}
	inst->ret = fn.ret ? clone_typeref(*fn.ret) : nullptr;
	if (fn.body) {
		inst->body = clone_stmt(*fn.body);
	}
	inst->mangled				 = key;
	FnDecl *raw					 = inst.get();
	generic_instance_cache_[cache_key] = raw;
	generic_instances_owned_.push_back(std::move(inst));
	generic_instances_view_.push_back(raw);
	Module &origin = fn.decl_module ? *fn.decl_module : m;
	auto saved_path = origin.lexical_path;
	origin.lexical_path = origin.mod_path;
	origin.lexical_path.insert(origin.lexical_path.end(), raw->owner_path.begin(), raw->owner_path.end());
	resolve_fn_signature(*raw, origin);
	if (!raw->is_macro && !generic_checked_.count(raw)) {
		generic_checked_.insert(raw);
		check_fn(*raw, origin);
	}
	origin.lexical_path = std::move(saved_path);
	generic_in_progress_.erase(cache_key);
	return raw;
}

StType *Sema::instantiate_generic_type(Symbol &sym, TypeRef &tr, Module &m) {
	std::vector<GenericParam>			  *params = nullptr;
	std::vector<std::unique_ptr<TypeRef>> *fields = nullptr;
	std::vector<std::string>			   names;
	StType *(*make_unused)(void) = nullptr;
	(void)make_unused;
	if (sym.kind == SymKind::Struct) {
		params = &sym.st->generic_params;
		fields = &sym.st->field_types;
		names  = sym.st->field_names;
	} else if (sym.kind == SymKind::Union) {
		params = &sym.un->generic_params;
		fields = &sym.un->field_types;
		names  = sym.un->field_names;
	} else if (sym.kind == SymKind::TypeBinding) {
		params = &sym.tb->generic_params;
	} else if (sym.kind == SymKind::Enum) {
		params = &sym.en->generic_params;
	}
	if (!params || tr.generic_args.size() != params->size()) {
		log::report(log::Level::Error, tr.loc,
					"generic type '" + tr.name + "' requires " +
						(params ? std::to_string(params->size()) : std::string("0")) +
						" argument(s)");
		return types_.void_ty();
	}
	FnDecl env;
	for (size_t i = 0; i < params->size(); ++i) {
		const auto &p = (*params)[i];
		const auto &a = tr.generic_args[i];
		if (p.kind == GenericParam::Kind::Type) {
			if (a.kind != GenericArg::Kind::Type || !a.type) {
				log::report(log::Level::Error, a.loc, "expected type generic argument");
				return types_.void_ty();
			}
			env.generic_type_subst[p.name] = resolve_type(*a.type, m);
		} else {
			int64_t v	  = 0;
			bool	got_v = false;
			if (a.kind == GenericArg::Kind::Value && a.value) {
				if (eval_generic_const_expr(*a.value, env.generic_const_subst, v)) {
					got_v = true;
				} else {
					int64_t  sval  = 0;
					uint64_t uval  = 0;
					bool	 is_un = false;
					if (eval_comptime_int_expr(*a.value, m, sval, uval, is_un)) {
						v	  = sval;
						got_v = true;
					}
				}
			} else if (a.kind == GenericArg::Kind::Type && a.type &&
					   a.type->kind == TypeRef::Named) {
				Expr ident;
				ident.kind = ExprKind::Ident;
				ident.loc  = a.type->loc;
				if (!a.type->name.empty()) {
					ident.path = split_dotted_path(a.type->name);
				} else {
					ident.path = {a.type->single};
				}
				ident.str_val = a.type->single;
				if (eval_generic_const_expr(ident, env.generic_const_subst, v)) {
					got_v = true;
				} else {
					int64_t  sval  = 0;
					uint64_t uval  = 0;
					bool	 is_un = false;
					if (eval_comptime_int_expr(ident, m, sval, uval, is_un)) {
						v	  = sval;
						got_v = true;
					}
				}
			}
			if (got_v) {
				env.generic_const_subst[p.name] = v;
			} else {
				log::report(log::Level::Error, a.loc,
							"generic const argument must be a compile-time value");
				return types_.void_ty();
			}
		}
	}
	for (const auto &p : *params) {
		if (p.kind == GenericParam::Kind::Type && !p.type_set.empty()) {
			StType *supplied = env.generic_type_subst[p.name];
			if (!generic_type_allowed(p, supplied)) {
				log::report(log::Level::Error, tr.loc,
							"generic parameter '" + p.name + "' supplied type '" +
								type_name_for_diag(supplied) + "', allowed set " +
								generic_constraint_text(p),
							"E_GENERIC_CONSTRAINT", "sema");
				return types_.void_ty();
			}
		}
		if (p.kind == GenericParam::Kind::Type && !p.trait_bounds.empty()) {
			StType *supplied = env.generic_type_subst[p.name];
			Module &decl_m = sym.owner_module ? *sym.owner_module : m;
			for (const auto &bound_ref : p.trait_bounds) {
				if (!bound_ref) continue;
				std::string tname = bound_ref->name.empty() ? bound_ref->single : bound_ref->name;
				Symbol *trait_sym = resolve_path(split_scopes(tname), decl_m);
				if (!trait_sym) {
					trait_sym = resolve_path_any(split_scopes(tname), decl_m);
				}
				if (!trait_sym || trait_sym->kind != SymKind::Trait || !trait_sym->trait_decl) {
					log::report(log::Level::Error, tr.loc,
								"unknown trait '" + tname + "' in generic constraint",
								"E_UNDEFINED_TYPE", "sema");
					return types_.void_ty();
				}
				TraitDecl *trait = trait_sym->trait_decl;
				if (!type_implements_trait(supplied, trait)) {
					log::report(log::Level::Error, tr.loc,
								"type '" + type_name_for_diag(supplied) + "' does not implement trait '" +
									trait->name + "'",
								"E_GENERIC_CONSTRAINT", "sema");
					return types_.void_ty();
				}
			}
		}
	}
	std::vector<GenericMangleArg> mangle_args;
	mangle_args.reserve(params->size());
	for (const auto &p : *params) {
		GenericMangleArg arg;
		if (p.kind == GenericParam::Kind::Type) {
			arg.kind = GenericMangleArg::Kind::Type;
			arg.type = env.generic_type_subst[p.name];
		} else {
			arg.kind	= GenericMangleArg::Kind::ConstInt;
			arg.int_val = env.generic_const_subst[p.name];
		}
		mangle_args.push_back(std::move(arg));
	}
	std::string specialized = mangle_generic(sym.mangled, mangle_args);
	if (StType *cached = types_.find_named(specialized)) {
		return cached;
	}
	FnDecl *saved	 = generic_context_;
	generic_context_ = &env;
	StType *result	 = nullptr;
	Module &origin = sym.owner_module ? *sym.owner_module : m;
	if (sym.kind == SymKind::TypeBinding) {
		result = types_.type_binding_of(specialized, resolve_type(*sym.tb->target, origin),
									   sym.tb->is_pub);
	} else if (sym.kind == SymKind::Enum) {
		StType *backing = sym.en->backing ? underlying_type(resolve_type(*sym.en->backing, origin))
										  : types_.sint(32);
		std::vector<std::string>	 member_names;
		std::vector<uint64_t>		 member_values;
		std::vector<EnumVariantInfo> variants;
		bool						 is_tagged = false;
		for (const auto &member : sym.en->members) {
			member_names.push_back(member.name);
			member_values.push_back(member.resolved);
			EnumVariantInfo vi;
			vi.payload_kind = member.payload_kind;
			if (member.payload_kind != EnumPayloadKind::Unit) {
				is_tagged = true;
				for (const auto &pf : member.payload_fields) {
					StType *ft = pf.ty ? resolve_type(*pf.ty, origin) : nullptr;
					vi.field_types.push_back(ft);
					vi.field_names.push_back(pf.name);
				}
			}
			variants.push_back(std::move(vi));
		}
		result = types_.enum_of(specialized, backing, std::move(member_names),
								std::move(member_values), sym.en->is_pub, is_tagged,
								std::move(variants));
		if (sym.en->resolved) {
			result->repr		   = sym.en->resolved->repr;
			result->explicit_align = sym.en->resolved->explicit_align;
			result->layout_size	   = sym.en->resolved->layout_size;
			result->layout_align   = sym.en->resolved->layout_align;
			result->layout_loc	   = sym.en->resolved->layout_loc;
		}
	} else {
		std::vector<StType *> fts;
		for (auto &f : *fields) {
			fts.push_back(resolve_type(*f, origin));
		}
		result = sym.kind == SymKind::Struct
					 ? types_.struct_of(specialized, names, fts, sym.st->is_pub, sym.st->field_is_pub)
					 : types_.union_of(specialized, names, fts, sym.un->is_pub, sym.un->field_is_pub);
		if (sym.kind == SymKind::Struct && sym.st->resolved) {
			result->is_opaque = sym.st->resolved->is_opaque;
			result->repr = sym.st->resolved->repr;
			result->explicit_align = sym.st->resolved->explicit_align;
			result->layout_size = sym.st->resolved->layout_size;
			result->layout_align = sym.st->resolved->layout_align;
			result->layout_loc = sym.st->resolved->layout_loc;
		} else if (sym.kind == SymKind::Union && sym.un->resolved) {
			result->repr = sym.un->resolved->repr;
		}
	}
	generic_context_ = saved;
	if (result) {
		result->generic_origin = &sym;
		result->decl_module	   = sym.owner_module ? sym.owner_module : &m;
		if (sym.st && sym.st->resolved) {
			result->scope_path = sym.st->resolved->scope_path;
		} else if (sym.un && sym.un->resolved) {
			result->scope_path = sym.un->resolved->scope_path;
		} else if (sym.en && sym.en->resolved) {
			result->scope_path = sym.en->resolved->scope_path;
		} else {
			result->scope_path = {sym.simple};
		}
		for (const auto &p : *params) {
			if (p.kind == GenericParam::Kind::Type) {
				result->generic_arg_is_const.push_back(false);
				result->generic_type_args.push_back(env.generic_type_subst[p.name]);
				result->generic_const_args.push_back(0);
			} else {
				result->generic_arg_is_const.push_back(true);
				result->generic_type_args.push_back(nullptr);
				result->generic_const_args.push_back(env.generic_const_subst[p.name]);
			}
		}
	}
	return result ? result : types_.void_ty();
}

void Sema::validate_aggregate_layouts() {
	std::vector<std::pair<StType *, SourceLoc>> aggregates;
	std::function<void(const std::vector<TopDeclPtr> &)> collect_aggregates;
	collect_aggregates = [&](const std::vector<TopDeclPtr> &decls) {
		for (const auto &td : decls) {
			if (!td) {
				continue;
			}
			if (td->kind == TopKind::Mod && td->mod_decl) {
				collect_aggregates(td->mod_decl->body);
			} else if (td->kind == TopKind::Struct && td->st) {
				if (td->st->resolved && td->st->generic_params.empty()) {
					aggregates.emplace_back(td->st->resolved, td->st->loc);
				}
			} else if (td->kind == TopKind::Union && td->un && td->un->resolved &&
					   td->un->generic_params.empty()) {
				aggregates.emplace_back(td->un->resolved, td->un->loc);
			} else if (td->kind == TopKind::Enum && td->en && td->en->resolved &&
					   td->en->generic_params.empty() && td->en->resolved->is_tagged_enum) {
				aggregates.emplace_back(td->en->resolved, td->en->loc);
			}
		}
	};
	for (Module *m : resolver_.all()) {
		collect_aggregates(m->decls);
	}

	for (auto &[aggregate, loc] : aggregates) {
		if (aggregate->kind == TypeKind::Enum) {
			for (size_t vi = 0; vi < aggregate->enum_variants.size(); ++vi) {
				const auto &variant = aggregate->enum_variants[vi];
				for (size_t fi = 0; fi < variant.field_types.size(); ++fi) {
					StType *field = underlying_type(variant.field_types[fi]);
					if (field && field->kind == TypeKind::VaList) {
						log::report(log::Level::Error, loc,
									"@va.list may only be declared as a local variable");
					}
					std::unordered_set<StType *> active;
					if (!storage_type_valid(field, active)) {
						log::report(log::Level::Error, loc,
									"payload field of variant '" + aggregate->field_names[vi] +
										"' in enum '" + aggregate->name +
										"' must have a non-void, non-zero-sized storage type");
					} else if (aggregate->repr == ReprKind::C && !is_ffi_safe_value_type(field)) {
						log::report(log::Level::Error, loc,
									"#[repr(\"C\")] type '" + aggregate->name +
										"' cannot contain #[repr(\"Stella\")] type '" +
										type_name_for_diag(field) +
										"' by value (use a pointer for opaque handles)");
					}
				}
			}
			continue;
		}
		std::unordered_set<std::string> names;
		for (size_t i = 0; i < aggregate->field_names.size(); ++i) {
			const std::string &name = aggregate->field_names[i];
			if (!names.insert(name).second) {
				log::report(log::Level::Error, loc,
							"duplicate " +
								(aggregate->kind == TypeKind::Struct ? std::string("struct")
																	 : std::string("union")) +
								" member name '" + name + "'");
			}
			StType *field = i < aggregate->fields.size() ? underlying_type(aggregate->fields[i]) : nullptr;
			if (field && field->kind == TypeKind::VaList) {
				log::report(log::Level::Error, loc,
							"@va.list may only be declared as a local variable");
			}
			std::unordered_set<StType *> active;
			if (!storage_type_valid(field, active)) {
				log::report(log::Level::Error, loc,
							"member '" + name + "' of aggregate '" + aggregate->name +
								"' must have a non-void, non-zero-sized storage type");
			} else if (aggregate->repr == ReprKind::C && !is_ffi_safe_value_type(field)) {
				log::report(log::Level::Error, loc,
							"#[repr(\"C\")] type '" + aggregate->name + "' cannot contain " +
								"#[repr(\"Stella\")] type '" + type_name_for_diag(field) +
								"' by value (use a pointer for opaque handles)");
			}
		}
	}

	std::unordered_map<StType *, int>		state;
	std::set<std::pair<StType *, StType *>> reported;
	std::function<void(StType *, StType *)> visit = [&](StType *owner, StType *field) {
		field = underlying_type(field);
		if (!field) {
			return;
		}
		if (field->kind == TypeKind::Pointer) {
			return;
		}
		if (field->kind == TypeKind::Array) {
			visit(owner, field->base);
			return;
		}
		if (field->kind != TypeKind::Struct && field->kind != TypeKind::Union &&
			!(field->kind == TypeKind::Enum && field->is_tagged_enum)) {
			return;
		}
		int &s = state[field];
		if (s == 1) {
			if (reported.insert({owner, field}).second) {
				log::report(log::Level::Error, {},
							"by-value recursive aggregate cycle involving '" + field->name + "'");
			}
			return;
		}
		if (s == 2) {
			return;
		}
		s = 1;
		if (field->kind == TypeKind::Enum) {
			for (const auto &v : field->enum_variants) {
				for (auto *nested : v.field_types) {
					visit(field, nested);
				}
			}
		} else {
			for (auto *nested : field->fields) {
				visit(field, nested);
			}
		}
		s = 2;
	};
	for (auto &[aggregate, loc] : aggregates) {
		(void)loc;
		visit(aggregate, aggregate);
	}
}

void Sema::resolve_fn_signature(FnDecl &fn, Module &m) {
	if (fn.fn_sig) {
		return; // already resolved in Phase 1
	}
	FnDecl *saved_generic = generic_context_;
	if (fn.is_generic_instance) {
		generic_context_ = &fn;
	}
	fn.abi = resolve_abi_attr(fn.attrs);
	if (!fn.is_macro && fn.is_variadic && fn.abi != AbiKind::C) {
		log::report(log::Level::Error, fn.loc,
					"variadic functions (...) are only supported with #[abi(\"C\")]");
	}
	for (auto &p : fn.params) {
		if (!p.resolved) {
			p.resolved = resolve_type(*p.ty, m);
		}
		std::unordered_set<StType *> active;
		if (!p.is_comptime && (!p.ty || !p.ty->sema_invalid) &&
			!storage_type_valid(p.resolved, active)) {
			log::report(log::Level::Error, p.loc,
						"parameter '" + p.name +
							"' must have a non-void, non-zero-sized storage type");
		} else if (fn.abi == AbiKind::C && !p.is_comptime &&
				   (!p.ty || !p.ty->sema_invalid) && !is_ffi_safe_value_type(p.resolved)) {
			log::report(log::Level::Error, p.loc,
						"#[abi(\"C\")] parameter '" + p.name + "' of type '" +
							type_name_for_diag(p.resolved) +
							"' is not FFI-safe (use #[repr(\"C\")] or pass a pointer)");
		}
	}
	StType				 *ret = resolve_type(*fn.ret, m);
	if (fn.abi == AbiKind::C && (!fn.ret || !fn.ret->sema_invalid) &&
		!is_ffi_safe_value_type(ret)) {
		log::report(log::Level::Error, fn.ret ? fn.ret->loc : fn.loc,
					"#[abi(\"C\")] return type '" + type_name_for_diag(ret) +
						"' is not FFI-safe (use #[repr(\"C\")] or return a pointer)");
	}
	std::vector<StType *> ptypes;
	ptypes.reserve(fn.params.size());
	for (auto &p : fn.params) {
		if (!p.is_comptime) {
			ptypes.push_back(p.resolved);
		}
	}
	fn.fn_sig		 = types_.function_of(std::move(ptypes), ret, fn.is_variadic, fn.abi);
	generic_context_ = saved_generic;
}

void Sema::check_fn(FnDecl &fn, Module &m) {
	FnDecl *saved_generic = generic_context_;
	if (fn.is_generic_instance) {
		generic_context_ = &fn;
	}
	resolve_fn_signature(fn, m); // idempotent: resolved in Phase 1
	StType *ret = fn.fn_sig->fn_ret;
	// Validate per-parameter attributes (e.g. #[!alias]). These are scoped to
	// a single parameter, not the whole signature, so they are checked here
	// rather than in validate_attributes (which is keyed to TopDecl kinds).
	// Pointer-typedness of the tagged param is enforced at the same site that
	// owns the resolved type.
	for (auto &p : fn.params) {
		std::set<std::pair<std::string, bool>> seen_attrs;
		for (auto &a : p.attrs.attrs) {
			const AttributeSpec *spec = lookup_attribute_spec(a.name);
			if (!spec) {
				log::report(log::Level::Error, a.loc, "unknown " + attr_diag_prefix(a));
				continue;
			}
			if (!attribute_target_allowed(*spec, AttributeTarget::Parameter)) {
				log::report(log::Level::Error, a.loc,
							attr_diag_prefix(a) + " is not valid on parameters");
				continue;
			}
			bool polarity_ok = spec->polarity == AttributePolarity::Either ||
							   (spec->polarity == AttributePolarity::PositiveOnly && !a.negated) ||
							   (spec->polarity == AttributePolarity::NegativeOnly && a.negated);
			if (!polarity_ok) {
				log::report(log::Level::Error, a.loc,
							attr_diag_prefix(a) +
								(a.negated ? " does not support negation" : " requires negation"));
				continue;
			}
			if (!seen_attrs.insert({a.name, a.negated}).second) {
				log::report(log::Level::Error, a.loc,
							attr_diag_prefix(a) + " is specified more than once");
			}
			if (spec->arg_kind != AttributeArgKind::None) {
				log::report(log::Level::Error, a.loc, attr_diag_prefix(a) + " takes no arguments");
			}
			if (a.name == "alias") {
				StType *pt = p.resolved ? underlying_type(p.resolved) : nullptr;
				if (!pt || pt->kind != TypeKind::Pointer) {
					log::report(
						log::Level::Error, a.loc,
						std::string("attribute '#[!alias]' requires a pointer parameter; '") +
							p.name + "' of function '" + fn.name + "' is not a pointer");
				}
			}
		}
	}
	ScopeStack scope;
	scope.enter();
	ScopeGuard scope_guard(current_scope_, &scope);
	FnDecl *saved_active	= active_fn_;
	auto	saved_va_states = std::move(va_states_);
	auto	saved_va_locals = std::move(va_list_locals_);
	active_fn_				= &fn;
	va_states_.clear();
	va_list_locals_.clear();
	for (auto &p : fn.params) {
		Symbol s;
		s.kind	 = SymKind::Param;
		s.simple = p.name;
		s.type	 = p.resolved;
		s.loc	 = p.loc;
		s.usage	 = &p.used;
		s.is_mutable = p.is_mutable;
		if (p.is_comptime && generic_context_) {
			auto it = generic_context_->generic_type_subst.find(p.name);
			if (it != generic_context_->generic_type_subst.end()) {
				s.comptime_type_value = it->second;
			}
		}
		if (p.name == "_") {
			continue;
		}
		if (!scope.define(p.name, s)) {
			log::report(log::Level::Error, p.loc, "duplicate parameter '" + p.name + "'");
			if (Symbol *previous = scope.lookup_current(p.name)) {
				log::report(log::Level::Note, previous->loc,
							"previous parameter declaration is here");
			}
		}
	}
	active_locals_.clear();
	bool is_void = ret && underlying_type(ret)->kind == TypeKind::Void;
	if (has_attr(fn.attrs, "must_tail") && !fn.body) {
		log::report(log::Level::Error, fn.loc, "attribute '#[must_tail]' requires a function body");
	}
	// External/decl-only (no body) functions are exempt -- they have no local
	// control flow to police. #[export] declarations may forward to C ABI.
	if (fn.body) {
		check_stmt(*fn.body, fn, m, scope, ret);
		for (auto *local : va_list_locals_) {
			auto it = va_states_.find(local);
			if (it != va_states_.end() &&
				(it->second & static_cast<unsigned>(VaState::Initialized)) &&
				!(it->second == static_cast<unsigned>(VaState::Ended))) {
				log::report(log::Level::Error, local->loc,
							"@va.list variable '" + local->name +
								"' must be ended exactly once on every reachable path");
			}
		}
		if (has_attr(fn.attrs, "must_tail") && !definite_return(*fn.body)) {
			log::report(
				log::Level::Error, fn.loc,
				"attribute '#[must_tail]' requires every reachable return to be a direct call");
		}
		if (!is_void && fn.body->kind == StmtKind::Block && !definite_return(*fn.body)) {
			log::report(log::Level::Error, fn.loc,
						std::string("non-void function '") + fn.name +
							"' must return a value on all paths (control "
							"reaches end of function)");
		}
	}
	if (fn.body) {
		for (auto &p : fn.params) {
			if (!p.is_comptime && !p.used && (p.name.empty() || p.name[0] != '_')) {
				log::report(log::Level::Warning, p.loc, "unused parameter '" + p.name + "'");
			}
		}
		for (auto *local : active_locals_) {
			if (local && !local->used && (local->name.empty() || local->name[0] != '_')) {
				log::report(log::Level::Warning, local->loc,
							std::string("unused ") +
								(local->is_const ? "local constant '" : "variable '") +
								local->name + "'");
			}
		}
	}
	scope.leave();
	active_fn_		 = saved_active;
	va_states_		 = std::move(saved_va_states);
	va_list_locals_	 = std::move(saved_va_locals);
	generic_context_ = saved_generic;
}

} // namespace stellac
