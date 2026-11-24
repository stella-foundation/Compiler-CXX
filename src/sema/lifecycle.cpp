#include "internal.hpp"

namespace stellac {

void Sema::prune_module_conditionals(Module &m) {
	prune_decls(m.decls, m);
}

void Sema::prune_decls(std::vector<TopDeclPtr> &decls, Module &m) {
	StaticConsts			consts;
	std::vector<TopDeclPtr> active_decls;
	for (auto &td : decls) {
		if (!td) {
			continue;
		}
		if (td->kind == TopKind::Conditional && td->conditional) {
			std::vector<TopDeclPtr> selected;
			bool					chosen = false;
			for (auto &arm : td->conditional->arms) {
				bool take = false;
				if (!arm.cond) {
					take = !chosen;
				} else {
					std::unordered_set<std::string> active_refs;
					auto value = eval_static_expr(*arm.cond, opts_.flags, consts, active_refs);
					if (!value || value->kind != StaticValue::Bool) {
						log::report(log::Level::Error, arm.cond->loc,
									"top-level if condition must be a compile-time bool");
					} else {
						take = value->b;
					}
				}
				if (take && !chosen) {
					chosen = true;
					for (auto &child : arm.body) {
						selected.push_back(std::move(child));
					}
				}
			}
			// Parse all arms, but only the selected arm reaches later phases.
			prune_decls(selected, m);
			for (auto &child : selected) {
				if (child && child->kind == TopKind::Mod && child->mod_decl &&
					!child->mod_decl->body.empty()) {
					prune_decls(child->mod_decl->body, m);
				}
				if (child && child->kind == TopKind::Const && child->glob && child->glob->init) {
					consts[child->glob->name] = child->glob->init.get();
				}
				active_decls.push_back(std::move(child));
			}
			continue;
		}
		if (td->kind == TopKind::Mod && td->mod_decl && !td->mod_decl->body.empty()) {
			prune_decls(td->mod_decl->body, m);
		}
		if (td->kind == TopKind::Const && td->glob && td->glob->init) {
			consts[td->glob->name] = td->glob->init.get();
		}
		active_decls.push_back(std::move(td));
	}
	decls = std::move(active_decls);
}

bool Sema::run() {
	// (0) load imports transitively, seeding from every root module of
	// the current translation unit (the entry file's top-level `struct` namespaces
	// -- typically just "main" for an executable, or the declared module(s)
	// for a `-c`/`--emit-ir` library build).
	std::unordered_set<std::string> loaded;
	std::vector<Module *>			queue;
	auto							roots = resolver_.root_modules();
	if (roots.empty()) {
		log::report(log::Level::Error, {}, "no root module");
		return false;
	}
	for (auto *m : roots) {
		prune_module_conditionals(*m);
		std::string k = m->fpath;
		if (loaded.insert(k).second) {
			queue.push_back(m);
		}
	}

	for (size_t i = 0; i < queue.size(); ++i) {
		Module *m = queue[i];
		std::function<void(const std::vector<TopDeclPtr> &)> load_nested_imports;
		load_nested_imports = [&](const std::vector<TopDeclPtr> &decls) {
			for (const auto &decl : decls) {
				if (!decl) continue;
				if (decl->kind == TopKind::Import && decl->imp) {
					ImportDecl &imp = *decl->imp;
					auto targets = resolver_.load_import_targets(imp, *m);
					imp.resolved = !targets.empty();
					for (Module *sub : targets) {
						if (loaded.insert(sub->fpath).second) {
							prune_module_conditionals(*sub);
							queue.push_back(sub);
						}
					}
					if (targets.empty()) {
						log::report(log::Level::Error, imp.loc,
								"source file `" + imp.written_path + "` could not be found",
								"E_SOURCE_NOT_FOUND", "resolver");
					}
				} else if (decl->kind == TopKind::Mod && decl->mod_decl) {
					load_nested_imports(decl->mod_decl->body);
				} else if (decl->kind == TopKind::Conditional && decl->conditional) {
					for (const auto &arm : decl->conditional->arms) load_nested_imports(arm.body);
				}
			}
		};
		load_nested_imports(m->decls);
	}

	// (1) symbol registration + type resolution, split into two sub-passes
	// so a root module's signature resolution can see imported modules'
	// symbol names + self-contained types (struct/enum) which are registered
	// in the same Phase 1a loop. Modules are queued roots-first (imports
	// later), so resolving fn signatures inline during 1a would race an
	// imported enum/struct not yet registered. 1a registers names + creates
	// struct/enum StTypes; 1b resolves fn signatures, type binding targets, and
	// var/const types once all names are visible.
	for (auto *m : queue) {
		register_module_symbols(*m);
	}
	visible_symbol_cache_ready_ = true;
	for (auto *m : queue) {
		resolve_module_types(*m);
	}
	validate_public_namespace_surface();
	for (auto *m : queue) {
		resolve_aggregate_fields(*m);
	}
	validate_aggregate_layouts();
	validate_trait_conformance();
	// Resolver/type-registration failures invalidate the body graph. Stop here
	// so unused-name analysis cannot turn an import failure into extra noise.
	if (log::has_error()) {
		return false;
	}

	// (2) typecheck bodies.
	for (auto *m : queue) {
		check_module_bodies(*m);
	}
	// Unused-name warnings are derived from successful semantic use. Once an
	// import or another semantic error has invalidated that graph, emitting
	// those warnings only obscures the actionable diagnostic.
	if (log::has_error()) {
		return false;
	}
	for (auto *m : queue) {
		mark_deferred_body_uses(*m);
	}
	for (auto *m : queue) {
		mark_import_uses_in_source(*m);
	}
	for (auto *m : queue) {
		emit_unused_warnings(*m);
	}

	return !log::has_error();
}

} // namespace stellac
