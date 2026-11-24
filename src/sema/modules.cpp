#include "internal.hpp"

namespace stellac {

bool Sema::ensure_const_value(Symbol &s) {
	if (s.has_const_value) {
		return true;
	}
	if (!s.glob || s.kind != SymKind::Const) {
		return false;
	}
	GlobalDecl &g = *s.glob;
	if (g.is_thread_local) {
		log::report(log::Level::Error, g.loc,
					"'thread_local' cannot be used with 'const' because const globals are compile-time values");
	}
	if (g.is_static) {
		log::report(log::Level::Error, g.loc,
					"'static' cannot be used with 'const' because const globals are compile-time values");
	}
	if (!g.init) {
		if (g.is_extern) {
			return true;
		}
		log::report(log::Level::Error, g.loc,
					"const global '" + g.name + "' requires an initializer");
		return false;
	}
	if (evaluating_consts_.count(&s)) {
		log::report(log::Level::Error, g.loc,
					"circular dependency in constant definition '" + g.name + "'");
		return false;
	}
	evaluating_consts_.insert(&s);
	struct Cleanup {
		std::unordered_set<Symbol *> &set;
		Symbol						 *sym;
		~Cleanup() {
			set.erase(sym);
		}
	} cleanup{evaluating_consts_, &s};

	if (!s.owner_module) {
		return false;
	}
	Module &decl_mod = *s.owner_module;

	auto saved_path = decl_mod.lexical_path;
	if (!g.owner_path.empty()) {
		decl_mod.lexical_path = decl_mod.mod_path;
		decl_mod.lexical_path.insert(decl_mod.lexical_path.end(), g.owner_path.begin(),
									 g.owner_path.end());
	}
	struct PathGuard {
		Module					&m;
		std::vector<std::string> path;
		~PathGuard() {
			m.lexical_path = std::move(path);
		}
	} path_guard{decl_mod, std::move(saved_path)};

	FnDecl dummy;
	dummy.name		  = "<global-init>";
	dummy.loc		  = g.loc;
	dummy.decl_module = &decl_mod;
	auto vret		  = std::make_unique<TypeRef>();
	vret->kind		  = TypeRef::Named;
	vret->name		  = "void";
	vret->single	  = "void";
	dummy.ret		  = std::move(vret);
	dummy.fn_sig	  = types_.function_of({}, types_.void_ty(), false);

	ScopeStack scope;
	scope.enter();
	ScopeGuard scope_guard(current_scope_, &scope);

	if (g.ty && g.init) {
		if (g.init->kind == ExprKind::SrcEmbed) {
			resolve_src_embed(*g.init, dummy, decl_mod, scope);
		}
		if (!g.init->sema_invalid || !g.ty->array_len_inferred) {
			infer_array_lengths(*g.ty, *g.init);
		} else {
			g.ty->array_len_inferred = false;
			g.ty->sema_invalid		 = true;
		}
	}
	StType					   *ty = g.ty ? resolve_type(*g.ty, decl_mod) : nullptr;
	std::unordered_set<StType *> active;
	if ((!g.ty || !g.ty->sema_invalid) && ty && !storage_type_valid(ty, active)) {
		log::report(log::Level::Error, g.loc,
					"global '" + g.name +
						"' must have a non-void, non-zero-sized storage type");
	}
	StType *ity = check_expr_with_expected(*g.init, dummy, decl_mod, scope, ty);
	if (ty && ity) {
		check_type_compat(ty, ity, *g.init, g.loc,
						  "global initializer value not representable in '" +
							  type_name_for_diag(ty) + "'",
						  "global initializer type mismatch: expected '" +
							  type_name_for_diag(ty) + "', got '" +
							  type_name_for_diag(ity) + "'");
	}
	const Expr *inner = g.init.get();
	while (inner && (inner->kind == ExprKind::Cast || inner->kind == ExprKind::Unary)) {
		inner = inner->lhs.get();
	}
	bool is_const =
		g.init->has_const_value ||
		(inner &&
		 (is_const_int_expr(const_cast<Expr &>(*inner)) ||
		  inner->kind == ExprKind::StringLit || inner->kind == ExprKind::NullLit ||
		  inner->kind == ExprKind::StructLit || inner->kind == ExprKind::ArrayLit));
	if (!is_const) {
		log::report(log::Level::Error, g.init->loc,
					"'const' global initializer must be a compile-time constant expression");
		return false;
	}

	s.type = ty ? ty : ity;
	if (g.init->has_const_value) {
		s.has_const_value		   = true;
		s.const_value_is_float	   = g.init->const_value_is_float;
		s.const_int_high_bits_ones = g.init->const_int_high_bits_ones;
		s.const_bool_val		   = g.init->is_const_bool ? g.init->const_bool_val : false;
		s.const_int_val			   = g.init->int_val;
		s.const_flt_val			   = g.init->flt_val;
	} else if (inner && is_const_int_expr(const_cast<Expr &>(*inner))) {
		s.has_const_value = true;
		s.const_int_val	  = static_cast<uint64_t>(eval_const_int(const_cast<Expr &>(*inner)));
	} else if (inner && (inner->kind == ExprKind::StringLit || inner->kind == ExprKind::NullLit ||
						 inner->kind == ExprKind::StructLit || inner->kind == ExprKind::ArrayLit)) {
		s.has_const_value = true;
	}
	return true;
}

void Sema::check_module_bodies(Module &m) {
	auto &module_tbl = module_syms_[source_key(m)];
	for (auto &td : m.decls) {
		if (td->kind == TopKind::Fn) {
			if (td->fn->is_macro || !td->fn->generic_params.empty()) {
				continue;
			}
			check_fn(*td->fn, m);
		} else if (td->kind == TopKind::Impl && td->impl_decl) {
			ImplDecl &im = *td->impl_decl;
			for (auto &c : im.consts) {
				if (!c) continue;
				for (auto &entry : module_tbl) {
					if (entry.second.glob == c.get()) {
						ensure_const_value(entry.second);
						break;
					}
				}
			}
			if (im.resolved_target_type && !is_poison(im.resolved_target_type)) {
				active_self_type_ = im.resolved_target_type;
				for (auto &method : im.methods) {
					if (method && method->body && method->generic_params.empty()) {
						check_fn(*method, m);
					}
				}
				active_self_type_ = nullptr;
			}
		} else if (td->kind == TopKind::Var || td->kind == TopKind::Const) {
			GlobalDecl					&g		 = *td->glob;
			if (g.is_const) {
				auto sit = module_tbl.find(g.name);
				if (sit != module_tbl.end()) {
					ensure_const_value(sit->second);
				}
				continue;
			}
			StType						*decl_ty = g.ty ? resolve_type(*g.ty, m) : nullptr;
			std::unordered_set<StType *> active;
			if ((!g.ty || !g.ty->sema_invalid) && !storage_type_valid(decl_ty, active)) {
				log::report(log::Level::Error, g.loc,
							"global '" + g.name +
								"' must have a non-void, non-zero-sized storage type");
			}
			if (g.init) {
				// Build a minimal dummy FnDecl so check_expr has a context
				// for const expressions (e.g. enum member lookups). The
				// dummy has a void return and no params.
				FnDecl dummy;
				dummy.name	 = "<global-init>";
				dummy.loc	 = g.loc;
				// Synthesize a void return type
				auto vret	 = std::make_unique<TypeRef>();
				vret->kind	 = TypeRef::Named;
				vret->name	 = "void";
				vret->single = "void";
				dummy.ret	 = std::move(vret);
				dummy.fn_sig = types_.function_of({}, types_.void_ty(), false);

				ScopeStack scope;
				scope.enter();
				ScopeGuard scope_guard(current_scope_, &scope);
				StType *ty	= g.ty ? resolve_type(*g.ty, m) : nullptr;
				StType *ity = check_expr_with_expected(*g.init, dummy, m, scope, ty);
				if (ty && ity) {
					check_type_compat(ty, ity, *g.init, g.loc,
									  "global initializer value not representable in '" +
										  type_name_for_diag(ty) + "'",
									  "global initializer type mismatch: expected '" +
										  type_name_for_diag(ty) + "', got '" +
										  type_name_for_diag(ity) + "'");
				}
				scope.leave();
			}
		}
	}
	std::function<void(const std::vector<TopDeclPtr> &, std::vector<std::string>)> check_nested;
	check_nested = [&](const std::vector<TopDeclPtr> &decls, std::vector<std::string> prefix) {
		for (const auto &td : decls) {
			if (!td) continue;
			if (td->kind == TopKind::Mod && td->mod_decl) {
				std::vector<std::string> child = prefix;
				child.push_back(td->mod_decl->name);
				check_nested(td->mod_decl->body, std::move(child));
			} else if (td->kind == TopKind::Fn && td->fn &&
					   !td->fn->is_macro && td->fn->generic_params.empty()) {
				auto saved_path = m.lexical_path;
				m.lexical_path = m.mod_path;
				m.lexical_path.insert(m.lexical_path.end(), prefix.begin(), prefix.end());
				check_fn(*td->fn, m);
				m.lexical_path = std::move(saved_path);
			} else if (td->kind == TopKind::Impl && td->impl_decl) {
				ImplDecl &im = *td->impl_decl;
				for (auto &c : im.consts) {
					if (!c) continue;
					for (auto &entry : module_tbl) {
						if (entry.second.glob == c.get()) {
							ensure_const_value(entry.second);
							break;
						}
					}
				}
				if (im.resolved_target_type && !is_poison(im.resolved_target_type)) {
					active_self_type_ = im.resolved_target_type;
					for (auto &method : im.methods) {
						if (method && method->body && method->generic_params.empty()) {
							auto saved_path = m.lexical_path;
							m.lexical_path = m.mod_path;
							m.lexical_path.insert(m.lexical_path.end(), prefix.begin(), prefix.end());
							check_fn(*method, m);
							m.lexical_path = std::move(saved_path);
						}
					}
					active_self_type_ = nullptr;
				}
			} else if ((td->kind == TopKind::Var || td->kind == TopKind::Const) && td->glob) {
				GlobalDecl &g = *td->glob;
				if (g.is_const) {
					for (auto &entry : module_tbl) {
						if (entry.second.glob == &g) {
							ensure_const_value(entry.second);
							break;
						}
					}
					continue;
				}
				if (!g.init) continue;
				FnDecl dummy;
				dummy.name = "<global-init>";
				dummy.loc = g.loc;
				dummy.ret = std::make_unique<TypeRef>();
				dummy.ret->kind = TypeRef::Named;
				dummy.ret->name = "void";
				dummy.ret->single = "void";
				dummy.fn_sig = types_.function_of({}, types_.void_ty(), false);
				ScopeStack scope;
				scope.enter();
				ScopeGuard scope_guard(current_scope_, &scope);
				auto saved_path = m.lexical_path;
				m.lexical_path = m.mod_path;
				m.lexical_path.insert(m.lexical_path.end(), prefix.begin(), prefix.end());
				StType *ty = g.ty ? resolve_type(*g.ty, m) : nullptr;
				check_expr_with_expected(*g.init, dummy, m, scope, ty);
				m.lexical_path = std::move(saved_path);
				scope.leave();
				for (auto &entry : module_tbl) {
					if (entry.second.glob == &g) {
						entry.second.type = ty;
						entry.second.has_const_value =
							!g.is_mutable && g.init->has_const_value;
						entry.second.const_value_is_float = g.init->const_value_is_float;
						entry.second.const_int_high_bits_ones = g.init->const_int_high_bits_ones;
						entry.second.const_bool_val = g.init->const_bool_val;
						entry.second.const_int_val = g.init->int_val;
						entry.second.const_flt_val = g.init->flt_val;
						break;
					}
				}
			}
		}
	};
	for (const auto &td : m.decls) {
		if (td && td->kind == TopKind::Mod && td->mod_decl) {
			check_nested(td->mod_decl->body, {td->mod_decl->name});
		}
	}
}

void Sema::mark_deferred_body_uses(Module &m) {
	using ScopeNames = std::vector<std::unordered_set<std::string>>;

	auto is_bound = [](const ScopeNames &scopes, const std::string &name) {
		for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
			if (it->count(name)) {
				return true;
			}
		}
		return false;
	};

