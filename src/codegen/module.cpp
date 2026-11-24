#include "internal.hpp"

#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace stellac {

namespace {

llvm::ConstantInt *constant_int_from_bits(llvm::IntegerType *type, uint64_t low_bits,
										 bool high_bits_ones) {
	const unsigned width = type->getBitWidth();
	llvm::APInt bits(width, low_bits);
	if (width > 64 && high_bits_ones) {
		bits |= llvm::APInt::getHighBitsSet(width, width - 64);
	}
	return llvm::cast<llvm::ConstantInt>(llvm::ConstantInt::get(type, bits));
}

bool global_has_external_name(const GlobalDecl &global) {
	return global.is_pub || global.is_extern || has_attr(global.attrs, "link_name");
}

} // namespace

bool Codegen::emit_all(ModuleResolver &resolver) {
	// Root namespaces of this translation unit: the entry file's top-level `struct`
	// blocks (e.g. "main" for an exe, or the declared module for a `-c` lib
	// build). Bodies are emitted for these and, at optimized levels, for the
	// selected dependency closures of imported public functions.
	auto							 roots = resolver.root_modules();
	std::set<Module *>				 root_set(roots.begin(), roots.end());
	std::unordered_set<FnDecl *>		 root_source_functions;
	std::unordered_set<GlobalDecl *> root_source_globals;
	std::function<void(const std::vector<TopDeclPtr> &, Module *)> collect_source_decls;
	collect_source_decls = [&](const std::vector<TopDeclPtr> &decls, Module *root) {
		for (const auto &td : decls) {
			if (!td || !belongs_to_translation_unit(*root, *td)) continue;
			if (td->kind == TopKind::Fn && td->fn) {
				root_source_functions.insert(td->fn.get());
			} else if ((td->kind == TopKind::Var || td->kind == TopKind::Const) && td->glob) {
				root_source_globals.insert(td->glob.get());
			} else if (td->kind == TopKind::Impl && td->impl_decl) {
				for (const auto &c : td->impl_decl->consts) {
					if (c) {
						root_source_globals.insert(c.get());
					}
				}
				for (const auto &method : td->impl_decl->methods) {
					if (method) {
						root_source_functions.insert(method.get());
					}
				}
			} else if (td->kind == TopKind::Mod && td->mod_decl) {
				collect_source_decls(td->mod_decl->body, root);
			}
		}
	};
	for (Module *root : roots) {
		collect_source_decls(root->decls, root);
	}

	// Semantic `used` flags describe the whole loaded module graph: checking an
	// unused public library function can mark its own callees as used.  Codegen
	// needs a narrower root set, namely references from declarations that are
	// actually emitted in this translation unit.  The dependency closures below
	// are seeded from these references for every optimization level.
	std::unordered_set<FnDecl *>	 root_imported_functions;
	std::unordered_set<FnDecl *> macro_imported_functions;
	std::unordered_set<GlobalDecl *> root_imported_globals;

	std::unordered_map<Module *, std::unordered_map<std::string, GlobalDecl *>> global_decl_index;
	auto index_module_globals = [&](Module *module) -> const std::unordered_map<std::string, GlobalDecl *> & {
		auto [it, inserted] = global_decl_index.emplace(module, std::unordered_map<std::string, GlobalDecl *>{});
		if (!inserted) {
			return it->second;
		}
		auto &map = it->second;
		std::function<void(const std::vector<TopDeclPtr> &)> index_decls;
		index_decls = [&](const std::vector<TopDeclPtr> &decls) {
			for (const auto &td : decls) {
				if (!td) continue;
				if ((td->kind == TopKind::Var || td->kind == TopKind::Const) && td->glob) {
					map.emplace(td->glob->name, td->glob.get());
				} else if (td->kind == TopKind::Impl && td->impl_decl) {
					for (const auto &c : td->impl_decl->consts) {
						if (c) {
							map.emplace(c->name, c.get());
						}
					}
				} else if (td->kind == TopKind::Mod && td->mod_decl) {
					index_decls(td->mod_decl->body);
				}
			}
		};
		if (module) {
			index_decls(module->decls);
		}
		return map;
	};

	auto find_global_decl = [&](const Symbol &symbol) -> GlobalDecl * {
		if (symbol.glob) {
			return symbol.glob;
		}
		if (symbol.owner_module) {
			const auto &map = index_module_globals(symbol.owner_module);
			auto		it	= map.find(symbol.simple);
			return it == map.end() ? nullptr : it->second;
		}
		for (Module *module : resolver.all()) {
			if (!module || module->mod_path != symbol.module_path) {
				continue;
			}
			const auto &map = index_module_globals(module);
			auto		it	= map.find(symbol.simple);
			if (it != map.end()) {
				return it->second;
			}
		}
		return nullptr;
	};

	std::function<void(const Expr &, Module &, std::vector<std::unordered_set<std::string>> &)>
		collect_root_expr;
	std::function<void(const Stmt &, Module &, std::vector<std::unordered_set<std::string>> &)>
		collect_root_stmt;

	bool collecting_generic_instance = false;
	std::vector<FnDecl *>	  *delta_imported_functions = nullptr;
	std::vector<GlobalDecl *> *delta_imported_globals	= nullptr;
	auto record_imported_fn = [&](FnDecl *fn) {
		if (root_imported_functions.insert(fn).second && delta_imported_functions) {
			delta_imported_functions->push_back(fn);
		}
	};
	auto record_imported_global = [&](GlobalDecl *global) {
		if (root_imported_globals.insert(global).second && delta_imported_globals) {
			delta_imported_globals->push_back(global);
		}
	};
	collect_root_expr = [&](const Expr &e, Module &module,
							std::vector<std::unordered_set<std::string>> &locals) {
		if (e.resolved_fn && e.resolved_fn->decl_module &&
			(!root_set.count(e.resolved_fn->decl_module) ||
			 !root_source_functions.count(e.resolved_fn))) {
			record_imported_fn(e.resolved_fn);
			if (e.binding_module || (collecting_generic_instance &&
									!e.resolved_fn->is_pub && !e.resolved_fn->is_extern)) {
				macro_imported_functions.insert(e.resolved_fn);
			}
		}

		std::vector<std::string> path;
		if (e.kind == ExprKind::Ident) {
			path = e.path;
		} else if (e.kind == ExprKind::Member) {
			flatten_member_path(e, path);
		}
		bool local = false;
		if (!path.empty()) {
			for (auto it = locals.rbegin(); it != locals.rend(); ++it) {
				if (it->count(path.front())) {
					local = true;
					break;
				}
			}
		}
		if (!path.empty() && !local) {
			Symbol *symbol = sema_.resolve_path(path, module);
			if (symbol) {
				if (symbol->kind == SymKind::Fn) {
					if ((collecting_generic_instance || symbol->owner_module != &module) &&
						symbol->fn) {
						record_imported_fn(symbol->fn);
					}
				} else if (symbol->kind == SymKind::Var || symbol->kind == SymKind::Const) {
					GlobalDecl *global		  = find_global_decl(*symbol);
					const bool	root_fragment = root_set.count(&module) && global &&
						!root_source_globals.count(global);
					if (global && (collecting_generic_instance ||
								   symbol->owner_module != &module || root_fragment)) {
						record_imported_global(global);
					}
				}
			}
		}

		if (e.lhs) {
			collect_root_expr(*e.lhs, module, locals);
		}
		if (e.rhs) {
			collect_root_expr(*e.rhs, module, locals);
		}
		if (e.condition) {
			collect_root_expr(*e.condition, module, locals);
		}
		if (e.then_expr) {
			collect_root_expr(*e.then_expr, module, locals);
		}
		if (e.else_expr) {
			collect_root_expr(*e.else_expr, module, locals);
		}
		for (const auto &arg : e.args) {
			collect_root_expr(*arg, module, locals);
		}
		if (e.block) {
			collect_root_stmt(*e.block, module, locals);
		}
	};

	collect_root_stmt = [&](const Stmt &s, Module &module,
							std::vector<std::unordered_set<std::string>> &locals) {
		bool pushed_scope = s.kind == StmtKind::Block || s.kind == StmtKind::For || s.kind == StmtKind::Defer;
		if (pushed_scope) {
			locals.emplace_back();
		}
		for (const auto &child : s.body) {
			collect_root_stmt(*child, module, locals);
		}
		if (s.expr) {
			collect_root_expr(*s.expr, module, locals);
		}
		for (const auto &var : s.vars) {
			if (var.init) {
				collect_root_expr(*var.init, module, locals);
			}
			locals.back().insert(var.name);
		}
		if (s.cond) {
			collect_root_expr(*s.cond, module, locals);
		}
		if (s.then_block) {
			collect_root_stmt(*s.then_block, module, locals);
		}
		if (s.else_block) {
			collect_root_stmt(*s.else_block, module, locals);
		}
		if (s.for_init) {
			collect_root_stmt(*s.for_init, module, locals);
		}
		if (s.for_cond) {
			collect_root_expr(*s.for_cond, module, locals);
		}
		if (s.for_step) {
			collect_root_expr(*s.for_step, module, locals);
		}
		if (s.loop_body) {
			collect_root_stmt(*s.loop_body, module, locals);
		}
		if (s.defer_body) {
			collect_root_stmt(*s.defer_body, module, locals);
		}
		if (s.scrutinee) {
			collect_root_expr(*s.scrutinee, module, locals);
		}
		for (const auto &arm : s.arms) {
			for (const auto &pat : arm.patterns) {
				if (pat.expr) {
					collect_root_expr(*pat.expr, module, locals);
				}
			}
			if (arm.body) {
				collect_root_stmt(*arm.body, module, locals);
			}
		}
		if (s.ret_val) {
			collect_root_expr(*s.ret_val, module, locals);
		}
		for (const auto &operand : s.asm_outputs) {
			if (operand.operand) {
				collect_root_expr(*operand.operand, module, locals);
			}
		}
		for (const auto &operand : s.asm_inputs) {
			if (operand.operand) {
				collect_root_expr(*operand.operand, module, locals);
			}
		}
		if (pushed_scope) {
			locals.pop_back();
		}
	};

	std::function<void(const std::vector<TopDeclPtr> &, Module &)> scan_root_decls;
	scan_root_decls = [&](const std::vector<TopDeclPtr> &decls, Module &root) {
		for (const auto &td : decls) {
			if (!td || !belongs_to_translation_unit(root, *td)) {
				continue;
			}
			if (td->kind == TopKind::Fn && td->fn && td->fn->body) {
				std::vector<std::unordered_set<std::string>> locals(1);
				for (const auto &param : td->fn->params) {
					locals.front().insert(param.name);
				}
				collect_root_stmt(*td->fn->body, root, locals);
			} else if ((td->kind == TopKind::Var || td->kind == TopKind::Const) && td->glob && td->glob->init) {
				std::vector<std::unordered_set<std::string>> locals(1);
				collect_root_expr(*td->glob->init, root, locals);
			} else if (td->kind == TopKind::Impl && td->impl_decl) {
				for (const auto &c : td->impl_decl->consts) {
					if (c && c->init) {
						std::vector<std::unordered_set<std::string>> locals(1);
						collect_root_expr(*c->init, root, locals);
					}
				}
				for (const auto &method : td->impl_decl->methods) {
					if (method && method->body) {
						std::vector<std::unordered_set<std::string>> locals(1);
						for (const auto &param : method->params) {
							locals.front().insert(param.name);
						}
						collect_root_stmt(*method->body, root, locals);
					}
				}
			} else if (td->kind == TopKind::Mod && td->mod_decl) {
				scan_root_decls(td->mod_decl->body, root);
			}
		}
	};

	for (Module *root : roots) {
		scan_root_decls(root->decls, *root);
	}
	if (!roots.empty()) {
		for (FnDecl *instance : sema_.generic_instances()) {
			if (instance && instance->used && instance->body) {
				std::vector<std::unordered_set<std::string>> locals(1);
				for (const auto &param : instance->params) {
					locals.front().insert(param.name);
				}
				collecting_generic_instance = true;
				collect_root_stmt(*instance->body,
					instance->decl_module ? *instance->decl_module : *roots.front(), locals);
				collecting_generic_instance = false;
			}
		}
	}

	// For each imported module, compute the function bodies that may be cloned
	// into this translation unit. A private helper is fine when it is reachable
	// from a public function and every dependency can be represented safely here.
	// This must happen before declarations are emitted so every cloned call has
	// an internal declaration available during body lowering.
	struct ImportedClosure {
		std::unordered_set<FnDecl *>	 functions;
		std::unordered_set<GlobalDecl *> globals;
		std::unordered_set<GlobalDecl *> canonical_globals;
	};
	std::unordered_map<Module *, ImportedClosure> imported_closures;
	std::unordered_set<Module *> macro_closures;
	// A cloned body can contain a macro expansion whose private references are
	// bound to a different imported module.  Discovering that reference while
	// scanning a closure must schedule the referenced module's closure too;
	// otherwise declare_fn() would leave an internal-linkage declaration with no
	// definition, which LLVM rejects.  Repeat the two discovery passes until no
	// closure or cross-module reference grows.
	auto imported_state_size = [&]() {
		size_t size = root_imported_functions.size() + root_imported_globals.size() +
			macro_imported_functions.size() + macro_closures.size();
		for (const auto &[module, closure] : imported_closures) {
			(void)module;
			size += closure.functions.size() + closure.globals.size() +
				closure.canonical_globals.size();
		}
		return size;
	};
	struct FnInfo {
		bool							 safe = true;
		std::unordered_set<FnDecl *>	 direct_calls;
		std::unordered_set<GlobalDecl *> direct_globals;
		std::unordered_set<GlobalDecl *> address_globals;
	};
	struct ModuleAnalysisCache {
		std::unordered_map<FnDecl *, FnInfo>		infos;
		std::unordered_map<FnDecl *, unsigned char> states;
	};
	std::unordered_map<Module *, ModuleAnalysisCache> module_caches;
	std::unordered_set<FnDecl *>					  scanned_closure_functions;
	std::vector<FnDecl *>							  new_closure_functions;
	std::vector<GlobalDecl *>						  new_closure_globals;

	bool imported_state_changed = true;
	while (imported_state_changed) {
		size_t before_imported_state = imported_state_size();
		for (auto *m : resolver.all()) {
			if (root_set.count(m)) {
				continue;
			}

		ModuleAnalysisCache						 &cache	 = module_caches[m];
		std::unordered_map<FnDecl *, FnInfo>	 &infos	 = cache.infos;
		std::unordered_map<FnDecl *, unsigned char> &states = cache.states;
		std::function<bool(FnDecl *)>				analyze;

		analyze = [&](FnDecl *f) {
			if (!f || !f->body) {
				return f && f->is_extern;
			}
			if (auto state = states.find(f); state != states.end()) {
				if (state->second == 1) {
					return true;
				}
				return infos[f].safe;
			}
			states[f]										  = 1;
			FnInfo										&info = infos[f];
			std::vector<std::unordered_set<std::string>> locals(1);
			for (const auto &param : f->params) {
				locals.front().insert(param.name);
			}

			auto is_local = [&](const std::string &name) {
				for (auto scope = locals.rbegin(); scope != locals.rend(); ++scope) {
					if (scope->count(name)) {
						return true;
					}
				}
				return false;
			};
			auto referenced_global = [&](const Expr &e) -> GlobalDecl * {
				std::vector<std::string> path;
				if (e.kind == ExprKind::Ident) {
					path = e.path;
				} else if (e.kind == ExprKind::Member) {
					if (!flatten_member_path(e, path)) {
						return nullptr;
					}
				} else {
					return nullptr;
				}
				if (path.empty() || (path.size() == 1 && is_local(path.front()))) {
					return nullptr;
				}
				// A member of an aggregate global (for example `&state.field`) still
				// makes the root object address-significant. Try the complete
				// path first, then its qualified prefixes to find that root global.
				for (size_t length = path.size(); length > 0; --length) {
					std::vector<std::string> prefix(path.begin(), path.begin() + length);
					Symbol					*symbol = sema_.resolve_path(prefix, *m);
					if (!symbol ||
						(symbol->kind != SymKind::Var && symbol->kind != SymKind::Const) ||
						symbol->owner_module != m) {
						continue;
					}
					if (auto *found = find_global_decl(*symbol)) {
						return found;
					}
				}
				return nullptr;
			};
			auto cloneable_private_value = [&](GlobalDecl &global) {
				if (global_has_external_name(global)) {
					return true;
				}
				if (!global.is_const || global.is_thread_local || !global.ty ||
					has_attr(global.attrs, "used")) {
					return false;
				}
				StType *type = underlying_type(sema_.resolve_type(*global.ty, *m));
				return type && (type->kind == TypeKind::Bool || type->kind == TypeKind::UInt ||
								type->kind == TypeKind::SInt || type->kind == TypeKind::Float ||
								type->kind == TypeKind::Enum);
			};

			std::function<void(const Expr &)> visit_expr;
			std::function<void(const Stmt &)> visit_stmt;
			auto							  record_function = [&](FnDecl *callee) {
				if (!callee) {
					return true;
				}
				if (callee->decl_module != m) {
					if (!callee->is_pub && !callee->is_extern) {
						// A private function from another namespace cannot be
						// represented by an external declaration. Keep the owning
						// body out of the imported optimization clone instead of
						// producing an internal declaration with no definition.
						return false;
					}
					// A function value or call may cross a nested namespace
					// module boundary. Schedule that function for the owning
					// module's closure; otherwise an internal declaration can be
					// emitted without its body.
					root_imported_functions.insert(callee);
					return true;
				}
				if (!callee->body) {
					if (callee->is_extern) {
						info.direct_calls.insert(callee);
					}
					return callee->is_extern;
				}
				info.direct_calls.insert(callee);
				return analyze(callee);
			};
			visit_expr = [&](const Expr &e) {
				if (!info.safe) {
					return;
				}
				if (GlobalDecl *global = referenced_global(e)) {
					info.direct_globals.insert(global);
					if (!cloneable_private_value(*global)) {
						// Private storage is canonical in its owner object. Only scalar
						// constants can be copied into an available_externally clone
						// without leaving a cross-object reference or changing address,
						// aggregate, or thread-local identity.
						info.safe = false;
					}
				}
				if (e.kind == ExprKind::AddrOf && e.lhs) {
					if (GlobalDecl *global = referenced_global(*e.lhs)) {
						info.address_globals.insert(global);
						if (!global_has_external_name(*global)) {
							// Address identity cannot be preserved by an
							// available_externally copy of private storage.
							info.safe = false;
						}
					}
				}
				FnDecl *value_function = nullptr;
				if (e.kind == ExprKind::Ident) {
					value_function = e.resolved_fn;
				} else if (e.kind == ExprKind::Member) {
					std::vector<std::string> path;
					if (flatten_member_path(e, path)) {
						Symbol *symbol = sema_.resolve_path(path, *m);
						if (symbol && symbol->kind == SymKind::Fn) {
							value_function =
								symbol->fn ? symbol->fn
										   : (!symbol->overloads.empty() ? symbol->overloads.front()
																		 : nullptr);
						}
					}
				}
				if (value_function && !record_function(value_function)) {
					info.safe = false;
					return;
				}
				if (e.kind == ExprKind::Call) {
					if (e.resolved_fn) {
						if (!record_function(e.resolved_fn)) {
							info.safe = false;
							return;
						}
					} else if (!value_function) {
						// An indirect call may hide a dependency that cannot be
						// reproduced in this translation unit.
						info.safe = false;
						return;
					}
				}
				if (e.lhs) {
					visit_expr(*e.lhs);
				}
				if (e.rhs) {
					visit_expr(*e.rhs);
				}
				if (e.condition) {
					visit_expr(*e.condition);
				}
				if (e.then_expr) {
					visit_expr(*e.then_expr);
				}
				if (e.else_expr) {
					visit_expr(*e.else_expr);
				}
				for (const auto &arg : e.args) {
					visit_expr(*arg);
				}
				if (e.block) {
					visit_stmt(*e.block);
				}
			};
			visit_stmt = [&](const Stmt &s) {
				if (!info.safe) {
					return;
				}
				bool pushed_scope = s.kind == StmtKind::Block;
				if (pushed_scope) {
					locals.emplace_back();
				}
				for (const auto &child : s.body) {
					visit_stmt(*child);
				}
				if (s.expr) {
					visit_expr(*s.expr);
				}
				for (const auto &var : s.vars) {
					if (var.init) {
						visit_expr(*var.init);
					}
				}
				for (const auto &var : s.vars) {
					locals.back().insert(var.name);
				}
				if (s.cond) {
					visit_expr(*s.cond);
				}
				if (s.then_block) {
					visit_stmt(*s.then_block);
				}
				if (s.else_block) {
					visit_stmt(*s.else_block);
				}
				if (s.for_init) {
					visit_stmt(*s.for_init);
				}
				if (s.for_cond) {
					visit_expr(*s.for_cond);
				}
				if (s.for_step) {
					visit_expr(*s.for_step);
				}
				if (s.loop_body) {
					visit_stmt(*s.loop_body);
				}
				if (s.defer_body) {
					visit_stmt(*s.defer_body);
				}
				if (s.scrutinee) {
					visit_expr(*s.scrutinee);
				}
				for (const auto &arm : s.arms) {
					for (const auto &pat : arm.patterns) {
						if (pat.expr) {
							visit_expr(*pat.expr);
						}
					}
					if (arm.body) {
						visit_stmt(*arm.body);
					}
				}
				if (s.ret_val) {
					visit_expr(*s.ret_val);
				}
				for (const auto &operand : s.asm_outputs) {
					if (operand.operand) {
						visit_expr(*operand.operand);
					}
				}
				for (const auto &operand : s.asm_inputs) {
					if (operand.operand) {
						visit_expr(*operand.operand);
					}
				}
				if (s.kind == StmtKind::Asm && !s.asm_code.empty()) {
					// Non-empty inline assembly may name private symbols without
					// an AST reference, so do not clone such a function
					// speculatively. An empty asm block is only a compiler
					// barrier and is safe to clone.
					info.safe = false;
				}
				if (pushed_scope) {
					locals.pop_back();
				}
			};

			visit_stmt(*f->body);
			states[f] = 2;
			return info.safe;
		};

		std::function<void(FnDecl *, ImportedClosure &)> collect;
		collect = [&](FnDecl *f, ImportedClosure &closure) {
			if (!f || !closure.functions.insert(f).second) {
				return;
			}
			for (GlobalDecl *global : infos[f].direct_globals) {
				closure.globals.insert(global);
			}
			for (GlobalDecl *global : infos[f].address_globals) {
				closure.canonical_globals.insert(global);
			}
			for (FnDecl *callee : infos[f].direct_calls) {
				collect(callee, closure);
			}
		};
		for (FnDecl *function : root_imported_functions) {
			if (!function || function->decl_module != m || function->generic_params.size() != 0 ||
				!analyze(function)) {
				continue;
			}
			collect(function, imported_closures[m]);
		}
		for (FnDecl *function : macro_imported_functions) {
			if (function && function->decl_module == m) {
				macro_closures.insert(m);
				break;
			}
		}
		}
	// A selected imported body can call a function in another imported module.
	// The per-module closure analysis intentionally follows only same-module
	// calls (private helpers are the important case there), so collect the
	// cross-module boundary references from the selected bodies as well.  Keep
	// same-module private helpers out of the declaration set: they are either
	// cloned by the closure or remain in their owning object file.
		if (inline_imports_) {
		for (auto &[module, closure] : imported_closures) {
			for (FnDecl *function : closure.functions) {
				if (!function || !function->body ||
					!scanned_closure_functions.insert(function).second) {
					continue;
				}
				new_closure_functions.clear();
				new_closure_globals.clear();
				delta_imported_functions = &new_closure_functions;
				delta_imported_globals	 = &new_closure_globals;
				std::vector<std::unordered_set<std::string>> locals(1);
				for (const auto &param : function->params) {
					locals.front().insert(param.name);
				}
				collect_root_stmt(*function->body, *module, locals);
				delta_imported_functions = nullptr;
				delta_imported_globals	 = nullptr;
				for (FnDecl *added_fn : new_closure_functions) {
					if (added_fn && added_fn->decl_module == module) {
						root_imported_functions.erase(added_fn);
					}
				}
				for (GlobalDecl *added_global : new_closure_globals) {
					// Globals do not carry an owning-module back pointer.  A
					// same-module private global is already represented by the
					// closure's globals set and must not become an external decl.
					std::function<bool(const std::vector<TopDeclPtr> &)> module_owns_global;
					module_owns_global = [&](const std::vector<TopDeclPtr> &decls) -> bool {
						for (const auto &td : decls) {
							if (!td) continue;
							if ((td->kind == TopKind::Var || td->kind == TopKind::Const) &&
								td->glob.get() == added_global) {
								return true;
							}
							if (td->kind == TopKind::Impl && td->impl_decl) {
								for (const auto &c : td->impl_decl->consts) {
									if (c.get() == added_global) {
										return true;
									}
								}
							}
							if (td->kind == TopKind::Mod && td->mod_decl &&
								module_owns_global(td->mod_decl->body)) {
								return true;
							}
						}
						return false;
					};
					if (module_owns_global(module->decls)) {
						root_imported_globals.erase(added_global);
					}
				}
			}
		}
		}
	imported_state_changed = imported_state_size() != before_imported_state;
	}

	// (1) register struct types and declare cross-module symbols across
	// every reachable module. Struct types are IR-only (not object symbols), so
	// declaring them in each TU is harmless. Functions and globals from
	// imported (non-root) modules are normally emitted as extern declarations;
	// selected dependency closures additionally get compiler-internal helper
	// definitions for optimization. Sema still exposes only pub items across
	// module boundaries.
	for (auto *m : resolver.all()) {
		const bool is_root = (root_set.count(m) != 0);
		for (auto &td : m->decls) {
			const bool is_root_source_decl = is_root && belongs_to_translation_unit(*m, *td);
			switch (td->kind) {
			case TopKind::Struct:
				if (td->st->generic_params.empty()) {
					declare_struct(*td->st, *m);
				}
				break;
			case TopKind::Union:
				if (td->un->generic_params.empty()) {
					declare_union(*td->un, *m);
				}
				break;
			case TopKind::Fn:
				if (td->fn->is_macro) {
					break;
				}
				if (!td->fn->generic_params.empty()) {
					break;
				}
				// Root-source functions are declared before their bodies are emitted.
				// Imported declarations are limited to functions referenced by this
				// translation unit (plus explicit retention attributes). At optimized
				// levels, selected closure members are also declared before cloning.

				// A private function is declared here only when its body will also
				// be cloned below. Its InternalLinkage keeps it compiler-internal.
				if (is_root_source_decl ||
					root_imported_functions.count(td->fn.get()) != 0 ||
					has_attr(td->fn->attrs, "used") ||
					((inline_imports_ || macro_closures.count(m) != 0) &&
						 imported_closures[m].functions.count(td->fn.get()) != 0)) {
					declare_fn(*td->fn, *m);
				}
				break;
			case TopKind::Var:
			case TopKind::Const:
				if (is_root_source_decl) {
					emit_global(*td->glob, *m);
				} else if ((global_has_external_name(*td->glob) &&
							(root_imported_globals.count(td->glob.get()) != 0 ||
							 has_attr(td->glob->attrs, "used"))) ||
						   (inline_imports_ &&
							imported_closures[m].globals.count(td->glob.get()) != 0)) {
					declare_global(*td->glob, *m);
				}
				break;
			case TopKind::Impl:
				if (td->impl_decl) {
					for (auto &c : td->impl_decl->consts) {
						if (!c || !td->impl_decl->generic_params.empty()) {
							continue;
						}
						if (is_root_source_decl) {
							emit_global(*c, *m);
						} else if ((global_has_external_name(*c) &&
									(root_imported_globals.count(c.get()) != 0 ||
									 has_attr(c->attrs, "used"))) ||
								   (inline_imports_ &&
									imported_closures[m].globals.count(c.get()) != 0)) {
							declare_global(*c, *m);
						}
					}
					for (auto &method : td->impl_decl->methods) {
						if (!method || method->is_macro || !method->generic_params.empty()) {
							continue;
						}
						if (is_root_source_decl ||
							root_imported_functions.count(method.get()) != 0 ||
							has_attr(method->attrs, "used") ||
							((inline_imports_ || macro_closures.count(m) != 0) &&
								 imported_closures[m].functions.count(method.get()) != 0)) {
							declare_fn(*method, *m);
						}
					}
				}
				break;
			default:
				break;
			}
		}
	}
	// The declarations inside block-level `mod` blocks remain owned by the
	// containing source file. Emit/declare them recursively instead of creating
	// synthetic child modules or duplicate AST declarations.
	std::function<void(Module *, const std::vector<TopDeclPtr> &)> declare_nested;
	declare_nested = [&](Module *m, const std::vector<TopDeclPtr> &decls) {
		const bool is_root = root_set.count(m) != 0;
		for (const auto &td : decls) {
			if (!td) continue;
			if (td->kind == TopKind::Mod && td->mod_decl) {
				declare_nested(m, td->mod_decl->body);
			} else if (td->kind == TopKind::Struct && td->st) {
				if (td->st->generic_params.empty()) declare_struct(*td->st, *m);
			} else if (td->kind == TopKind::Union && td->un &&
					   td->un->generic_params.empty()) {
				declare_union(*td->un, *m);
			} else if (td->kind == TopKind::Fn && td->fn && !td->fn->is_macro &&
					   td->fn->generic_params.empty()) {
				const bool source = is_root && root_source_functions.count(td->fn.get());
				if (source || root_imported_functions.count(td->fn.get()) != 0 ||
					has_attr(td->fn->attrs, "used") ||
					(inline_imports_ && imported_closures[m].functions.count(td->fn.get()) != 0)) {
					declare_fn(*td->fn, *m);
				}
			} else if (td->kind == TopKind::Impl && td->impl_decl) {
				for (auto &c : td->impl_decl->consts) {
					if (!c || !td->impl_decl->generic_params.empty()) {
						continue;
					}
					const bool source = is_root && root_source_globals.count(c.get());
					if (source) {
						emit_global(*c, *m);
					} else if ((global_has_external_name(*c) &&
								(root_imported_globals.count(c.get()) != 0 ||
								 has_attr(c->attrs, "used"))) ||
							   (inline_imports_ &&
								imported_closures[m].globals.count(c.get()) != 0)) {
						declare_global(*c, *m);
					}
				}
				for (auto &method : td->impl_decl->methods) {
					if (!method || method->is_macro || !method->generic_params.empty()) {
						continue;
					}
					const bool source = is_root && root_source_functions.count(method.get());
					if (source || root_imported_functions.count(method.get()) != 0 ||
						has_attr(method->attrs, "used") ||
						(inline_imports_ && imported_closures[m].functions.count(method.get()) != 0)) {
						declare_fn(*method, *m);
					}
				}
			} else if ((td->kind == TopKind::Var || td->kind == TopKind::Const) && td->glob) {
				const bool source = is_root && root_source_globals.count(td->glob.get());
				if (source) {
					emit_global(*td->glob, *m);
				} else if ((global_has_external_name(*td->glob) &&
							(root_imported_globals.count(td->glob.get()) != 0 ||
							 has_attr(td->glob->attrs, "used"))) ||
						   (inline_imports_ &&
							imported_closures[m].globals.count(td->glob.get()) != 0)) {
					declare_global(*td->glob, *m);
				}
			}
		}
	};
	for (Module *m : resolver.all()) {
		for (const auto &td : m->decls) {
			if (td && td->kind == TopKind::Mod && td->mod_decl) {
				declare_nested(m, td->mod_decl->body);
			}
		}
	}
	// Generic bodies are source-level templates and have no ABI until a call
	// specializes them.  All concrete instances are declared in this TU so
	// mutually-recursive specializations can refer to one another.
	if (!roots.empty()) {
		for (FnDecl *instance : sema_.generic_instances()) {
			if (instance && instance->used && !instance->is_macro) {
				declare_fn(*instance, instance->decl_module ? *instance->decl_module : *roots.front());
			}
		}
	}
	// (2) emit bodies for the root module(s). Imported functions normally come
	// from their own compilation unit; optimized imports additionally emit the
	// selected dependency closures below.
	//
	// When `inline_imports_` is set (driven by -O1/-O2/-O3/-Oz), emit the
	// selected closure. Public functions use `available_externally`, allowing
	// LLVM to fold across the module boundary; private helpers remain internal
	// to this importing TU. Mutable globals are never cloned, so the real object
	// still comes from the owning module.
	for (auto *root : roots) {
		for (auto &td : root->decls) {
			if (td->kind == TopKind::Fn && !td->fn->is_macro && td->fn->generic_params.empty() &&
				belongs_to_translation_unit(*root, *td)) {
				emit_fn_body(*td->fn, *root);
			} else if (td->kind == TopKind::Impl && td->impl_decl &&
					   belongs_to_translation_unit(*root, *td)) {
				for (auto &method : td->impl_decl->methods) {
					if (method && method->body && !method->is_macro && method->generic_params.empty()) {
						emit_fn_body(*method, *root);
					}
				}
			}
		}
	}
	for (auto *root : roots) {
		std::function<void(const std::vector<TopDeclPtr> &)> emit_nested;
		emit_nested = [&](const std::vector<TopDeclPtr> &decls) {
			for (const auto &td : decls) {
				if (!td) continue;
				if (td->kind == TopKind::Mod && td->mod_decl) {
					emit_nested(td->mod_decl->body);
				} else if (td->kind == TopKind::Fn && td->fn && !td->fn->is_macro &&
						   td->fn->generic_params.empty() &&
						   root_source_functions.count(td->fn.get())) {
					auto saved_path = root->lexical_path;
					root->lexical_path = root->mod_path;
					root->lexical_path.insert(root->lexical_path.end(), td->fn->owner_path.begin(),
										 td->fn->owner_path.end());
					emit_fn_body(*td->fn, *root);
					root->lexical_path = std::move(saved_path);
				} else if (td->kind == TopKind::Impl && td->impl_decl) {
					for (auto &method : td->impl_decl->methods) {
						if (method && method->body && !method->is_macro &&
							method->generic_params.empty() &&
							root_source_functions.count(method.get())) {
							auto saved_path = root->lexical_path;
							root->lexical_path = root->mod_path;
							root->lexical_path.insert(root->lexical_path.end(),
													  method->owner_path.begin(),
													  method->owner_path.end());
							emit_fn_body(*method, *root);
							root->lexical_path = std::move(saved_path);
						}
					}
				}
			}
		};
		for (const auto &td : root->decls) {
			if (td && td->kind == TopKind::Mod && td->mod_decl) {
				emit_nested(td->mod_decl->body);
			}
		}
	}
	if (!roots.empty()) {
		for (FnDecl *instance : sema_.generic_instances()) {
			if (instance && instance->used && !instance->is_macro && instance->body) {
				Module &origin = instance->decl_module ? *instance->decl_module : *roots.front();
				auto saved_path = origin.lexical_path;
				origin.lexical_path = origin.mod_path;
				origin.lexical_path.insert(origin.lexical_path.end(), instance->owner_path.begin(),
					instance->owner_path.end());
				emit_fn_body(*instance, origin);
				origin.lexical_path = std::move(saved_path);
			}
		}
	}
	if (inline_imports_ || !macro_closures.empty()) {
		for (auto *m : resolver.all()) {
			if (root_set.count(m)) {
				continue;
			}
			std::function<void(const std::vector<TopDeclPtr> &)> emit_imported_decls;
			emit_imported_decls = [&](const std::vector<TopDeclPtr> &decls) {
				for (const auto &td : decls) {
					if (!td) continue;
					if (td->kind == TopKind::Mod && td->mod_decl) {
						emit_imported_decls(td->mod_decl->body);
						continue;
					}
					if (td->kind == TopKind::Fn && !td->fn->is_macro) {
						FnDecl &f = *td->fn;
						if (!f.body || !f.generic_params.empty() ||
							imported_closures[m].functions.count(&f) == 0) {
							continue;
						}
						const bool macro_clone =
							macro_closures.count(m) != 0 && !f.is_pub && !f.is_extern;
						if (!inline_imports_ && !macro_clone) {
							continue;
						}
						std::string nm = fn_symbol_name(f);
						auto		it = fns_.find(nm);
						if (it == fns_.end()) {
							continue;
						}
						if (f.is_pub && inline_imports_) {
							it->second->setLinkage(llvm::GlobalValue::AvailableExternallyLinkage);
						}
						auto saved_path = m->lexical_path;
						m->lexical_path = m->mod_path;
						m->lexical_path.insert(m->lexical_path.end(), f.owner_path.begin(),
											   f.owner_path.end());
						emit_fn_body(f, *m);
						m->lexical_path = std::move(saved_path);
						continue;
					}
					if (td->kind == TopKind::Impl && td->impl_decl) {
						for (auto &method : td->impl_decl->methods) {
							if (!method || method->is_macro || !method->body ||
								!method->generic_params.empty() ||
								imported_closures[m].functions.count(method.get()) == 0) {
								continue;
							}
							const bool required_private_clone = macro_closures.count(m) != 0 &&
								!method->is_pub && !method->is_extern;
							if (!inline_imports_ && !required_private_clone) {
								continue;
							}
							std::string nm = fn_symbol_name(*method);
							auto		it = fns_.find(nm);
							if (it == fns_.end()) {
								continue;
							}
							if (method->is_pub && inline_imports_) {
								it->second->setLinkage(
									llvm::GlobalValue::AvailableExternallyLinkage);
							}
							auto saved_path = m->lexical_path;
							m->lexical_path = m->mod_path;
							m->lexical_path.insert(m->lexical_path.end(), method->owner_path.begin(),
												   method->owner_path.end());
							emit_fn_body(*method, *m);
							m->lexical_path = std::move(saved_path);
						}
						for (auto &c : td->impl_decl->consts) {
							if (!c || !td->impl_decl->generic_params.empty()) {
								continue;
							}
							GlobalDecl &g		= *c;
							const auto	closure = imported_closures.find(m);
							const bool	imported = closure != imported_closures.end() &&
												   closure->second.globals.count(&g) != 0;
							if (!g.is_pub && !imported) {
								continue;
							}
							const bool canonical =
								!g.is_const ||
								(closure != imported_closures.end() &&
								 closure->second.canonical_globals.count(&g) != 0);
							if (canonical) {
								continue;
							}
							std::string nm = mangle_global(*m, g);
							auto		it = globals_.find(nm);
							if (it == globals_.end()) {
								continue;
							}
							llvm::GlobalVariable *gv   = it->second;
							StType				 *ty   = sema_.resolve_type(*g.ty, *m);
							llvm::Type			 *lt   = map_type(ty);
							llvm::Constant		 *init = lower_global_init(g, *m, ty, lt, nm);
							if (!init) {
								continue;
							}
							gv->setInitializer(init);
							gv->setLinkage(llvm::GlobalValue::AvailableExternallyLinkage);
						}
						continue;
					}
					if (td->kind != TopKind::Var && td->kind != TopKind::Const) {
						continue;
					}
					GlobalDecl &g		= *td->glob;
					const auto	closure = imported_closures.find(m);
					const bool	imported =
						closure != imported_closures.end() && closure->second.globals.count(&g) != 0;
					if (!g.is_pub && !imported) {
						continue;
					}
					// Mutable or address-significant globals must remain declarations
					// of the canonical owner-side object. Only immutable value data
					// may receive an available_externally initializer for folding.
					const bool canonical =
						!g.is_const || (closure != imported_closures.end() &&
										closure->second.canonical_globals.count(&g) != 0);
					if (canonical) {
						continue;
					}
					std::string nm = mangle_global(*m, g);
					auto		it = globals_.find(nm);
					if (it == globals_.end()) {
						continue;
					}
					llvm::GlobalVariable *gv   = it->second;
					StType				 *ty   = sema_.resolve_type(*g.ty, *m);
					llvm::Type			 *lt   = map_type(ty);
					llvm::Constant		 *init = lower_global_init(g, *m, ty, lt, nm);
					if (!init) {
						continue;
					}
					gv->setInitializer(init);
					gv->setLinkage(llvm::GlobalValue::AvailableExternallyLinkage);
				}
			};
			emit_imported_decls(m->decls);
		}
	}

	emit_tls_dynamic_initializers();

	// Whole-module verification catches structural issues that span functions
	// (e.g. invalid globals, broken metadata). Errors stream to stderr.
	if (llvm::verifyModule(l_.module(), &llvm::errs())) {
		log::report(log::Level::Error, {},
					"llvm module verification failed; see diagnostics above");
	}
	return !log::has_error();
}

