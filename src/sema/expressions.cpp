#include "internal.hpp"
#include <filesystem>
#include <fstream>

namespace stellac {

bool Sema::field_accessible(const StType &agg, size_t idx, const Module &from) const {
	if (idx < agg.field_is_pub.size() && agg.field_is_pub[idx]) return true;
	if (!agg.decl_module) return true; // compiler-synthesized aggregate: no owner to protect
	return agg.decl_module == &from || resolver_.same_source_file(*agg.decl_module, from);
}

void Sema::report_private_field(const SourceLoc &loc, const StType &agg, size_t idx) const {
	std::string f_name = (idx < agg.field_names.size()) ? agg.field_names[idx] : "";
	std::string t_name = type_name_for_diag(const_cast<StType *>(&agg));
	log::error(loc, "field `" + f_name + "` of `" + t_name + "` is private")
		.code(log::DiagCode::PrivateMember)
		.phase("semantic")
		.help("mark the field `pub` in its declaration, or access it from the declaring module")
		.emit();
}

namespace {

static bool is_volatile_lvalue(const Expr &expr) {
	auto pointer_is_volatile = [](const Expr *value) {
		StType *type = value ? underlying_type(value->ty) : nullptr;
		return type && (type->kind == TypeKind::Pointer || type->kind == TypeKind::Slice) &&
			   type->is_volatile;
	};
	switch (expr.kind) {
	case ExprKind::Deref:
	case ExprKind::Index:
	case ExprKind::Member:
		return pointer_is_volatile(expr.lhs.get()) ||
			(expr.lhs && is_volatile_lvalue(*expr.lhs));
	default:
		return false;
	}
}

} // namespace

std::string Sema::find_similar_identifier(const std::string &name, const ScopeStack &scope,
										  const Module &m) {
	if (name == "_") {
		return "";
	}
	std::vector<std::string> candidates;
	scope.collect_names(candidates);
	auto it = module_syms_.find(source_key(m));
	if (it != module_syms_.end()) {
		for (const auto &pair : it->second) {
			candidates.push_back(pair.second.simple.empty() ? pair.first : pair.second.simple);
		}
	}
	std::string	 best_candidate;
	size_t		 best_dist	 = SIZE_MAX;
	const size_t max_allowed = (name.size() <= 3) ? 1 : ((name.size() <= 8) ? 2 : 3);
	for (const auto &cand : candidates) {
		if (cand == name || cand.empty()) {
			continue;
		}
		size_t dist = levenshtein_distance(name, cand);
		if (dist <= max_allowed && dist < best_dist) {
			best_dist	   = dist;
			best_candidate = cand;
		}
	}
	return best_candidate;
}

std::string Sema::candidate_rejection_reason(FnDecl &cand, const Expr &call, Module &call_module,
											 bool cross_module) {
	if (cross_module && !cand.is_pub) {
		return "candidate is private; add 'pub' to call it from another module";
	}
	size_t n_fixed = cand.params.size();
	if (!cand.is_variadic && call.args.size() != n_fixed) {
		std::string arg_word = (n_fixed == 1) ? "argument" : "arguments";
		return "expects " + std::to_string(n_fixed) + " " + arg_word + ", but " +
			   std::to_string(call.args.size()) + " were supplied";
	}
	if (cand.is_variadic && call.args.size() < n_fixed) {
		std::string arg_word = (n_fixed == 1) ? "argument" : "arguments";
		return "expects at least " + std::to_string(n_fixed) + " " + arg_word + ", but " +
			   std::to_string(call.args.size()) + " were supplied";
	}
	for (size_t i = 0; i < std::min(call.args.size(), n_fixed); ++i) {
		StType *pty = cand.params[i].resolved;
		if (!pty && cand.params[i].ty) {
			pty = resolve_type(*cand.params[i].ty, call_module);
		}
		StType *aty = call.args[i]->ty;
		if (is_contextual_enum_variant_expr(*call.args[i])) {
			uint64_t val = 0;
			std::string_view vname = contextual_enum_variant_name(*call.args[i]);
			if (!enum_member_value(pty, vname, val)) {
				return "argument " + std::to_string(i + 1) + ": '" + std::string(vname) +
					   "' is not a member of enum '" + type_name_for_diag(pty) + "'";
			}
			continue;
		}
		if (eq_type(pty, aty)) {
			continue;
		}
		if (is_contextual_literal(*call.args[i])) {
			auto fv = value_fits(pty, *call.args[i]);
			if (fv == FitResult::Fit) {
				continue;
			}
			if (fv == FitResult::NoFit) {
				return "argument " + std::to_string(i + 1) + ": value not representable in '" +
					   type_name_for_diag(pty) + "'";
			}
		}
		if (arg_type_compat(pty, aty)) {
			continue;
		}
		StType *dst = underlying_type(pty);
		StType *src = underlying_type(aty);
		if (dst && src && dst->kind == TypeKind::Pointer && src->kind == TypeKind::Pointer) {
			if (eq_type(dst->base, src->base)) {
				if (!src->is_mutable && dst->is_mutable) {
					return "cannot convert argument " + std::to_string(i + 1) + " from '" +
						   type_name_for_diag(aty) + "' to '" + type_name_for_diag(pty) +
						   "' (cannot convert immutable pointer to mutable pointer)";
				}
				if (src->is_volatile && !dst->is_volatile) {
					return "cannot convert argument " + std::to_string(i + 1) + " from '" +
						   type_name_for_diag(aty) + "' to '" + type_name_for_diag(pty) +
						   "' (discards 'volatile' qualifier)";
				}
			}
		}
		return "argument " + std::to_string(i + 1) + " type mismatch: expected '" +
			   type_name_for_diag(pty) + "', got '" + type_name_for_diag(aty) + "'";
	}
	if (cand.is_variadic) {
		for (size_t i = n_fixed; i < call.args.size(); ++i) {
			if (!supported_va_type(call.args[i]->ty)) {
				return "variadic argument " + std::to_string(i + 1) + " has unsupported type '" +
					   type_name_for_diag(call.args[i]->ty) + "' (expected scalar or pointer)";
			}
		}
	}
	if (!cand.generic_params.empty()) {
		return "generic constraint not satisfied or type deduction failed";
	}
	return "incompatible arguments";
}

StType *Sema::resolve_src_embed(Expr &e, FnDecl &ctx, Module &m, ScopeStack &scope) {
	auto fail = [&]() -> StType * {
		e.kind		   = ExprKind::StringLit;
		e.is_cstring   = false;
		e.str_val.clear();
		e.args.clear();
		e.sema_invalid = true;
		e.ty		   = types_.slice(types_.uint(8), false, false);
		return e.ty;
	};

	if (e.args.size() != 1 || !e.args.front()) {
		log::error(e.loc, "@src.embed expects one file path argument")
			.code(log::DiagCode::InvalidCall);
		return fail();
	}

	check_expr(*e.args[0], ctx, m, scope);
	if (e.args[0]->sema_invalid) {
		return fail();
	}
	if (e.args[0]->kind != ExprKind::StringLit || e.args[0]->is_cstring) {
		log::error(e.args[0]->loc, "@src.embed file path must be a string literal")
			.code(log::DiagCode::TypeMismatch);
		return fail();
	}
	if (e.args[0]->str_val.empty()) {
		log::error(e.args[0]->loc, "@src.embed file path must not be empty")
			.code(log::DiagCode::InvalidCall);
		return fail();
	}

	const std::string	   &source_file = !e.args[0]->loc.file.empty()
											  ? e.args[0]->loc.file.str()
											  : (!e.loc.file.empty() ? e.loc.file.str() : m.fpath);
	std::filesystem::path	requested(e.args[0]->str_val);
	std::filesystem::path	resolved = requested.is_absolute() || source_file.empty()
										   ? requested
										   : (std::filesystem::path(source_file).parent_path() / requested);

	std::error_code ec;
	if (!std::filesystem::exists(resolved, ec) || ec) {
		log::error(e.loc, "cannot embed '" + e.args[0]->str_val + "': file not found ('" +
							  resolved.lexically_normal().string() + "')")
			.code(log::DiagCode::InvalidCall);
		return fail();
	}
	if (std::filesystem::is_directory(resolved, ec) || ec) {
		log::error(e.loc, "cannot embed '" + e.args[0]->str_val + "': path is a directory ('" +
							  resolved.lexically_normal().string() + "')")
			.code(log::DiagCode::InvalidCall);
		return fail();
	}

	std::ifstream in(resolved, std::ios::binary);
	if (!in.is_open()) {
		log::error(e.loc, "cannot open embedded file '" + resolved.lexically_normal().string() + "'")
			.code(log::DiagCode::InvalidCall);
		return fail();
	}

	std::string contents;
	char		buf[8192];
	while (in.read(buf, sizeof(buf))) {
		contents.append(buf, static_cast<size_t>(in.gcount()));
	}
	if (in.gcount() > 0) {
		contents.append(buf, static_cast<size_t>(in.gcount()));
	}
	if (in.bad()) {
		log::error(e.loc, "failed reading embedded file '" + resolved.lexically_normal().string() + "'")
			.code(log::DiagCode::InvalidCall);
		return fail();
	}

	embedded_files_.push_back(resolved.lexically_normal().string());
	e.kind		 = ExprKind::StringLit;
	e.is_cstring = false;
	e.str_val	 = std::move(contents);
	e.args.clear();
	e.ty = types_.slice(types_.uint(8), false, false);
	return e.ty;
}

StType *Sema::check_layout_operand(Expr &operand, FnDecl &ctx, Module &m, ScopeStack &scope) {
	if (operand.layout_type) {
		// Compound type operands are parsed separately from expressions so an
		// array type such as `[66]u8` cannot be mistaken for an array literal.
		// Resolve it here, where the current module and generic substitutions are
		// available, and expose the same type-valued shape used by named types.
		operand.type_value	 = resolve_type(*operand.layout_type, m);
		operand.ty			 = types_.type_ty();
		operand.sema_invalid = operand.layout_type->sema_invalid;
		return operand.type_value;
	}
	StType *operand_ty = check_expr(operand, ctx, m, scope);
	StType *underlying = underlying_type(operand_ty);
	if (underlying && underlying->kind == TypeKind::Type) {
		// Type names and comptime type parameters are expression-level type
		// values. Layout builtins consume their payload, not the `type` marker.
		return operand.type_value;
	}
	return operand_ty;
}

StType *Sema::check_expr(Expr &e, FnDecl &ctx, Module &m, ScopeStack &scope) {
	MacroDiagnosticContextGuard<Expr> diagnostic_context(e);
	if (e.kind == ExprKind::Unary || e.kind == ExprKind::Binary ||
		e.kind == ExprKind::Conditional) {
		clear_folded_value(e);
	}
	switch (e.kind) {
	case ExprKind::IntLit:
		e.has_const_value = true;
		// Integer literals are untyped until check_expr_with_expected supplies
		// an explicit integer destination. A missing destination is an error,
		// not an implicit integer type.
		if (!e.ty) {
			log::report(log::Level::Error, e.loc,
						"integer literal requires an explicit integer type or typed context");
			e.ty		   = types_.void_ty();
			e.sema_invalid = true;
		}
		return e.ty;
	case ExprKind::FloatLit:
		e.ty				   = types_.float_ty(64);
		e.has_const_value	   = true;
		e.const_value_is_float = true;
		return e.ty;
	case ExprKind::StringLit:
		e.ty = e.is_cstring ? types_.pointer(types_.uint(8), false)
							: types_.slice(types_.uint(8), false, false);
		return e.ty;
	case ExprKind::SrcFile:
	case ExprKind::SrcFn: {
		const SourceLoc &source = e.invocation_loc.file.empty() ? e.loc : e.invocation_loc;
		e.str_val = e.kind == ExprKind::SrcFile
						? source.file.str()
						: (ctx.full_path.empty() ? ctx.name : join_path(ctx.full_path));
		e.ty = types_.slice(types_.uint(8), false, false);
		return e.ty;
	}
	case ExprKind::SrcLine:
	case ExprKind::SrcColumn: {
		const SourceLoc &source = e.invocation_loc.file.empty() ? e.loc : e.invocation_loc;
		e.int_val = static_cast<uint64_t>(e.kind == ExprKind::SrcLine ? source.line : source.column);
		e.ty = types_.uint(32);
		e.has_const_value = true;
		return e.ty;
	}
	case ExprKind::SrcEmbed:
		return resolve_src_embed(e, ctx, m, scope);
	case ExprKind::Stringify:
		if (e.args.size() != 1 || !e.args.front()) {
			log::report(log::Level::Error, e.loc,
						"@syn.stringify expects one syntax argument");
			e.ty = types_.void_ty();
			e.sema_invalid = true;
			return e.ty;
		}
		// Stringification is syntax-only: the operand is deliberately not
		// semantically checked, so an identifier can be rendered even when it
		// is only meaningful at a macro call site.
		e.kind = ExprKind::StringLit;
		e.str_val = stringify_expr(*e.args.front());
		e.args.clear();
		e.ty = types_.slice(types_.uint(8), false, false);
		return e.ty;
	case ExprKind::DiagWarn:
	case ExprKind::DiagErr: {
		const char *builtin_name = e.kind == ExprKind::DiagErr ? "@diag.err" : "@diag.warn";
		e.ty = types_.void_ty();
		if (e.args.size() != 2 || !e.args[0] || !e.args[1]) {
			log::error(e.loc, std::string(builtin_name) + " expects two arguments: (description, message)")
				.code(log::DiagCode::InvalidCall);
			e.sema_invalid = true;
			return e.ty;
		}

		check_expr(*e.args[0], ctx, m, scope);
		check_expr(*e.args[1], ctx, m, scope);

		bool valid_args = true;
		if (e.args[0]->kind != ExprKind::StringLit) {
			log::error(e.args[0]->loc, std::string(builtin_name) + " description must be a string literal")
				.code(log::DiagCode::TypeMismatch);
			valid_args = false;
		} else if (e.args[0]->str_val.empty()) {
			log::error(e.args[0]->loc, std::string(builtin_name) + " description must not be empty")
				.code(log::DiagCode::InvalidCall);
			valid_args = false;
		}
		if (e.args[1]->kind != ExprKind::StringLit) {
			log::error(e.args[1]->loc, std::string(builtin_name) + " message must be a string literal")
				.code(log::DiagCode::TypeMismatch);
			valid_args = false;
		}
		if (!valid_args) {
			e.sema_invalid = true;
			return e.ty;
		}

		std::string description = e.args[0]->str_val;
		std::string message		= e.args[1]->str_val;

		if (e.kind == ExprKind::DiagErr) {
			log::error(e.loc, message).code(description).preserve_case(true).emit();
			e.sema_invalid = true;
		} else {
			log::warning(e.loc, message).code(description).preserve_case(true).emit();
		}
		return e.ty;
	}
	case ExprKind::VariadicSplice:
		log::report(log::Level::Error, e.loc,
					"variadic macro splice '...' is only valid in a macro body call argument list");
		e.ty = types_.void_ty();
		e.sema_invalid = true;
		return e.ty;
	case ExprKind::CharLit:
		if (!e.ty) {
			e.ty = types_.uint(8);
		}
		e.has_const_value = true;
		return e.ty;
	case ExprKind::BoolLit:
		e.ty			  = types_.bool_ty();
		e.has_const_value = e.is_const_bool = true;
		return e.ty;
	case ExprKind::NullLit:
		e.ty = types_.pointer(types_.void_ty());
		return e.ty;
	case ExprKind::DefineRef:
		e.ty			= types_.bool_ty();
		e.is_const_bool = e.has_const_value = true;
		e.const_bool_val = opts_.flags.count(e.str_val) && opts_.flags.at(e.str_val);
		return e.ty;
	case ExprKind::TypeValue:
		if (e.layout_type && !e.type_value) {
			e.type_value   = resolve_type(*e.layout_type, m);
			e.sema_invalid = e.layout_type->sema_invalid;
		}
		e.ty = types_.type_ty();
		return e.ty;
	case ExprKind::EnumMember:
		// Macro substitution can carry a previously resolved enum shorthand
		// into an explicit cast or another typed context. Preserve that type;
		// only an unresolved `.Member` needs contextual lookup.
		if (e.ty && underlying_type(e.ty) && underlying_type(e.ty)->kind == TypeKind::Enum) {
			return e.ty;
		}
		log::report(log::Level::Error, e.loc,
					"enum member shorthand '." + e.member +
						"' requires an expected enum type");
		e.ty = types_.void_ty();
		e.sema_invalid = true;
		return e.ty;
	case ExprKind::Conditional:
		return check_expr_with_expected(e, ctx, m, scope, nullptr);
	case ExprKind::Block: {
		if (!e.block) {
			log::report(log::Level::Error, e.loc, "empty block expression");
			e.ty = types_.void_ty();
			e.sema_invalid = true;
			return e.ty;
		}
		StType *expected_result = expected_block_expr_;
		expected_block_expr_ = nullptr;
		block_expected_stack_.push_back(expected_result);
		auto saved_yields = std::move(block_yield_types_);
		block_yield_types_.clear();
		++block_expr_depth_;
		check_stmt(*e.block, ctx, m, scope, nullptr);
		--block_expr_depth_;
		auto yields = std::move(block_yield_types_);
		block_yield_types_ = std::move(saved_yields);
		block_expected_stack_.pop_back();

		if (yields.empty()) {
			log::report(log::Level::Error, e.loc,
						"block expression must yield a value on at least one path");
			e.ty = types_.void_ty();
			e.sema_invalid = true;
			return e.ty;
		}
		if (!definite_yield(*e.block)) {
			log::report(log::Level::Error, e.loc,
						"block expression does not yield a value on every path");
			e.ty = types_.void_ty();
			e.sema_invalid = true;
			return e.ty;
		}
		StType *result = expected_result;
		for (StType *yield_ty : yields) {
			if (!yield_ty || underlying_type(yield_ty)->kind == TypeKind::Void) {
				continue;
			}
			if (!result || underlying_type(result)->kind == TypeKind::Void) {
				result = yield_ty;
			} else if (!eq_type(result, yield_ty)) {
				log::report(log::Level::Error, e.loc,
							"block expression yields values of different types ('" +
								type_name_for_diag(result) + "' and '" +
								type_name_for_diag(yield_ty) + "')");
			}
		}
		e.ty = result ? result : types_.void_ty();
		return e.ty;
	}
	case ExprKind::Ident: {
		Module &name_module = e.binding_module ? *e.binding_module : m;
		// local scope first
		if (e.path.size() == 1) {
			if (generic_context_) {
				const auto &generic_params =
					generic_context_->generic_template
						? generic_context_->generic_template->generic_params
						: generic_context_->generic_params;
				for (const auto &gp : generic_params) {
					if (gp.kind == GenericParam::Kind::Type && gp.name == e.path[0]) {
						auto it = generic_context_->generic_type_subst.find(gp.name);
						if (it != generic_context_->generic_type_subst.end()) {
							e.type_value = it->second;
							e.ty		 = types_.type_ty();
							return e.ty;
						}
					}
					if (gp.kind == GenericParam::Kind::Const && gp.name == e.path[0]) {
						if (gp.const_type) {
						e.ty = resolve_type(*gp.const_type, name_module);
						} else {
							log::report(log::Level::Error, e.loc,
										"const generic parameter requires an explicit type");
							e.ty		   = types_.void_ty();
							e.sema_invalid = true;
						}
						e.has_const_value = true;
						if (generic_context_->generic_const_subst.count(gp.name)) {
							e.int_val = static_cast<uint64_t>(
								generic_context_->generic_const_subst.at(gp.name));
						}
						if (generic_context_->generic_const_string_subst.count(gp.name)) {
							e.str_val = generic_context_->generic_const_string_subst.at(gp.name);
						}
						return e.ty;
					}
				}
			}
			const bool allow_local_lookup = !e.binding_module || e.macro_argument ||
				e.path[0].rfind("$macro.", 0) == 0;
			if (allow_local_lookup) {
				if (auto *ls = scope.lookup(e.path[0])) {
					if (!checking_assignment_lhs_ && ls->usage) {
						*ls->usage = true;
					}
					e.ty = ls->type;
					if (ls->comptime_type_value) {
						e.type_value = ls->comptime_type_value;
						e.ty = types_.type_ty();
						return e.ty;
					}
					e.is_lvalue = (ls->kind == SymKind::Param || ls->kind == SymKind::Var) &&
						ls->is_mutable;
					e.is_addressable = ls->kind == SymKind::Param || ls->kind == SymKind::Var ||
						ls->kind == SymKind::Const;
					if (ls->kind == SymKind::Const && ls->has_const_value) {
						e.has_const_value = true;
						e.is_const_bool = ls->const_bool_val && !ls->const_value_is_float;
						e.const_bool_val = ls->const_bool_val;
						e.const_value_is_float = ls->const_value_is_float;
						e.const_int_high_bits_ones = ls->const_int_high_bits_ones;
						e.int_val = ls->const_int_val;
						e.flt_val = ls->const_flt_val;
					}
					return e.ty;
				}
			}
		}
		// enum member access: Enum.Member or namespace.Enum.Member
		if (e.path.size() >= 2) {
			std::vector<std::string> head(e.path.begin(), e.path.end() - 1);
			Symbol					*en = resolve_path(head, name_module);
			if (en && en->kind == SymKind::Enum && en->en) {
				const std::string &mem = e.path.back();
				for (auto &mb : en->en->members) {
					if (mb.name == mem) {
						e.ty	  = en->type;
						e.int_val = static_cast<uint64_t>(mb.resolved);
						return e.ty;
					}
				}
				log::report(log::Level::Error, e.loc,
							"enum '" + en->simple + "' has no member '" + mem + "'");
				return types_.void_ty();
			}
		}
		Symbol *sym = resolve_path(e.path, name_module);
		// Primitive and named types are compile-time values of type `type`.
		if (!sym && e.path.size() == 1) {
			if (StType *primitive = prim_from_name(types_, e.path[0])) {
				e.type_value = primitive;
				e.ty		 = types_.type_ty();
				return e.ty;
			}
		}
		if (!sym && e.path.size() == 1 && e.path[0] == "Self" && active_self_type_) {
			e.type_value = active_self_type_;
			e.ty		 = types_.type_ty();
			return e.ty;
		}
		if (!sym) {
			if (Symbol *raw_sym = resolve_path_any(e.path, name_module);
				raw_sym && raw_sym->kind == SymKind::Mod) {
				sym = raw_sym;
			}
		}
		if (!sym) {
			if (has_unresolved_import(name_module, e.path)) {
				e.sema_invalid = true;
				e.ty		   = types_.poison_ty();
				return e.ty;
			}
			if (std::string missing = missing_child_module(resolver_, *this, name_module, e.path);
				!missing.empty()) {
				log::report(log::Level::Error, e.loc, "module `" + missing + "` could not be found",
							"E_MODULE_NOT_FOUND", "resolver");
				e.sema_invalid = true;
				e.ty		   = types_.poison_ty();
				return e.ty;
			}
			std::string suggestion = find_similar_identifier(e.str_val, scope, name_module);
			auto diag = log::error(e.loc, "undefined identifier '" + e.str_val + "'")
							.code("E_UNDEFINED_NAME")
							.phase("semantic");
			if (!suggestion.empty()) {
				diag.help("did you mean `" + suggestion + "`?");
				diag.fixit(e.loc, suggestion, "did you mean `" + suggestion + "`?");
			}
			diag.emit();
			e.sema_invalid = true;
			e.ty		   = types_.poison_ty();
			return e.ty;
		}
		if (sym->kind == SymKind::Mod) {
			log::error(e.loc, "module '" + e.str_val + "' cannot be used as a value")
				.code("E_INVALID_EXPR")
				.phase("semantic")
				.emit();
			e.sema_invalid = true;
			e.ty		   = types_.poison_ty();
			return e.ty;
		}
		if (sym->kind == SymKind::Fn) {
			FnDecl *f = sym->fn ? sym->fn : (!sym->overloads.empty() ? sym->overloads[0] : nullptr);
			if (f && f->is_macro) {
				log::report(log::Level::Error, e.loc,
							"macro names cannot be used as values");
				e.sema_invalid = true;
				e.ty = types_.void_ty();
				return e.ty;
			}
			if (f) {
				f->used = true;
			}
			e.resolved_fn = f;
			e.ty		  = f ? f->fn_sig : nullptr;
			return e.ty;
		}
		if (sym->kind == SymKind::Struct || sym->kind == SymKind::Union ||
			sym->kind == SymKind::TypeBinding || sym->kind == SymKind::Enum) {
			TypeRef type_ref;
			type_ref.kind		  = TypeRef::Named;
			type_ref.loc		  = e.loc;
			type_ref.name		  = join_path(e.path);
			type_ref.single		  = e.path.empty() ? std::string{} : e.path.back();
			type_ref.generic_args = std::move(e.generic_args);
			e.type_value		  = resolve_type(type_ref, name_module);
			e.ty				  = types_.type_ty();
			return e.ty;
		}
		if (sym->kind == SymKind::Enum) {
			log::report(log::Level::Error, e.loc,
						"enum type '" + sym->simple + "' used as a value; use '" + sym->simple +
							".<member>'");
			return types_.void_ty();
		}
		e.ty = sym->type;
		if (sym->usage) {
			*sym->usage = true;
		}
		e.is_lvalue = sym->kind == SymKind::Var && sym->is_mutable;
		e.is_addressable = sym->kind == SymKind::Var || sym->kind == SymKind::Const;
		if (sym->kind == SymKind::Const) {
			if (!sym->has_const_value) {
				ensure_const_value(*sym);
			}
			if (sym->has_const_value) {
				e.has_const_value	   = true;
				e.is_const_bool		   = sym->const_bool_val && !sym->const_value_is_float;
				e.const_bool_val	   = sym->const_bool_val;
				e.const_value_is_float = sym->const_value_is_float;
				e.const_int_high_bits_ones = sym->const_int_high_bits_ones;
				e.int_val			   = sym->const_int_val;
				e.flt_val			   = sym->const_flt_val;
			}
		}
		return e.ty;
	}
	case ExprKind::Binary: {
		StType *l = nullptr;
		StType *r = nullptr;
		if (e.lhs && e.rhs && is_contextual_literal(*e.lhs) && !is_contextual_literal(*e.rhs)) {
			r = check_expr(*e.rhs, ctx, m, scope);
			if (e.rhs && (e.rhs->sema_invalid || is_poison(r))) {
				e.sema_invalid = true;
				e.ty		   = types_.poison_ty();
				return e.ty;
			}
			l = check_expr_with_expected(*e.lhs, ctx, m, scope, r);
		} else {
			l = check_expr(*e.lhs, ctx, m, scope);
			if (e.lhs && (e.lhs->sema_invalid || is_poison(l))) {
				e.sema_invalid = true;
				e.ty		   = types_.poison_ty();
				return e.ty;
			}
			StType *lu			  = underlying_type(l);
			bool	pointer_arith = e.str_val == "+" || e.str_val == "-";
			if (pointer_arith && lu && lu->kind == TypeKind::Pointer && e.rhs &&
				is_contextual_literal(*e.rhs)) {
				r = check_expr_with_expected(*e.rhs, ctx, m, scope, types_.isize_ty());
			} else {
				r = check_expr_with_expected(*e.rhs, ctx, m, scope, l);
			}
		}
		if ((e.lhs && (e.lhs->sema_invalid || is_poison(l))) ||
			(e.rhs && (e.rhs->sema_invalid || is_poison(r)))) {
			e.sema_invalid = true;
			e.ty		   = types_.poison_ty();
			return e.ty;
		}
		if (e.str_val == "==" || e.str_val == "!=" || e.str_val == "<" || e.str_val == ">" ||
			e.str_val == "<=" || e.str_val == ">=" || e.str_val == "&&" || e.str_val == "||") {
			auto is_numeric = [](StType *t) -> bool {
				t = underlying_type(t);
				return t && (t->kind == TypeKind::UInt || t->kind == TypeKind::SInt ||
							 t->kind == TypeKind::Float);
			};
			auto is_bool = [](StType *t) -> bool {
				t = underlying_type(t);
				return t && t->kind == TypeKind::Bool;
			};
			auto is_ptr = [](StType *t) -> bool {
				t = underlying_type(t);
				return t && t->kind == TypeKind::Pointer;
			};
			if (!eq_type(l, r)) {
				if (is_numeric(r) && is_contextual_literal(*e.lhs)) {
					l = check_expr_with_expected(*e.lhs, ctx, m, scope, r);
				} else if (is_ptr(r) && e.lhs->kind == ExprKind::NullLit) {
					l = check_expr_with_expected(*e.lhs, ctx, m, scope, r);
				}
			}
			bool valid = false;
			if (e.str_val == "&&" || e.str_val == "||") {
				valid = is_bool(l) && is_bool(r);
			} else if (is_numeric(l) && is_numeric(r)) {
				valid = eq_type(l, r);
			} else if (is_bool(l) && is_bool(r)) {
				valid = eq_type(l, r) && (e.str_val == "==" || e.str_val == "!=");
			} else if (is_ptr(l) && is_ptr(r)) {
				valid = arg_type_compat(l, r) || arg_type_compat(r, l);
			} else if (l && r && arg_type_compat(l, r)) {
				StType *lu = underlying_type(l);
				if (lu && lu->kind == TypeKind::Enum && !lu->is_tagged_enum) {
					valid = e.str_val == "==" || e.str_val == "!=" ||
							e.str_val == "<" || e.str_val == "<=" ||
							e.str_val == ">" || e.str_val == ">=";
				} else if (lu && lu->kind == TypeKind::Function) {
					valid = e.str_val == "==" || e.str_val == "!=";
				}
			}
			if (!valid) {
				log::report(log::Level::Error, e.loc,
							std::string("operator '") + e.str_val + "' cannot be applied to '" +
								type_name_for_diag(l) + "' and '" + type_name_for_diag(r) + "'");
				e.sema_invalid = true;
				e.ty		   = types_.poison_ty();
				return e.ty;
			}
			e.ty = types_.bool_ty();
			fold_known_expr(e, types_.pointer_width());
			return e.ty;
		}
		// Arithmetic/bitwise operators (+ - * / % << >> & | ^) require
		// non-bool operands: `bool` is a native, non-integer type per
		// design.md and must not be silently coerced into an integer.
		auto is_bool = [&](StType *t) {
			StType *ut = underlying_type(t);
			return ut && ut->kind == TypeKind::Bool;
		};
		if (is_bool(l) || is_bool(r)) {
			log::report(log::Level::Error, e.loc,
						std::string("operator '") + e.str_val +
							"' cannot be applied to 'bool' operands");
			e.sema_invalid = true;
			e.ty		   = types_.poison_ty();
			return e.ty;
		}
		const bool explicit_integer_arithmetic = e.str_val == "+%" || e.str_val == "-%" ||
			e.str_val == "*%" || e.str_val == "+|" || e.str_val == "-|" || e.str_val == "*|";
		// Check for incompatible operand combinations before codegen.
		auto is_ptr = [](StType *t) -> bool {
			StType *ut = underlying_type(t);
			return ut && ut->kind == TypeKind::Pointer;
		};
		auto is_int = [](StType *t) -> bool {
			StType *ut = underlying_type(t);
			return ut && (ut->kind == TypeKind::UInt || ut->kind == TypeKind::SInt);
		};
		auto is_float = [](StType *t) -> bool {
			StType *ut = underlying_type(t);
			return ut && ut->kind == TypeKind::Float;
		};
		if (e.str_val == "<<" || e.str_val == ">>" || e.str_val == "&" ||
			e.str_val == "|" || e.str_val == "^") {
			if (!is_int(l) || !is_int(r)) {
				log::report(log::Level::Error, e.loc,
							std::string("operator '") + e.str_val +
								"' requires integer operands");
				e.sema_invalid = true;
				e.ty		   = types_.poison_ty();
				return e.ty;
			}
		}
		bool l_ptr = is_ptr(l);
		bool r_ptr = is_ptr(r);
		if (l_ptr || r_ptr) {
			if (l_ptr && r_ptr) {
				if (e.str_val != "-") {
					log::report(log::Level::Error, e.loc,
								std::string("operator '") + e.str_val +
									"' cannot be applied to two pointer operands");
					e.sema_invalid = true;
					e.ty		   = types_.poison_ty();
					return e.ty;
				}
				if (!eq_type(underlying_type(l)->base, underlying_type(r)->base)) {
					log::report(log::Level::Error, e.loc,
								"pointer difference requires equivalent pointee types");
					e.sema_invalid = true;
					e.ty		   = types_.poison_ty();
					return e.ty;
				}
				StType *pointee	 = underlying_type(underlying_type(l)->base);
				bool	complete = pointee && pointee->kind != TypeKind::Void &&
								   pointee->kind != TypeKind::Function &&
								   pointee->kind != TypeKind::Opaque;
				if (!complete) {
					log::report(log::Level::Error, e.loc,
								"pointer arithmetic requires a complete object pointee");
					e.sema_invalid = true;
					e.ty		   = types_.poison_ty();
					return e.ty;
				}
				e.ty = types_.isize_ty();
				fold_known_expr(e, types_.pointer_width());
				return e.ty;
			}
			if (e.str_val != "+" && e.str_val != "-") {
				log::report(log::Level::Error, e.loc,
							std::string("operator '") + e.str_val +
								"' cannot be applied to pointer operands");
				e.sema_invalid = true;
				e.ty		   = types_.poison_ty();
				return e.ty;
			}
			if (l_ptr && !is_int(r)) {
				log::report(log::Level::Error, e.loc,
							"pointer arithmetic requires integer operand on the right");
				e.sema_invalid = true;
				e.ty		   = types_.poison_ty();
				return e.ty;
			}
			if (r_ptr && !is_int(l)) {
				log::report(log::Level::Error, e.loc,
							"pointer arithmetic requires integer operand on the left");
				e.sema_invalid = true;
				e.ty		   = types_.poison_ty();
				return e.ty;
			}
			if (r_ptr && e.str_val == "-") {
				log::report(log::Level::Error, e.loc, "integer-minus-pointer is not supported");
				e.sema_invalid = true;
				e.ty		   = types_.poison_ty();
				return e.ty;
			}
			StType *pointer	 = l_ptr ? underlying_type(l) : underlying_type(r);
			StType *pointee	 = pointer ? underlying_type(pointer->base) : nullptr;
			bool	complete = pointee && pointee->kind != TypeKind::Void &&
							   pointee->kind != TypeKind::Function &&
							   pointee->kind != TypeKind::Opaque;
			if (!complete) {
				log::report(log::Level::Error, e.loc,
							"pointer arithmetic requires a complete object pointee");
				e.sema_invalid = true;
				e.ty		   = types_.poison_ty();
				return e.ty;
			}
			e.ty = l_ptr ? l : r;
			fold_known_expr(e, types_.pointer_width());
			return e.ty;
		}
		if (explicit_integer_arithmetic && (!is_int(l) || !is_int(r))) {
			log::report(log::Level::Error, e.loc,
						std::string("operator '") + e.str_val + "' requires integer operands");
			e.sema_invalid = true;
			e.ty		   = types_.poison_ty();
			return e.ty;
		}
		if ((is_int(l) || is_float(l)) && (is_int(r) || is_float(r)) && !eq_type(l, r)) {
			if (is_contextual_literal(*e.lhs)) {
				l = check_expr_with_expected(*e.lhs, ctx, m, scope, r);
			} else if (is_contextual_literal(*e.rhs)) {
				r = check_expr_with_expected(*e.rhs, ctx, m, scope, l);
			}
			if (!eq_type(l, r)) {
				log::report(log::Level::Error, e.loc,
							std::string("operator '") + e.str_val +
								"' requires operands of the same type, got '" +
								type_name_for_diag(l) + "' and '" + type_name_for_diag(r) + "'");
				e.sema_invalid = true;
				e.ty		   = types_.poison_ty();
				return e.ty;
			}
		}
		if (!(is_int(l) || is_float(l)) || !(is_int(r) || is_float(r))) {
			log::report(log::Level::Error, e.loc,
						std::string("operator '") + e.str_val + "' requires numeric operands");
			e.sema_invalid = true;
			e.ty		   = types_.poison_ty();
			return e.ty;
		}
		if (is_int(l) && e.rhs && e.rhs->has_const_value && !e.rhs->const_value_is_float) {
			if ((e.str_val == "/" || e.str_val == "%") && e.rhs->int_val == 0 &&
				!e.rhs->const_int_high_bits_ones) {
				log::report(log::Level::Error, e.rhs->loc,
							e.str_val == "/" ? "division by zero" : "modulo by zero");
				e.sema_invalid = true;
				e.ty		   = types_.poison_ty();
				return e.ty;
			}
			if (e.str_val == "<<" || e.str_val == ">>") {
				StType *lu		 = underlying_type(l);
				StType *ru		 = underlying_type(r);
				int		width	 = scalar_size_bits(lu, types_.pointer_width());
				int		r_width	 = scalar_size_bits(ru, types_.pointer_width());
				bool	r_is_neg = ru && ru->kind == TypeKind::SInt && r_width > 0 &&
								   (r_width <= 64 ? ((e.rhs->int_val >> (r_width - 1)) & 1) != 0
												  : e.rhs->const_int_high_bits_ones);
				if (r_is_neg || e.rhs->const_int_high_bits_ones ||
					(width > 0 && e.rhs->int_val >= static_cast<uint64_t>(width))) {
					log::report(log::Level::Error, e.rhs->loc,
								"shift count out of bounds for " + std::to_string(width) +
									"-bit operand");
					e.sema_invalid = true;
					e.ty		   = types_.poison_ty();
					return e.ty;
				}
			}
		}
		e.ty = l ? l : r;
		fold_known_expr(e, types_.pointer_width());
		return e.ty;
	}
	case ExprKind::Unary: {
		StType *ot = check_expr(*e.lhs, ctx, m, scope);
		if (e.lhs && (e.lhs->sema_invalid || is_poison(ot))) {
			e.sema_invalid = true;
			e.ty		   = types_.poison_ty();
			return e.ty;
		}
		StType *u  = underlying_type(ot);
		bool	ok = false;
		if (e.str_val == "!") {
			ok = u && u->kind == TypeKind::Bool;
		} else if (e.str_val == "-") {
			ok = u && (u->kind == TypeKind::SInt || u->kind == TypeKind::UInt ||
					   u->kind == TypeKind::Float);
		} else if (e.str_val == "~") {
			ok = u && (u->kind == TypeKind::SInt || u->kind == TypeKind::UInt);
		}
		if (!ok) {
			log::report(log::Level::Error, e.loc,
						std::string("operator '") + e.str_val + "' cannot be applied to '" +
							type_name_for_diag(ot) + "'");
			e.ty		   = types_.poison_ty();
			e.sema_invalid = true;
			return e.ty;
		} else {
			e.ty = (e.str_val == "!") ? types_.bool_ty() : ot;
		}
		fold_known_expr(e, types_.pointer_width());
		return e.ty;
	}
	case ExprKind::Assign: {
		bool saved_lhs			 = checking_assignment_lhs_;
		checking_assignment_lhs_ = e.lhs && e.lhs->kind == ExprKind::Ident;
		StType *l				 = check_expr(*e.lhs, ctx, m, scope);
		checking_assignment_lhs_ = saved_lhs;
		StType *r				 = check_expr_with_expected(*e.rhs, ctx, m, scope, l);
		// The left-hand side must be an lvalue (writable storage location).
		// Literals, constants, rvalue temporaries, and function pointers
		// are not valid assignment targets.
		if (!e.lhs->is_lvalue) {
			const Symbol *binding = nullptr;
			if (e.lhs->kind == ExprKind::Ident && !e.lhs->path.empty()) {
				if (e.lhs->path.size() == 1 && !e.lhs->binding_module) {
					binding = scope.lookup(e.lhs->path.front());
				}
				if (!binding) {
					Module &binding_module = e.lhs->binding_module ? *e.lhs->binding_module : m;
					binding = resolve_path(e.lhs->path, binding_module);
				}
			}
			if (binding && binding->kind == SymKind::Param && !binding->is_mutable) {
				log::error(e.lhs->loc,
						   "cannot assign to immutable parameter `" + binding->simple + "`")
					.code("E_IMMUTABLE_ASSIGNMENT")
					.phase("sema")
					.note(binding->loc, "parameter declared here")
					.fixit(binding->loc, "mut " + binding->simple,
						   "consider making the parameter mutable: `mut " + binding->simple + "`")
					.emit();
			} else if (binding && binding->kind == SymKind::Var && !binding->is_mutable) {
				log::report(log::Level::Error, e.lhs->loc,
							"cannot assign to immutable binding '" + binding->simple + "'",
							"E_IMMUTABLE_ASSIGNMENT", "sema");
			} else {
				log::report(log::Level::Error, e.lhs->loc,
							"left-hand side of assignment is not a valid lvalue");
			}
		}
		// Typecheck the assignment after contextual literal typing. Typed values
		// must match the lhs exactly; explicit `cast` is required otherwise.
		if (l && r && !e.rhs->sema_invalid && !e.lhs->sema_invalid) {
			check_type_compat(l, r, *e.rhs, e.loc,
							  "Assignment value not representable in '" + type_name_for_diag(l) +
								  "'",
							  "Assignment type mismatch: cannot assign '" + type_name_for_diag(r) +
								  "' to '" + type_name_for_diag(l) + "'");
		}
		e.ty = l ? l : r;
		return e.ty;
	}
	case ExprKind::Call: {
		Module &call_module = e.binding_module ? *e.binding_module : m;

		if (e.lhs && e.lhs->kind == ExprKind::EnumMember) {
			log::report(log::Level::Error, e.loc,
						"enum variant shorthand '." + e.lhs->member +
							"(...)' requires an expected enum type");
			e.ty		   = types_.void_ty();
			e.sema_invalid = true;
			return e.ty;
		}

		// Tagged enum tuple variant construction: `EnumName.Variant(args...)` or `EnumName<T>.Variant(args...)`.
		if (e.lhs && e.lhs->kind == ExprKind::Member && e.lhs->lhs) {
			std::vector<std::string> vpath;
			if (flatten_member_path(*e.lhs, vpath) && vpath.size() >= 2) {
				std::vector<std::string> head(vpath.begin(), vpath.end() - 1);
				Symbol *en_sym = resolve_path(head, call_module);
				if (en_sym && en_sym->kind == SymKind::Enum && en_sym->en) {
					StType *enum_ty = en_sym->type;
					if (!en_sym->en->generic_params.empty()) {
						TypeRef tr;
						tr.loc = e.lhs->loc;
						for (const auto &ga : e.lhs->lhs->generic_args) {
							tr.generic_args.push_back(clone_generic_arg(ga));
						}
						enum_ty = instantiate_generic_type(*en_sym, tr, call_module);
					}
					StType *ut = underlying_type(enum_ty);
					if (ut && ut->kind == TypeKind::Enum) {
						size_t vidx = SIZE_MAX;
						for (size_t vi = 0; vi < ut->field_names.size(); ++vi) {
							if (ut->field_names[vi] == vpath.back()) {
								vidx = vi;
								break;
							}
						}
						if (vidx == SIZE_MAX) {
							log::report(log::Level::Error, e.loc,
										"enum '" + type_name_for_diag(enum_ty) +
											"' has no variant '" + vpath.back() + "'");
							e.ty		   = types_.void_ty();
							e.sema_invalid = true;
							return e.ty;
						}
						const auto &vi = ut->enum_variants[vidx];
						if (vi.payload_kind != EnumPayloadKind::Tuple) {
							log::report(log::Level::Error, e.loc,
										"variant '" + vpath.back() + "' of enum '" +
											type_name_for_diag(enum_ty) +
											"' cannot be constructed with positional arguments");
							e.ty		   = types_.void_ty();
							e.sema_invalid = true;
							return e.ty;
						}
						if (e.args.size() != vi.field_types.size()) {
							log::report(log::Level::Error, e.loc,
										"variant '" + vpath.back() + "' expects " +
											std::to_string(vi.field_types.size()) +
											" argument(s), got " + std::to_string(e.args.size()));
						}
						for (size_t ai = 0; ai < e.args.size() && ai < vi.field_types.size(); ++ai) {
							StType *ft	= vi.field_types[ai];
							StType *aty = check_expr_with_expected(*e.args[ai], ctx, m, scope, ft);
							if (ft && aty && !e.args[ai]->sema_invalid && !eq_type(ft, aty)) {
								log::report(log::Level::Error, e.args[ai]->loc,
											"variant payload type mismatch: expected '" +
												type_name_for_diag(ft) + "', got '" +
												type_name_for_diag(aty) + "'");
							}
						}
						e.ty	  = enum_ty;
						e.int_val = ut->enum_values[vidx];
						e.lhs->ty = enum_ty;
						return e.ty;
					}
				}
			}
		}

		bool is_receiver_call = false;
		if (e.lhs && e.lhs->kind == ExprKind::Member && e.lhs->lhs) {
			std::vector<std::string> path;
			if (flatten_member_path(*e.lhs, path)) {
				if (!path.empty() && scope.lookup(path[0])) {
					is_receiver_call = true;
				} else {
					Symbol *sym		= resolve_path(path, call_module);
					Symbol *raw_sym = sym ? sym : resolve_path_any(path, call_module);
					if (!raw_sym || raw_sym->kind != SymKind::Fn) {
						is_receiver_call = true;
					}
				}
			} else {
				is_receiver_call = true;
			}
		}

		if (is_receiver_call) {
			return resolve_ufcs_call(e, ctx, m, scope);
		}

		std::vector<std::string> call_path;
		if (e.lhs->kind == ExprKind::Ident) {
			call_path = e.lhs->path;
		} else {
			flatten_member_path(*e.lhs, call_path);
		}
		if (!call_path.empty()) {
			Symbol *sym		= resolve_path(call_path, call_module);
			Symbol *raw_sym = sym ? sym : resolve_path_any(call_path, call_module);
			if (!sym && !raw_sym && has_unresolved_import(call_module, call_path)) {
				e.sema_invalid = true;
				e.ty		   = types_.void_ty();
				return e.ty;
			}
			if (!sym && !raw_sym) {
				if (std::string missing = missing_child_module(resolver_, *this, call_module, call_path);
					!missing.empty()) {
					log::report(log::Level::Error, e.loc,
								"module `" + missing + "` could not be found", "E_MODULE_NOT_FOUND",
								"resolver");
					e.sema_invalid = true;
					e.ty		   = types_.void_ty();
					return e.ty;
				}
			}
			bool saved_defer_aggregate	= defer_contextual_aggregate_;
			StType *saved_expected_res	= expected_generic_result_;
			defer_contextual_aggregate_ = true;
			expected_generic_result_	= nullptr;
			for (auto &arg : e.args) {
				if (!is_contextual_literal(*arg)) {
					check_expr(*arg, ctx, m, scope);
				}
			}
			defer_contextual_aggregate_ = saved_defer_aggregate;
			expected_generic_result_	= saved_expected_res;
			for (auto &arg : e.args) {
				if (arg && (arg->sema_invalid || is_poison(arg->ty))) {
					e.sema_invalid = true;
					e.ty		   = types_.poison_ty();
					return e.ty;
				}
			}
			if (raw_sym && raw_sym->kind == SymKind::Fn && !raw_sym->overloads.empty()) {
				const bool				 cross_module = raw_sym->owner_module != &call_module &&
					!resolver_.same_source_file(*raw_sym->owner_module, call_module);
				std::vector<FnDecl *>	 candidates;
				std::vector<std::string> generic_constraint_rejections;
				std::unordered_map<const FnDecl *, std::string> generic_rejection_map;
				for (FnDecl *cand : raw_sym->overloads) {
					if (!cross_module || (cand && cand->is_pub)) {
						if (cand && !cand->generic_params.empty()) {
							if (FnDecl *instance =
								instantiate_generic_fn(*cand, e, e.args, call_module, ctx)) {
								candidates.push_back(instance);
							} else if (!last_generic_constraint_error_.empty()) {
								generic_constraint_rejections.push_back(
									last_generic_constraint_error_);
								generic_rejection_map[cand] = last_generic_constraint_error_;
							} else {
								generic_rejection_map[cand] =
									"could not infer generic arguments from call arguments";
							}
						} else {
							candidates.push_back(cand);
						}
					}
				}
				if (candidates.empty()) {
					if (!generic_constraint_rejections.empty()) {
						log::report(log::Level::Error, e.loc, generic_constraint_rejections.front(),
									"E_GENERIC_CONSTRAINT", "sema");
					} else {
						// A visible generic overload can fail specialization (for
						// example because literal arguments do not provide enough
						// information, or because the argument count is wrong). That
						// is not a visibility failure; only report the visibility
						// diagnostic when no visible overload exists at all.
						bool has_visible_overload = false;
						for (FnDecl *cand : raw_sym->overloads) {
							if (cand && (!cross_module || cand->is_pub)) {
								has_visible_overload = true;
								break;
							}
						}
						if (has_visible_overload) {
							auto err = log::error(e.loc,
										"no matching public function for call to '" +
											join_path(call_path) + "' with " +
											std::to_string(e.args.size()) + " argument(s)")
										.code(log::DiagCode::InvalidCall);
							std::unordered_set<const FnDecl *> seen;
							for (FnDecl *cand : raw_sym->overloads) {
								if (!cand) continue;
								FnDecl *orig = (cand->is_generic_instance && cand->generic_template)
									? cand->generic_template
									: cand;
								if (!seen.insert(orig).second) continue;
								if (cross_module && !cand->is_pub) continue;
								SourceLoc cand_loc = orig->loc;
								std::string reason = candidate_rejection_reason(*cand, e, call_module, cross_module);
								err.note(cand_loc, "candidate function not viable: " + reason);
							}
							err.emit();
						} else {
							log::report(log::Level::Error, e.loc,
										"function '" + join_path(call_path) +
											"' is not public in module '" +
											join_path(raw_sym->module_path) + "'");
						}
					}
					e.sema_invalid = true;
					e.ty		   = types_.void_ty();
					return e.ty;
				}
				struct ViableCand {
					FnDecl *fn		= nullptr;
					int		penalty = 0;
				};
				std::vector<ViableCand> viables;
				bool		 literal_nofit = false;
				size_t		literal_nofit_index = 0;
				StType		*literal_nofit_type = nullptr;

				for (FnDecl *cand : candidates) {
					size_t n_fixed = cand->params.size();
					if (!cand->is_variadic && e.args.size() != n_fixed) {
						continue;
					}
					if (cand->is_variadic && e.args.size() < n_fixed) {
						continue;
					}

					bool ok	 = true;
					int	 pen = 0;
					for (size_t i = 0; i < e.args.size(); ++i) {
						if (i < n_fixed) {
							StType *pty = cand->params[i].resolved;
							if (!pty && cand->params[i].ty) {
								pty = resolve_type(*cand->params[i].ty, call_module);
							}
							if (!pty) {
								ok = false;
								break;
							}

							StType *aty = e.args[i]->ty;
							if (is_contextual_enum_variant_expr(*e.args[i])) {
								uint64_t enum_value = 0;
								if (enum_member_value(pty, contextual_enum_variant_name(*e.args[i]),
													  enum_value)) {
									continue;
								}
								ok = false;
								break;
							}
							if (eq_type(pty, aty)) {
								pen += 0;
							} else {
								bool is_arg_int	  = types_.is_int_like(aty);
								bool is_param_int = types_.is_int_like(pty);
								bool is_arg_float =
									(underlying_type(aty) && underlying_type(aty)->kind == TypeKind::Float);
								bool is_param_float =
									(underlying_type(pty) && underlying_type(pty)->kind == TypeKind::Float);

								auto fv = is_contextual_literal(*e.args[i])
										  ? value_fits(pty, *e.args[i])
										  : FitResult::Unknown;
								if (fv == FitResult::NoFit && cand->is_generic_instance &&
									cand->generic_template && cand->generic_template->is_macro &&
									 e.args[i]->kind == ExprKind::FloatLit && expected_generic_result_ &&
									 eq_type(pty, expected_generic_result_) && underlying_type(pty)->kind == TypeKind::Float) {
									fv = FitResult::Fit;
								}
								// A generic parameter can be bound by an earlier
								// argument and reused in a later parameter. In that
								// case an exact integer literal may be contextually
								// converted to the specialized float type (for example
								// pow(2 cast f32, 3)). This is deliberately limited to
								// generic parameter slots; ordinary integer-to-float
								// calls still require an explicit cast.
								if (fv == FitResult::Unknown && cand->is_generic_instance &&
									cand->generic_template &&
									i < cand->generic_template->params.size() &&
									cand->generic_template->params[i].ty &&
									type_ref_contains_generic(*cand->generic_template->params[i].ty,
															  *cand->generic_template) &&
									generic_integer_literal_fits_float(pty, *e.args[i])) {
									fv = FitResult::Fit;
								}
								if (fv == FitResult::Fit) {
									if ((is_arg_int && is_param_float) ||
										(is_arg_float && is_param_int)) {
										pen += 3;
									} else {
										pen += 1;
									}
								} else if (fv == FitResult::NoFit) {
									if (!literal_nofit) {
										literal_nofit = true;
										literal_nofit_index = i;
										literal_nofit_type = pty;
									}
									ok = false;
									break;
								} else {
									if (arg_type_compat(pty, aty)) {
										if ((is_arg_int && is_param_float) ||
											(is_arg_float && is_param_int)) {
											pen += 4;
										} else {
											pen += 2;
										}
									} else {
										ok = false;
										break;
									}
								}
							}
						}
					}
					if (ok) {
						if (cand->is_generic_instance) {
							bool constrained = false;
							if (cand->generic_template) {
								for (const auto &gp : cand->generic_template->generic_params) {
									if (!gp.type_set.empty() || !gp.trait_bounds.empty()) {
										constrained = true;
										break;
									}
								}
							}
							pen += constrained ? 1 : 2;
						}
						viables.push_back({cand, pen});
					}
				}

				if (viables.empty()) {
					bool hidden_match = false;
					if (cross_module) {
						for (FnDecl *hidden : raw_sym->overloads) {
							if (!hidden || hidden->is_pub) {
								continue;
							}
							size_t n_fixed = hidden->params.size();
							if ((!hidden->is_variadic && e.args.size() != n_fixed) ||
								(hidden->is_variadic && e.args.size() < n_fixed)) {
								continue;
							}
							bool fits = true;
							for (size_t i = 0; i < n_fixed; ++i) {
								StType *pty = hidden->params[i].resolved;
								if (is_contextual_enum_variant_expr(*e.args[i])) {
									uint64_t enum_value = 0;
									if (!enum_member_value(pty,
														   contextual_enum_variant_name(*e.args[i]),
														   enum_value)) {
										fits = false;
										break;
									}
									continue;
								}
								auto	fv	= is_contextual_literal(*e.args[i])
												  ? value_fits(pty, *e.args[i])
												  : FitResult::Unknown;
								if (fv == FitResult::NoFit ||
									(fv == FitResult::Unknown &&
									 !arg_type_compat(pty, e.args[i]->ty))) {
									fits = false;
									break;
								}
							}
							if (fits) {
								hidden_match = true;
								log::report(
									log::Level::Error, e.loc,
									"function '" + join_path(raw_sym->module_path) + "." +
										fn_signature_for_diag(*hidden) +
										"' is private; add 'pub' to call it from another module");
								break;
							}
						}
					}
					const bool all_generic_failed =
						candidates.empty() && !generic_constraint_rejections.empty();
					if (!hidden_match && all_generic_failed) {
						log::report(log::Level::Error, e.loc, generic_constraint_rejections.front(),
									"E_GENERIC_CONSTRAINT", "sema");
					}
					if (!hidden_match && !all_generic_failed && literal_nofit) {
						bool is_float_target = literal_nofit_type &&
							underlying_type(literal_nofit_type) &&
							underlying_type(literal_nofit_type)->kind == TypeKind::Float;
						std::string remedy = is_float_target ? "; append '~' to agree to lossy rounding"
															 : "; use an explicit cast";
						log::report(
							log::Level::Error, e.args[literal_nofit_index]->loc,
							"argument " + std::to_string(literal_nofit_index + 1) +
								" of call to '" + join_path(call_path) +
								"' has a value not exactly representable in '" +
								type_name_for_diag(literal_nofit_type) +
								"'" + remedy);
					} else if (!hidden_match && !all_generic_failed) {
						auto err = log::error(e.loc,
									"no matching public function for call to '" +
										join_path(call_path) + "' with " +
										std::to_string(e.args.size()) + " argument(s)")
									.code(log::DiagCode::InvalidCall);
						std::unordered_set<const FnDecl *> seen;
						for (FnDecl *cand : raw_sym->overloads) {
							if (!cand || (cross_module && !cand->is_pub)) continue;
							FnDecl *orig = (cand->is_generic_instance && cand->generic_template)
								? cand->generic_template
								: cand;
							if (!seen.insert(orig).second) continue;
							SourceLoc cand_loc = orig->loc;
							std::string reason;
							auto rgit = generic_rejection_map.find(orig);
							if (rgit != generic_rejection_map.end()) {
								reason = rgit->second;
							} else if (!orig->generic_params.empty()) {
								// Find specialized candidate if one exists
								FnDecl *spec = nullptr;
								for (FnDecl *c : candidates) {
									if (c && c->is_generic_instance && c->generic_template == orig) {
										spec = c;
										break;
									}
								}
								reason = spec ? candidate_rejection_reason(*spec, e, call_module, cross_module)
											  : "could not infer generic arguments from call arguments";
							} else {
								reason = candidate_rejection_reason(*cand, e, call_module, cross_module);
							}
							err.note(cand_loc, "candidate function not viable: " + reason);
						}
						err.emit();
					}
					e.sema_invalid = true;
					e.ty		   = types_.void_ty();
					return e.ty;
				}

				int min_pen = viables[0].penalty;
				for (auto &v : viables) {
					if (v.penalty < min_pen) {
						min_pen = v.penalty;
					}
				}

				std::vector<FnDecl *> best_cands;
				for (auto &v : viables) {
					if (v.penalty == min_pen) {
						best_cands.push_back(v.fn);
					}
				}

				if (best_cands.size() > 1) {
					log::report(log::Level::Error, e.loc,
								"ambiguous call to overloaded function '" + join_path(call_path) +
									"'");
					e.sema_invalid = true;
					e.ty		   = types_.void_ty();
					return e.ty;
				}

				FnDecl *selected = best_cands[0];
				e.resolved_fn	 = selected;
				selected->used	 = true;
				for (size_t i = 0; i < selected->params.size() && i < e.args.size(); ++i) {
					if (is_contextual_literal(*e.args[i])) {
						check_expr_with_expected(*e.args[i], ctx, m, scope,
													 selected->params[i].resolved);
					}
				}
				if (selected->is_macro) {
					if (!selected->macro_returns_value && allowed_macro_call_ == &e) {
						e.lhs->ty = selected->fn_sig;
						e.ty = types_.void_ty();
						return e.ty;
					}
					if (!selected->macro_returns_value) {
						log::report(log::Level::Error, e.loc,
										"macro has no value; it can only be used as a standalone statement");
						e.sema_invalid = true;
						e.ty = types_.void_ty();
						return e.ty;
					}
					auto expanded = expand_macro_call(e, ctx, m, scope);
					if (!expanded) {
						e.sema_invalid = true;
						e.ty = types_.void_ty();
						return e.ty;
					}
					StType *saved_block_expected = expected_block_expr_;
					auto replacement = std::make_unique<Expr>();
					replacement->kind = ExprKind::Block;
					replacement->loc = e.loc;
					replacement->block = std::move(expanded);
					if (!expected_block_expr_ && selected->fn_sig && selected->fn_sig->fn_ret &&
						underlying_type(selected->fn_sig->fn_ret)->kind != TypeKind::Void) {
						expected_block_expr_ = selected->fn_sig->fn_ret;
					}
					e = std::move(*replacement);
					FnDecl *previous_generic = generic_context_;
					if (selected->is_generic_instance) {
						generic_context_ = selected;
					}
					macro_control_bases_.push_back({loop_stack_.size(), switch_stack_.size()});
					++macro_expansion_depth_;
					StType *result = check_expr(e, ctx, m, scope);
					--macro_expansion_depth_;
					macro_control_bases_.pop_back();
					generic_context_ = previous_generic;
					expected_block_expr_ = saved_block_expected;
					return result;
				}
				if (selected->is_variadic && !selected->is_macro) {
					for (size_t i = selected->params.size(); i < e.args.size(); ++i) {
						if (!e.args[i]->sema_invalid && !supported_va_type(e.args[i]->ty)) {
							log::report(log::Level::Error, e.args[i]->loc,
										"variadic argument " + std::to_string(i + 1) +
											" has unsupported type '" +
											type_name_for_diag(e.args[i]->ty) +
											"' (expected scalar or pointer)");
						}
						if (selected->is_extern) {
							StType *aty = underlying_type(e.args[i]->ty);
							if (aty && aty->kind == TypeKind::Enum) {
								aty = underlying_type(aty->base);
							}
							if (aty && aty->kind == TypeKind::Float && aty->width < 64) {
								log::report(log::Level::Error, e.args[i]->loc,
											"external variadic argument " + std::to_string(i + 1) +
												" of type '" + type_name_for_diag(aty) +
												"' requires an explicit cast to f64");
							} else if (aty && aty->kind == TypeKind::Bool) {
								log::report(
									log::Level::Error, e.args[i]->loc,
									"external variadic argument " + std::to_string(i + 1) +
										" requires an explicit cast to a 32-bit integer type");
							} else if (aty &&
									   (aty->kind == TypeKind::UInt ||
										aty->kind == TypeKind::SInt) &&
									   aty->width > 0 && aty->width < 32) {
								log::report(
									log::Level::Error, e.args[i]->loc,
									"external variadic argument " + std::to_string(i + 1) +
										" of type '" + type_name_for_diag(aty) +
										"' requires an explicit cast to a 32-bit integer type");
							}
						}
					}
				}
				e.lhs->ty = selected->fn_sig;
				e.ty = selected->fn_sig
						   ? selected->fn_sig->fn_ret
						   : (selected->ret ? resolve_type(*selected->ret, call_module) : types_.void_ty());
				return e.ty;
			}
		}
		return check_indirect_call(e, ctx, m, scope);
	}
	case ExprKind::Member: {
		Module &member_module = e.binding_module ? *e.binding_module : m;
		std::vector<std::string> path;
		if (flatten_member_path(e, path) && path.size() >= 2) {
			if (has_unresolved_import(member_module, path)) {
				e.sema_invalid = true;
				e.ty		   = types_.void_ty();
				return e.ty;
			}
			if (std::string missing = missing_child_module(resolver_, *this, member_module, path);
				!missing.empty()) {
				log::report(log::Level::Error, e.loc, "module `" + missing + "` could not be found",
							"E_MODULE_NOT_FOUND", "resolver");
				e.sema_invalid = true;
				e.ty		   = types_.void_ty();
				return e.ty;
			}
			std::vector<std::string> head(path.begin(), path.end() - 1);
			Symbol					*en = resolve_path(head, member_module);
			if (en && en->kind == SymKind::Enum && en->en) {
				StType *enum_ty = en->type;
				if (!en->en->generic_params.empty() && e.lhs && !e.lhs->generic_args.empty()) {
					TypeRef tr;
					tr.loc = e.lhs->loc;
					for (const auto &ga : e.lhs->generic_args) {
						tr.generic_args.push_back(clone_generic_arg(ga));
					}
					enum_ty = instantiate_generic_type(*en, tr, member_module);
				}
				StType *ut = underlying_type(enum_ty);
				for (size_t vi = 0; vi < en->en->members.size(); ++vi) {
					const auto &mb = en->en->members[vi];
					if (mb.name == path.back()) {
						if (ut && ut->is_tagged_enum && vi < ut->enum_variants.size() &&
							ut->enum_variants[vi].payload_kind != EnumPayloadKind::Unit) {
							log::report(log::Level::Error, e.loc,
										"tagged enum variant '" + mb.name +
											"' requires payload arguments");
							e.sema_invalid = true;
						}
						e.ty	  = enum_ty;
						e.int_val = mb.resolved;
						return e.ty;
					}
				}
			}
			Symbol *qualified = resolve_path(path, member_module);
			if (!qualified && en &&
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
					if (it != table->second.end()) qualified = &it->second;
				}
			}
			if (!qualified) {
				if (Symbol *raw_qual = resolve_path_any(path, member_module);
					raw_qual && raw_qual->kind == SymKind::Mod) {
					qualified = raw_qual;
				}
			}
			if (qualified && qualified->kind == SymKind::Mod) {
				log::error(e.loc, "module '" + join_path(path) + "' cannot be used as a value")
					.code("E_INVALID_EXPR")
					.phase("semantic")
					.emit();
				e.sema_invalid = true;
				e.ty		   = types_.poison_ty();
				return e.ty;
			}
			if (qualified &&
				(qualified->kind == SymKind::Struct || qualified->kind == SymKind::Union ||
				 qualified->kind == SymKind::TypeBinding || qualified->kind == SymKind::Enum)) {
				TypeRef type_ref;
				type_ref.kind	= TypeRef::Named;
				type_ref.loc	= e.loc;
				type_ref.name	= join_path(path);
				type_ref.single = path.back();
				for (const auto &ga : e.generic_args) {
					type_ref.generic_args.push_back(clone_generic_arg(ga));
				}
				e.type_value	= resolve_type(type_ref, member_module);
				e.ty			= types_.type_ty();
				return e.ty;
			}
			if (qualified &&
				(qualified->kind == SymKind::Var || qualified->kind == SymKind::Const ||
				 qualified->kind == SymKind::Fn)) {
				if (qualified->kind == SymKind::Fn) {
					FnDecl *f =
						qualified->fn
							? qualified->fn
							: (!qualified->overloads.empty() ? qualified->overloads[0] : nullptr);
					if (f && f->is_macro) {
						log::report(log::Level::Error, e.loc,
										"macro names cannot be used as values");
						e.sema_invalid = true;
						e.ty = types_.void_ty();
						return e.ty;
					}
					if (f) {
						f->used = true;
					}
				} else if (qualified->usage) {
					*qualified->usage = true;
				}
				e.ty = qualified->type;
				if (qualified->kind == SymKind::Fn && qualified->fn) {
					e.ty = qualified->fn->fn_sig;
				}
				e.is_lvalue = qualified->kind == SymKind::Var && qualified->is_mutable;
				e.is_addressable = qualified->kind == SymKind::Var ||
					qualified->kind == SymKind::Const;
				if (qualified->kind == SymKind::Const) {
					if (!qualified->has_const_value) {
						ensure_const_value(*qualified);
					}
					if (qualified->has_const_value) {
						e.has_const_value = true;
						e.is_const_bool = qualified->const_bool_val && !qualified->const_value_is_float;
						e.const_bool_val = qualified->const_bool_val;
						e.const_value_is_float = qualified->const_value_is_float;
						e.const_int_high_bits_ones = qualified->const_int_high_bits_ones;
						e.int_val = qualified->const_int_val;
						e.flt_val = qualified->const_flt_val;
					}
				}
				return e.ty;
			}
		}
		StType *bt = underlying_type(check_expr(*e.lhs, ctx, m, scope));
		bool	 writable_base = e.lhs->is_lvalue;
		bool	 addressable_base = e.lhs->is_addressable;
		if (bt && bt->kind == TypeKind::Pointer) {
			while (bt && bt->kind == TypeKind::Pointer) {
				StType *pointee = underlying_type(bt->base);
				if (pointee &&
					(pointee->kind == TypeKind::Struct || pointee->kind == TypeKind::Union)) {
					writable_base = bt->is_mutable;
					addressable_base = true;
					bt = pointee;
					break;
				}
				bt = pointee;
			}
		}
		if (bt && bt->kind == TypeKind::Slice) {
			if (e.member == "ptr") {
				e.ty = types_.pointer(bt->base, bt->is_mutable, bt->is_volatile);
				return e.ty;
			}
			if (e.member == "len") {
				e.ty = types_.usize_ty();
				return e.ty;
			}
			log::error(e.loc, "no field '" + e.member + "' on slice")
				.code("E_UNKNOWN_MEMBER")
				.phase("semantic")
				.emit();
			e.sema_invalid = true;
			e.ty = types_.poison_ty();
			return e.ty;
		}
		if (bt && bt->kind == TypeKind::Array) {
			if (e.member == "len") {
				e.ty = types_.usize_ty();
				e.has_const_value = true;
				e.int_val = bt->array_len;
				return e.ty;
			}
			log::error(e.loc, "no field '" + e.member + "' on array")
				.code("E_UNKNOWN_MEMBER")
				.phase("semantic")
				.emit();
			e.sema_invalid = true;
			e.ty = types_.poison_ty();
			return e.ty;
		}
		if (bt && (bt->kind == TypeKind::Struct || bt->kind == TypeKind::Union)) {
			const Module &member_module = e.binding_module ? *e.binding_module : m;
			for (size_t i = 0; i < bt->field_names.size(); ++i) {
				if (bt->field_names[i] == e.member) {
					if (!field_accessible(*bt, i, member_module)) {
						report_private_field(e.loc, *bt, i);
						e.sema_invalid = true;
						e.ty		   = types_.poison_ty();
						return e.ty;
					}
					e.ty		= bt->fields[i];
					e.is_lvalue = writable_base;
					e.is_addressable = addressable_base;
					return e.ty;
				}
			}
			std::string suggestion;
			size_t best_dist = SIZE_MAX;
			size_t max_d = e.member.size() <= 3 ? 1 : (e.member.size() <= 8 ? 2 : 3);
			for (const auto &fn : bt->field_names) {
				size_t d = levenshtein_distance(e.member, fn);
				if (d <= max_d && d < best_dist) {
					best_dist = d;
					suggestion = fn;
				}
			}
			auto diag = log::error(e.loc, "no field '" + e.member + "' on aggregate")
							.code("E_UNKNOWN_MEMBER")
							.phase("semantic");
			if (!suggestion.empty()) {
				diag.help("did you mean `" + suggestion + "`?");
				diag.fixit(e.loc, suggestion, "did you mean `" + suggestion + "`?");
			}
			diag.emit();
		} else if (!e.lhs->sema_invalid && !is_poison(bt)) {
			log::report(log::Level::Error, e.loc, "member access on non-aggregate");
		}
		e.sema_invalid = true;
		e.ty = types_.poison_ty();
		return e.ty;
	}
	case ExprKind::Index: {
		StType *bt = check_expr(*e.lhs, ctx, m, scope);
		StType *bu = underlying_type(bt);
		if (e.is_bit_range) {
			StType *value_type = bu;
			if (!value_type || (value_type->kind != TypeKind::UInt &&
								value_type->kind != TypeKind::SInt)) {
				log::report(log::Level::Error, e.loc,
							"bit-range extraction requires an integer operand");
				e.ty = types_.void_ty();
				e.sema_invalid = true;
				return e.ty;
			}
			auto check_bound = [&](std::unique_ptr<Expr> &bound, const char *name,
								 uint64_t &value) -> bool {
				if (!bound) {
					log::report(log::Level::Error, e.loc,
								std::string("bit-range extraction requires a ") + name + " bound");
					return false;
				}
				StType *bound_type = is_contextual_integer_expr(*bound)
					? check_expr_with_expected(*bound, ctx, m, scope, types_.sint(64))
					: check_expr(*bound, ctx, m, scope);
					StType *bound_underlying = underlying_type(bound_type);
				if (!bound->has_const_value || bound->const_value_is_float || bound->is_const_bool ||
					!bound_underlying || (bound_underlying->kind != TypeKind::UInt &&
										 bound_underlying->kind != TypeKind::SInt)) {
					log::report(log::Level::Error, bound->loc,
								std::string("bit-range ") + name + " bound must be a compile-time integer constant");
					return false;
				}
				int width = scalar_size_bits(bound_underlying, types_.pointer_width());
				if (bound_underlying->kind == TypeKind::SInt && width > 0 &&
					((width <= 64 && ((bound->int_val >> (width - 1)) & 1) != 0) ||
					 (width > 64 && bound->const_int_high_bits_ones))) {
					log::report(log::Level::Error, bound->loc,
								std::string("bit-range ") + name + " bound must be nonnegative");
					return false;
				}
				value = bound->int_val;
				return true;
			};
			bool bounds_valid = check_bound(e.rhs, "high", e.bit_range_high) &&
				check_bound(e.index_low, "low", e.bit_range_low);
			if (!bounds_valid) {
				e.ty = types_.void_ty();
				e.sema_invalid = true;
				return e.ty;
			}
			const uint64_t source_width = static_cast<uint64_t>(value_type->width);
			if (e.bit_range_high < e.bit_range_low) {
				log::report(log::Level::Error, e.loc,
							"bit-range high bound must be greater than or equal to low bound");
				e.ty = types_.void_ty();
				e.sema_invalid = true;
				return e.ty;
			}
			if (e.bit_range_high >= source_width) {
				log::report(log::Level::Error, e.loc,
							"bit-range bound is outside the integer width of " +
								type_name_for_diag(bt));
				e.ty = types_.void_ty();
				e.sema_invalid = true;
				return e.ty;
			}
			const uint64_t range_width = e.bit_range_high - e.bit_range_low + 1;
			if (range_width > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
				log::report(log::Level::Error, e.loc, "bit-range result width is too large");
				e.ty = types_.void_ty();
				e.sema_invalid = true;
				return e.ty;
			}
			e.ty = types_.uint(static_cast<int>(range_width));
			return e.ty;
		}
		StType *it = (bu && (bu->kind == TypeKind::Pointer || bu->kind == TypeKind::Array ||
							 bu->kind == TypeKind::Slice) &&
					  e.rhs && is_contextual_literal(*e.rhs))
						 ? check_expr_with_expected(*e.rhs, ctx, m, scope, types_.isize_ty())
						 : check_expr(*e.rhs, ctx, m, scope);
		StType *iu = underlying_type(it);
		if (!iu || (iu->kind != TypeKind::UInt && iu->kind != TypeKind::SInt)) {
			log::report(log::Level::Error, e.rhs->loc, "array index must be an integer");
		}
		bt = underlying_type(bt);
		if (bt && bt->kind == TypeKind::Pointer) {
			e.ty		= underlying_type(bt->base);
			e.is_lvalue = bt->is_mutable;
			e.is_addressable = true;
			return e.ty;
		}
		auto is_signed_const_neg = [&](const Expr &idx, StType *idx_type) -> bool {
			if (!idx_type || idx_type->kind != TypeKind::SInt) {
				return false;
			}
			int width = scalar_size_bits(idx_type, types_.pointer_width());
			if (width <= 0) {
				return false;
			}
			return width <= 64 ? ((idx.int_val >> (width - 1)) & 1) != 0
							   : idx.const_int_high_bits_ones;
		};
		if (bt && bt->kind == TypeKind::Slice) {
			if (e.rhs && e.rhs->has_const_value && !e.rhs->const_value_is_float &&
				is_signed_const_neg(*e.rhs, iu)) {
				log::report(log::Level::Error, e.rhs->loc, "slice index cannot be negative");
			}
			e.ty		= underlying_type(bt->base);
			e.is_lvalue = bt->is_mutable;
			e.is_addressable = true;
			return e.ty;
		}
		if (bt && bt->kind == TypeKind::Array) {
			if (e.rhs && e.rhs->has_const_value && !e.rhs->const_value_is_float && iu) {
				if (is_signed_const_neg(*e.rhs, iu)) {
					log::report(log::Level::Error, e.rhs->loc, "array index cannot be negative");
				} else if (e.rhs->const_int_high_bits_ones || e.rhs->int_val >= bt->array_len) {
					log::report(log::Level::Error, e.rhs->loc,
								"array index " + std::to_string(e.rhs->int_val) +
									" is out of bounds for array of length " +
									std::to_string(bt->array_len));
				}
			}
			e.ty		= underlying_type(bt->base);
			e.is_lvalue = e.lhs->is_lvalue;
			e.is_addressable = e.lhs->is_addressable;
			return e.ty;
		}
		log::report(log::Level::Error, e.loc, "index of non-pointer, non-array");
		return types_.void_ty();
	}
	case ExprKind::Slice: {
		StType *bt = underlying_type(check_expr(*e.lhs, ctx, m, scope));
		auto check_bound = [&](const std::unique_ptr<Expr> &bound, const char *label,
							   bool &has_const, uint64_t &const_val) -> bool {
			if (!bound) {
				has_const = false;
				return true;
			}
			StType *it = is_contextual_literal(*bound)
							 ? check_expr_with_expected(*bound, ctx, m, scope, types_.usize_ty())
							 : check_expr(*bound, ctx, m, scope);
			StType *iu = underlying_type(it);
			if (!iu || (iu->kind != TypeKind::UInt && iu->kind != TypeKind::SInt)) {
				log::report(log::Level::Error, bound->loc,
							std::string("slice ") + label + " bound must be an integer");
				return false;
			}
			if (bound->has_const_value && !bound->const_value_is_float) {
				if (iu->kind == TypeKind::SInt && static_cast<int64_t>(bound->int_val) < 0) {
					log::report(log::Level::Error, bound->loc,
								std::string("slice ") + label + " bound cannot be negative");
					return false;
				}
				has_const = true;
				const_val = bound->int_val;
			}
			return true;
		};
		bool has_start = false, has_end = false;
		uint64_t start_val = 0, end_val = 0;
		bool bounds_ok = check_bound(e.rhs, "start", has_start, start_val);
		bounds_ok = check_bound(e.index_low, "end", has_end, end_val) && bounds_ok;
		if (!e.rhs) {
			has_start = true;
			start_val = 0;
		}
		if (bounds_ok && has_start && has_end && start_val > end_val) {
			log::report(log::Level::Error, e.loc, "slice start bound exceeds end bound");
		}
		if (bt && bt->kind == TypeKind::Pointer) {
			if (!e.index_low) {
				log::report(log::Level::Error, e.loc,
							"slicing a pointer requires an explicit end bound");
			}
			e.ty = types_.slice(underlying_type(bt->base), bt->is_mutable, bt->is_volatile);
			return e.ty;
		}
		if (bt && bt->kind == TypeKind::Slice) {
			e.ty = bt;
			return e.ty;
		}
		if (bt && bt->kind == TypeKind::Array) {
			if (bounds_ok && has_start && start_val > bt->array_len) {
				log::report(log::Level::Error, e.rhs ? e.rhs->loc : e.loc,
							"slice start bound is out of bounds for array");
			}
			if (bounds_ok && has_end && end_val > bt->array_len) {
				log::report(log::Level::Error, e.index_low->loc,
							"slice end bound is out of bounds for array");
			}
			e.ty = types_.slice(underlying_type(bt->base), e.lhs->is_lvalue, false);
			return e.ty;
		}
		if (!e.lhs->sema_invalid && !is_poison(bt)) {
			log::report(log::Level::Error, e.loc,
						"cannot slice value of type '" + type_name_for_diag(bt) + "'");
		}
		e.sema_invalid = true;
		e.ty = types_.poison_ty();
		return e.ty;
	}
	case ExprKind::VariantInit: {
		if (!e.lhs) {
			log::report(log::Level::Error, e.loc,
						"enum variant shorthand '." + e.member +
							" { ... }' requires an expected enum type");
			e.ty		   = types_.void_ty();
			e.sema_invalid = true;
			return e.ty;
		}
		Module &init_module = e.binding_module ? *e.binding_module : m;
		std::vector<std::string> path;
		if (!flatten_member_path(*e.lhs, path)) {
			log::report(log::Level::Error, e.loc, "invalid enum type in variant initializer");
			e.ty		   = types_.void_ty();
			e.sema_invalid = true;
			return e.ty;
		}
		Symbol *en_sym = resolve_path(path, init_module);
		if (!en_sym || en_sym->kind != SymKind::Enum || !en_sym->en) {
			log::report(log::Level::Error, e.loc,
						"'" + join_path(path) + "' is not an enum type");
			e.ty		   = types_.void_ty();
			e.sema_invalid = true;
			return e.ty;
		}
		StType *enum_ty = en_sym->type;
		if (!en_sym->en->generic_params.empty()) {
			TypeRef tr;
			tr.loc = e.lhs->loc;
			const auto &gargs = !e.generic_args.empty() ? e.generic_args : e.lhs->generic_args;
			for (const auto &ga : gargs) {
				tr.generic_args.push_back(clone_generic_arg(ga));
			}
			enum_ty = instantiate_generic_type(*en_sym, tr, init_module);
		}
		StType *ut = underlying_type(enum_ty);
		if (!ut || ut->kind != TypeKind::Enum) {
			e.ty		   = types_.void_ty();
			e.sema_invalid = true;
			return e.ty;
		}
		size_t vidx = SIZE_MAX;
		for (size_t vi = 0; vi < ut->field_names.size(); ++vi) {
			if (ut->field_names[vi] == e.member) {
				vidx = vi;
				break;
			}
		}
		if (vidx == SIZE_MAX) {
			log::report(log::Level::Error, e.loc,
						"enum '" + type_name_for_diag(enum_ty) + "' has no variant '" +
							e.member + "'");
			e.ty		   = types_.void_ty();
			e.sema_invalid = true;
			return e.ty;
		}
		const auto &vi = ut->enum_variants[vidx];
		if (vi.payload_kind != EnumPayloadKind::Struct) {
			log::report(log::Level::Error, e.loc,
						"variant '" + e.member + "' of enum '" +
							type_name_for_diag(enum_ty) +
							"' cannot be constructed with struct-style fields");
			e.ty		   = types_.void_ty();
			e.sema_invalid = true;
			return e.ty;
		}
		std::unordered_set<std::string> seen_fields;
		for (size_t ai = 0; ai < e.field_names.size() && ai < e.args.size(); ++ai) {
			const std::string &fn = e.field_names[ai];
			if (!seen_fields.insert(fn).second) {
				log::report(log::Level::Error, e.args[ai]->loc,
							"duplicate field '" + fn + "' in variant initializer");
			}
			size_t fi = SIZE_MAX;
			for (size_t k = 0; k < vi.field_names.size(); ++k) {
				if (vi.field_names[k] == fn) {
					fi = k;
					break;
				}
			}
			if (fi == SIZE_MAX) {
				log::report(log::Level::Error, e.args[ai]->loc,
							"variant '" + e.member + "' has no field '" + fn + "'");
				check_expr(*e.args[ai], ctx, m, scope);
			} else {
				StType *ft	= vi.field_types[fi];
				StType *aty = check_expr_with_expected(*e.args[ai], ctx, m, scope, ft);
				if (ft && aty && !e.args[ai]->sema_invalid && !eq_type(ft, aty)) {
					log::report(log::Level::Error, e.args[ai]->loc,
								"variant field '" + fn + "' type mismatch: expected '" +
									type_name_for_diag(ft) + "', got '" +
									type_name_for_diag(aty) + "'");
				}
			}
		}
		for (const auto &req : vi.field_names) {
			if (!seen_fields.count(req)) {
				log::report(log::Level::Error, e.loc,
							"missing field '" + req + "' in variant '" + e.member + "'");
			}
		}
		e.ty	  = enum_ty;
		e.int_val = ut->enum_values[vidx];
		return e.ty;
	}
	case ExprKind::StructLit: {
		for (auto &a : e.args) {
			check_expr(*a, ctx, m, scope);
		}
		if (!defer_contextual_aggregate_) {
			log::report(log::Level::Error, e.loc,
						"aggregate literal requires an expected struct or union type");
		}
		e.ty = types_.void_ty();
		return e.ty;
	}
	case ExprKind::ArrayLit: {
		StType *elem_ty = nullptr;
		for (auto &a : e.args) {
			StType *at = check_expr(*a, ctx, m, scope);
			if (!elem_ty) {
				elem_ty = at;
			} else if (!eq_type(elem_ty, at)) {
				log::report(log::Level::Error, e.loc,
							"array literal elements have mismatched types");
			}
		}
		if (!elem_ty) {
			elem_ty = types_.void_ty();
		}
		e.ty = types_.array_of(elem_ty, e.args.size());
		return e.ty;
	}
	case ExprKind::AddrOf: {
		StType *ot = check_expr(*e.lhs, ctx, m, scope);
		// The operand of `&` must be an lvalue -- taking the address of a
		// literal, constant, function, or rvalue temporary is not valid.
		// Function names are an explicit exception: `&fn_name` yields the
		// function pointer, already idempotent in codegen.
		if (!e.lhs->is_addressable && e.lhs->kind != ExprKind::Ident) {
			log::report(log::Level::Error, e.lhs->loc, "operand of '&' is not a valid lvalue");
		}
		// Function values are already opaque pointers at the LLVM level and
		// their Stella type carries the complete signature.  Keep `&fn` at
		// that function type so assignment and indirect-call checking retain
		// the variadic bit instead of manufacturing a pointer-to-function
		// type that the rest of the language does not use.
		if (underlying_type(ot) && underlying_type(ot)->kind == TypeKind::Function) {
			e.ty = ot;
			return e.ty;
		}
		e.ty = types_.pointer(ot, e.lhs->is_lvalue, is_volatile_lvalue(*e.lhs));
		return e.ty;
	}
	case ExprKind::Deref: {
		StType *bt = check_expr(*e.lhs, ctx, m, scope);
		bt		   = underlying_type(bt);
		if (bt && bt->kind == TypeKind::Pointer) {
			StType *pointee = underlying_type(bt->base);
			if (!pointee || pointee->kind == TypeKind::Void ||
				pointee->kind == TypeKind::Opaque || pointee->is_opaque) {
				log::report(log::Level::Error, e.loc,
							"cannot dereference pointer to incomplete type '" +
								type_name_for_diag(bt->base) + "'");
				e.sema_invalid = true;
				e.ty		   = types_.poison_ty();
				return e.ty;
			}
			e.ty		= pointee;
			e.is_lvalue = bt->is_mutable;
			e.is_addressable = true;
			return e.ty;
		}
		log::report(log::Level::Error, e.loc, "dereference of non-pointer");
		e.sema_invalid = true;
		e.ty		   = types_.poison_ty();
		return e.ty;
	}
	case ExprKind::Sizeof: {
		StType *target_ty = e.lhs ? check_layout_operand(*e.lhs, ctx, m, scope) : nullptr;
		StType *underlying = target_ty ? underlying_type(target_ty) : nullptr;
		if (!target_ty || underlying->kind == TypeKind::Void || underlying->is_opaque) {
			log::report(log::Level::Error, e.loc,
						"cannot compute @of.size for an incomplete or invalid type");
		}
		e.ty = types_.usize_ty();
		return e.ty;
	}
	case ExprKind::Alignof: {
		StType *target_ty = e.lhs ? check_layout_operand(*e.lhs, ctx, m, scope) : nullptr;
		StType *underlying = target_ty ? underlying_type(target_ty) : nullptr;
		if (!target_ty || underlying->kind == TypeKind::Void || underlying->is_opaque) {
			log::report(log::Level::Error, e.loc,
						"cannot compute @of.align for an incomplete or invalid type");
		}
		e.ty = types_.usize_ty();
		return e.ty;
	}
	case ExprKind::Bitsof: {
		StType *target_ty = e.lhs ? check_layout_operand(*e.lhs, ctx, m, scope) : nullptr;
		StType *underlying = target_ty ? underlying_type(target_ty) : nullptr;
		if (!target_ty || underlying->kind == TypeKind::Void || underlying->is_opaque) {
			log::report(log::Level::Error, e.loc,
						"cannot compute @of.bits for an incomplete or invalid type");
		}
		e.ty = types_.usize_ty();
		return e.ty;
	}
	case ExprKind::Offsetof: {
		StType *target_ty = e.lhs ? underlying_type(check_layout_operand(*e.lhs, ctx, m, scope)) : nullptr;
		if (!target_ty ||
			(target_ty->kind != TypeKind::Struct && target_ty->kind != TypeKind::Union)) {
			log::report(log::Level::Error, e.loc, "@of.offset expects a struct or union type");
			e.ty = types_.usize_ty();
			return e.ty;
		}
		if (target_ty->is_opaque) {
			log::report(log::Level::Error, e.loc,
						"cannot compute @of.offset for an incomplete or invalid type");
			e.ty = types_.usize_ty();
			return e.ty;
		}
		size_t idx = field_index(target_ty, e.offsetof_member);
		if (idx == SIZE_MAX) {
			log::report(log::Level::Error, e.loc,
						"struct '" + target_ty->name + "' has no field '" + e.offsetof_member +
							"'");
		} else {
			const Module &access_mod = e.binding_module ? *e.binding_module : m;
			if (!field_accessible(*target_ty, idx, access_mod)) {
				report_private_field(e.loc, *target_ty, idx);
			}
		}
		e.ty = types_.usize_ty();
		return e.ty;
	}
	case ExprKind::Cast: {
		Module &cast_module = e.binding_module ? *e.binding_module : m;
		// Typecheck the operand first so e.lhs->ty is populated (codegen
		// reads it to pick zext/trunc/bitcast/ptrtoint/etc.) and so any
		// errors inside the operand surface. The cast result type is the
		// declared target.
		e.ty = e.cast_type ? resolve_type(*e.cast_type, cast_module) : types_.void_ty();
		if (e.lhs) {
			if (is_contextual_literal(*e.lhs) && e.ty && underlying_type(e.ty) &&
				underlying_type(e.ty)->kind != TypeKind::Void) {
				check_expr_with_expected(*e.lhs, ctx, m, scope, e.ty);
			} else {
				check_expr(*e.lhs, ctx, m, scope);
			}
		}
		fold_known_expr(e, types_.pointer_width());
		StType *from = e.lhs ? underlying_type(e.lhs->ty) : nullptr;
		StType *to	 = underlying_type(e.ty);
		if (e.is_bitcast) {
			// bitcast: reinterprets the exact same bits as another type.
			// Only allowed when both source and destination are scalars
			// (int/float/pointer/enum) of identical bit size.
			int sb = scalar_size_bits(from, types_.pointer_width());
			int db = scalar_size_bits(to, types_.pointer_width());
			if (sb == 0 || db == 0) {
				log::report(log::Level::Error, e.loc,
							"bitcast requires scalar operand and target types");
			} else if (sb != db) {
				log::report(log::Level::Error, e.loc,
							"bitcast requires source and target of equal size (" +
								std::to_string(sb) + " bits vs " + std::to_string(db) + " bits)");
			}
		} else {
			// cast: normal value/type conversion (int widening/narrowing,
			// bool<->int, int<->float, unit-enum<->int, ptr<->ptr, ptr<->int).
			// Aggregates, tagged enums, void, bool<->ptr/float/enum, and
			// discarding `volatile` are rejected.
			auto is_int_scalar = [](StType *t) {
				return t && (t->kind == TypeKind::UInt || t->kind == TypeKind::SInt);
			};
			if (!is_cast_scalar(from) || !is_cast_scalar(to)) {
				log::report(log::Level::Error, e.loc,
							"operand and target must be scalar (bool/int/float/pointer/unit-enum) types");
			} else if (to->kind == TypeKind::Void || (from && from->kind == TypeKind::Void)) {
				log::report(log::Level::Error, e.loc, "cannot cast to/from void");
			} else if ((from->kind == TypeKind::Bool || to->kind == TypeKind::Bool) &&
					   !(from->kind == TypeKind::Bool ? (to->kind == TypeKind::Bool || is_int_scalar(to))
													  : is_int_scalar(from))) {
				log::report(log::Level::Error, e.loc,
							"cannot cast between '" + type_name_for_diag(from) + "' and '" +
								type_name_for_diag(to) + "'");
			} else if ((from->kind == TypeKind::Pointer && to->kind == TypeKind::Float) ||
					   (from->kind == TypeKind::Float && to->kind == TypeKind::Pointer)) {
				log::report(log::Level::Error, e.loc,
							"cannot cast between '" + type_name_for_diag(from) + "' and '" +
								type_name_for_diag(to) + "'");
			} else if (from->kind == TypeKind::Pointer && to->kind == TypeKind::Pointer &&
					   from->is_volatile && !to->is_volatile) {
				log::report(log::Level::Error, e.loc,
							"cast cannot discard 'volatile' qualifier from '" +
								type_name_for_diag(from) + "'");
			} else if (to->kind == TypeKind::Float && e.lhs &&
					   float_leaves_fit(*e.lhs, to->width) == FitResult::NoFit) {
				log::report(log::Level::Error, e.lhs->loc,
							"floating-point literal value is not exactly representable in '" +
								type_name_for_diag(to) + "'; append '~' to agree to lossy rounding");
			}
		}
		return e.ty;
	}
	case ExprKind::VaStart:
	case ExprKind::VaCopy:
	case ExprKind::VaArg:
	case ExprKind::VaEnd: {
		auto operand = [&](Expr &op, VarDeclInfo *&local, bool &parameter, bool mutates) -> bool {
			local					  = nullptr;
			parameter				  = false;
			bool saved				  = checking_va_list_operand_;
			checking_va_list_operand_ = true;
			StType *ot				  = check_expr(op, ctx, m, scope);
			checking_va_list_operand_ = saved;
			StType *u				  = underlying_type(ot);
			// A pointer borrows an initialized cursor. Mutating builtins require
			// mutable pointee storage, independently of the pointer binding.
			if (u && u->kind == TypeKind::Pointer && underlying_type(u->base) &&
				underlying_type(u->base)->kind == TypeKind::VaList) {
				if (mutates && !u->is_mutable) {
					log::report(log::Level::Error, op.loc,
						"va builtin requires a mutable pointer to @va.list");
					return false;
				}
				parameter = true;
				return true;
			}
			if (!u || u->kind != TypeKind::VaList) {
				log::report(log::Level::Error, op.loc,
							"va builtin operand must have type @va.list");
				return false;
			}
			if (op.kind != ExprKind::Ident || op.path.size() != 1) {
				log::report(log::Level::Error, op.loc,
							"va builtin operand must be an @va.list variable or parameter");
				return false;
			}
			Symbol *sym = scope.lookup(op.path[0]);
			if (!sym || (sym->kind != SymKind::Var && sym->kind != SymKind::Param) ||
				(sym->kind == SymKind::Var && !sym->local_decl)) {
				log::report(log::Level::Error, op.loc,
							"va builtin operand must be an @va.list variable or parameter");
				return false;
			}
			if (mutates && !op.is_lvalue) {
				const std::string builtin = e.kind == ExprKind::VaStart ? "@va.start"
											: e.kind == ExprKind::VaArg ? "@va.arg"
											: e.kind == ExprKind::VaEnd ? "@va.end"
																		: "@va.copy";
				const std::string binding = sym->kind == SymKind::Param ? "parameter" : "binding";
				log::error(op.loc, builtin + " cannot mutate immutable " + binding + " `" +
									   sym->simple + "`")
					.code("E_IMMUTABLE_ASSIGNMENT")
					.phase("sema")
					.note(sym->loc, binding + " declared here")
					.fixit(sym->loc, "mut " + sym->simple,
						   "consider making the " + binding + " mutable: `mut " + sym->simple + "`")
					.emit();
				return false;
			}
			if (sym->kind == SymKind::Param) {
				op.va_list_param = parameter = true;
			} else {
				op.va_list_decl = local = sym->local_decl;
			}
			return true;
		};

		if (e.kind == ExprKind::VaStart && (!ctx.is_variadic || active_fn_ != &ctx)) {
			log::report(log::Level::Error, e.loc,
						"@va.start is only valid inside a variadic function");
		} else if (e.kind == ExprKind::VaStart && ctx.abi != AbiKind::C) {
			log::report(log::Level::Error, e.loc,
						"@va.start is only valid inside #[abi(\"C\")] functions");
		}
		if (e.kind == ExprKind::VaCopy) {
			if (e.args.size() >= 1 && e.args[0]) {
				VarDeclInfo *dst	   = nullptr;
				bool		 dst_param = false;
				bool		 dst_ok	   = operand(*e.args[0], dst, dst_param, true);
				if (e.args.size() >= 2 && e.args[1]) {
					VarDeclInfo *src	   = nullptr;
					bool		 src_param = false;
					bool		 src_ok	   = operand(*e.args[1], src, src_param, false);
					if (dst_param) {
						log::report(log::Level::Error, e.args[0]->loc,
									"@va.copy destination must be a local @va.list variable");
					} else if (dst_ok &&
							   va_states_[dst] != static_cast<unsigned>(VaState::Uninitialized)) {
						log::report(log::Level::Error, e.args[0]->loc,
									"@va.copy destination must be uninitialized");
					}
					bool src_initialized =
						src_param ||
						(src_ok && va_states_[src] == static_cast<unsigned>(VaState::Initialized));
					if (src_ok && !src_initialized) {
						log::report(log::Level::Error, e.args[1]->loc,
									"@va.copy source must be initialized and not ended");
					}
					if (dst_ok && !dst_param && src_ok &&
						va_states_[dst] == static_cast<unsigned>(VaState::Uninitialized) &&
						src_initialized) {
						va_states_[dst] = static_cast<unsigned>(VaState::Initialized);
					}
				}
			}
			e.ty = types_.void_ty();
			return e.ty;
		}

		if (e.args.empty() || !e.args[0]) {
			log::report(log::Level::Error, e.loc, "va builtin requires a @va.list operand");
			e.ty = types_.void_ty();
			return e.ty;
		}
		VarDeclInfo *list		= nullptr;
		bool		 list_param = false;
		bool		 list_ok	= operand(*e.args[0], list, list_param, true);
		unsigned	 state		= list && va_states_.count(list) ? va_states_[list] : 0;
		if (e.kind == ExprKind::VaStart) {
			if (list_param) {
				log::report(log::Level::Error, e.args[0]->loc,
							"@va.start destination must be a local @va.list variable");
			} else if (list && state != static_cast<unsigned>(VaState::Uninitialized)) {
				log::report(log::Level::Error, e.loc,
							"@va.start destination is already initialized");
			} else if (list_ok && list && ctx.is_variadic && active_fn_ == &ctx) {
				va_states_[list] = static_cast<unsigned>(VaState::Initialized);
			}
			e.ty = types_.void_ty();
			return e.ty;
		}
		if (e.kind == ExprKind::VaArg) {
			if (list_ok && !list_param && list &&
				state != static_cast<unsigned>(VaState::Initialized)) {
				log::report(log::Level::Error, e.loc,
							(state & static_cast<unsigned>(VaState::Ended))
								? "@va.arg used after @va.end"
								: "@va.arg used before @va.start");
			}
			StType *target = e.va_arg_ty ? resolve_type(*e.va_arg_ty, m) : types_.void_ty();
			if (!supported_va_type(target)) {
				log::report(log::Level::Error, e.loc,
							"@va.arg does not support type '" + type_name_for_diag(target) +
								"' (expected scalar or pointer)");
				e.ty = types_.void_ty();
			} else {
				e.ty = target;
			}
			return e.ty;
		}
		if (list_ok && !list_param && list &&
			state != static_cast<unsigned>(VaState::Initialized)) {
			log::report(log::Level::Error, e.loc,
						(state & static_cast<unsigned>(VaState::Ended))
							? "@va.end called more than once"
							: "@va.end used before @va.start");
		} else if (list) {
			va_states_[list] = static_cast<unsigned>(VaState::Ended);
		}
		e.ty = types_.void_ty();
		return e.ty;
	}
	}
	return types_.void_ty();
}