	std::function<void(const TypeRef &, ScopeNames &)> scan_type;
	std::function<void(const Expr &, ScopeNames &)>	   scan_expr;
	std::function<void(const Stmt &, ScopeNames &)>	   scan_stmt;

	// Generic templates are not typechecked until specialization, and macro
	// bodies are checked only after expansion. Their direct function
	// dependencies are still syntactically meaningful now. Resolve those
	// references with the same module lookup used by semantic checking.
	// Local bindings are excluded so a parameter or local function pointer does
	// not accidentally mark a top-level function with the same name.
	auto mark_path = [&](const std::vector<std::string> &path, const ScopeNames &scopes) {
		if (path.empty() || is_bound(scopes, path.front())) {
			return;
		}
		Symbol *sym = resolve_path(path, m);
		if (!sym || sym->kind != SymKind::Fn) {
			return;
		}
		if (sym->fn) {
			sym->fn->used = true;
		}
		for (FnDecl *fn : sym->overloads) {
			if (fn) {
				fn->used = true;
			}
		}
	};

	scan_type = [&](const TypeRef &type, ScopeNames &scopes) {
		if (type.base) {
			scan_type(*type.base, scopes);
		}
		for (const auto &param : type.fn_params) {
			if (param) {
				scan_type(*param, scopes);
			}
		}
		if (type.fn_ret) {
			scan_type(*type.fn_ret, scopes);
		}
		if (type.array_len_expr) {
			scan_expr(*type.array_len_expr, scopes);
		}
		if (type.typeof_expr) {
			scan_expr(*type.typeof_expr, scopes);
		}
		for (const auto &arg : type.generic_args) {
			if (arg.type) {
				scan_type(*arg.type, scopes);
			}
			if (arg.value) {
				scan_expr(*arg.value, scopes);
			}
		}
	};