void Codegen::declare_struct(StructDecl &s, Module &m) {
	(void)m;
	if (!s.resolved) {
		return;
	}
	llvm::StructType *st = llvm::cast<llvm::StructType>(map_named_struct(s.resolved));
	apply_struct_attrs(st, s);
}

void Codegen::declare_union(UnionDecl &u, Module &m) {
	(void)m;
	if (!u.resolved) {
		return;
	}
	(void)map_named_union(u.resolved);
}

void Codegen::declare_fn(FnDecl &f, Module &m) {
	if (f.is_variadic && !target_supports_va_lists()) {
		log::report(log::Level::Error, f.loc,
					"variadic functions have no supported va-list lowering for target '" +
						l_.module().getTargetTriple().str() + "'");
	}
	StType *ret =
		f.fn_sig ? f.fn_sig->fn_ret : (f.ret ? sema_.resolve_type(*f.ret, m) : t_.void_ty());
	std::vector<StType *> st_params;
	for (auto &p : f.params) {
		if (p.is_comptime) {
			continue;
		}
		StType *pt = p.resolved ? p.resolved : sema_.resolve_type(*p.ty, m);
		st_params.push_back(pt);
	}
	llvm::FunctionType *fnTy = nullptr;
	CAbiSignature		c_sig;
	if (f.abi == AbiKind::C) {
		c_sig = compute_c_abi_signature(st_params, ret, f.is_variadic);
		fnTy  = c_sig.fn_type;
	} else {
		std::vector<llvm::Type *> params;
		params.reserve(st_params.size());
		for (StType *pt : st_params) {
			params.push_back(map_type(pt));
		}
		fnTy = llvm::FunctionType::get(map_type(ret), params, f.is_variadic);
	}
	std::string nm	 = fn_symbol_name(f);
	// `pub` is the source-level visibility boundary. A private Stella
	// function therefore has the same object-level linkage as a C `static`
	// function and cannot collide with a same-named function in another
	// translation unit. `extern` and `#[link_name]` remain externally linked
	// even without `pub`.
	const bool	explicit_external = f.is_pub || f.is_extern || has_attr(f.attrs, "link_name") ||
									  (f.is_variadic && !target_supports_va_lists());
	auto	   *fn				  = llvm::Function::Create(
		fnTy, explicit_external ? llvm::Function::ExternalLinkage : llvm::Function::InternalLinkage,
		nm, l_.module());
	fn->setCallingConv(f.abi == AbiKind::C ? llvm::CallingConv::C : llvm::CallingConv::Fast);
	if (f.abi == AbiKind::C) {
		apply_c_abi_attrs(fn, c_sig);
	}
	if (f.is_generic_instance) {
		fn->setLinkage(llvm::GlobalValue::LinkOnceODRLinkage);
	}
	fn->addFnAttr(llvm::Attribute::NoUnwind);
	fns_[nm] = fn;
	unsigned arg_idx = 0;
	if (f.abi == AbiKind::C && c_sig.ret.kind == CAbiArgInfo::Kind::SRet &&
		arg_idx < fn->arg_size()) {
		fn->getArg(arg_idx)->setName("sret");
		++arg_idx;
	}
	size_t abi_pi = 0;
	for (size_t pi = 0; pi < f.params.size(); ++pi) {
		if (f.params[pi].is_comptime) {
			continue;
		}
		if (f.abi == AbiKind::C && abi_pi < c_sig.params.size() &&
			c_sig.params[abi_pi].kind == CAbiArgInfo::Kind::Ignore) {
			++abi_pi;
			continue;
		}
		if (arg_idx < fn->arg_size()) {
			fn->getArg(arg_idx)->setName(f.params[pi].name);
			if (has_negated_attr(f.params[pi].attrs, "alias")) {
				fn->addParamAttr(arg_idx, llvm::Attribute::NoAlias);
			}
			++arg_idx;
		}
		++abi_pi;
	}
	apply_fn_attrs(fn, f.attrs);
}