StType *Sema::check_indirect_call(Expr &e, FnDecl &ctx, Module &m, ScopeStack &scope) {
	StType *ct = check_expr(*e.lhs, ctx, m, scope);
	(void)ct;
	// Indirect call: the callee value has a function (or pointer-to-
	// function) type -- e.g. `fp(args)` where fp is a variable/parameter
	// of type `fn(...) ...`, or the result of `&fn_name`.
	StType *lht = underlying_type(e.lhs->ty);
	StType *ft	= lht;
	if (ft && ft->kind == TypeKind::Pointer) {
		ft = underlying_type(ft->base);
	}
	if (!ft || ft->kind != TypeKind::Function) {
		log::report(log::Level::Error, e.loc, "call of non-function value");
		for (auto &a : e.args) {
			if (!is_contextual_literal(*a)) {
				check_expr(*a, ctx, m, scope);
			}
		}
		e.sema_invalid = true;
		e.ty		   = types_.void_ty();
		return e.ty;
	}
	size_t n_fixed = ft->fn_params.size();
	for (size_t i = 0; i < e.args.size(); ++i) {
		if (i < n_fixed) {
			check_expr_with_expected(*e.args[i], ctx, m, scope, ft->fn_params[i]);
		} else {
			check_expr(*e.args[i], ctx, m, scope);
		}
		if (i < n_fixed) {
			auto fv = is_contextual_literal(*e.args[i])
						  ? value_fits(ft->fn_params[i], *e.args[i])
						  : FitResult::Unknown;
			if (fv == FitResult::NoFit) {
				log::report(log::Level::Error, e.args[i]->loc,
							"argument " + std::to_string(i + 1) +
								" of indirect call: value not representable in '" +
								type_name_for_diag(ft->fn_params[i]) + "'");
			} else if (fv == FitResult::Unknown &&
					   !arg_type_compat(ft->fn_params[i], e.args[i]->ty)) {
				StType *dst = underlying_type(ft->fn_params[i]);
				StType *src = underlying_type(e.args[i]->ty);
				std::string msg;
				if (dst && src && dst->kind == TypeKind::Pointer && src->kind == TypeKind::Pointer &&
					eq_type(dst->base, src->base)) {
					if (!src->is_mutable && dst->is_mutable) {
						msg = "argument " + std::to_string(i + 1) +
							  " of indirect call: cannot convert from '" +
							  type_name_for_diag(e.args[i]->ty) + "' to '" +
							  type_name_for_diag(ft->fn_params[i]) +
							  "' (cannot convert immutable pointer to mutable pointer)";
					} else if (src->is_volatile && !dst->is_volatile) {
						msg = "argument " + std::to_string(i + 1) +
							  " of indirect call: cannot convert from '" +
							  type_name_for_diag(e.args[i]->ty) + "' to '" +
							  type_name_for_diag(ft->fn_params[i]) +
							  "' (discards 'volatile' qualifier)";
					} else {
						msg = "argument " + std::to_string(i + 1) +
							  " of indirect call: type mismatch: expected '" +
							  type_name_for_diag(ft->fn_params[i]) + "', got '" +
							  type_name_for_diag(e.args[i]->ty) + "'";
					}
				} else {
					msg = "argument " + std::to_string(i + 1) +
						  " of indirect call: type mismatch: expected '" +
						  type_name_for_diag(ft->fn_params[i]) + "', got '" +
						  type_name_for_diag(e.args[i]->ty) + "'";
				}
				log::error(e.args[i]->loc, msg).code(log::DiagCode::InvalidCall).emit();
			}
		}
	}
	if (!ft->fn_variadic) {
		if (e.args.size() != n_fixed) {
			log::report(log::Level::Error, e.loc,
						"indirect call expects " + std::to_string(n_fixed) +
							" argument(s), got " + std::to_string(e.args.size()));
		}
	} else if (e.args.size() < n_fixed) {
		log::report(log::Level::Error, e.loc,
					"indirect call expects at least " + std::to_string(n_fixed) +
						" argument(s), got " + std::to_string(e.args.size()));
	}
	if (ft->fn_variadic) {
		for (size_t i = n_fixed; i < e.args.size(); ++i) {
			if (!supported_va_type(e.args[i]->ty)) {
				log::report(log::Level::Error, e.args[i]->loc,
							"variadic argument " + std::to_string(i + 1) +
								" has unsupported type '" + type_name_for_diag(e.args[i]->ty) +
								"' (expected scalar or pointer)");
			}
		}
	}
	e.ty = ft->fn_ret;
	return e.ty;
}