	scan_expr = [&](const Expr &expr, ScopeNames &scopes) {
		if (expr.kind == ExprKind::Ident) {
			mark_path(expr.path, scopes);
		} else if (expr.kind == ExprKind::Member) {
			std::vector<std::string> path;
			if (flatten_member_path(expr, path)) {
				mark_path(path, scopes);
			}
		}
		if (expr.lhs) {
			scan_expr(*expr.lhs, scopes);
		}
		if (expr.rhs) {
			scan_expr(*expr.rhs, scopes);
		}
		if (expr.condition) {
			scan_expr(*expr.condition, scopes);
		}
		if (expr.then_expr) {
			scan_expr(*expr.then_expr, scopes);
		}
		if (expr.else_expr) {
			scan_expr(*expr.else_expr, scopes);
		}
		for (const auto &arg : expr.args) {
			if (arg) {
				scan_expr(*arg, scopes);
			}
		}
		if (expr.block) {
			scan_stmt(*expr.block, scopes);
		}
		for (const auto &arg : expr.generic_args) {
			if (arg.type) {
				scan_type(*arg.type, scopes);
			}
			if (arg.value) {
				scan_expr(*arg.value, scopes);
			}
		}
		if (expr.cast_type) {
			scan_type(*expr.cast_type, scopes);
		}
		if (expr.va_arg_ty) {
			scan_type(*expr.va_arg_ty, scopes);
		}
	};