void Codegen::emit_fn_body(FnDecl &f, Module &m) {
	std::string nm = fn_symbol_name(f);
	auto		it = fns_.find(nm);
	if (it == fns_.end()) {
		return;
	}
	llvm::Function *fn = it->second;
	if (!f.body) {
		return;
	}
	if (f.is_variadic && !target_supports_va_lists()) {
		// declare_fn already emitted the user-facing target diagnostic. Do not
		// manufacture IR using an unsized placeholder va-list type.
		return;
	}
	llvm::BasicBlock *bb = llvm::BasicBlock::Create(l_.ctx(), "entry", fn);
	llvm::IRBuilder<> builder(l_.ctx());
	builder.SetInsertPoint(bb);

	local_stack_.clear();
	local_stack_.emplace_back();
	builder_	 = &builder;
	cur_fn_		 = &f;
	cur_llvm_fn_ = fn;
	sink_block_	 = nullptr; // per-function dead-pool block (opt 7)
	va_list_storage_.clear();
	defer_scopes_.clear();
	defer_scopes_.emplace_back();
	break_stack_.clear();
	continue_stack_.clear();
	block_expr_stack_.clear();
	abi_result_expr_ = nullptr;
	sret_ptr_		 = nullptr;

	StType *ret =
		f.fn_sig ? f.fn_sig->fn_ret : (f.ret ? sema_.resolve_type(*f.ret, m) : t_.void_ty());
	std::vector<StType *> st_params;
	for (auto &p : f.params) {
		if (!p.is_comptime) {
			st_params.push_back(p.resolved ? p.resolved : sema_.resolve_type(*p.ty, m));
		}
	}
	CAbiSignature c_sig;
	unsigned	  arg_idx = 0;
	if (f.abi == AbiKind::C) {
		c_sig = compute_c_abi_signature(st_params, ret, f.is_variadic);
		if (c_sig.ret.kind == CAbiArgInfo::Kind::SRet && arg_idx < fn->arg_size()) {
			sret_ptr_ = fn->getArg(arg_idx++);
		}
	}

	size_t abi_pi = 0;
	for (size_t idx = 0; idx < f.params.size(); ++idx) {
		if (f.params[idx].is_comptime) {
			continue;
		}
		StType		*pt	  = st_params[abi_pi];
		llvm::Type	*plt  = map_type(pt);
		llvm::Value *slot = alloca_in_entry_typed(pt, plt, f.params[idx].name.c_str());
		if (f.abi == AbiKind::C && abi_pi < c_sig.params.size()) {
			const auto &pi_info = c_sig.params[abi_pi];
			if (pi_info.kind == CAbiArgInfo::Kind::Ignore) {
				builder.CreateStore(llvm::Constant::getNullValue(plt), slot);
			} else if (arg_idx < fn->arg_size()) {
				llvm::Value *incoming = fn->getArg(arg_idx++);
				if (pi_info.kind == CAbiArgInfo::Kind::ByVal ||
					pi_info.kind == CAbiArgInfo::Kind::Indirect) {
					const auto &dl	  = l_.module().getDataLayout();
					llvm::Align align = dl.getABITypeAlign(plt);
					llvm::Value *loaded = builder.CreateAlignedLoad(plt, incoming, align);
					builder.CreateAlignedStore(loaded, slot, align);
				} else {
					if (incoming->getType() != plt) {
						incoming = coerce_abi_via_memory(builder, incoming, plt);
					}
					builder.CreateStore(incoming, slot);
				}
			}
		} else if (arg_idx < fn->arg_size()) {
			llvm::Value *incoming = fn->getArg(arg_idx++);
			if (incoming->getType() != plt) {
				incoming = coerce_abi_via_memory(builder, incoming, plt);
			}
			builder.CreateStore(incoming, slot);
		}
		if (f.params[idx].name != "_") {
			local_stack_.back().emplace(f.params[idx].name, LocalVal{slot, pt, true});
		}
		++abi_pi;
	}

	for (auto &st : f.body->body) {
		emit_stmt(*st, f, m);
	}
	// Returns, breaks, and continues park the builder on a synthetic sink so
	// later AST nodes can still be visited. If that sink is still empty and
	// unreachable when the body is complete, discard it before adding the
	// normal fallthrough return. Otherwise it would become a misleading
	// `ret undef` block in the emitted IR.
	if (builder.GetInsertBlock() == sink_block_ && sink_block_ &&
		!sink_block_->hasNPredecessorsOrMore(1) && sink_block_->empty()) {
		llvm::BasicBlock *dead = sink_block_;
		builder.ClearInsertionPoint();
		sink_block_ = nullptr;
		dead->eraseFromParent();
	}
	if (builder.GetInsertBlock() && !builder.GetInsertBlock()->hasTerminator() &&
		builder.GetInsertBlock() != sink_block_) {
		emit_active_defers(0, f, m);
	}
	if (builder.GetInsertBlock() && !builder.GetInsertBlock()->hasTerminator()) {
		if (has_negated_attr(f.attrs, "return")) {
			builder.CreateUnreachable();
		} else if (ret->kind == TypeKind::Void || sret_ptr_ != nullptr) {
			builder.CreateRetVoid();
		} else {
			builder.CreateRet(llvm::UndefValue::get(fn->getReturnType()));
		}
	}

#ifndef NDEBUG
	// Verify the freshly-built function. Catch malformed IR (mismatched PHI
	// types, dominates errors, bad incoming values...) now instead of letting
	// the LLVM backend silently drop or mis-compile code.
	std::string				 verr;
	llvm::raw_string_ostream vos(verr);
	if (llvm::verifyFunction(*fn, &vos)) {
		vos.flush();
		log::report(log::Level::Error, f.loc,
					"llvm function verification failed for '" + nm + "':\n" + verr);
	}
#endif

	cur_llvm_fn_ = nullptr;
	cur_fn_		 = nullptr;
	sink_block_	 = nullptr;
	sret_ptr_	 = nullptr;
	va_list_storage_.clear();
}