StType *Sema::resolve_ufcs_call(Expr &e, FnDecl &ctx, Module &m, ScopeStack &scope) {
	Module &call_module = e.binding_module ? *e.binding_module : m;

	// 1. Evaluate receiver
	StType *saved_expected_res = expected_generic_result_;
	expected_generic_result_   = nullptr;
	StType *rx_type			   = check_expr(*e.lhs->lhs, ctx, m, scope);
	expected_generic_result_   = saved_expected_res;
	if (e.lhs->lhs->sema_invalid || is_poison(rx_type)) {
		e.sema_invalid = true;
		e.ty		   = types_.poison_ty();
		return e.ty;
	}

	// 2. Priority 1 (Direct Fields)
	StType *bt = underlying_type(rx_type);
	while (bt && bt->kind == TypeKind::Pointer) {
		bt = underlying_type(bt->base);
	}
	bool has_direct_field = false;
	bool private_field_hidden = false;
	size_t private_field_idx = SIZE_MAX;
	if (bt && (bt->kind == TypeKind::Struct || bt->kind == TypeKind::Union)) {
		for (size_t i = 0; i < bt->field_names.size(); ++i) {
			if (bt->field_names[i] == e.lhs->member) {
				const Module &lhs_mod = e.lhs->binding_module ? *e.lhs->binding_module : call_module;
				if (field_accessible(*bt, i, lhs_mod)) {
					has_direct_field = true;
				} else {
					private_field_hidden = true;
					private_field_idx = i;
				}
				break;
			}
		}
	}
	if (has_direct_field) {
		check_expr(*e.lhs, ctx, m, scope);
		return check_indirect_call(e, ctx, m, scope);
	}

	// 3. Priority 2 (UFCS Free Function)
	std::vector<FnDecl *> raw_candidates;

	// Type-scoped inherent impl candidates (e.g. Vec2.length for Vec2)
	if (bt && (bt->kind == TypeKind::Struct || bt->kind == TypeKind::Union ||
			   bt->kind == TypeKind::Enum)) {
		if (bt->decl_module && !bt->scope_path.empty()) {
			auto tbl_it = module_syms_.find(source_key(*bt->decl_module));
			if (tbl_it != module_syms_.end()) {
				std::string method_key = join_path(bt->scope_path) + "." + e.lhs->member;
				auto sym_it = tbl_it->second.find(method_key);
				if (sym_it != tbl_it->second.end() && sym_it->second.kind == SymKind::Fn) {
					for (FnDecl *cand : sym_it->second.overloads) {
						if (cand &&
							std::find(raw_candidates.begin(), raw_candidates.end(), cand) ==
								raw_candidates.end()) {
							raw_candidates.push_back(cand);
						}
					}
				}
			}
		}
		if (!bt->name.empty()) {
			std::vector<std::string> struct_segs = demangle(bt->name);
			struct_segs.push_back(e.lhs->member);
			Symbol *struct_sym = resolve_path(struct_segs, call_module);
			Symbol *raw_struct_sym =
				struct_sym ? struct_sym : resolve_path_any(struct_segs, call_module);
			if (raw_struct_sym && raw_struct_sym->kind == SymKind::Fn) {
				for (FnDecl *cand : raw_struct_sym->overloads) {
					if (cand && std::find(raw_candidates.begin(), raw_candidates.end(), cand) ==
									raw_candidates.end()) {
						raw_candidates.push_back(cand);
					}
				}
			}
		}
	}

	// Module-visible free function candidates
	Symbol *free_sym	 = resolve_path({e.lhs->member}, call_module);
	Symbol *raw_free_sym = free_sym ? free_sym : resolve_path_any({e.lhs->member}, call_module);
	if (raw_free_sym && raw_free_sym->kind == SymKind::Fn) {
		for (FnDecl *cand : raw_free_sym->overloads) {
			if (cand && std::find(raw_candidates.begin(), raw_candidates.end(), cand) ==
							raw_candidates.end()) {
				raw_candidates.push_back(cand);
			}
		}
	}

	// Trait-scoped candidates implemented for the receiver type
	if (bt) {
		std::vector<FnDecl *> trait_cands = find_trait_methods_for_type(bt, e.lhs->member);
		for (FnDecl *cand : trait_cands) {
			if (cand && std::find(raw_candidates.begin(), raw_candidates.end(), cand) ==
							raw_candidates.end()) {
				raw_candidates.push_back(cand);
			}
		}
	}

	if (raw_candidates.empty()) {
		if (private_field_hidden) {
			report_private_field(e.lhs->loc, *bt, private_field_idx);
			e.sema_invalid = true;
			e.ty		   = types_.poison_ty();
			return e.ty;
		}
		if (bt && bt->kind == TypeKind::Slice) {
			if (e.member == "ptr") {
				e.ty = types_.pointer(bt->base, bt->is_mutable, bt->is_volatile);
				return e.ty;
			}
			if (e.member == "len") {
				e.ty = types_.usize_ty();
				return e.ty;
			}
			log::error(e.loc, "no field '" + e.member + "' on slice")
				.code("E_UNKNOWN_MEMBER")
				.phase("semantic")
				.emit();
			e.sema_invalid = true;
			e.ty = types_.poison_ty();
			return e.ty;
		}
		if (bt && bt->kind == TypeKind::Array) {
			if (e.member == "len") {
				e.ty = types_.usize_ty();
				e.has_const_value = true;
				e.int_val = bt->array_len;
				return e.ty;
			}
			log::error(e.loc, "no field '" + e.member + "' on array")
				.code("E_UNKNOWN_MEMBER")
				.phase("semantic")
				.emit();
			e.sema_invalid = true;
			e.ty = types_.poison_ty();
			return e.ty;
		}
		if (bt && (bt->kind == TypeKind::Struct || bt->kind == TypeKind::Union)) {
			auto err = log::error(e.lhs->loc, "no field or method '" + e.lhs->member +
												  "' found for type '" +
												  type_name_for_diag(rx_type) + "'")
						   .code(log::DiagCode::InvalidCall);
			err.emit();
		} else {
			auto err = log::error(e.lhs->loc, "no function '" + e.lhs->member +
												  "' found for type '" +
												  type_name_for_diag(rx_type) + "'")
						   .code(log::DiagCode::InvalidCall);
			err.emit();
		}
		for (auto &arg : e.args) {
			if (!is_contextual_literal(*arg)) {
				check_expr(*arg, ctx, m, scope);
			}
		}
		e.sema_invalid = true;
		e.ty		   = types_.void_ty();
		return e.ty;
	}

	bool saved_defer			= defer_contextual_aggregate_;
	defer_contextual_aggregate_ = true;
	expected_generic_result_	= nullptr;
	for (auto &arg : e.args) {
		if (!is_contextual_literal(*arg)) {
			check_expr(*arg, ctx, m, scope);
		}
	}
	defer_contextual_aggregate_ = saved_defer;
	expected_generic_result_	= saved_expected_res;
	for (auto &arg : e.args) {
		if (arg && (arg->sema_invalid || is_poison(arg->ty))) {
			e.sema_invalid = true;
			e.ty		   = types_.poison_ty();
			return e.ty;
		}
	}

	std::vector<FnDecl *>	 candidates;
	std::vector<std::string> generic_constraint_rejections;

	for (FnDecl *cand : raw_candidates) {
		if (!cand) continue;
		bool cross = cand->decl_module != &call_module &&
					 !resolver_.same_source_file(*cand->decl_module, call_module);
		if (cross && !cand->is_pub) {
			continue;
		}

		if (!cand->generic_params.empty()) {
			std::vector<std::unique_ptr<Expr>> synth_args;
			synth_args.reserve(1 + e.args.size());

			auto r			  = std::make_unique<Expr>();
			r->loc			  = e.lhs->lhs->loc;
			r->is_lvalue	  = e.lhs->lhs->is_lvalue;
			r->is_addressable = e.lhs->lhs->is_addressable;

			if (!cand->params.empty() && cand->params[0].ty &&
				cand->params[0].ty->kind == TypeRef::Pointer &&
				underlying_type(rx_type)->kind != TypeKind::Pointer) {
				r->ty = types_.pointer(rx_type, r->is_lvalue, false);
			} else if (!cand->params.empty() && cand->params[0].ty &&
					   cand->params[0].ty->kind != TypeRef::Pointer &&
					   underlying_type(rx_type)->kind == TypeKind::Pointer) {
				r->ty = underlying_type(rx_type)->base;
			} else {
				r->ty = rx_type;
			}
			synth_args.push_back(std::move(r));

			for (const auto &arg : e.args) {
				auto a_clone			= std::make_unique<Expr>();
				a_clone->loc			= arg->loc;
				a_clone->ty				= arg->ty;
				a_clone->kind			= arg->kind;
				a_clone->int_val		= arg->int_val;
				a_clone->flt_val		= arg->flt_val;
				a_clone->str_val		= arg->str_val;
				a_clone->has_const_value = arg->has_const_value;
				synth_args.push_back(std::move(a_clone));
			}

			if (FnDecl *instance = instantiate_generic_fn(*cand, e, synth_args, call_module, ctx)) {
				candidates.push_back(instance);
			} else if (!last_generic_constraint_error_.empty()) {
				generic_constraint_rejections.push_back(last_generic_constraint_error_);
			}
		} else {
			candidates.push_back(cand);
		}
	}

	enum class ReceiverAdaptation { Direct, AutoRef, AutoDeref };
	struct UfcsViableCand {
		FnDecl			  *fn		  = nullptr;
		int				   penalty	  = 0;
		ReceiverAdaptation adaptation = ReceiverAdaptation::Direct;
	};

	std::vector<UfcsViableCand> viables;

	for (FnDecl *cand : candidates) {
		size_t n_fixed = cand->params.size();
		if (n_fixed == 0) continue;
		if (!cand->is_variadic && (1 + e.args.size()) != n_fixed) continue;
		if (cand->is_variadic && (1 + e.args.size()) < n_fixed) continue;

		StType *pty = cand->params[0].resolved;
		if (!pty && cand->params[0].ty) {
			pty = resolve_type(*cand->params[0].ty, call_module);
		}
		if (!pty) continue;

		ReceiverAdaptation adapt  = ReceiverAdaptation::Direct;
		int				   rx_pen = 0;
		bool			   rx_ok  = false;

		if (eq_type(pty, rx_type)) {
			adapt  = ReceiverAdaptation::Direct;
			rx_pen = 0;
			rx_ok  = true;
		} else if (arg_type_compat(pty, rx_type)) {
			adapt  = ReceiverAdaptation::Direct;
			rx_pen = 1;
			rx_ok  = true;
		} else if (underlying_type(pty) && underlying_type(pty)->kind == TypeKind::Pointer &&
				   eq_type(underlying_type(pty)->base, rx_type)) {
			StType *dst_ptr = underlying_type(pty);
			if (dst_ptr->is_mutable) {
				if (e.lhs->lhs->is_addressable && e.lhs->lhs->is_lvalue) {
					adapt  = ReceiverAdaptation::AutoRef;
					rx_pen = 1;
					rx_ok  = true;
				}
			} else {
				if (e.lhs->lhs->is_addressable) {
					adapt  = ReceiverAdaptation::AutoRef;
					rx_pen = 1;
					rx_ok  = true;
				}
			}
		} else if (underlying_type(rx_type) &&
				   underlying_type(rx_type)->kind == TypeKind::Pointer &&
				   eq_type(underlying_type(rx_type)->base, pty)) {
			adapt  = ReceiverAdaptation::AutoDeref;
			rx_pen = 2;
			rx_ok  = true;
		}

		if (!rx_ok) continue;

		int	 args_pen = 0;
		bool args_ok  = true;
		for (size_t i = 0; i < e.args.size(); ++i) {
			if (i + 1 < n_fixed) {
				StType *arg_pty = cand->params[i + 1].resolved;
				if (!arg_pty && cand->params[i + 1].ty) {
					arg_pty = resolve_type(*cand->params[i + 1].ty, call_module);
				}
				if (!arg_pty) {
					args_ok = false;
					break;
				}

				StType *aty = e.args[i]->ty;
				if (is_contextual_enum_variant_expr(*e.args[i])) {
					uint64_t val = 0;
					if (!enum_member_value(arg_pty, contextual_enum_variant_name(*e.args[i]), val)) {
						args_ok = false;
						break;
					}
					continue;
				}
				if (eq_type(arg_pty, aty)) {
					args_pen += 0;
				} else {
					bool is_arg_int	  = types_.is_int_like(aty);
					bool is_param_int = types_.is_int_like(arg_pty);
					bool is_arg_float =
						(underlying_type(aty) && underlying_type(aty)->kind == TypeKind::Float);
					bool is_param_float =
						(underlying_type(arg_pty) &&
						 underlying_type(arg_pty)->kind == TypeKind::Float);

					auto fv = is_contextual_literal(*e.args[i])
								  ? value_fits(arg_pty, *e.args[i])
								  : FitResult::Unknown;
					if (fv == FitResult::Fit) {
						if ((is_arg_int && is_param_float) || (is_arg_float && is_param_int)) {
							args_pen += 3;
						} else {
							args_pen += 1;
						}
					} else if (fv == FitResult::NoFit) {
						args_ok = false;
						break;
					} else if (arg_type_compat(arg_pty, aty)) {
						if ((is_arg_int && is_param_float) || (is_arg_float && is_param_int)) {
							args_pen += 4;
						} else {
							args_pen += 2;
						}
					} else {
						args_ok = false;
						break;
					}
				}
			}
		}

		if (!args_ok) continue;

		int total_pen = rx_pen + args_pen;
		if (cand->is_generic_instance) {
			bool constrained = false;
			if (cand->generic_template) {
				for (const auto &gp : cand->generic_template->generic_params) {
					if (!gp.type_set.empty() || !gp.trait_bounds.empty()) {
						constrained = true;
						break;
					}
				}
			}
			total_pen += constrained ? 1 : 2;
		}
		viables.push_back({cand, total_pen, adapt});
	}

	if (viables.empty()) {
		if (raw_candidates.empty() && private_field_hidden) {
			report_private_field(e.lhs->loc, *bt, private_field_idx);
			e.sema_invalid = true;
			e.ty		   = types_.poison_ty();
			return e.ty;
		}
		bool all_private = !raw_candidates.empty();
		for (FnDecl *c : raw_candidates) {
			bool cross = c->decl_module != &call_module &&
						 !resolver_.same_source_file(*c->decl_module, call_module);
			if (!cross || c->is_pub) {
				all_private = false;
				break;
			}
		}
		if (all_private) {
			log::report(log::Level::Error, e.loc,
						"function '" + e.lhs->member +
							"' is private; add 'pub' to call it from another module");
		} else if (!generic_constraint_rejections.empty()) {
			log::report(log::Level::Error, e.loc, generic_constraint_rejections.front(),
						"E_GENERIC_CONSTRAINT", "sema");
		} else {
			auto err = log::error(e.loc,
								  "no matching function for UFCS call to '" + e.lhs->member +
									  "' with receiver of type '" + type_name_for_diag(rx_type) +
									  "' and " + std::to_string(e.args.size()) + " argument(s)")
						   .code(log::DiagCode::InvalidCall);
			std::unordered_set<const FnDecl *> seen;
			const auto &cands_to_report = candidates.empty() ? raw_candidates : candidates;
			for (FnDecl *cand : cands_to_report) {
				if (!cand) continue;
				FnDecl *orig = (cand->is_generic_instance && cand->generic_template)
								   ? cand->generic_template
								   : cand;
				if (!seen.insert(orig).second) continue;
				bool cross = cand->decl_module != &call_module &&
							 !resolver_.same_source_file(*cand->decl_module, call_module);
				if (cross && !cand->is_pub) continue;

				std::string note;
				if (cand->params.empty()) {
					note = "candidate takes 0 arguments, but receiver was provided";
				} else if (!cand->is_variadic && (1 + e.args.size()) != cand->params.size()) {
					note = "expects " + std::to_string(cand->params.size()) +
						   " argument(s), but " + std::to_string(1 + e.args.size()) +
						   " were supplied (including receiver)";
				} else {
					StType *pty = cand->params[0].resolved;
					if (!pty && cand->params[0].ty &&
						(cand->generic_params.empty() || cand->is_generic_instance)) {
						pty = resolve_type(*cand->params[0].ty, call_module);
					}
					if (pty && underlying_type(pty)->kind == TypeKind::Pointer &&
						eq_type(underlying_type(pty)->base, rx_type)) {
						if (!e.lhs->lhs->is_addressable) {
							note = "receiver is an rvalue temporary and cannot be auto-referenced "
								   "to pointer parameter '" +
								   type_name_for_diag(pty) + "'";
						} else if (underlying_type(pty)->is_mutable && !e.lhs->lhs->is_lvalue) {
							note = "receiver is immutable and cannot be passed to mutable pointer "
								   "parameter '" +
								   type_name_for_diag(pty) + "'";
						}
					} else if (pty && !eq_type(pty, rx_type) && !arg_type_compat(pty, rx_type)) {
						note = "receiver type mismatch: expected '" + type_name_for_diag(pty) +
							   "', got '" + type_name_for_diag(rx_type) + "'";
					}
				}
				if (note.empty()) {
					Expr synth_call;
					synth_call.loc = e.loc;
					auto r0		   = std::make_unique<Expr>();
					r0->loc		   = e.lhs->lhs->loc;
					r0->ty		   = rx_type;
					synth_call.args.push_back(std::move(r0));
					for (const auto &arg : e.args) {
						auto a	= std::make_unique<Expr>();
						a->loc	= arg->loc;
						a->ty	= arg->ty;
						a->kind = arg->kind;
						synth_call.args.push_back(std::move(a));
					}
					note = candidate_rejection_reason(*cand, synth_call, call_module, cross);
				}
				err.note(orig->loc, "candidate function not viable: " + note);
			}
			err.emit();
		}
		e.sema_invalid = true;
		e.ty		   = types_.void_ty();
		return e.ty;
	}

	int min_pen = viables[0].penalty;
	for (const auto &v : viables) {
		if (v.penalty < min_pen) {
			min_pen = v.penalty;
		}
	}

	std::vector<UfcsViableCand> best_cands;
	for (const auto &v : viables) {
		if (v.penalty == min_pen) {
			best_cands.push_back(v);
		}
	}

	if (best_cands.size() > 1) {
		auto err =
			log::error(e.loc,
					   "ambiguous call to overloaded function '" + e.lhs->member + "' via UFCS")
				.code(log::DiagCode::InvalidCall);
		err.emit();
		e.sema_invalid = true;
		e.ty		   = types_.void_ty();
		return e.ty;
	}

	FnDecl			  *selected		  = best_cands[0].fn;
	ReceiverAdaptation selected_adapt = best_cands[0].adaptation;

	std::vector<std::unique_ptr<Expr>> new_args;
	new_args.reserve(1 + e.args.size());

	std::unique_ptr<Expr> arg0;
	if (selected_adapt == ReceiverAdaptation::Direct) {
		arg0 = std::move(e.lhs->lhs);
	} else if (selected_adapt == ReceiverAdaptation::AutoRef) {
		auto addr			 = std::make_unique<Expr>();
		addr->kind			 = ExprKind::AddrOf;
		addr->loc			 = e.lhs->lhs->loc;
		addr->lhs			 = std::move(e.lhs->lhs);
		addr->ty			 = selected->params[0].resolved;
		addr->is_lvalue		 = false;
		addr->is_addressable = false;
		arg0				 = std::move(addr);
	} else if (selected_adapt == ReceiverAdaptation::AutoDeref) {
		auto deref			  = std::make_unique<Expr>();
		deref->kind			  = ExprKind::Deref;
		deref->loc			  = e.lhs->lhs->loc;
		deref->lhs			  = std::move(e.lhs->lhs);
		deref->ty			  = selected->params[0].resolved;
		deref->is_lvalue	  = rx_type->is_mutable;
		deref->is_addressable = true;
		arg0				  = std::move(deref);
	}

	new_args.push_back(std::move(arg0));
	for (auto &arg : e.args) {
		new_args.push_back(std::move(arg));
	}
	e.args = std::move(new_args);

	e.lhs->kind = ExprKind::Ident;
	e.lhs->path = selected->full_path;
	e.lhs->ty	= selected->fn_sig;
	e.lhs->member.clear();
	e.lhs->lhs.reset();

	e.resolved_fn  = selected;
	selected->used = true;
	if (selected->decl_module && selected->decl_module != &call_module) {
		mark_import_use(selected->full_path, call_module);
	}

	for (size_t i = 0; i < selected->params.size() && i < e.args.size(); ++i) {
		if (is_contextual_literal(*e.args[i])) {
			check_expr_with_expected(*e.args[i], ctx, m, scope, selected->params[i].resolved);
		}
	}

	if (selected->is_macro) {
		if (!selected->macro_returns_value && allowed_macro_call_ == &e) {
			e.lhs->ty = selected->fn_sig;
			e.ty	  = types_.void_ty();
			return e.ty;
		}
		if (!selected->macro_returns_value) {
			log::report(log::Level::Error, e.loc,
						"macro has no value; it can only be used as a standalone statement");
			e.sema_invalid = true;
			e.ty		   = types_.void_ty();
			return e.ty;
		}
		auto expanded = expand_macro_call(e, ctx, m, scope);
		if (!expanded) {
			e.sema_invalid = true;
			e.ty		   = types_.void_ty();
			return e.ty;
		}
		StType *saved_block_expected = expected_block_expr_;
		auto	replacement			 = std::make_unique<Expr>();
		replacement->kind			 = ExprKind::Block;
		replacement->loc			 = e.loc;
		replacement->block			 = std::move(expanded);
		if (!expected_block_expr_ && selected->fn_sig && selected->fn_sig->fn_ret &&
			underlying_type(selected->fn_sig->fn_ret)->kind != TypeKind::Void) {
			expected_block_expr_ = selected->fn_sig->fn_ret;
		}
		e						 = std::move(*replacement);
		FnDecl *previous_generic = generic_context_;
		if (selected->is_generic_instance) {
			generic_context_ = selected;
		}
		macro_control_bases_.push_back({loop_stack_.size(), switch_stack_.size()});
		++macro_expansion_depth_;
		StType *result = check_expr(e, ctx, m, scope);
		--macro_expansion_depth_;
		macro_control_bases_.pop_back();
		generic_context_	 = previous_generic;
		expected_block_expr_ = saved_block_expected;
		return result;
	}

	if (selected->is_variadic && !selected->is_macro) {
		for (size_t i = selected->params.size(); i < e.args.size(); ++i) {
			if (!e.args[i]->sema_invalid && !supported_va_type(e.args[i]->ty)) {
				log::report(log::Level::Error, e.args[i]->loc,
							"variadic argument " + std::to_string(i + 1) +
								" has unsupported type '" + type_name_for_diag(e.args[i]->ty) +
								"' (expected scalar or pointer)");
			}
			if (selected->is_extern) {
				StType *aty = underlying_type(e.args[i]->ty);
				if (aty && aty->kind == TypeKind::Enum) {
					aty = underlying_type(aty->base);
				}
				if (aty && aty->kind == TypeKind::Float && aty->width < 64) {
					log::report(log::Level::Error, e.args[i]->loc,
								"external variadic argument " + std::to_string(i + 1) +
									" of type '" + type_name_for_diag(aty) +
									"' requires an explicit cast to f64");
				} else if (aty && aty->kind == TypeKind::Bool) {
					log::report(log::Level::Error, e.args[i]->loc,
								"external variadic argument " + std::to_string(i + 1) +
									" requires an explicit cast to a 32-bit integer type");
				} else if (aty && (aty->kind == TypeKind::UInt || aty->kind == TypeKind::SInt) &&
						   aty->width > 0 && aty->width < 32) {
					log::report(log::Level::Error, e.args[i]->loc,
								"external variadic argument " + std::to_string(i + 1) +
									" of type '" + type_name_for_diag(aty) +
									"' requires an explicit cast to a 32-bit integer type");
				}
			}
		}
	}

	e.ty = selected->fn_sig
			   ? selected->fn_sig->fn_ret
			   : (selected->ret ? resolve_type(*selected->ret, call_module) : types_.void_ty());
	return e.ty;
}

} // namespace stellac