	scan_stmt = [&](const Stmt &stmt, ScopeNames &scopes) {
		switch (stmt.kind) {
		case StmtKind::Block:
			scopes.emplace_back();
			for (const auto &child : stmt.body) {
				if (child) {
					scan_stmt(*child, scopes);
				}
			}
			scopes.pop_back();
			return;
		case StmtKind::ExprStmt:
			if (stmt.expr) {
				scan_expr(*stmt.expr, scopes);
			}
			return;
		case StmtKind::VarDecl:
		case StmtKind::ConstDecl:
			for (const auto &var : stmt.vars) {
				if (var.ty) {
					scan_type(*var.ty, scopes);
				}
				if (var.init) {
					scan_expr(*var.init, scopes);
				}
				scopes.back().insert(var.name);
			}
			return;
		case StmtKind::If:
			if (stmt.cond) {
				scan_expr(*stmt.cond, scopes);
			}
			if (stmt.then_block) {
				scan_stmt(*stmt.then_block, scopes);
			}
			if (stmt.else_block) {
				scan_stmt(*stmt.else_block, scopes);
			}
			return;
		case StmtKind::For:
			scopes.emplace_back();
			if (stmt.for_init) {
				scan_stmt(*stmt.for_init, scopes);
			}
			if (stmt.for_cond) {
				scan_expr(*stmt.for_cond, scopes);
			}
			if (stmt.for_step) {
				scan_expr(*stmt.for_step, scopes);
			}
			if (stmt.loop_body) {
				scan_stmt(*stmt.loop_body, scopes);
			}
			scopes.pop_back();
			return;
		case StmtKind::Defer:
			if (stmt.defer_body) {
				scopes.emplace_back();
				scan_stmt(*stmt.defer_body, scopes);
				scopes.pop_back();
			}
			return;
		case StmtKind::Match:
			if (stmt.scrutinee) {
				scan_expr(*stmt.scrutinee, scopes);
			}
			for (const auto &arm : stmt.arms) {
				for (const auto &pat : arm.patterns) {
					if (pat.expr) {
						scan_expr(*pat.expr, scopes);
					}
				}
				if (arm.body) {
					scan_stmt(*arm.body, scopes);
				}
			}
			return;
		case StmtKind::Return:
		case StmtKind::Yield:
			if (stmt.ret_val) {
				scan_expr(*stmt.ret_val, scopes);
			}
			return;
		case StmtKind::Asm:
			for (const auto &operand : stmt.asm_outputs) {
				if (operand.operand) {
					scan_expr(*operand.operand, scopes);
				}
			}
			for (const auto &operand : stmt.asm_inputs) {
				if (operand.operand) {
					scan_expr(*operand.operand, scopes);
				}
			}
			return;
		default:
			return;
		}
	};