void Codegen::emit_global(GlobalDecl &g, Module &m) {
	if (g.is_extern) {
		declare_global(g, m);
		return;
	}
	StType		   *ty	 = sema_.resolve_type(*g.ty, m);
	llvm::Type	   *lt	 = map_type(ty);
	std::string		nm	 = mangle_global(m, g);
	llvm::Constant *init = lower_global_init(g, m, ty, lt, nm);
	const bool dynamic_tls_init = g.is_thread_local && !g.is_const && g.init && !init;
	if (g.init && !init && !dynamic_tls_init) {
		log::report(log::Level::Error, g.init->loc,
					"global initializer is not a compile-time constant");
	}
	llvm::Constant *fallback =
		lt->isVoidTy() ? llvm::UndefValue::get(lt) : llvm::Constant::getNullValue(lt);
	auto *gv = new llvm::GlobalVariable(
		l_.module(), lt, g.is_const || !g.is_mutable,
		global_has_external_name(g) ? llvm::GlobalValue::ExternalLinkage
										: llvm::GlobalValue::InternalLinkage,
		init ? init : fallback, nm);
	globals_[nm] = gv;
	if (g.is_thread_local) {
		gv->setThreadLocal(true);
	}
	apply_global_attrs(gv, g.attrs);
	if (dynamic_tls_init) {
		auto *guard = new llvm::GlobalVariable(
			l_.module(), l_.llvm_int(8), false, llvm::GlobalValue::InternalLinkage,
			llvm::ConstantInt::get(l_.llvm_int(8), 0), nm + ".tls_guard");
		guard->setThreadLocal(true);
		tls_init_guards_[nm] = guard;
		const bool externally_visible = g.is_pub || g.is_extern || has_attr(g.attrs, "link_name");
		auto *init_fn = llvm::Function::Create(
			llvm::FunctionType::get(llvm::Type::getVoidTy(l_.ctx()), false),
			externally_visible ? llvm::GlobalValue::ExternalLinkage
							   : llvm::GlobalValue::InternalLinkage,
			nm + ".tls_init", l_.module());
		tls_init_functions_[nm] = init_fn;
		tls_dynamic_initializers_.emplace_back(&g, &m);
	}
	if (!gv->getAlign().has_value()) {
		gv->setAlignment(storage_align(ty, lt));
	}
}

