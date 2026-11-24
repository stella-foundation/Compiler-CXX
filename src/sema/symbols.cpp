#include "internal.hpp"

namespace stellac {

static void collect_imports(const std::vector<TopDeclPtr> &decls,
							std::vector<ImportDecl *> &out) {
	for (const auto &decl : decls) {
		if (!decl) continue;
		if (decl->kind == TopKind::Import && decl->imp) {
			out.push_back(decl->imp.get());
		} else if (decl->kind == TopKind::Mod && decl->mod_decl) {
			collect_imports(decl->mod_decl->body, out);
		} else if (decl->kind == TopKind::Conditional && decl->conditional) {
			for (const auto &arm : decl->conditional->arms) collect_imports(arm.body, out);
		}
	}
}

static void apply_layout_requirements(StType *type, const AttributeList &attrs) {
	const Attribute *layout = find_attr(attrs, "layout");
	if (!type || !layout) return;
	type->layout_loc = layout->loc;
	for (size_t i = 0; i < layout->args.size(); ++i) {
		ParsedInt value = parse_int_literal(layout->args[i]);
		if (!value.ok || value.overflow) continue;
		if (i == 0) type->layout_size = value.value;
		if (i == 1) type->layout_align = value.value;
	}
}

void Sema::register_module_symbols(Module &m) {
	std::string mkey = source_key(m);
	auto	   &tbl	 = module_syms_[mkey];
	for (auto &td : m.decls) {
		Symbol s;
		s.owner_module = &m;
		s.module_path = m.mod_path;
		s.loc		  = {};
		switch (td->kind) {
		case TopKind::Import:
			validate_attributes(td->imp->attrs, TopKind::Import, td->imp->loc);
			break;
		case TopKind::Fn: {
			FnDecl &f	  = *td->fn;
			f.decl_module = &m;
			if (f.is_macro) {
				validate_macro_body(f);
			}
			validate_generic_params(f.generic_params);
			if (!f.is_macro && !f.generic_params.empty() && (f.is_extern || has_attr(f.attrs, "link_name"))) {
				log::report(log::Level::Error, f.loc,
							"generic extern/link_name functions are not supported");
			}
			validate_attributes(f.attrs, TopKind::Fn, f.loc);
			std::vector<std::string> segs = m.mod_path;
			for (auto &o : f.owner_path) {
				segs.push_back(o);
			}
			segs.push_back(f.name);
			f.full_path = segs;

			std::vector<std::string> parts = f.owner_path;
			parts.push_back(f.name);
			std::string key = join_path(parts);

			auto		it = tbl.find(key);
			if (it != tbl.end()) {
				if (it->second.kind != SymKind::Fn) {
					log::report(log::Level::Error, f.loc,
								"duplicate symbol '" + key +
									"' (already declared as non-function)");
				} else {
					it->second.overloads.push_back(&f);
					if (f.is_pub) {
						it->second.is_pub = true;
					}
					f.sym = &it->second;
				}
			} else {
				Symbol sym_entry;
				sym_entry.owner_module = &m;
				sym_entry.kind	 = SymKind::Fn;
				sym_entry.simple = f.name;
				sym_entry.fn	 = &f;
				sym_entry.overloads.push_back(&f);
				sym_entry.is_pub	  = f.is_pub;
				sym_entry.loc		  = f.loc;
				sym_entry.module_path = m.mod_path;
				auto res			  = tbl.emplace(key, sym_entry);
				f.sym				  = &res.first->second;
				if (f.owner_path.empty() && key != f.name) {
					tbl.emplace(f.name, sym_entry);
				}
			}
			break;
		}
		case TopKind::Struct: {
			StructDecl &st = *td->st;
			validate_generic_params(st.generic_params);
			validate_attributes(st.attrs, TopKind::Struct, st.loc);
			std::string mn = mangle_in(m, st.name);
			s.kind		   = SymKind::Struct;
			s.simple	   = st.name;
			s.st		   = &st;
			s.is_pub	   = st.is_pub;
			s.mangled	   = mn;
			s.loc		   = st.loc;
			// Build StType now (fields resolved later when types resolve).
			std::vector<StType *> ftypes(st.field_types.size(), nullptr);
			st.resolved = types_.struct_of(mn, st.field_names, ftypes, st.is_pub, st.field_is_pub);
			st.resolved->is_opaque   = st.is_opaque;
			st.resolved->repr		 = resolve_repr_attr(st.attrs);
			st.resolved->decl_module = &m;
			st.resolved->scope_path  = {st.name};
			apply_layout_requirements(st.resolved, st.attrs);
			if (const Attribute *al = find_attr(st.attrs, "align")) {
				if (auto alignment = valid_alignment_value(*al)) {
					st.resolved->explicit_align = *alignment;
				}
			}
			s.type = st.resolved;
			auto ins = tbl.emplace(st.name, s);
			if (!ins.second) {
				log::report(log::Level::Error, st.loc, "duplicate symbol '" + st.name + "'");
			} else {
				st.resolved->generic_origin = &ins.first->second;
			}
			break;
		}
		case TopKind::Mod: {
			ModDecl &md = *td->mod_decl;
			validate_attributes(md.attrs, TopKind::Mod, md.loc);
			auto it = tbl.find(md.name);
			if (it != tbl.end()) {
				if (it->second.kind != SymKind::Mod) {
					log::report(log::Level::Error, md.loc, "duplicate symbol '" + md.name + "'");
				} else {
					it->second.is_pub = it->second.is_pub || md.is_pub;
				}
			} else {
				s.kind	   = SymKind::Mod;
				s.simple   = md.name;
				s.mod_decl = &md;
				s.is_pub   = md.is_pub;
				s.mangled  = mangle_in(m, md.name);
				s.loc	   = md.loc;
				tbl.emplace(md.name, s);
			}
			break;
		}
		case TopKind::Union: {
			UnionDecl &u = *td->un;
			validate_generic_params(u.generic_params);
			validate_attributes(u.attrs, TopKind::Union, u.loc);
			std::string mn = mangle_in(m, u.name);
			s.kind		   = SymKind::Union;
			s.simple	   = u.name;
			s.un		   = &u;
			s.is_pub	   = u.is_pub;
			s.mangled	   = mn;
			s.loc		   = u.loc;
			std::vector<StType *> ftypes(u.field_types.size(), nullptr);
			u.resolved = types_.union_of(mn, u.field_names, ftypes, u.is_pub, u.field_is_pub);
			u.resolved->repr		= resolve_repr_attr(u.attrs);
			u.resolved->decl_module = &m;
			u.resolved->scope_path  = {u.name};
			s.type	   = u.resolved;
			if (const Attribute *al = find_attr(u.attrs, "align")) {
				if (auto alignment = valid_alignment_value(*al)) {
					u.resolved->explicit_align = *alignment;
				}
			}
			auto ins = tbl.emplace(u.name, s);
			if (!ins.second) {
				log::report(log::Level::Error, u.loc, "duplicate symbol '" + u.name + "'");
			} else {
				u.resolved->generic_origin = &ins.first->second;
			}
			break;
		}
		case TopKind::TypeBinding: {
			TypeBindingDecl &tb = *td->tb;
			validate_generic_params(tb.generic_params);
			validate_attributes(tb.attrs, TopKind::TypeBinding, tb.loc);
			std::string mn = mangle_in(m, tb.name);
			s.kind		   = SymKind::TypeBinding;
			s.simple	   = tb.name;
			s.tb		   = &tb;
			s.is_pub	   = tb.is_pub;
			s.mangled	   = mn;
			s.loc		   = tb.loc;
			// Target type resolution deferred to Phase 1b: a cross-module
			// type binding target (e.g. `type Foo = std.Bar`) needs the imported
			// module's symbols, which may not be registered yet in 1a.
			if (!tbl.emplace(tb.name, s).second) {
				log::report(log::Level::Error, tb.loc, "duplicate symbol '" + tb.name + "'");
			}
			break;
		}
		case TopKind::Var:
		case TopKind::Const: {
			GlobalDecl &g = *td->glob;
			validate_attributes(g.attrs, td->kind, g.loc);
			std::string mn = mangle_global(m, g);
			s.kind		   = g.is_const ? SymKind::Const : SymKind::Var;
			s.is_mutable = g.is_mutable;
			s.simple	   = g.name;
			s.is_pub	   = g.is_pub;
			s.mangled	   = mn;
			s.loc		   = g.loc;
			s.usage		   = &g.used;
			s.glob		   = &g;
			// Type resolution deferred to Phase 1b so cross-module type
			// references in a global's declared type see imported symbols.
			if (!tbl.emplace(g.name, s).second) {
				log::report(log::Level::Error, g.loc, "duplicate symbol '" + g.name + "'");
			}
			break;
		}
		case TopKind::Enum: {
			EnumDecl &en = *td->en;
			validate_generic_params(en.generic_params);
			validate_attributes(en.attrs, TopKind::Enum, en.loc);
			std::string mn = mangle_in(m, en.name);
			StType *backing = nullptr;
			if (en.backing) {
				StType *bt = underlying_type(resolve_type(*en.backing, m));
				if (bt && (bt->kind == TypeKind::SInt || bt->kind == TypeKind::UInt)) {
					backing = bt;
				} else if (bt) {
					log::report(log::Level::Error, en.backing->loc,
								"enum backing type must be an integer, got '" + bt->name + "'");
				}
			} else {
				log::report(log::Level::Error, en.loc,
							"enum '" + en.name +
								"' requires an explicit backing type (e.g. 'enum " + en.name +
								": u32 { ... }')");
			}
			if (!backing) {
				backing = types_.sint(32);
			}
			std::vector<std::string>	 names;
			std::vector<uint64_t>		 values;
			std::vector<EnumVariantInfo> variants;
			std::unordered_set<uint64_t> seen_values;
			bool						 is_tagged = false;
			names.reserve(en.members.size());
			values.reserve(en.members.size());
			variants.reserve(en.members.size());
			uint64_t	   next = 0;
			const int	   bw	= (backing->width <= 0) ? types_.pointer_width() : backing->width;
			const uint64_t mask = (bw >= 64) ? ~uint64_t{0} : ((uint64_t{1} << bw) - 1);
			for (size_t member_index = 0; member_index < en.members.size(); ++member_index) {
				auto &mb = en.members[member_index];
				for (size_t previous = 0; previous < names.size(); ++previous) {
					if (names[previous] == mb.name) {
						log::report(log::Level::Error, mb.loc,
									"duplicate enum member name '" + mb.name + "'");
						break;
					}
				}
				uint64_t v = next;
				if (mb.value) {
					if (is_const_int_expr(*mb.value)) {
						if (mb.value->kind == ExprKind::IntLit ||
							mb.value->kind == ExprKind::CharLit) {
							v = mb.value->int_val;
						} else {
							v = static_cast<uint64_t>(eval_const_int(*mb.value));
						}
					} else {
						log::report(log::Level::Error, mb.loc,
									"enum member initializer must be a constant integer expression");
					}
				}
				mb.resolved = v;
				if (!enum_value_fits(backing, v, mb.value.get())) {
					log::report(log::Level::Error, mb.loc,
								"value of enum member '" + mb.name +
									"' does not fit backing type '" + backing->name + "'");
				} else if (!seen_values.insert(v & mask).second) {
					log::report(log::Level::Error, mb.loc,
								"duplicate enum discriminant value for member '" + mb.name + "'");
				}
				if (!next_enum_value(backing, v, next)) {
					if (member_index + 1 < en.members.size() &&
						!en.members[member_index + 1].value) {
						log::report(log::Level::Error, mb.loc,
									"enum member '" + mb.name + "' value exhausts backing type '" +
										backing->name +
										"'; the next member cannot be auto-incremented");
					}
					next = v;
				}
				EnumVariantInfo vi;
				vi.payload_kind = mb.payload_kind;
				if (mb.payload_kind != EnumPayloadKind::Unit) {
					is_tagged = true;
				}
				names.push_back(mb.name);
				values.push_back(v);
				variants.push_back(std::move(vi));
			}
			en.resolved = types_.enum_of(mn, backing, std::move(names), std::move(values),
										 en.is_pub, is_tagged, std::move(variants));
			en.resolved->repr = resolve_repr_attr(en.attrs);
			if (const Attribute *al = find_attr(en.attrs, "align")) {
				if (auto alignment = valid_alignment_value(*al)) {
					en.resolved->explicit_align = *alignment;
				}
			}
			apply_layout_requirements(en.resolved, en.attrs);
			en.resolved->decl_module = &m;
			en.resolved->scope_path  = {en.name};
			s.kind					 = SymKind::Enum;
			s.simple				 = en.name;
			s.en					 = &en;
			s.is_pub				 = en.is_pub;
			s.mangled				 = mn;
			s.loc					 = en.loc;
			s.type					 = en.resolved;
			auto ins				 = tbl.emplace(en.name, s);
			if (!ins.second) {
				log::report(log::Level::Error, en.loc, "duplicate symbol '" + en.name + "'");
			} else if (!en.generic_params.empty()) {
				en.resolved->generic_origin = &ins.first->second;
			}
			for (size_t member_index = 0; member_index < en.members.size(); ++member_index) {
				const auto &mb = en.members[member_index];
				if (mb.payload_kind == EnumPayloadKind::Unit) continue;
				auto &vi = en.resolved->enum_variants[member_index];
				std::unordered_set<std::string> vfield_names;
				for (const auto &pf : mb.payload_fields) {
					if (mb.payload_kind == EnumPayloadKind::Struct &&
						!vfield_names.insert(pf.name).second) {
						log::report(log::Level::Error, pf.loc,
									"duplicate field '" + pf.name + "' in variant '" +
										mb.name + "'");
					}
					StType *ft = en.generic_params.empty() && pf.ty
									 ? resolve_type(*pf.ty, m)
									 : nullptr;
					vi.field_types.push_back(ft);
					vi.field_names.push_back(pf.name);
				}
			}
			break;
		}
		case TopKind::Trait: {
			TraitDecl &tr = *td->trait_decl;
			tr.decl_module = &m;
			validate_attributes(tr.attrs, TopKind::Trait, tr.loc);
			std::string mn = mangle_in(m, tr.name);
			s.kind = SymKind::Trait;
			s.simple = tr.name;
			s.trait_decl = &tr;
			s.is_pub = tr.is_pub;
			s.mangled = mn;
			s.loc = tr.loc;
			s.module_path = m.mod_path;
			if (!tbl.emplace(tr.name, s).second) {
				log::report(log::Level::Error, tr.loc, "duplicate symbol '" + tr.name + "'");
			}
			break;
		}
		case TopKind::Impl: {
			ImplDecl &im = *td->impl_decl;
			im.decl_module = &m;
			validate_attributes(im.attrs, TopKind::Impl, im.loc);
			break;
		}
		default:
			break;
		}
	}

	// Declarations in a mod body are namespace members. Enum construction is
	// kept in a helper because it also needs to build the enum's distinct
	// StType before the namespace table can expose it.
	auto register_namespace_enum = [&](EnumDecl &en, const std::string &key,
									   const std::vector<std::string> &scope_path) {
		validate_generic_params(en.generic_params);
		validate_attributes(en.attrs, TopKind::Enum, en.loc);
		std::string mn = mangle_in(m, key);
		Symbol sym;
		sym.owner_module = &m;
		sym.kind = SymKind::Enum;
		sym.simple = en.name;
		sym.en = &en;
		sym.is_pub = en.is_pub;
		sym.mangled = mn;
		sym.loc = en.loc;
		sym.module_path = m.mod_path;

		StType *backing = nullptr;
		if (en.backing) {
			StType *bt = underlying_type(resolve_type(*en.backing, m));
			if (bt && (bt->kind == TypeKind::SInt || bt->kind == TypeKind::UInt)) {
				backing = bt;
			} else if (bt) {
				log::report(log::Level::Error, en.backing->loc,
							"enum backing type must be an integer, got '" + bt->name + "'");
			}
		} else {
			log::report(log::Level::Error, en.loc,
						"enum '" + en.name +
							"' requires an explicit backing type (e.g. 'enum " + en.name +
							": u32 { ... }')");
		}
		if (!backing) {
			backing = types_.sint(32);
		}
		std::vector<std::string>	 names;
		std::vector<uint64_t>		 values;
		std::vector<EnumVariantInfo> variants;
		std::unordered_set<uint64_t> seen_values;
		bool						 is_tagged = false;
		names.reserve(en.members.size());
		values.reserve(en.members.size());
		variants.reserve(en.members.size());
		uint64_t	   next = 0;
		const int	   bw	= (backing->width <= 0) ? types_.pointer_width() : backing->width;
		const uint64_t mask = (bw >= 64) ? ~uint64_t{0} : ((uint64_t{1} << bw) - 1);
		for (size_t member_index = 0; member_index < en.members.size(); ++member_index) {
			auto &mb = en.members[member_index];
			for (size_t previous = 0; previous < names.size(); ++previous) {
				if (names[previous] == mb.name) {
					log::report(log::Level::Error, mb.loc,
								"duplicate enum member name '" + mb.name + "'");
					break;
				}
			}
			uint64_t v = next;
			if (mb.value) {
				if (is_const_int_expr(*mb.value)) {
					if (mb.value->kind == ExprKind::IntLit ||
						mb.value->kind == ExprKind::CharLit) {
						v = mb.value->int_val;
					} else {
						v = static_cast<uint64_t>(eval_const_int(*mb.value));
					}
				} else {
					log::report(log::Level::Error, mb.loc,
								"enum member initializer must be a constant integer expression");
				}
			}
			mb.resolved = v;
			if (!enum_value_fits(backing, v, mb.value.get())) {
				log::report(log::Level::Error, mb.loc,
							"value of enum member '" + mb.name +
								"' does not fit backing type '" + backing->name + "'");
			} else if (!seen_values.insert(v & mask).second) {
				log::report(log::Level::Error, mb.loc,
							"duplicate enum discriminant value for member '" + mb.name + "'");
			}
			if (!next_enum_value(backing, v, next)) {
				if (member_index + 1 < en.members.size() &&
					!en.members[member_index + 1].value) {
					log::report(log::Level::Error, mb.loc,
								"enum member '" + mb.name + "' value exhausts backing type '" +
									backing->name +
									"'; the next member cannot be auto-incremented");
				}
				next = v;
			}
			EnumVariantInfo vi;
			vi.payload_kind = mb.payload_kind;
			if (mb.payload_kind != EnumPayloadKind::Unit) {
				is_tagged = true;
				std::unordered_set<std::string> vfield_names;
				for (const auto &pf : mb.payload_fields) {
					if (mb.payload_kind == EnumPayloadKind::Struct &&
						!vfield_names.insert(pf.name).second) {
						log::report(log::Level::Error, pf.loc,
									"duplicate field '" + pf.name + "' in variant '" +
										mb.name + "'");
					}
					StType *ft = en.generic_params.empty() && pf.ty
									 ? resolve_type(*pf.ty, m)
									 : nullptr;
					vi.field_types.push_back(ft);
					vi.field_names.push_back(pf.name);
				}
			}
			names.push_back(mb.name);
			values.push_back(v);
			variants.push_back(std::move(vi));
		}
		en.resolved = types_.enum_of(mn, backing, std::move(names), std::move(values),
									 en.is_pub, is_tagged, std::move(variants));
		en.resolved->repr = resolve_repr_attr(en.attrs);
		if (const Attribute *al = find_attr(en.attrs, "align")) {
			if (auto alignment = valid_alignment_value(*al)) {
				en.resolved->explicit_align = *alignment;
			}
		}
		apply_layout_requirements(en.resolved, en.attrs);
		if (en.resolved) {
			en.resolved->decl_module = &m;
			en.resolved->scope_path  = scope_path;
		}

		sym.type = en.resolved;
		auto ins = tbl.emplace(key, std::move(sym));
		if (!ins.second) {
			log::report(log::Level::Error, en.loc, "duplicate enum '" + en.name + "'");
		} else if (en.resolved) {
			en.resolved->generic_origin = &ins.first->second;
		}
	};

	// Block-level `mod` declarations keep their declarations in the defining
	// file's AST. Register those members in the same source-file symbol table
	// using their structural path; no declarations are copied into another module.
	std::function<void(const std::vector<TopDeclPtr> &, std::vector<std::string>)>
		register_nested;
	register_nested = [&](const std::vector<TopDeclPtr> &decls, std::vector<std::string> prefix) {
		for (const auto &td : decls) {
			if (!td) continue;
			if (td->kind == TopKind::Mod && td->mod_decl) {
				ModDecl &md = *td->mod_decl;
				md.owner_path = prefix;
				validate_attributes(md.attrs, TopKind::Mod, md.loc);
				const std::string key = join_path(prefix) + (prefix.empty() ? "" : ".") + md.name;
				auto it = tbl.find(key);
				if (it != tbl.end()) {
					if (it->second.kind != SymKind::Mod) {
						log::report(log::Level::Error, md.loc,
									"duplicate symbol '" + md.name + "'");
					} else {
						it->second.is_pub = it->second.is_pub || md.is_pub;
					}
				} else {
					Symbol sym;
					sym.owner_module = &m;
					sym.kind = SymKind::Mod;
					sym.simple = md.name;
					sym.mod_decl = &md;
					sym.is_pub = md.is_pub;
					sym.loc = md.loc;
					sym.module_path = m.mod_path;
					sym.mangled = mangle_in(m, key);
					tbl.emplace(key, sym);
				}
				std::vector<std::string> child = prefix;
				child.push_back(md.name);
				register_nested(md.body, std::move(child));
				continue;
			}
			if (td->kind == TopKind::Struct && td->st) {
				StructDecl &st = *td->st;
				validate_generic_params(st.generic_params);
				validate_attributes(st.attrs, TopKind::Struct, st.loc);
				const std::string key = join_path(prefix) + (prefix.empty() ? "" : ".") + st.name;
				std::vector<std::string> scope_path = prefix;
				scope_path.push_back(st.name);
				Symbol sym;
				sym.owner_module = &m;
				sym.kind = SymKind::Struct;
				sym.simple = st.name;
				sym.st = &st;
				sym.is_pub = st.is_pub;
				sym.loc = st.loc;
				sym.module_path = m.mod_path;
				sym.mangled = mangle_in(m, key);
				std::vector<StType *> fields(st.field_types.size(), nullptr);
				st.resolved = types_.struct_of(sym.mangled, st.field_names, fields, st.is_pub, st.field_is_pub);
				st.resolved->is_opaque   = st.is_opaque;
				st.resolved->repr		 = resolve_repr_attr(st.attrs);
				st.resolved->decl_module = &m;
				st.resolved->scope_path  = scope_path;
				apply_layout_requirements(st.resolved, st.attrs);
				if (const Attribute *al = find_attr(st.attrs, "align")) {
					if (auto alignment = valid_alignment_value(*al)) {
						st.resolved->explicit_align = *alignment;
					}
				}
				sym.type = st.resolved;
				auto ins = tbl.emplace(key, sym);
				if (!ins.second) {
					log::report(log::Level::Error, st.loc, "duplicate symbol '" + st.name + "'");
				} else {
					st.resolved->generic_origin = &ins.first->second;
				}
				continue;
			}
			if (td->kind == TopKind::Union && td->un) {
				UnionDecl &u = *td->un;
				validate_generic_params(u.generic_params);
				validate_attributes(u.attrs, TopKind::Union, u.loc);
				const std::string key = join_path(prefix) + (prefix.empty() ? "" : ".") + u.name;
				std::vector<std::string> scope_path = prefix;
				scope_path.push_back(u.name);
				Symbol sym;
				sym.owner_module = &m;
				sym.kind = SymKind::Union;
				sym.simple = u.name;
				sym.un = &u;
				sym.is_pub = u.is_pub;
				sym.loc = u.loc;
				sym.module_path = m.mod_path;
				sym.mangled = mangle_in(m, key);
				std::vector<StType *> fields(u.field_types.size(), nullptr);
				u.resolved = types_.union_of(sym.mangled, u.field_names, fields, u.is_pub, u.field_is_pub);
				u.resolved->repr		= resolve_repr_attr(u.attrs);
				u.resolved->decl_module = &m;
				u.resolved->scope_path  = scope_path;
				sym.type = u.resolved;
				if (const Attribute *al = find_attr(u.attrs, "align")) {
					if (auto alignment = valid_alignment_value(*al)) {
						u.resolved->explicit_align = *alignment;
					}
				}
				auto ins = tbl.emplace(key, std::move(sym));
				if (!ins.second) {
					log::report(log::Level::Error, u.loc, "duplicate union '" + u.name + "'");
				} else {
					u.resolved->generic_origin = &ins.first->second;
				}
				continue;
			}
			if (td->kind == TopKind::TypeBinding && td->tb) {
				TypeBindingDecl &tb = *td->tb;
				validate_generic_params(tb.generic_params);
				validate_attributes(tb.attrs, TopKind::TypeBinding, tb.loc);
				const std::string key = join_path(prefix) + (prefix.empty() ? "" : ".") + tb.name;
				Symbol sym;
				sym.owner_module = &m;
				sym.kind = SymKind::TypeBinding;
				sym.simple = tb.name;
				sym.tb = &tb;
				sym.is_pub = tb.is_pub;
				sym.loc = tb.loc;
				sym.module_path = m.mod_path;
				sym.mangled = mangle_in(m, key);
				if (!tbl.emplace(key, std::move(sym)).second) {
					log::report(log::Level::Error, tb.loc, "duplicate type binding '" + tb.name + "'");
				}
				continue;
			}
			if (td->kind == TopKind::Enum && td->en) {
				const std::string key = join_path(prefix) + (prefix.empty() ? "" : ".") + td->en->name;
				std::vector<std::string> scope_path = prefix;
				scope_path.push_back(td->en->name);
				auto saved_path = m.lexical_path;
				m.lexical_path = m.mod_path;
				m.lexical_path.insert(m.lexical_path.end(), prefix.begin(), prefix.end());
				register_namespace_enum(*td->en, key, scope_path);
				m.lexical_path = std::move(saved_path);
				continue;
			}
			if (td->kind == TopKind::Fn && td->fn) {
				FnDecl &f = *td->fn;
				f.decl_module = &m;
				if (f.is_macro) {
					validate_macro_body(f);
				}
				validate_generic_params(f.generic_params);
				if (!f.is_macro && !f.generic_params.empty() &&
					(f.is_extern || has_attr(f.attrs, "link_name"))) {
					log::report(log::Level::Error, f.loc,
								"generic extern/link_name functions are not supported");
				}
				validate_attributes(f.attrs, TopKind::Fn, f.loc);
				std::vector<std::string> owner = prefix;
				owner.insert(owner.end(), f.owner_path.begin(), f.owner_path.end());
				f.owner_path = owner;
				f.full_path = m.mod_path;
				f.full_path.insert(f.full_path.end(), owner.begin(), owner.end());
				f.full_path.push_back(f.name);
				std::vector<std::string> parts = owner;
				parts.push_back(f.name);
				const std::string key = join_path(parts);
				auto it = tbl.find(key);
				if (it != tbl.end() && it->second.kind == SymKind::Fn) {
					it->second.overloads.push_back(&f);
					it->second.is_pub = it->second.is_pub || f.is_pub;
					f.sym = &it->second;
				} else if (it == tbl.end()) {
					Symbol sym;
					sym.owner_module = &m;
					sym.kind = SymKind::Fn;
					sym.simple = f.name;
					sym.fn = &f;
					sym.overloads.push_back(&f);
					sym.is_pub = f.is_pub;
					sym.loc = f.loc;
					sym.module_path = m.mod_path;
					auto inserted = tbl.emplace(key, sym);
					f.sym = &inserted.first->second;
				} else {
					log::report(log::Level::Error, f.loc,
								"duplicate symbol '" + key +
									"' (already declared as non-function)");
				}
				continue;
			}
			if ((td->kind == TopKind::Var || td->kind == TopKind::Const) && td->glob) {
				GlobalDecl &g = *td->glob;
				validate_attributes(g.attrs, td->kind, g.loc);
				g.owner_path = prefix;
				std::vector<std::string> parts = prefix;
				parts.push_back(g.name);
				const std::string key = join_path(parts);
				if (!tbl.count(key)) {
					Symbol sym;
					sym.owner_module = &m;
					sym.kind = g.is_const ? SymKind::Const : SymKind::Var;
					sym.is_mutable = g.is_mutable;
					sym.simple = g.name;
					sym.is_pub = g.is_pub;
					sym.loc = g.loc;
					sym.usage = &g.used;
					sym.glob = &g;
					sym.module_path = m.mod_path;
					sym.mangled = mangle_global(m, g);
					tbl.emplace(key, sym);
				} else {
					log::report(log::Level::Error, g.loc, "duplicate symbol '" + g.name + "'");
				}
				continue;
			}
			if (td->kind == TopKind::Trait && td->trait_decl) {
				TraitDecl &tr = *td->trait_decl;
				tr.decl_module = &m;
				validate_attributes(tr.attrs, TopKind::Trait, tr.loc);
				const std::string key = join_path(prefix) + (prefix.empty() ? "" : ".") + tr.name;
				Symbol sym;
				sym.owner_module = &m;
				sym.kind = SymKind::Trait;
				sym.simple = tr.name;
				sym.trait_decl = &tr;
				sym.is_pub = tr.is_pub;
				sym.loc = tr.loc;
				sym.module_path = m.mod_path;
				sym.mangled = mangle_in(m, key);
				if (!tbl.emplace(key, sym).second) {
					log::report(log::Level::Error, tr.loc, "duplicate trait '" + tr.name + "'");
				}
				continue;
			}
			if (td->kind == TopKind::Impl && td->impl_decl) {
				td->impl_decl->decl_module = &m;
				td->impl_decl->owner_path  = prefix;
				validate_attributes(td->impl_decl->attrs, TopKind::Impl, td->impl_decl->loc);
				continue;
			}
		}
	};
	for (const auto &td : m.decls) {
		if (td && td->kind == TopKind::Mod && td->mod_decl) {
			register_nested(td->mod_decl->body, {td->mod_decl->name});
		}
	}

	// Register inherent `impl` methods and associated constants into the
	// defining module's symbol table under `<TypePath>.<member>` after all
	// module and nominal declarations in the file have been registered.
	std::function<void(const std::vector<TopDeclPtr> &, std::vector<std::string>)>
		register_inherent_impls;
	register_inherent_impls = [&](const std::vector<TopDeclPtr> &decls,
								  std::vector<std::string>		 prefix) {
		for (const auto &td : decls) {
			if (!td) continue;
			if (td->kind == TopKind::Mod && td->mod_decl) {
				std::vector<std::string> child = prefix;
				child.push_back(td->mod_decl->name);
				register_inherent_impls(td->mod_decl->body, std::move(child));
				continue;
			}
			if (td->kind != TopKind::Impl || !td->impl_decl) {
				continue;
			}
			ImplDecl &im	= *td->impl_decl;
			im.decl_module	= &m;
			im.owner_path	= prefix;
			if (!im.is_inherent) {
				continue;
			}
			if (!im.target_type || im.target_type->kind != TypeRef::Named) {
				log::error(im.loc, "inherent 'impl' target must be a nominal type")
					.code("E_INVALID_IMPL")
					.phase("semantic")
					.emit();
				if (im.target_type) {
					im.target_type->sema_invalid = true;
				}
				continue;
			}
			std::string written_name =
				im.target_type->name.empty() ? im.target_type->single : im.target_type->name;
			std::vector<std::string> target_segs = split_dotted_path(written_name);
			std::vector<std::string> type_owner	 = target_segs;
			if (!prefix.empty()) {
				std::vector<std::string> candidate = prefix;
				candidate.insert(candidate.end(), target_segs.begin(), target_segs.end());
				if (tbl.count(join_path(candidate))) {
					type_owner = std::move(candidate);
				}
			}
			if (!tbl.count(join_path(type_owner)) && !m.mod_path.empty() &&
				target_segs.size() > m.mod_path.size() &&
				std::equal(m.mod_path.begin(), m.mod_path.end(), target_segs.begin())) {
				std::vector<std::string> stripped(
					target_segs.begin() + static_cast<std::ptrdiff_t>(m.mod_path.size()),
					target_segs.end());
				if (tbl.count(join_path(stripped))) {
					type_owner = std::move(stripped);
				}
			}
			const std::string target_key = join_path(type_owner);
			auto			  target_it	 = tbl.find(target_key);
			const std::vector<GenericParam> *decl_generic_params = nullptr;
			if (target_it != tbl.end()) {
				Symbol &ts = target_it->second;
				if (ts.kind == SymKind::Struct && ts.st) {
					decl_generic_params = &ts.st->generic_params;
				} else if (ts.kind == SymKind::Union && ts.un) {
					decl_generic_params = &ts.un->generic_params;
				} else if (ts.kind == SymKind::Enum && ts.en) {
					decl_generic_params = &ts.en->generic_params;
				} else if (ts.kind == SymKind::TypeBinding && ts.tb) {
					decl_generic_params = &ts.tb->generic_params;
				}
			}
			if (im.generic_params.empty() && !im.target_type->generic_args.empty() &&
				decl_generic_params &&
				im.target_type->generic_args.size() == decl_generic_params->size()) {
				bool all_unbound_names = true;
				for (const auto &ga : im.target_type->generic_args) {
					if (ga.kind != GenericArg::Kind::Type || !ga.type ||
						ga.type->kind != TypeRef::Named || !ga.type->generic_args.empty() ||
						is_prim_type_name(ga.type->single) ||
						tbl.count(ga.type->single) != 0) {
						all_unbound_names = false;
						break;
					}
				}
				if (all_unbound_names) {
					for (size_t i = 0; i < decl_generic_params->size(); ++i) {
						GenericParam gp = clone_generic_param((*decl_generic_params)[i]);
						gp.name			= im.target_type->generic_args[i].type->single;
						gp.loc			= im.target_type->generic_args[i].loc;
						im.generic_params.push_back(std::move(gp));
					}
				}
			} else if (!im.generic_params.empty() && decl_generic_params) {
				if (im.target_type->generic_args.empty() &&
					im.generic_params.size() == decl_generic_params->size()) {
					for (const auto &gp : im.generic_params) {
						GenericArg ga;
						ga.kind			= GenericArg::Kind::Type;
						ga.loc			= gp.loc;
						ga.type			= std::make_unique<TypeRef>();
						ga.type->kind	= TypeRef::Named;
						ga.type->name	= gp.name;
						ga.type->single = gp.name;
						ga.type->loc	= gp.loc;
						im.target_type->generic_args.push_back(std::move(ga));
					}
				}
				for (size_t i = 0; i < im.generic_params.size(); ++i) {
					auto &gp = im.generic_params[i];
					const GenericParam *match_decl = nullptr;
					for (const auto &dp : *decl_generic_params) {
						if (dp.name == gp.name && dp.kind == gp.kind) {
							match_decl = &dp;
							break;
						}
					}
					if (!match_decl && i < decl_generic_params->size() &&
						(*decl_generic_params)[i].kind == gp.kind) {
						match_decl = &(*decl_generic_params)[i];
					}
					if (match_decl && gp.kind == GenericParam::Kind::Type &&
						gp.type_set.empty() && gp.trait_bounds.empty()) {
						gp.type_set = match_decl->type_set;
						for (const auto &tb : match_decl->trait_bounds) {
							if (tb) {
								gp.trait_bounds.push_back(clone_typeref(*tb));
							}
						}
					}
				}
			}
			validate_generic_params(im.generic_params);

			auto qualified_target	 = clone_typeref(*im.target_type);
			qualified_target->name	 = target_key;
			qualified_target->single = type_owner.empty() ? written_name : type_owner.back();

			for (auto &c : im.consts) {
				if (!c) continue;
				validate_attributes(c->attrs, TopKind::Const, c->loc);
				substitute_self_in_typeref(c->ty, *qualified_target);
				substitute_self_in_expr(c->init, *qualified_target);
				c->owner_path = type_owner;
				std::string const_key = target_key + "." + c->name;
				Symbol sym;
				sym.owner_module = &m;
				sym.kind		 = SymKind::Const;
				sym.is_mutable	 = false;
				sym.simple		 = c->name;
				sym.is_pub		 = c->is_pub;
				sym.loc			 = c->loc;
				sym.usage		 = &c->used;
				sym.glob		 = c.get();
				sym.module_path	 = m.mod_path;
				sym.mangled		 = mangle_global(m, *c);
				if (!tbl.emplace(const_key, std::move(sym)).second) {
					log::report(log::Level::Error, c->loc,
								"duplicate symbol '" + const_key + "'");
				}
			}
			for (auto &method : im.methods) {
				if (!method) continue;
				method->decl_module = &m;
				method->is_method	= true;
				method->owner_path	= type_owner;
				method->full_path	= m.mod_path;
				method->full_path.insert(method->full_path.end(), type_owner.begin(),
										 type_owner.end());
				method->full_path.push_back(method->name);
				substitute_self_in_fn(*method, *qualified_target);
				if (!im.generic_params.empty()) {
					std::vector<GenericParam> combined;
					size_t					  prepended = 0;
					for (const auto &gp : im.generic_params) {
						bool exists = false;
						for (const auto &mp : method->generic_params) {
							if (mp.name == gp.name) {
								exists = true;
								break;
							}
						}
						if (!exists) {
							combined.push_back(clone_generic_param(gp));
							++prepended;
						}
					}
					for (auto &mp : method->generic_params) {
						combined.push_back(std::move(mp));
					}
					method->generic_params			 = std::move(combined);
					method->impl_generic_param_count = prepended;
				}
				validate_generic_params(method->generic_params);
				if (!method->generic_params.empty() &&
					(method->is_extern || has_attr(method->attrs, "link_name"))) {
					log::report(log::Level::Error, method->loc,
								"generic extern/link_name functions are not supported");
				}
				validate_attributes(method->attrs, TopKind::Fn, method->loc);
				std::string fn_key = target_key + "." + method->name;
				auto		it	   = tbl.find(fn_key);
				if (it != tbl.end()) {
					if (it->second.kind != SymKind::Fn) {
						log::report(log::Level::Error, method->loc,
									"duplicate symbol '" + fn_key +
										"' (already declared as non-function)");
					} else {
						it->second.overloads.push_back(method.get());
						if (method->is_pub) {
							it->second.is_pub = true;
						}
						method->sym = &it->second;
					}
				} else {
					Symbol sym;
					sym.owner_module = &m;
					sym.kind		 = SymKind::Fn;
					sym.simple		 = method->name;
					sym.fn			 = method.get();
					sym.overloads.push_back(method.get());
					sym.is_pub		= method->is_pub;
					sym.loc			= method->loc;
					sym.module_path = m.mod_path;
					auto inserted	= tbl.emplace(fn_key, std::move(sym));
					method->sym		= &inserted.first->second;
				}
			}
		}
	};
	register_inherent_impls(m.decls, {});
}

void Sema::resolve_module_types(Module &m) {
	std::string mkey   = source_key(m);
	auto		tbl_it = module_syms_.find(mkey);
	if (tbl_it == module_syms_.end()) {
		return;
	}
	auto &tbl = tbl_it->second;
	for (auto &td : m.decls) {
		switch (td->kind) {
		case TopKind::Fn: {
			if (td->fn->generic_params.empty()) {
				resolve_fn_signature(*td->fn, m);
			}
			break;
		}
		case TopKind::TypeBinding: {
			TypeBindingDecl &tb = *td->tb;
			if (!tb.generic_params.empty()) {
				break;
			}
			auto sit = tbl.find(tb.name);
			if (sit != tbl.end()) {
				resolve_type_binding(sit->second, m);
			}
			break;
		}
		case TopKind::Var:
		case TopKind::Const: {
			GlobalDecl &g = *td->glob;
			if (g.is_const) {
				auto sit = tbl.find(g.name);
				if (sit != tbl.end()) {
					ensure_const_value(sit->second);
				}
				break;
			}
			if (g.ty && g.init) {
				if (g.init->kind == ExprKind::SrcEmbed) {
					FnDecl dummy;
					dummy.name		  = "<global-init>";
					dummy.loc		  = g.loc;
					dummy.decl_module = &m;
					ScopeStack global_scope;
					global_scope.enter();
					resolve_src_embed(*g.init, dummy, m, global_scope);
				}
				if (!g.init->sema_invalid || !g.ty->array_len_inferred) {
					infer_array_lengths(*g.ty, *g.init);
				} else {
					g.ty->array_len_inferred = false;
					g.ty->sema_invalid		 = true;
				}
			}
			StType *ty = g.ty ? resolve_type(*g.ty, m) : nullptr;
			if (underlying_type(ty) && underlying_type(ty)->kind == TypeKind::VaList) {
				log::report(log::Level::Error, g.loc,
							"@va.list may only be declared as a local variable");
			}
			auto sit = tbl.find(g.name);
			if (sit != tbl.end()) {
				sit->second.type = ty;
			}
			break;
		}
		case TopKind::Trait: {
			TraitDecl &tr = *td->trait_decl;
			active_self_type_ = types_.self_ty();
			for (auto &proto : tr.methods) {
				if (proto) {
					proto->decl_module = &m;
					resolve_fn_signature(*proto, m);
				}
			}
			active_self_type_ = nullptr;
			break;
		}
		default:
			break;
		}
	}
	for (auto &td : m.decls) {
		if (td->kind == TopKind::Fn) {
			FnDecl &f = *td->fn;
			if (!f.generic_params.empty()) {
				continue;
			}
			if (f.is_macro) {
				continue;
			}
			f.mangled = mangle_fn(m, f);
			if (f.sym && f.sym->mangled.empty()) {
				f.sym->mangled = f.mangled;
			}
		}
	}
	std::function<void(const std::vector<TopDeclPtr> &, std::vector<std::string>)> resolve_nested;
	resolve_nested = [&](const std::vector<TopDeclPtr> &decls, std::vector<std::string> prefix) {
		for (const auto &td : decls) {
			if (!td) continue;
			if (td->kind == TopKind::Mod && td->mod_decl) {
				std::vector<std::string> child = prefix;
				child.push_back(td->mod_decl->name);
				resolve_nested(td->mod_decl->body, std::move(child));
				continue;
			}
			if (td->kind == TopKind::Fn && td->fn && td->fn->generic_params.empty()) {
				auto saved_path = m.lexical_path;
				m.lexical_path = m.mod_path;
				m.lexical_path.insert(m.lexical_path.end(), prefix.begin(), prefix.end());
				resolve_fn_signature(*td->fn, m);
				td->fn->mangled = mangle_fn(m, *td->fn);
				if (td->fn->sym && td->fn->sym->mangled.empty()) {
					td->fn->sym->mangled = td->fn->mangled;
				}
				m.lexical_path = std::move(saved_path);
				continue;
			}
			if (td->kind == TopKind::TypeBinding && td->tb && td->tb->generic_params.empty()) {
				TypeBindingDecl &tb = *td->tb;
				auto saved_path = m.lexical_path;
				m.lexical_path = m.mod_path;
				m.lexical_path.insert(m.lexical_path.end(), prefix.begin(), prefix.end());
				const std::string key = join_path(prefix) + (prefix.empty() ? "" : ".") + tb.name;
				auto sit = tbl.find(key);
				if (sit != tbl.end()) {
					resolve_type_binding(sit->second, m);
				}
				m.lexical_path = std::move(saved_path);
				continue;
			}
			if ((td->kind == TopKind::Var || td->kind == TopKind::Const) && td->glob) {
				GlobalDecl &g = *td->glob;
				if (g.is_const) {
					for (auto &entry : tbl) {
						if (entry.second.glob == &g) {
							ensure_const_value(entry.second);
							break;
						}
					}
					continue;
				}
				if (td->glob->ty) {
					auto saved_path = m.lexical_path;
					m.lexical_path = m.mod_path;
					m.lexical_path.insert(m.lexical_path.end(), prefix.begin(), prefix.end());
					StType *ty = resolve_type(*g.ty, m);
					m.lexical_path = std::move(saved_path);
					for (auto &entry : tbl) {
						if (entry.second.glob == &g) {
							entry.second.type = ty;
							break;
						}
					}
				}
			}
			if (td->kind == TopKind::Trait && td->trait_decl) {
				TraitDecl &tr = *td->trait_decl;
				active_self_type_ = types_.self_ty();
				for (auto &proto : tr.methods) {
					if (proto) {
						proto->decl_module = &m;
						resolve_fn_signature(*proto, m);
					}
				}
				active_self_type_ = nullptr;
				continue;
			}
		}
	};
	for (const auto &td : m.decls) {
		if (td && td->kind == TopKind::Mod && td->mod_decl) {
			resolve_nested(td->mod_decl->body, {td->mod_decl->name});
		}
	}

	// Resolve inherent `impl` blocks (both top-level and inside `mod` blocks).
	std::function<void(const std::vector<TopDeclPtr> &, std::vector<std::string>)>
		resolve_inherent_impls;
	resolve_inherent_impls = [&](const std::vector<TopDeclPtr> &decls,
								 std::vector<std::string>		prefix) {
		for (const auto &td : decls) {
			if (!td) continue;
			if (td->kind == TopKind::Mod && td->mod_decl) {
				std::vector<std::string> child = prefix;
				child.push_back(td->mod_decl->name);
				resolve_inherent_impls(td->mod_decl->body, std::move(child));
				continue;
			}
			if (td->kind != TopKind::Impl || !td->impl_decl || !td->impl_decl->is_inherent) {
				continue;
			}
			ImplDecl &im = *td->impl_decl;
			if (!im.target_type || im.target_type->sema_invalid ||
				im.target_type->kind != TypeRef::Named) {
				continue;
			}
			std::string written_name =
				im.target_type->name.empty() ? im.target_type->single : im.target_type->name;
			std::vector<std::string> target_segs = split_dotted_path(written_name);
			std::vector<std::string> type_owner	 = target_segs;
			if (!prefix.empty()) {
				std::vector<std::string> candidate = prefix;
				candidate.insert(candidate.end(), target_segs.begin(), target_segs.end());
				if (tbl.count(join_path(candidate))) {
					type_owner = std::move(candidate);
				}
			}
			if (!tbl.count(join_path(type_owner)) && !m.mod_path.empty() &&
				target_segs.size() > m.mod_path.size() &&
				std::equal(m.mod_path.begin(), m.mod_path.end(), target_segs.begin())) {
				std::vector<std::string> stripped(
					target_segs.begin() + static_cast<std::ptrdiff_t>(m.mod_path.size()),
					target_segs.end());
				if (tbl.count(join_path(stripped))) {
					type_owner = std::move(stripped);
				}
			}
			const std::string target_key = join_path(type_owner);
			auto			  target_it	 = tbl.find(target_key);
			if (target_it == tbl.end()) {
				auto saved_path = m.lexical_path;
				m.lexical_path	= m.mod_path;
				m.lexical_path.insert(m.lexical_path.end(), prefix.begin(), prefix.end());
				Symbol *foreign = resolve_path_any(target_segs, m);
				m.lexical_path	= std::move(saved_path);
				if (foreign && foreign->kind == SymKind::Mod) {
					log::error(im.target_type->loc,
							   "'" + written_name + "' is a module, not a type")
						.code("E_UNDEFINED_TYPE")
						.phase("semantic")
						.emit();
				} else if (foreign && foreign->owner_module &&
						   !resolver_.same_source_file(m, *foreign->owner_module)) {
					log::error(im.loc,
							   "cannot define inherent implementation for foreign type '" +
								   written_name + "'")
						.code("E_ORPHAN_RULE")
						.phase("semantic")
						.emit();
				} else {
					log::error(im.target_type->loc, "undefined type '" + written_name + "'")
						.code("E_UNDEFINED_TYPE")
						.phase("semantic")
						.emit();
				}
				im.target_type->sema_invalid = true;
				continue;
			}
			Symbol &ts = target_it->second;
			if (ts.kind == SymKind::Mod) {
				log::error(im.target_type->loc,
						   "'" + written_name + "' is a module, not a type")
					.code("E_UNDEFINED_TYPE")
					.phase("semantic")
					.emit();
				im.target_type->sema_invalid = true;
				continue;
			}
			if (ts.kind != SymKind::Struct && ts.kind != SymKind::Union &&
				ts.kind != SymKind::Enum && ts.kind != SymKind::TypeBinding) {
				log::error(im.target_type->loc,
						   "cannot define inherent implementation for '" + written_name + "'")
					.code("E_INVALID_IMPL")
					.phase("semantic")
					.emit();
				im.target_type->sema_invalid = true;
				continue;
			}
			auto saved_path = m.lexical_path;
			m.lexical_path	= m.mod_path;
			m.lexical_path.insert(m.lexical_path.end(), prefix.begin(), prefix.end());
			if (im.generic_params.empty()) {
				im.resolved_target_type = resolve_type(*im.target_type, m);
			}
			active_self_type_ = im.resolved_target_type;
			for (auto &c : im.consts) {
				if (!c) continue;
				auto sit = tbl.find(target_key + "." + c->name);
				if (sit != tbl.end()) {
					ensure_const_value(sit->second);
				}
			}
			for (auto &method : im.methods) {
				if (!method) continue;
				if (method->generic_params.empty()) {
					resolve_fn_signature(*method, m);
					method->mangled = mangle_fn(m, *method);
					if (method->sym && method->sym->mangled.empty()) {
						method->sym->mangled = method->mangled;
					}
				}
			}
			active_self_type_ = nullptr;
			m.lexical_path	  = std::move(saved_path);
		}
	};
	resolve_inherent_impls(m.decls, {});

	for (auto &pair : tbl) {
		Symbol &s = pair.second;
		if (s.kind == SymKind::Fn && s.overloads.size() > 1) {
			for (size_t i = 0; i < s.overloads.size(); ++i) {
				for (size_t j = i + 1; j < s.overloads.size(); ++j) {
					FnDecl *f1 = s.overloads[i];
					FnDecl *f2 = s.overloads[j];
					if (f1->generic_params.size() != f2->generic_params.size()) {
						continue;
					}
					if (!f1->generic_params.empty()) {
						// Reject identical generic overload signatures (matching constraints + param structure).
						auto same_constraints = [](const GenericParam &a, const GenericParam &b) {
							if (a.kind != b.kind || a.type_set.size() != b.type_set.size() ||
								a.trait_bounds.size() != b.trait_bounds.size()) {
								return false;
							}
							for (size_t k = 0; k < a.type_set.size(); ++k) {
								if (a.type_set[k].kind != b.type_set[k].kind ||
									a.type_set[k].name != b.type_set[k].name) {
									return false;
								}
							}
							for (size_t k = 0; k < a.trait_bounds.size(); ++k) {
								if (a.trait_bounds[k]->name != b.trait_bounds[k]->name) {
									return false;
								}
							}
							return true;
						};
						bool gp_match = true;
						std::unordered_map<std::string, size_t> gp_idx1, gp_idx2;
						for (size_t k = 0; k < f1->generic_params.size(); ++k) {
							if (!same_constraints(f1->generic_params[k], f2->generic_params[k])) {
								gp_match = false;
								break;
							}
							gp_idx1[f1->generic_params[k].name] = k;
							gp_idx2[f2->generic_params[k].name] = k;
						}
						if (!gp_match || f1->params.size() != f2->params.size() ||
							f1->is_variadic != f2->is_variadic || f1->is_macro != f2->is_macro) {
							continue;
						}
						std::function<bool(const TypeRef *, const TypeRef *)> eq_typeref;
						eq_typeref = [&](const TypeRef *a, const TypeRef *b) -> bool {
							if (!a || !b) return a == b;
							if (a->kind != b->kind || a->is_mutable != b->is_mutable ||
								a->is_volatile != b->is_volatile || a->array_len != b->array_len ||
								a->fn_variadic != b->fn_variadic ||
								a->fn_params.size() != b->fn_params.size() ||
								a->generic_args.size() != b->generic_args.size()) {
								return false;
							}
							if (a->kind == TypeRef::Named) {
								auto it1 = gp_idx1.find(a->single);
								auto it2 = gp_idx2.find(b->single);
								if (it1 != gp_idx1.end() || it2 != gp_idx2.end()) {
									if (it1 == gp_idx1.end() || it2 == gp_idx2.end() ||
										it1->second != it2->second) {
										return false;
									}
								} else if (a->name != b->name) {
									return false;
								}
							}
							if (!eq_typeref(a->base.get(), b->base.get()) ||
								!eq_typeref(a->fn_ret.get(), b->fn_ret.get())) {
								return false;
							}
							for (size_t k = 0; k < a->fn_params.size(); ++k) {
								if (!eq_typeref(a->fn_params[k].get(), b->fn_params[k].get())) {
									return false;
								}
							}
							return true;
						};
						bool match = true;
						for (size_t k = 0; k < f1->params.size(); ++k) {
							if (!eq_typeref(f1->params[k].ty.get(), f2->params[k].ty.get())) {
								match = false;
								break;
							}
						}
						if (match) {
							log::report(log::Level::Error, f2->loc,
										"duplicate overload for generic function '" + f2->name + "'");
						}
						continue;
					}
					if (f1->params.size() == f2->params.size() &&
						f1->is_variadic == f2->is_variadic && f1->is_macro == f2->is_macro) {
						bool match = true;
						for (size_t k = 0; k < f1->params.size(); ++k) {
							if (!eq_type(f1->params[k].resolved, f2->params[k].resolved)) {
								match = false;
								break;
							}
						}
						if (match) {
							log::error(f2->loc, "redefinition of function '" + f2->name +
												"' with identical parameter types")
								.code("E_DUPLICATE_DECLARATION")
								.phase("semantic")
								.note(f1->loc, "previous definition of '" + f1->name + "' was here")
								.emit();
						}
					}
				}
			}
		}
	}
}

void Sema::validate_public_namespace_surface() {
	std::map<std::string, std::vector<FnDecl *>> functions;
	std::map<std::string, std::vector<std::pair<Module *, TopDecl *>>> other;
	for (Module *module : resolver_.all()) {
		if (!module) {
			continue;
		}
		const std::string prefix = join_path(module->mod_path);
		for (auto &decl : module->decls) {
			if (!decl) {
				continue;
			}
			if (decl->kind == TopKind::Fn && decl->fn && decl->fn->is_pub) {
				functions[prefix + "\n" + decl->fn->name].push_back(decl->fn.get());
			} else if (decl->kind != TopKind::Import && decl->kind != TopKind::Fn) {
				bool is_pub = decl->kind == TopKind::Struct ? decl->st->is_pub :
					decl->kind == TopKind::Union ? decl->un->is_pub :
					decl->kind == TopKind::TypeBinding ? decl->tb->is_pub :
					decl->kind == TopKind::Enum ? decl->en->is_pub :
					decl->kind == TopKind::Trait ? decl->trait_decl->is_pub :
					(decl->kind == TopKind::Var || decl->kind == TopKind::Const) && decl->glob->is_pub;
				if (is_pub) {
					other[prefix + "\n" + std::to_string(static_cast<int>(decl->kind)) + "\n" +
										(decl->kind == TopKind::Struct ? decl->st->name :
										 decl->kind == TopKind::Union ? decl->un->name :
										 decl->kind == TopKind::TypeBinding ? decl->tb->name :
										 decl->kind == TopKind::Enum ? decl->en->name :
										 decl->kind == TopKind::Trait ? decl->trait_decl->name : decl->glob->name)].push_back({module, decl.get()});
				}
			}
		}
	}
	for (auto &[key, overloads] : functions) {
		for (size_t i = 0; i < overloads.size(); ++i) {
			for (size_t j = i + 1; j < overloads.size(); ++j) {
				FnDecl *a = overloads[i];
				FnDecl *b = overloads[j];
				if (a->params.size() != b->params.size() || a->is_variadic != b->is_variadic) {
					continue;
				}
				bool same = true;
				for (size_t p = 0; p < a->params.size(); ++p) {
					same = same && eq_type(a->params[p].resolved, b->params[p].resolved);
				}
				if (same) {
					const std::string namespace_name =
						key.substr(0, key.find('\n')).empty()
							? "<global>"
							: key.substr(0, key.find('\n'));
					log::report(log::Level::Error, b->loc,
								"duplicate public function in namespace '" + namespace_name + "'");
				}
			}
		}
	}
	for (const auto &[key, declarations] : other) {
		if (declarations.size() > 1) {
			TopDecl *duplicate = declarations[1].second;
			SourceLoc loc{};
			switch (duplicate->kind) {
			case TopKind::Struct:
				loc = duplicate->st->loc;
				break;
			case TopKind::Union:
				loc = duplicate->un->loc;
				break;
			case TopKind::TypeBinding:
				loc = duplicate->tb->loc;
				break;
			case TopKind::Enum:
				loc = duplicate->en->loc;
				break;
			case TopKind::Var:
			case TopKind::Const:
				loc = duplicate->glob->loc;
				break;
			default:
				break;
			}
			const std::string namespace_name =
				key.substr(0, key.find('\n')).empty() ? "<global>" : key.substr(0, key.find('\n'));
			log::report(log::Level::Error, loc,
							"duplicate public declaration in namespace '" + namespace_name + "'");
		}
	}
}

Symbol *Sema::lookup_in_module(Module &m, const std::string &simple) {
	auto it = module_syms_.find(source_key(m));
	if (it == module_syms_.end()) {
		return nullptr;
	}
	auto sit = it->second.find(simple);
	return sit == it->second.end() ? nullptr : &sit->second;
}

Symbol *Sema::lookup_pub_in_module(Module &m, const std::string &simple) {
	Symbol *s = lookup_in_module(m, simple);
	if (!s || !s->is_pub) {
		return nullptr;
	}
	return s;
}

const std::vector<Module *> &Sema::exported_interface(Module &m) {
	auto cached = exported_interface_cache_.find(&m);
	if (cached != exported_interface_cache_.end()) {
		return cached->second;
	}
	// Install the source file itself before following pub-import edges. This
	// makes public-import cycles a finite fixed point over declaration owners.
	auto &result = exported_interface_cache_[&m];
	std::unordered_set<Module *> seen;
	result.push_back(&m);
	seen.insert(&m);
	auto add_unique = [&](Module *candidate) {
		if (seen.insert(candidate).second) {
			result.push_back(candidate);
		}
	};
	std::vector<ImportDecl *> imports;
	collect_imports(m.decls, imports);
	for (ImportDecl *import : imports) {
		if (!import || !import->is_pub) {
			continue;
		}
		if (import->targets.empty()) {
			if (import->target) {
				for (Module *candidate : exported_interface(*import->target)) {
					add_unique(candidate);
				}
			}
		} else {
			for (Module *target : import->targets) {
				if (!target) continue;
				for (Module *candidate : exported_interface(*target)) {
					add_unique(candidate);
				}
			}
		}
	}
	return result;
}

const std::vector<Module *> &Sema::visible_interface(Module &m) {
	auto cached = visible_interface_cache_.find(&m);
	if (cached != visible_interface_cache_.end()) {
		return cached->second;
	}
	auto &result = visible_interface_cache_[&m];
	std::unordered_set<Module *> seen;
	result.push_back(&m);
	seen.insert(&m);
	std::vector<ImportDecl *> imports;
	collect_imports(m.decls, imports);
	for (ImportDecl *import : imports) {
		if (!import) {
			continue;
		}
		if (import->targets.empty()) {
			if (import->target) {
				for (Module *candidate : exported_interface(*import->target)) {
					if (seen.insert(candidate).second) {
						result.push_back(candidate);
					}
				}
			}
		} else {
			for (Module *target : import->targets) {
				if (!target) continue;
				for (Module *candidate : exported_interface(*target)) {
					if (seen.insert(candidate).second) {
						result.push_back(candidate);
					}
				}
			}
		}
	}
	return result;
}

Symbol *Sema::resolve_path_any(const std::vector<std::string> &path, Module &from) {
	if (path.empty()) {
		return nullptr;
	}
	if (visible_symbol_cache_ready_) {
		resolve_cache_key_buf_.clear();
		resolve_cache_key_buf_.append(from.fpath);
		resolve_cache_key_buf_.push_back('\n');
		for (size_t i = 0; i < from.lexical_path.size(); ++i) {
			if (i > 0) {
				resolve_cache_key_buf_.push_back('.');
			}
			resolve_cache_key_buf_.append(from.lexical_path[i]);
		}
		resolve_cache_key_buf_.push_back('\n');
		for (size_t i = 0; i < path.size(); ++i) {
			if (i > 0) {
				resolve_cache_key_buf_.push_back('.');
			}
			resolve_cache_key_buf_.append(path[i]);
		}
		auto cached = visible_symbol_cache_.find(resolve_cache_key_buf_);
		if (cached != visible_symbol_cache_.end()) {
			return cached->second;
		}
	}
	// Build the requesting file's filtered namespace view. Explicit namespace
	// fragments may merge public overloads; headerless imports always bind
	// unqualified names and therefore collide across source files.
	const auto &visible = visible_interface(from);
	std::vector<Symbol *> candidates;
	std::vector<Module *> owners;
	for (Module *module : visible) {
		if (!module) {
			continue;
		}
		std::string name;
		bool matches = false;
		if (path.size() == 1) {
			matches = module == &from || module->mod_path.empty() || module->mod_path == from.mod_path;
			name = path.front();
			if (module->fpath == from.fpath &&
				module->mod_path.size() < from.lexical_path.size() &&
				std::equal(module->mod_path.begin(), module->mod_path.end(), from.lexical_path.begin())) {
				std::vector<std::string> scoped(from.lexical_path.begin() + module->mod_path.size(),
					from.lexical_path.end());
				scoped.push_back(path.front());
				name = join_path(scoped);
				matches = true;
			}
		} else if (module->mod_path.size() < path.size() &&
			std::equal(module->mod_path.begin(), module->mod_path.end(), path.begin())) {
			matches = true;
			name = join_path(std::vector<std::string>(path.begin() + module->mod_path.size(), path.end()));
		} else if (module == &from || module->fpath == from.fpath) {
			matches = true;
			name = join_path(path);
		}
		if (!matches) {
			continue;
		}
		Symbol *symbol = lookup_in_module(*module, name);
		if (!symbol && module->fpath == from.fpath && !from.lexical_path.empty() &&
			module->mod_path.size() <= from.lexical_path.size() &&
			std::equal(module->mod_path.begin(), module->mod_path.end(), from.lexical_path.begin())) {
			std::vector<std::string> scoped(from.lexical_path.begin() + module->mod_path.size(),
				from.lexical_path.end());
			scoped.insert(scoped.end(), path.begin(), path.end());
			name = join_path(scoped);
			symbol = lookup_in_module(*module, name);
		}
		if (!symbol && path.size() == 1 && module->fpath == from.fpath &&
			!from.lexical_path.empty()) {
			name = path.front();
			symbol = lookup_in_module(*module, name);
		}
		if (!symbol) {
			continue;
		}
		candidates.push_back(symbol);
		owners.push_back(module);
	}
	if (candidates.empty()) {
		if (visible_symbol_cache_ready_) {
			visible_symbol_cache_.emplace(resolve_cache_key_buf_, nullptr);
		}
		return nullptr;
	}
	if (candidates.size() > 1) {
		const bool all_functions =
			std::all_of(candidates.begin(), candidates.end(),
						[](const Symbol *candidate) { return candidate->kind == SymKind::Fn; });
		bool explicit_namespace_fragments = all_functions;
		if (path.size() == 1) {
			std::vector<std::string> namespace_path;
			for (Module *owner : owners) {
				if (!owner) {
					explicit_namespace_fragments = false;
					break;
				}
				const std::vector<std::string> &owner_path = owner->mod_path;
				const std::vector<std::string> &effective_path =
					owner == &from && owner_path.empty() ? from.lexical_path : owner_path;
				if (effective_path.empty()) {
					explicit_namespace_fragments = false;
					break;
				}
				if (namespace_path.empty()) {
					namespace_path = effective_path;
				} else if (namespace_path != effective_path) {
					explicit_namespace_fragments = false;
					break;
				}
			}
		} else {
			// Qualified lookup can reach nested namespace fragments with
			// different file-level prefixes, but a headerless source is still
			// an unqualified provider and must not be merged into that overload set.
			for (Module *owner : owners) {
				if (!owner || owner->mod_path.empty()) {
					explicit_namespace_fragments = false;
					break;
				}
			}
		}
		if (!explicit_namespace_fragments) {
			if (path.size() == 1) {
				log::report(log::Level::Error, from.mod_loc,
							"imported name '" + path.front() +
								"' is provided by multiple source files");
			} else {
				log::report(log::Level::Error, from.mod_loc,
							"qualified name '" + join_path(path) +
								"' is provided by multiple source files");
			}
			if (visible_symbol_cache_ready_) {
				visible_symbol_cache_.emplace(resolve_cache_key_buf_, nullptr);
			}
			return nullptr;
		}
		mark_import_use(path, from);
	}
	Symbol *result = candidates.front();
	if (candidates.size() > 1 && result->kind == SymKind::Fn) {
		Symbol merged = *result;
		merged.overloads.clear();
		for (const Symbol *candidate : candidates) {
			merged.overloads.insert(merged.overloads.end(), candidate->overloads.begin(),
									candidate->overloads.end());
		}
		merged.fn = merged.overloads.empty() ? nullptr : merged.overloads.front();
		merged_symbols_.push_back(std::move(merged));
		result = &merged_symbols_.back();
	}
	if (visible_symbol_cache_ready_) {
		visible_symbol_cache_.emplace(resolve_cache_key_buf_, result);
	}
	return result;
	/*
	// Namespace-capable structs nested in the current module are addressed
	// relative to that module (e.g. `foo.bar` from `main`).
	for (size_t i = path.size() - 1; i >= 1; --i) {
		std::vector<std::string> module_path = from.mod_path;
		module_path.insert(module_path.end(), path.begin(), path.begin() + i);
		if (Module *m = resolver_.find_loaded(module_path)) {
			if (Symbol *s = lookup_in_module(*m, path.back())) {
				return s;
			}
		}
	}
	// Try increasingly shorter module prefixes.
	for (size_t i = path.size() - 1; i >= 1; --i) {
		std::vector<std::string> module_path(path.begin(), path.begin() + i);
		std::string				 sym = path[i];
		Module					*m	 = resolver_.find_loaded(module_path);
		if (m) {
			Symbol *s = lookup_in_module(*m, sym);
			if (s) {
				return s;
			}
		}
	}
	// Same-module lookup: try full display path, then search enclosing
	// same-file namespace modules for a bare name.
	Symbol *s = lookup_in_module(from, join_path(path));
	if (s) {
		return s;
	}
	if (path.size() == 1) {
		Module *scope = &from;
		while (scope) {
			s = lookup_in_module(*scope, path[0]);
			if (s) {
				return s;
			}
			const auto &scope_path = scope->mod_path;
			if (scope_path.empty()) {
				break;
			}
			std::vector<std::string> parent_path(scope_path.begin(), scope_path.end() - 1);
			scope = parent_path.empty() ? nullptr : resolver_.find_loaded(parent_path);
		}
	}
	return nullptr; */
}

Symbol *Sema::resolve_path(const std::vector<std::string> &path, Module &from) {
	Symbol *s = resolve_path_any(path, from);
	if (s && s->owner_module != &from) {
		mark_import_use(path, from);
		if (marked_import_chains_[&from].insert(s->owner_module).second) {
			std::unordered_set<Module *> marked_files;
			std::function<void(Module &, Module *)> mark_chain = [&](Module &requester, Module *owner) {
				if (!marked_files.insert(&requester).second) {
					return;
				}
				std::vector<ImportDecl *> nested_imports;
				collect_imports(requester.decls, nested_imports);
				for (ImportDecl *import : nested_imports) {
					if (!import) {
						continue;
					}
					for (Module *target : import->targets.empty() ? std::vector<Module *>{import->target}
																						 : import->targets) {
						if (!target) continue;
						bool reaches = target == owner;
						if (!reaches) {
							for (Module *candidate : exported_interface(*target)) {
								if (candidate == owner) {
									reaches = true;
									break;
								}
							}
						}
						if (reaches) {
							import->used = true;
							mark_chain(*target, owner);
						}
					}
				}
			};
			mark_chain(from, s->owner_module);
		}
		// Namespace-capable structs are materialized as child modules, but a
		// child and its enclosing source file still form one lexical unit.
		// Keep private declarations visible across that same-file boundary.
		Module *target = resolver_.find_loaded(s->module_path);
		if (target && resolver_.same_source_file(*target, from)) {
			return s;
		}
	}
	if (!s || s->owner_module == &from) {
		return s;
	}
	if (s->kind == SymKind::Fn) {
		for (FnDecl *fn : s->overloads) {
			if (fn && fn->is_pub) {
				return s;
			}
		}
		return nullptr;
	}
	return s->is_pub ? s : nullptr;
}

void Sema::mark_import_use(const std::vector<std::string> &path, Module &from) {
	if (!marked_import_uses_[&from].insert(join_path(path)).second) {
		return;
	}
	std::vector<ImportDecl *> imports;
	collect_imports(from.decls, imports);
	for (ImportDecl *import : imports) {
		if (!import) {
			continue;
		}
		for (Module *target : import->targets.empty() ? std::vector<Module *>{import->target}
																					 : import->targets) {
			if (!target) continue;
			const auto &ns = target->mod_path;
			const bool qualified_match = path.size() > 1 && ns.size() + 1 == path.size() &&
				std::equal(ns.begin(), ns.end(), path.begin());
			const bool headerless_match = path.size() == 1 && ns.empty();
			if (qualified_match || headerless_match ||
				(target->mod_path == from.mod_path && path.size() == 1)) {
				import->used = true;
			}
		}
	}
	if (path.size() == 1) {
		return;
	}
	size_t					  best = 0;
	std::vector<ImportDecl *> selected;
	// Nested namespace modules are materialized as separate Module objects,
	// but declarations in a file-level module (including its imports) are in
	// scope for those nested modules. Search the current module and its
	// same-file ancestors so a reference from `main.Vec2` can mark an import
	// declared in `main`.
	for (Module *scope : resolver_.all()) {
		const bool is_current = scope == &from;
		const bool is_same_namespace_ancestor =
			scope && scope->mod_path.size() <= from.mod_path.size() &&
			std::equal(scope->mod_path.begin(), scope->mod_path.end(), from.mod_path.begin());
		const bool is_same_file_ancestor =
			!is_current && is_same_namespace_ancestor && resolver_.same_source_file(*scope, from);
		if (!scope || (!is_current && !is_same_file_ancestor)) {
			continue;
		}
		for (auto &td : scope->decls) {
			if (!td || td->kind != TopKind::Import || !td->imp) {
				continue;
			}
			auto consider = [&](const std::vector<std::string> &imp) {
				if (imp.empty() || imp.size() > path.size() || imp.size() < best) {
					return;
				}
				if (!std::equal(imp.begin(), imp.end(), path.begin())) {
					return;
				}
				if (imp.size() > best) {
					best = imp.size();
					selected.clear();
					selected.push_back(td->imp.get());
				} else if (imp.size() == best) {
					// A logical module may be assembled from several source files.
					// Identical imports in those fragments are all valid providers for
					// the same qualified reference; attributing the use to only the
					// first one produces a spurious warning in every later fragment.
					selected.push_back(td->imp.get());
				}
			};
			if (td->imp->group.empty()) {
				consider(td->imp->path);
			} else {
				for (const auto &imp : td->imp->group) {
					consider(imp);
				}
			}
		}
	}
	for (ImportDecl *imp : selected) {
		if (imp) {
			imp->used = true;
		}
	}
}

} // namespace stellac