	std::function<void(const std::vector<TopDeclPtr> &)> scan_deferred_decls;
	scan_deferred_decls = [&](const std::vector<TopDeclPtr> &decls) {
		for (const auto &td : decls) {
			if (!td) {
				continue;
			}
			if (td->kind == TopKind::Mod && td->mod_decl) {
				scan_deferred_decls(td->mod_decl->body);
				continue;
			}
			if (td->kind == TopKind::Impl && td->impl_decl) {
				for (const auto &method : td->impl_decl->methods) {
					if (!method || !method->body ||
						(!method->is_macro && method->generic_params.empty())) {
						continue;
					}
					ScopeNames scopes(1);
					for (const auto &param : method->params) {
						scopes.front().insert(param.name);
					}
					scan_stmt(*method->body, scopes);
				}
				continue;
			}
			if (td->kind != TopKind::Fn || !td->fn || !td->fn->body ||
				(!td->fn->is_macro && td->fn->generic_params.empty())) {
				continue;
			}
			ScopeNames scopes(1);
			for (const auto &param : td->fn->params) {
				scopes.front().insert(param.name);
			}
			scan_stmt(*td->fn->body, scopes);
		}
	};
	scan_deferred_decls(m.decls);
}

void Sema::mark_import_uses_in_source(Module &m) {
	// Body checking intentionally skips compile-time-dead branches. That is
	// correct for semantic validation and code generation, but it must not
	// make a portable fallback import look unused merely because this target
	// selected another branch. Walk the already parsed source separately and
	// mark only qualified paths that resolve to an imported symbol.
	auto mark_path = [&](const std::vector<std::string> &path) {
		if (path.size() < 2) {
			return;
		}
		Symbol *sym = resolve_path_any(path, m);
		if (sym && sym->owner_module != &m) {
			mark_import_use(path, m);
		}
	};

	std::function<void(const TypeRef &)>	  scan_type;
	std::function<void(const Expr &)>		  scan_expr;
	std::function<void(const Stmt &)>		  scan_stmt;
	std::function<void(const GenericParam &)> scan_generic_param;

	scan_generic_param = [&](const GenericParam &param) {
		if (param.const_type) {
			scan_type(*param.const_type);
		}
		for (const auto &bound : param.trait_bounds) {
			if (bound) {
				scan_type(*bound);
			}
		}
	};

	scan_type = [&](const TypeRef &type) {
		if (type.kind == TypeRef::Named && type.name.find('.') != std::string::npos) {
			mark_path(split_scopes(type.name));
		}
		if (type.base) {
			scan_type(*type.base);
		}
		for (const auto &param : type.fn_params) {
			if (param) {
				scan_type(*param);
			}
		}
		if (type.fn_ret) {
			scan_type(*type.fn_ret);
		}
		if (type.array_len_expr) {
			scan_expr(*type.array_len_expr);
		}
		if (type.typeof_expr) {
			scan_expr(*type.typeof_expr);
		}
		for (const auto &arg : type.generic_args) {
			if (arg.type) {
				scan_type(*arg.type);
			}
			if (arg.value) {
				scan_expr(*arg.value);
			}
		}
	};

	scan_expr = [&](const Expr &expr) {
		if (expr.kind == ExprKind::Ident || expr.kind == ExprKind::StructLit) {
			mark_path(expr.path);
		} else if (expr.kind == ExprKind::Member) {
			std::vector<std::string> path;
			if (flatten_member_path(expr, path)) {
				mark_path(path);
			}
		}
		if (expr.lhs) {
			scan_expr(*expr.lhs);
		}
		if (expr.rhs) {
			scan_expr(*expr.rhs);
		}
		if (expr.condition) {
			scan_expr(*expr.condition);
		}
		if (expr.then_expr) {
			scan_expr(*expr.then_expr);
		}
		if (expr.else_expr) {
			scan_expr(*expr.else_expr);
		}
		for (const auto &arg : expr.args) {
			if (arg) {
				scan_expr(*arg);
			}
		}
		if (expr.block) {
			scan_stmt(*expr.block);
		}
		for (const auto &arg : expr.generic_args) {
			if (arg.type) {
				scan_type(*arg.type);
			}
			if (arg.value) {
				scan_expr(*arg.value);
			}
		}
		if (expr.cast_type) {
			scan_type(*expr.cast_type);
		}
		if (expr.va_arg_ty) {
			scan_type(*expr.va_arg_ty);
		}
	};

	scan_stmt = [&](const Stmt &stmt) {
		switch (stmt.kind) {
		case StmtKind::Block:
			for (const auto &child : stmt.body) {
				if (child) {
					scan_stmt(*child);
				}
			}
			return;
		case StmtKind::ExprStmt:
			if (stmt.expr) {
				scan_expr(*stmt.expr);
			}
			return;
		case StmtKind::VarDecl:
		case StmtKind::ConstDecl:
			for (const auto &var : stmt.vars) {
				if (var.ty) {
					scan_type(*var.ty);
				}
				if (var.init) {
					scan_expr(*var.init);
				}
			}
			return;
		case StmtKind::If:
			if (stmt.cond) {
				scan_expr(*stmt.cond);
			}
			if (stmt.then_block) {
				scan_stmt(*stmt.then_block);
			}
			if (stmt.else_block) {
				scan_stmt(*stmt.else_block);
			}
			return;
		case StmtKind::For:
			if (stmt.for_init) {
				scan_stmt(*stmt.for_init);
			}
			if (stmt.for_cond) {
				scan_expr(*stmt.for_cond);
			}
			if (stmt.for_step) {
				scan_expr(*stmt.for_step);
			}
			if (stmt.loop_body) {
				scan_stmt(*stmt.loop_body);
			}
			return;
		case StmtKind::Defer:
			if (stmt.defer_body) {
				scan_stmt(*stmt.defer_body);
			}
			return;
		case StmtKind::Match:
			if (stmt.scrutinee) {
				scan_expr(*stmt.scrutinee);
			}
			for (const auto &arm : stmt.arms) {
				for (const auto &pat : arm.patterns) {
					if (pat.expr) {
						scan_expr(*pat.expr);
					}
				}
				if (arm.body) {
					scan_stmt(*arm.body);
				}
			}
			return;
		case StmtKind::Return:
		case StmtKind::Yield:
			if (stmt.ret_val) {
				scan_expr(*stmt.ret_val);
			}
			return;
		case StmtKind::Asm:
			for (const auto &operand : stmt.asm_outputs) {
				if (operand.operand) {
					scan_expr(*operand.operand);
				}
			}
			for (const auto &operand : stmt.asm_inputs) {
				if (operand.operand) {
					scan_expr(*operand.operand);
				}
			}
			return;
		default:
			return;
		}
	};

	std::function<void(const std::vector<TopDeclPtr> &)> scan_decls;
	scan_decls = [&](const std::vector<TopDeclPtr> &decls) {
		for (const auto &td : decls) {
			if (!td) {
				continue;
			}
			switch (td->kind) {
			case TopKind::Mod:
				if (td->mod_decl) {
					scan_decls(td->mod_decl->body);
				}
				break;
			case TopKind::Fn:
				if (!td->fn) {
					break;
				}
				for (const auto &param : td->fn->params) {
					if (param.ty) {
						scan_type(*param.ty);
					}
				}
				if (td->fn->ret) {
					scan_type(*td->fn->ret);
				}
				for (const auto &param : td->fn->generic_params) {
					scan_generic_param(param);
				}
				if (td->fn->body) {
					scan_stmt(*td->fn->body);
				}
				break;
			case TopKind::Struct:
				if (td->st) {
					for (const auto &type : td->st->field_types) {
						if (type) {
							scan_type(*type);
						}
					}
					for (const auto &param : td->st->generic_params) {
						scan_generic_param(param);
					}
				}
				break;
			case TopKind::Union:
				if (td->un) {
					for (const auto &type : td->un->field_types) {
						if (type) {
							scan_type(*type);
						}
					}
					for (const auto &param : td->un->generic_params) {
						scan_generic_param(param);
					}
				}
				break;
			case TopKind::TypeBinding:
				if (td->tb) {
					if (td->tb->target) {
						scan_type(*td->tb->target);
					}
					for (const auto &param : td->tb->generic_params) {
						scan_generic_param(param);
					}
				}
				break;
			case TopKind::Var:
			case TopKind::Const:
				if (td->glob) {
					if (td->glob->ty) {
						scan_type(*td->glob->ty);
					}
					if (td->glob->init) {
						scan_expr(*td->glob->init);
					}
				}
				break;
			case TopKind::Enum:
				if (td->en) {
					if (td->en->backing) {
						scan_type(*td->en->backing);
					}
					for (const auto &member : td->en->members) {
						if (member.value) {
							scan_expr(*member.value);
						}
					}
					for (const auto &param : td->en->generic_params) {
						scan_generic_param(param);
					}
				}
				break;
			case TopKind::Trait:
				if (td->trait_decl) {
					for (const auto &method : td->trait_decl->methods) {
						if (!method) continue;
						for (const auto &param : method->params) {
							if (param.ty) scan_type(*param.ty);
						}
						if (method->ret) scan_type(*method->ret);
					}
				}
				break;
			case TopKind::Impl:
				if (td->impl_decl) {
					if (!td->impl_decl->trait_path.empty()) {
						mark_path(td->impl_decl->trait_path);
					}
					if (td->impl_decl->target_type) {
						scan_type(*td->impl_decl->target_type);
					}
					for (const auto &param : td->impl_decl->generic_params) {
						scan_generic_param(param);
					}
					for (const auto &c : td->impl_decl->consts) {
						if (!c) continue;
						if (c->ty) scan_type(*c->ty);
						if (c->init) scan_expr(*c->init);
					}
					for (const auto &method : td->impl_decl->methods) {
						if (!method) continue;
						for (const auto &param : method->params) {
							if (param.ty) scan_type(*param.ty);
						}
						if (method->ret) scan_type(*method->ret);
						for (const auto &param : method->generic_params) {
							scan_generic_param(param);
						}
						if (method->body) scan_stmt(*method->body);
					}
				}
				break;
			default:
				break;
			}
		}
	};
	scan_decls(m.decls);
}