void Codegen::declare_global(GlobalDecl &g, Module &m) {
	// Imported public globals resolve to their owner object. Imported private
	// constants are declared only for available_externally optimization clones;
	// private mutable/address-sensitive globals are excluded from those clones.
	StType	   *ty = sema_.resolve_type(*g.ty, m);
	llvm::Type *lt = map_type(ty);
	std::string nm = mangle_global(m, g);
	auto	   *gv = new llvm::GlobalVariable(l_.module(), lt, g.is_const || !g.is_mutable,
											  llvm::GlobalValue::ExternalLinkage, nullptr, nm);
	globals_[nm]   = gv;
	if (g.is_thread_local) {
		gv->setThreadLocal(true);
		if (g.init && !g.is_const && !lower_global_init(g, m, ty, lt, nm)) {
			tls_init_functions_[nm] = llvm::Function::Create(
				llvm::FunctionType::get(llvm::Type::getVoidTy(l_.ctx()), false),
				llvm::GlobalValue::ExternalLinkage, nm + ".tls_init", l_.module());
		}
	}
	apply_global_attrs(gv, g.attrs);
	if (!gv->getAlign().has_value()) {
		gv->setAlignment(storage_align(ty, lt));
	}
}

void Codegen::ensure_tls_initialized(llvm::IRBuilder<> &builder, const std::string &name) {
	auto it = tls_init_functions_.find(name);
	if (it != tls_init_functions_.end()) {
		builder.CreateCall(it->second);
	}
}

void Codegen::emit_tls_dynamic_initializers() {
	for (const auto &[global, module] : tls_dynamic_initializers_) {
		if (!global || !module || !global->init) continue;
		const std::string name = mangle_global(*module, *global);
		auto init_it = tls_init_functions_.find(name);
		auto guard_it = tls_init_guards_.find(name);
		auto global_it = globals_.find(name);
		if (init_it == tls_init_functions_.end() || guard_it == tls_init_guards_.end() ||
			global_it == globals_.end()) continue;
		auto *fn = init_it->second;
		llvm::IRBuilder<> builder(l_.ctx());
		auto *entry = llvm::BasicBlock::Create(l_.ctx(), "entry", fn);
		auto *run = llvm::BasicBlock::Create(l_.ctx(), "initialize", fn);
		auto *done = llvm::BasicBlock::Create(l_.ctx(), "done", fn);
		builder.SetInsertPoint(entry);
		auto *state = builder.CreateLoad(l_.llvm_int(8), guard_it->second, "tls.state");
		auto *initialized = builder.CreateICmpNE(state, llvm::ConstantInt::get(l_.llvm_int(8), 0));
		builder.CreateCondBr(initialized, done, run);
		builder.SetInsertPoint(run);
		builder.CreateStore(llvm::ConstantInt::get(l_.llvm_int(8), 1), guard_it->second);

		FnDecl context;
		context.name = "<thread-local-init>";
		context.loc = global->loc;
		context.fn_sig = t_.function_of({}, t_.void_ty(), false);
		local_stack_.clear();
		local_stack_.emplace_back();
		builder_ = &builder;
		cur_fn_ = &context;
		cur_llvm_fn_ = fn;
		sink_block_ = nullptr;
		llvm::Value *value = emit_expr(*global->init, context, *module);
		if (value) {
			StType *ty = sema_.resolve_type(*global->ty, *module);
			store_value(global_it->second, value, ty, global->init->ty, global->loc);
		}
		builder.CreateStore(llvm::ConstantInt::get(l_.llvm_int(8), 2), guard_it->second);
		builder.CreateBr(done);
		builder.SetInsertPoint(done);
		builder.CreateRetVoid();
#ifndef NDEBUG
		std::string verification;
		llvm::raw_string_ostream stream(verification);
		if (llvm::verifyFunction(*fn, &stream)) {
			stream.flush();
			log::report(log::Level::Error, global->loc,
						"llvm function verification failed for thread-local initializer '" + name + "':\n" + verification);
		}
#endif
		builder_ = nullptr;
		cur_fn_ = nullptr;
		cur_llvm_fn_ = nullptr;
		local_stack_.clear();
	}
}