void Sema::emit_unused_warnings(Module &m) {
	for (auto &td : m.decls) {
		if (!td) {
			continue;
		}
		if (td->kind == TopKind::Import && td->imp) {
			if (!td->imp->is_pub && !td->imp->used) {
				log::report(log::Level::Warning, td->imp->loc,
							"unused import '" + td->imp->written_path + "'");
			}
		} else if (td->kind == TopKind::Fn && td->fn) {
			FnDecl &fn = *td->fn;
			if (!fn.is_pub && !fn.is_extern && fn.name != "main" && !has_attr(fn.attrs, "link_name") &&
				!fn.used && (fn.name.empty() || fn.name[0] != '_')) {
				log::report(log::Level::Warning, fn.loc,
							"unused private function '" + fn.name + "'");
			}
		} else if ((td->kind == TopKind::Var || td->kind == TopKind::Const) && td->glob) {
			GlobalDecl &g = *td->glob;
			if (!g.is_pub && !g.used && (g.name.empty() || g.name[0] != '_')) {
				log::report(log::Level::Warning, g.loc,
							std::string("unused private ") +
								(g.is_const ? "constant '" : "variable '") + g.name + "'");
			}
		}
	}
}

void Sema::resolve_aggregate_fields(Module &m) {
	std::function<void(const TopDeclPtr &, std::vector<std::string>)> resolve_decl;
	resolve_decl = [&](const TopDeclPtr &td, std::vector<std::string> prefix) {
		if (!td) {
			return;
		}
		if (td->kind == TopKind::Mod && td->mod_decl) {
			std::vector<std::string> lexical = prefix;
			lexical.push_back(td->mod_decl->name);
			for (const auto &child : td->mod_decl->body) {
				resolve_decl(child, lexical);
			}
			return;
		}
		std::vector<std::unique_ptr<TypeRef>> *field_types = nullptr;
		StType *aggregate = nullptr;
		std::string name;
		if (td->kind == TopKind::Struct && td->st) {
			field_types = &td->st->field_types;
			aggregate = td->st->resolved;
			name = td->st->name;
		} else if (td->kind == TopKind::Union && td->un) {
			field_types = &td->un->field_types;
			aggregate = td->un->resolved;
			name = td->un->name;
		} else {
			return;
		}

		std::vector<std::string> lexical = prefix;
		lexical.push_back(name);
		if (aggregate && ((td->kind == TopKind::Struct && td->st->generic_params.empty()) ||
						  (td->kind == TopKind::Union && td->un->generic_params.empty()))) {
			auto saved_path = m.lexical_path;
			m.lexical_path = m.mod_path;
			m.lexical_path.insert(m.lexical_path.end(), lexical.begin(), lexical.end());
			for (size_t i = 0; i < field_types->size(); ++i) {
				aggregate->fields[i] = resolve_type(*(*field_types)[i], m);
			}
			m.lexical_path = std::move(saved_path);
		}
	};

	for (const auto &td : m.decls) {
		if (td && (td->kind == TopKind::Struct || td->kind == TopKind::Union ||
				   td->kind == TopKind::Mod)) {
			resolve_decl(td, {});
		}
	}
}

} // namespace stellac