llvm::Constant *Codegen::lower_const_expr(Expr &e, Module &m, StType *expected_ty,
										  llvm::Type *expected_lt, const std::string &nm) {
	StType			 *target_ty = underlying_type(expected_ty);
	llvm::Type		 *target_lt = expected_lt ? expected_lt : map_type(target_ty);
	llvm::IRBuilder<> folder(l_.ctx());

	auto			  cast_const = [&](llvm::Constant *v, StType *src_ty, StType *dst_ty,
									   llvm::Type *dst_lt) -> llvm::Constant			  *{
		if (!v || !dst_lt) {
			return nullptr;
		}
		llvm::Type *src_lt = v->getType();
		if (src_lt == dst_lt) {
			return v;
		}
		StType *src = underlying_type(src_ty);
		StType *dst = underlying_type(dst_ty);
		return llvm::dyn_cast<llvm::Constant>(coerce(folder, v, dst_lt, src, dst));
	};

	auto function_constant = [&](FnDecl *function) -> llvm::Constant * {
		if (!function || !function->generic_params.empty()) {
			return nullptr;
		}
		std::string function_name = fn_symbol_name(*function);
		auto			 it = fns_.find(function_name);
		if (it == fns_.end() && function->decl_module) {
			// Globals may precede their function declarations in the source.
			// Materialize the declaration on demand so the function address is
			// still available as an LLVM constant.
			declare_fn(*function, *function->decl_module);
			it = fns_.find(function_name);
		}
		return it == fns_.end() ? nullptr : it->second;
	};

	switch (e.kind) {
	case ExprKind::IntLit:
	case ExprKind::CharLit:
		return target_lt->isIntegerTy() ? llvm::ConstantInt::get(target_lt, e.int_val, false)
										: nullptr;
	case ExprKind::BoolLit:
		return target_lt->isIntegerTy()
				   ? llvm::ConstantInt::get(target_lt, e.const_bool_val ? 1 : 0)
				   : nullptr;
	case ExprKind::FloatLit:
		return target_lt->isFloatingPointTy() ? llvm::ConstantFP::get(target_lt, e.flt_val)
											  : nullptr;
	case ExprKind::StringLit:
		if (target_ty && target_ty->kind == TypeKind::Array) {
			StType *elem = underlying_type(target_ty->base);
			const size_t expected_len = e.str_val.size() + (e.is_cstring ? 1 : 0);
			if (elem && elem->kind == TypeKind::UInt && elem->width == 8 &&
				target_ty->array_len == expected_len) {
				return llvm::ConstantDataArray::getString(l_.ctx(), e.str_val, e.is_cstring);
			}
			return nullptr;
		}
		if (e.is_cstring) {
			if (!target_lt->isPointerTy()) {
				return nullptr;
			}
			return cstring_global(e.str_val, nm + ".str");
		}
		if (!target_ty || target_ty->kind != TypeKind::Slice) {
			return nullptr;
		}
		return slice_string_global(e.str_val, nm + ".str");
	case ExprKind::NullLit:
		return target_lt->isPointerTy()
				   ? llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(target_lt))
				   : nullptr;
	case ExprKind::EnumMember:
		if (target_lt->isIntegerTy()) {
			StType *enum_type = underlying_type(e.ty);
			StType *backing = enum_type ? underlying_type(enum_type->base) : nullptr;
			return llvm::ConstantInt::get(target_lt, e.int_val,
										   is_signed_integer(backing));
		}
		return nullptr;
	case ExprKind::Ident:
		if (target_ty && target_ty->kind == TypeKind::Enum && target_lt->isIntegerTy()) {
			StType *backing = underlying_type(target_ty->base);
			const bool sign_extend = e.has_const_value
				? e.const_int_high_bits_ones
				: is_signed_integer(backing);
			return constant_int_from_bits(llvm::cast<llvm::IntegerType>(target_lt), e.int_val,
										  sign_extend);
		}
		if (e.resolved_fn) {
			return function_constant(e.resolved_fn);
		}
		if (e.has_const_value) {
			if (e.const_value_is_float && target_lt->isFloatingPointTy()) {
				return llvm::ConstantFP::get(target_lt, e.flt_val);
			}
			if (!e.const_value_is_float && target_lt->isIntegerTy()) {
				return constant_int_from_bits(
					llvm::cast<llvm::IntegerType>(target_lt),
					e.is_const_bool ? (e.const_bool_val ? 1 : 0) : e.int_val,
					e.const_int_high_bits_ones);
			}
		}
		return nullptr;
	case ExprKind::Cast: {
		StType		   *from_ty = e.lhs ? e.lhs->ty : nullptr;
		llvm::Type	   *from_lt = map_type(underlying_type(from_ty));
		llvm::Constant *v = e.lhs ? lower_const_expr(*e.lhs, m, from_ty, from_lt, nm) : nullptr;
		StType		   *cast_ty = e.ty ? e.ty : expected_ty;
		llvm::Type	   *cast_lt = map_type(cast_ty);
		if (!v) {
			return nullptr;
		}
		if (e.is_bitcast) {
			if (from_lt->isPointerTy() && cast_lt->isFloatingPointTy()) {
				llvm::Value *as_int =
					folder.CreatePtrToInt(v, l_.llvm_int(t_.pointer_width()));
				return llvm::dyn_cast<llvm::Constant>(folder.CreateBitCast(as_int, cast_lt));
			}
			if (from_lt->isFloatingPointTy() && cast_lt->isPointerTy()) {
				llvm::Value *as_int =
					folder.CreateBitCast(v, l_.llvm_int(t_.pointer_width()));
				return llvm::dyn_cast<llvm::Constant>(folder.CreateIntToPtr(as_int, cast_lt));
			}
			if (from_lt->isPointerTy() && cast_lt->isIntegerTy()) {
				return llvm::dyn_cast<llvm::Constant>(folder.CreatePtrToInt(v, cast_lt));
			}
			if (from_lt->isIntegerTy() && cast_lt->isPointerTy()) {
				return llvm::dyn_cast<llvm::Constant>(folder.CreateIntToPtr(v, cast_lt));
			}
			return llvm::dyn_cast<llvm::Constant>(folder.CreateBitCast(v, cast_lt));
		}
		return cast_const(v, from_ty, cast_ty, cast_lt);
	}
	case ExprKind::Index: {
		if (!e.is_bit_range || !e.lhs || !target_lt->isIntegerTy()) {
			return nullptr;
		}
		auto *value = lower_const_expr(*e.lhs, m, e.lhs->ty,
										map_type(underlying_type(e.lhs->ty)), nm);
		auto *integer = llvm::dyn_cast_or_null<llvm::ConstantInt>(value);
		if (!integer) {
			return nullptr;
		}
		llvm::APInt bits = integer->getValue().lshr(static_cast<unsigned>(e.bit_range_low));
		bits = bits.trunc(llvm::cast<llvm::IntegerType>(target_lt)->getBitWidth());
		return llvm::ConstantInt::get(l_.ctx(), bits);
	}
	case ExprKind::Unary: {
		if (!e.lhs) {
			return nullptr;
		}
		StType		   *child_ty = e.lhs->ty;
		llvm::Type	   *child_lt = map_type(underlying_type(child_ty));
		llvm::Constant *v		 = lower_const_expr(*e.lhs, m, child_ty, child_lt, nm);
		if (!v) {
			return nullptr;
		}
		if (e.str_val == "~" && v->getType()->isIntegerTy()) {
			return llvm::dyn_cast<llvm::Constant>(folder.CreateNot(v));
		}
		if (e.str_val == "-" && v->getType()->isIntegerTy()) {
			return llvm::dyn_cast<llvm::Constant>(folder.CreateNeg(v));
		}
		if (e.str_val == "-" && v->getType()->isFloatingPointTy()) {
			return llvm::dyn_cast<llvm::Constant>(folder.CreateFNeg(v));
		}
		if (e.str_val == "!" && v->getType()->isIntegerTy()) {
			return llvm::dyn_cast<llvm::Constant>(
				folder.CreateICmpEQ(v, llvm::ConstantInt::get(v->getType(), 0)));
		}
		return nullptr;
	}
	case ExprKind::Conditional: {
		// A constant condition is folded here so dead arms are not even
		// referenced by a global initializer.
		if (!e.condition || !e.condition->is_const_bool) {
			return nullptr;
		}
		Expr *selected = e.condition->const_bool_val ? e.then_expr.get() : e.else_expr.get();
		if (!selected) {
			return nullptr;
		}
		return lower_const_expr(*selected, m, expected_ty ? expected_ty : e.ty,
								expected_lt ? expected_lt : map_type(underlying_type(e.ty)), nm);
	}
	case ExprKind::Binary: {
		if (!e.lhs || !e.rhs) {
			return nullptr;
		}
		StType		   *result_ty = e.ty ? e.ty : expected_ty;
		llvm::Type	   *result_lt = map_type(underlying_type(result_ty));
		llvm::Constant *l =
			lower_const_expr(*e.lhs, m, e.lhs->ty, map_type(underlying_type(e.lhs->ty)), nm);
		llvm::Constant *r =
			lower_const_expr(*e.rhs, m, e.rhs->ty, map_type(underlying_type(e.rhs->ty)), nm);
		if (!l || !r) {
			return nullptr;
		}
		l = cast_const(l, e.lhs->ty, result_ty, result_lt);
		r = cast_const(r, e.rhs->ty, result_ty, result_lt);
		if (!l || !r) {
			return nullptr;
		}
		const std::string &op = e.str_val;
		if (result_lt->isFloatingPointTy()) {
			if (op == "+") {
				return llvm::dyn_cast<llvm::Constant>(folder.CreateFAdd(l, r));
			}
			if (op == "-") {
				return llvm::dyn_cast<llvm::Constant>(folder.CreateFSub(l, r));
			}
			if (op == "*") {
				return llvm::dyn_cast<llvm::Constant>(folder.CreateFMul(l, r));
			}
			if (op == "/") {
				return llvm::dyn_cast<llvm::Constant>(folder.CreateFDiv(l, r));
			}
			if (op == "%") {
				return llvm::dyn_cast<llvm::Constant>(folder.CreateFRem(l, r));
			}
		} else if (result_lt->isIntegerTy()) {
			bool sign = is_signed_integer(result_ty);
			if (op == "+%") {
				return llvm::dyn_cast<llvm::Constant>(folder.CreateAdd(l, r));
			}
			if (op == "-%") {
				return llvm::dyn_cast<llvm::Constant>(folder.CreateSub(l, r));
			}
			if (op == "*%") {
				return llvm::dyn_cast<llvm::Constant>(folder.CreateMul(l, r));
			}
			if (op == "*|") {
				auto *li = llvm::dyn_cast<llvm::ConstantInt>(l);
				auto *ri = llvm::dyn_cast<llvm::ConstantInt>(r);
				if (!li || !ri) {
					return nullptr;
				}
				const unsigned width = li->getBitWidth();
				const llvm::APInt lhs = li->getValue();
				const llvm::APInt rhs = ri->getValue();
				bool			  ov  = false;
				llvm::APInt		  result(width, 0);
				if (sign) {
					const llvm::APInt prod = lhs.smul_ov(rhs, ov);
					result = ov ? ((lhs.isNegative() != rhs.isNegative())
									   ? llvm::APInt::getSignedMinValue(width)
									   : llvm::APInt::getSignedMaxValue(width))
								: prod;
				} else {
					const llvm::APInt prod = lhs.umul_ov(rhs, ov);
					result = ov ? llvm::APInt::getMaxValue(width) : prod;
				}
				return llvm::ConstantInt::get(l_.ctx(), result);
			}
			if (op == "+|" || op == "-|") {
				auto *li = llvm::dyn_cast<llvm::ConstantInt>(l);
				auto *ri = llvm::dyn_cast<llvm::ConstantInt>(r);
				if (!li || !ri) {
					return nullptr;
				}
				const unsigned width = li->getBitWidth();
				llvm::APInt lhs = li->getValue();
				llvm::APInt rhs = ri->getValue();
				llvm::APInt result(width, 0);
				if (sign) {
					auto lhs_ext = lhs.sext(width + 1);
					auto rhs_ext = rhs.sext(width + 1);
					auto extended = op == "+|" ? lhs_ext + rhs_ext : lhs_ext - rhs_ext;
					auto minimum = llvm::APInt::getSignedMinValue(width).sext(width + 1);
					auto maximum = llvm::APInt::getSignedMaxValue(width).sext(width + 1);
					if (extended.slt(minimum)) result = llvm::APInt::getSignedMinValue(width);
					else if (extended.sgt(maximum)) result = llvm::APInt::getSignedMaxValue(width);
					else result = extended.trunc(width);
				} else if (op == "+|") {
					auto extended = lhs.zext(width + 1) + rhs.zext(width + 1);
					result = extended[width] ? llvm::APInt::getMaxValue(width) : extended.trunc(width);
				} else {
					result = lhs.ult(rhs) ? llvm::APInt(width, 0) : lhs - rhs;
				}
				return llvm::ConstantInt::get(l_.ctx(), result);
			}
			if (op == "+") {
				return llvm::dyn_cast<llvm::Constant>(folder.CreateAdd(l, r));
			}
			if (op == "-") {
				return llvm::dyn_cast<llvm::Constant>(folder.CreateSub(l, r));
			}
			if (op == "*") {
				return llvm::dyn_cast<llvm::Constant>(folder.CreateMul(l, r));
			}
			if (op == "/") {
				return llvm::dyn_cast<llvm::Constant>(sign ? folder.CreateSDiv(l, r)
														   : folder.CreateUDiv(l, r));
			}
			if (op == "%") {
				return llvm::dyn_cast<llvm::Constant>(sign ? folder.CreateSRem(l, r)
														   : folder.CreateURem(l, r));
			}
			if (op == "&") {
				return llvm::dyn_cast<llvm::Constant>(folder.CreateAnd(l, r));
			}
			if (op == "|") {
				return llvm::dyn_cast<llvm::Constant>(folder.CreateOr(l, r));
			}
			if (op == "^") {
				return llvm::dyn_cast<llvm::Constant>(folder.CreateXor(l, r));
			}
			if (op == "<<") {
				return llvm::dyn_cast<llvm::Constant>(folder.CreateShl(l, r));
			}
			if (op == ">>") {
				return llvm::dyn_cast<llvm::Constant>(sign ? folder.CreateAShr(l, r)
														   : folder.CreateLShr(l, r));
			}
		}
		return nullptr;
	}
	case ExprKind::ArrayLit: {
		if (target_ty && target_ty->kind == TypeKind::Slice && e.args.empty()) {
			return llvm::ConstantAggregateZero::get(target_lt);
		}
		if (!target_ty || target_ty->kind != TypeKind::Array || !target_lt->isArrayTy()) {
			return nullptr;
		}
		auto						 *arr_ty = llvm::cast<llvm::ArrayType>(target_lt);
		std::vector<llvm::Constant *> values;
		for (auto &arg : e.args) {
			llvm::Constant *value =
				lower_const_expr(*arg, m, target_ty->base, map_type(underlying_type(target_ty->base)), nm);
			if (!value) {
				return nullptr;
			}
			values.push_back(value);
		}
		while (values.size() < arr_ty->getNumElements()) {
			values.push_back(llvm::ConstantAggregateZero::get(arr_ty->getElementType()));
		}
		if (values.size() != arr_ty->getNumElements()) {
			return nullptr;
		}
		return llvm::ConstantArray::get(arr_ty, values);
	}
	case ExprKind::StructLit: {
		StType *aggregate = underlying_type(target_ty ? target_ty : e.ty);
		if (aggregate && aggregate->kind == TypeKind::Union && target_lt->isStructTy()) {
			StType *u		 = aggregate;
			auto   *union_ty = llvm::cast<llvm::StructType>(target_lt);
			if (e.field_names.size() != 1 || e.args.size() != 1) {
				return nullptr;
			}
			size_t idx = field_index(u, e.field_names[0]);
			if (idx == SIZE_MAX) {
				return nullptr;
			}
			llvm::Constant *member = lower_const_expr(*e.args[0], m, u->fields[idx],
													  map_type(underlying_type(u->fields[idx])), nm);
			if (!member) {
				return nullptr;
			}
			llvm::Type	   *carrier		  = union_ty->getElementType(0);
			llvm::Constant *carrier_value = nullptr;
			if (member->getType() == carrier) {
				carrier_value = member;
			} else if (auto *ca = llvm::dyn_cast<llvm::ArrayType>(carrier)) {
				if (ca->getElementType() == member->getType() && ca->getNumElements() > 0) {
					std::vector<llvm::Constant *> elems;
					elems.push_back(member);
					while (elems.size() < ca->getNumElements()) {
						elems.push_back(llvm::ConstantAggregateZero::get(ca->getElementType()));
					}
					carrier_value = llvm::ConstantArray::get(ca, elems);
				}
			} else {
				auto	&dl			  = l_.module().getDataLayout();
				uint64_t member_bits  = dl.getTypeAllocSize(member->getType()) * 8;
				uint64_t carrier_bits = dl.getTypeAllocSize(carrier) * 8;
				if (member_bits && carrier_bits >= member_bits) {
					auto		   *mi = llvm::IntegerType::get(l_.ctx(), member_bits);
					auto		   *ci = llvm::IntegerType::get(l_.ctx(), carrier_bits);
					llvm::Constant *bits =
						llvm::ConstantExpr::getCast(llvm::Instruction::BitCast, member, mi);
					if (carrier_bits > member_bits) {
						bits = llvm::ConstantExpr::getCast(llvm::Instruction::ZExt, bits, ci);
						if (!dl.isLittleEndian()) {
							auto *shift = llvm::ConstantInt::get(ci, carrier_bits - member_bits);
							bits = llvm::ConstantExpr::get(llvm::Instruction::Shl, bits, shift);
						}
					}
					carrier_value =
						llvm::ConstantExpr::getCast(llvm::Instruction::BitCast, bits, carrier);
				}
			}
			if (!carrier_value) {
				return nullptr;
			}
			std::vector<llvm::Constant *> fields;
			for (unsigned i = 0; i < union_ty->getNumElements(); ++i) {
				fields.push_back(llvm::ConstantAggregateZero::get(union_ty->getElementType(i)));
			}
			fields[0] = carrier_value;
			return llvm::ConstantStruct::get(union_ty, fields);
		}
		if (!aggregate || aggregate->kind != TypeKind::Struct || !target_lt->isStructTy()) {
			return nullptr;
		}
		StType						 *st		= aggregate;
		auto						 *struct_ty = llvm::cast<llvm::StructType>(target_lt);
		std::vector<llvm::Constant *> fields;
		for (unsigned i = 0; i < struct_ty->getNumElements(); ++i) {
			fields.push_back(llvm::ConstantAggregateZero::get(struct_ty->getElementType(i)));
		}
		for (size_t i = 0; i < e.field_names.size(); ++i) {
			size_t idx = field_index(st, e.field_names[i]);
			if (idx == SIZE_MAX || i >= e.args.size()) {
				return nullptr;
			}
			llvm::Constant *value = lower_const_expr(*e.args[i], m, st->fields[idx],
													 map_type(underlying_type(st->fields[idx])), nm);
			if (!value) {
				return nullptr;
			}
			fields[idx] = value;
		}
		return llvm::ConstantStruct::get(struct_ty, fields);
	}
	case ExprKind::Sizeof:
	case ExprKind::Alignof:
	case ExprKind::Bitsof:
	case ExprKind::Offsetof: {
		StType *st	   = layout_operand_type(e.lhs.get());
		st			   = underlying_type(st);
		uint64_t value = 0;
		bool	 layout_value_valid = true;
		if (st) {
			auto	   &dl	  = l_.module().getDataLayout();
			llvm::Type *st_lt = map_type(st);
			if (e.kind == ExprKind::Sizeof) {
				value = dl.getTypeAllocSize(st_lt);
			} else if (e.kind == ExprKind::Alignof) {
				value = std::max<uint64_t>(dl.getABITypeAlign(st_lt).value(), st->explicit_align);
			} else if (e.kind == ExprKind::Bitsof) {
				layout_value_valid = bitsof_type(st, t_.pointer_width(), dl, st_lt, value);
			} else {
				value = 0; // all union members, and the first struct member, are handled below
			}
			if (e.kind == ExprKind::Offsetof && st->kind == TypeKind::Struct &&
				st_lt->isStructTy()) {
				size_t idx = field_index(st, e.offsetof_member);
				if (idx != SIZE_MAX) {
					value = dl.getStructLayout(llvm::cast<llvm::StructType>(st_lt))
								->getElementOffset(idx);
				}
			}
		}
		if (!layout_value_valid || !fits_target_usize(value, t_.pointer_width())) {
			const char *intrinsic = e.kind == ExprKind::Sizeof	? "@of.size"
										: e.kind == ExprKind::Alignof ? "@of.align"
										: e.kind == ExprKind::Bitsof	? "@of.bits"
															: "@of.offset";
			log::report(log::Level::Error, e.loc,
						std::string(intrinsic) + " result does not fit the target usize type");
			return nullptr;
		}
		return target_lt->isIntegerTy() ? llvm::ConstantInt::get(target_lt, value, false) : nullptr;
	}
	default:
		return nullptr;
	}
}

llvm::Constant *Codegen::lower_global_init(GlobalDecl &g, Module &m, StType *ty, llvm::Type *&lt,
										   const std::string &nm) {
	if (!g.init) {
		return nullptr;
	}
	return lower_const_expr(*g.init, m, ty, lt, nm);
}

} // namespace stellac
