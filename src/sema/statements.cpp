#include "internal.hpp"

namespace stellac {

void Sema::check_stmt(Stmt &s, FnDecl &ctx, Module &m, ScopeStack &scope, StType *ret) {
	MacroDiagnosticContextGuard<Stmt> diagnostic_context(s);
	switch (s.kind) {
	case StmtKind::Block: {
		scope.enter();
		for (auto &st : s.body) {
			check_stmt(*st, ctx, m, scope, ret);
		}
		scope.leave();
		return;
	}
	case StmtKind::ExprStmt:
		if (s.expr) {
			Expr *previous_macro_call = allowed_macro_call_;
			allowed_macro_call_ = s.expr->kind == ExprKind::Call ? s.expr.get() : nullptr;
			check_expr(*s.expr, ctx, m, scope);
			allowed_macro_call_ = previous_macro_call;
			if (s.expr->kind == ExprKind::Call && s.expr->resolved_fn &&
				s.expr->resolved_fn->is_macro && !s.expr->sema_invalid) {
				FnDecl *macro = s.expr->resolved_fn;
				auto expanded = expand_macro_call(*s.expr, ctx, m, scope);
				if (!expanded) {
					return;
				}
				s.kind = StmtKind::Block;
				s.loc = s.expr->loc;
				s.binding_module = nullptr;
				s.invocation_loc = {};
				s.expansion_chain.clear();
				s.expr.reset();
				s.body = std::move(expanded->body);
				FnDecl *previous_generic = generic_context_;
				if (macro->is_generic_instance) {
					generic_context_ = macro;
				}
				macro_control_bases_.push_back({loop_stack_.size(), switch_stack_.size()});
				++macro_expansion_depth_;
				check_stmt(s, ctx, m, scope, ret);
				--macro_expansion_depth_;
				macro_control_bases_.pop_back();
				generic_context_ = previous_generic;
			}
		}
		return;
	case StmtKind::VarDecl:
	case StmtKind::ConstDecl: {
		for (auto &var : s.vars) {
			if (var.is_static && var.is_const) {
				log::report(log::Level::Error, var.loc,
							"'static' cannot be used with 'const' because const bindings are compile-time values");
			}
			if (var.is_thread_local && var.is_const) {
				log::report(log::Level::Error, var.loc,
							"'thread_local' cannot be used with 'const' because const bindings are compile-time values");
			}
			Module &var_module = var.binding_module ? *var.binding_module : m;
			std::set<std::pair<std::string, bool>> seen_attrs;
			for (auto &a : var.attrs.attrs) {
				const AttributeSpec *spec = lookup_attribute_spec(a.name);
				if (!spec) {
					log::report(log::Level::Error, a.loc, "unknown " + attr_diag_prefix(a));
					continue;
				}
				if (!attribute_target_allowed(*spec, AttributeTarget::Local)) {
					log::report(log::Level::Error, a.loc,
								attr_diag_prefix(a) + " is not valid on local declarations");
					continue;
				}
				if (spec->polarity != AttributePolarity::PositiveOnly || a.negated) {
					log::report(log::Level::Error, a.loc,
								attr_diag_prefix(a) + " does not support negation");
					continue;
				}
				if (!seen_attrs.insert({a.name, a.negated}).second) {
					log::report(log::Level::Error, a.loc,
								attr_diag_prefix(a) + " is specified more than once");
				}
				if (spec->arg_kind != AttributeArgKind::None) {
					log::report(log::Level::Error, a.loc,
								attr_diag_prefix(a) + " takes no arguments");
				}
			}
			if (var.ty && var.init) {
				if (var.init->kind == ExprKind::SrcEmbed) {
					resolve_src_embed(*var.init, ctx, m, scope);
				}
				if (!var.init->sema_invalid || !var.ty->array_len_inferred) {
					infer_array_lengths(*var.ty, *var.init);
				} else {
					var.ty->array_len_inferred = false;
					var.ty->sema_invalid	   = true;
				}
			}
			StType *ty	 = var.ty ? resolve_type(*var.ty, var_module) : nullptr;
			var.resolved = ty;
			std::unordered_set<StType *> storage_active;
			if (ty && !var.ty->sema_invalid && !storage_type_valid(ty, storage_active)) {
				log::report(log::Level::Error, var.loc,
							"binding '" + var.name +
								"' must have a non-void, non-zero-sized storage type");
			}
			if (!var.is_mutable && !var.init) {
				log::report(log::Level::Error, var.loc,
							"immutable binding '" + var.name + "' requires an initializer");
			}
			// `resolve_type` returns the shared void_ intern as a fallback when a
			// named type fails to resolve (it has already emitted "Unknown type
			// 'X'"). Detect that here so we don't cascade a secondary
			// "expected 'void', got '...'" mismatch onto the primary resolution
			// error; just typecheck/skip the initializer. The "user wrote void"
			// case (single == "void") is excluded so legitimate void slots still
			// reject non-void initializers.
			bool ty_failed = false;
			if (ty && ty == types_.void_ty() && var.ty && var.ty->kind == TypeRef::Named &&
				var.ty->single != "void" && var.ty->single != "Void") {
				ty_failed = true;
			}
			if (var.init) {
				StType *ity = check_expr_with_expected(*var.init, ctx, m, scope, ty);
				if (ty && ity && !ty_failed) {
					check_type_compat(
						ty, ity, *var.init, var.loc,
						"cannot initialize '" + var.name + "': value not representable in '" +
							type_name_for_diag(ty) + "'",
						"cannot initialize '" + var.name + "': expected '" +
							type_name_for_diag(ty) + "', got '" + type_name_for_diag(ity) + "'");
				}
				if (!ty) {
					ty = ity;
				}
				if (var.is_const && !var.init->has_const_value &&
					var.init->kind != ExprKind::StringLit &&
					var.init->kind != ExprKind::NullLit &&
					var.init->kind != ExprKind::StructLit && var.init->kind != ExprKind::ArrayLit) {
					log::report(
						log::Level::Error, var.init->loc,
						"'const' binding initializer must be a compile-time constant expression");
				}
				if (var.is_static || var.is_thread_local) {
					const Expr *inner = var.init.get();
					while (inner && (inner->kind == ExprKind::Cast || inner->kind == ExprKind::Unary)) {
						inner = inner->lhs.get();
					}
					const bool is_const =
						var.init->has_const_value ||
						(inner && (is_const_int_expr(const_cast<Expr &>(*inner)) ||
									 inner->kind == ExprKind::StringLit ||
									 inner->kind == ExprKind::NullLit ||
									 inner->kind == ExprKind::StructLit ||
									 inner->kind == ExprKind::ArrayLit));
					if (!is_const) {
						log::report(log::Level::Error, var.init->loc,
									"'" + std::string(var.is_static ? "static" : "thread_local") +
									"' local initializer must be a compile-time constant expression");
					}
				}
			}
			if (!ty) {
				log::report(log::Level::Error, s.loc, "cannot infer type");
				ty = types_.void_ty();
			}
			if (ty && underlying_type(ty)->kind == TypeKind::VaList) {
				if (!var.is_mutable || var.init) {
					log::report(log::Level::Error, var.loc,
								"@va.list locals must be mutable and uninitialized");
				}
			}
			Symbol sym;
			sym.kind	   = var.is_const ? SymKind::Const : SymKind::Var;
			sym.is_mutable = var.is_mutable;
			sym.simple	   = var.name;
			sym.type	   = ty;
			sym.loc		   = var.loc;
			sym.usage	   = &var.used;
			sym.local_decl = &var;
			if (!var.is_mutable && var.init && var.init->has_const_value) {
				sym.has_const_value		 = true;
				sym.const_value_is_float = var.init->const_value_is_float;
				sym.const_int_high_bits_ones = var.init->const_int_high_bits_ones;
				sym.const_bool_val = var.init->is_const_bool ? var.init->const_bool_val : false;
				sym.const_int_val  = var.init->int_val;
				sym.const_flt_val  = var.init->flt_val;
			}
			if (var.name != "_") {
				if (!scope.define(var.name, sym)) {
					auto diag = log::error(var.loc, "duplicate local binding '" + var.name + "'")
									.code("E_DUPLICATE_DECLARATION")
									.phase("semantic");
					if (Symbol *previous = scope.lookup_current(var.name)) {
						diag.note(previous->loc, "previous local binding was here");
					}
					diag.emit();
				}
			}
			active_locals_.push_back(&var);
			if (ty && underlying_type(ty)->kind == TypeKind::VaList) {
				va_list_locals_.insert(&var);
				va_states_[&var] = static_cast<unsigned>(VaState::Uninitialized);
			}
		}
		return;
	}
	case StmtKind::If: {
		if (s.cond) {
			StType *ct = check_expr(*s.cond, ctx, m, scope);
			if (!s.cond->sema_invalid) {
				expect_bool_cond(s.cond->loc, ct);
			}
		}
		if (s.cond && s.cond->is_const_bool) {
			bool dead = !s.cond->const_bool_val;
			if (dead) {
				if (s.else_block) {
					check_stmt(*s.else_block, ctx, m, scope, ret);
				}
			} else {
				check_stmt(*s.then_block, ctx, m, scope, ret);
			}
			return;
		}
		// A cursor transition in only one branch is not guaranteed on the
		// path after the if.  Check each arm from the same incoming state and
		// merge only transitions established by both arms.  This also keeps a
		// va_end immediately before a branch-local return from poisoning the
		// live path that continues after the if.
		auto in_va_states = va_states_;
		if (s.then_block) {
			check_stmt(*s.then_block, ctx, m, scope, ret);
		}
		auto then_va_states = va_states_;
		va_states_			= in_va_states;
		if (s.else_block) {
			check_stmt(*s.else_block, ctx, m, scope, ret);
		}
		auto else_va_states = va_states_;
		if (s.else_block) {
			va_states_		  = in_va_states;
			bool then_returns = s.then_block && definite_return(*s.then_block);
			bool else_returns = s.else_block && definite_return(*s.else_block);
			if (then_returns && !else_returns) {
				va_states_ = else_va_states;
			} else if (else_returns && !then_returns) {
				va_states_ = then_va_states;
			} else {
				for (auto &[local, state] : va_states_) {
					unsigned a = then_va_states.count(local) ? then_va_states[local] : state;
					unsigned b = else_va_states.count(local) ? else_va_states[local] : state;
					state	   = a | b;
				}
			}
		} else {
			if (s.then_block && definite_return(*s.then_block)) {
				va_states_ = in_va_states;
			} else {
				va_states_ = in_va_states;
				for (auto &[local, state] : va_states_) {
					if (then_va_states.count(local)) {
						state |= then_va_states[local];
					}
				}
			}
		}
		return;
	}
	case StmtKind::For: {
		scope.enter();
		if (s.for_init) {
			check_stmt(*s.for_init, ctx, m, scope, ret);
		}
		if (s.for_cond) {
			StType *ct = check_expr(*s.for_cond, ctx, m, scope);
			if (!s.for_cond->sema_invalid) {
				expect_bool_cond(s.for_cond->loc, ct);
			}
		}
		auto for_in_va_states = va_states_;
		if (s.for_step) {
			check_expr(*s.for_step, ctx, m, scope);
		}
		enter_loop();
		if (s.loop_body) {
			check_stmt(*s.loop_body, ctx, m, scope, ret);
		}
		auto for_body_va_states = va_states_;
		va_states_				= for_in_va_states;
		for (auto &[local, state] : va_states_) {
			if (for_body_va_states.count(local)) {
				state |= for_body_va_states[local];
			}
		}
		leave_loop();
		scope.leave();
		return;
	}
	case StmtKind::Match: {
		StType *scrut_ty = s.scrutinee ? check_expr(*s.scrutinee, ctx, m, scope) : nullptr;
		StType *ut		 = underlying_type(scrut_ty);
		bool	scrut_is_ptr	 = false;
		bool	scrut_ptr_mut	 = false;
		StType *enum_ty			 = nullptr;
		if (ut && ut->kind == TypeKind::Pointer) {
			StType *pointee = underlying_type(ut->base);
			if (pointee && pointee->kind == TypeKind::Enum) {
				scrut_is_ptr  = true;
				scrut_ptr_mut = ut->is_mutable;
				enum_ty		  = pointee;
			}
		} else if (ut && ut->kind == TypeKind::Enum) {
			enum_ty = ut;
		}
		bool ok_kind = enum_ty != nullptr ||
					   (ut && (ut->kind == TypeKind::SInt || ut->kind == TypeKind::UInt));
		if (!ok_kind && !is_poison(ut)) {
			log::report(log::Level::Error, s.loc,
						"match scrutinee must be an integer, enum, or pointer to enum");
		}

		uint64_t backing_mask = ~uint64_t{0};
		if (enum_ty && enum_ty->base) {
			StType *bt = underlying_type(enum_ty->base);
			int		bw = (bt && bt->width > 0) ? bt->width : types_.pointer_width();
			if (bw < 64) {
				backing_mask = (uint64_t{1} << bw) - 1;
			}
		}

		auto all_enum_variants_covered = [&](const std::unordered_set<uint64_t> &seen) -> bool {
			if (!enum_ty) return false;
			for (size_t i = 0; i < enum_ty->enum_values.size(); ++i) {
				if (!seen.count(enum_ty->enum_values[i] & backing_mask)) {
					return false;
				}
			}
			return true;
		};

		std::unordered_set<uint64_t>							 seen;
		bool													 has_wildcard = false;
		auto													 match_in_va_states = va_states_;
		std::vector<std::unordered_map<VarDeclInfo *, unsigned>> match_arm_states;

		for (auto &arm : s.arms) {
			va_states_ = match_in_va_states;
			if (has_wildcard || (enum_ty && all_enum_variants_covered(seen))) {
				log::report(log::Level::Error, arm.loc, "unreachable match arm");
			}
			const bool is_or_arm = arm.patterns.size() > 1;
			scope.enter();

			for (auto &pat : arm.patterns) {
				if (pat.kind == MatchPatternKind::Wildcard) {
					if (has_wildcard || (enum_ty && all_enum_variants_covered(seen))) {
						log::report(log::Level::Error, pat.loc, "unreachable wildcard pattern");
					}
					has_wildcard = true;
					continue;
				}

				if (pat.kind == MatchPatternKind::Value) {
					if (pat.expr) {
						check_expr_with_expected(*pat.expr, ctx, m, scope,
												 enum_ty ? enum_ty : scrut_ty);
					}
					if (enum_ty) {
						// Resolve unit variant tag (either `.Var` or `Enum.Var`).
						std::string vname;
						if (pat.expr && pat.expr->kind == ExprKind::EnumMember) {
							vname = pat.expr->member;
						} else if (pat.expr && pat.expr->kind == ExprKind::Member) {
							vname = pat.expr->member;
						}
						if (pat.expr && !pat.expr->sema_invalid && pat.expr->ty &&
							!eq_type(underlying_type(pat.expr->ty), enum_ty)) {
							log::report(log::Level::Error, pat.loc,
										"match pattern type mismatch: expected '" +
											type_name_for_diag(enum_ty) + "', got '" +
											type_name_for_diag(pat.expr->ty) + "'");
						}
						size_t vidx = SIZE_MAX;
						for (size_t vi = 0; vi < enum_ty->field_names.size(); ++vi) {
							if (enum_ty->field_names[vi] == vname) {
								vidx = vi;
								break;
							}
						}
						if (vidx != SIZE_MAX) {
							if (enum_ty->is_tagged_enum && vidx < enum_ty->enum_variants.size() &&
								enum_ty->enum_variants[vidx].payload_kind != EnumPayloadKind::Unit) {
								log::report(log::Level::Error, pat.loc,
											"tagged enum variant '" + vname +
												"' requires payload destructuring");
							}
							uint64_t tag = enum_ty->enum_values[vidx];
							pat.resolved_tag  = tag;
							pat.variant_index = vidx;
							if (!seen.insert(tag & backing_mask).second) {
								log::report(log::Level::Error, pat.loc,
											"duplicate pattern in match");
							}
						}
					} else if (pat.expr) {
						if (!is_const_int_expr(*pat.expr)) {
							log::report(log::Level::Error, pat.loc,
										"match pattern must be a compile-time constant");
						} else {
							uint64_t uv = 0;
							if (pat.expr->kind == ExprKind::IntLit ||
								pat.expr->kind == ExprKind::CharLit) {
								uv = pat.expr->int_val;
							} else {
								uv = static_cast<uint64_t>(eval_const_int(*pat.expr));
							}
							pat.resolved_tag = uv;
							if (!contains_sizeof(*pat.expr) && !seen.insert(uv).second) {
								log::report(log::Level::Error, pat.loc,
											"duplicate pattern in match");
							}
							if (!contains_sizeof(*pat.expr) && ut &&
								(ut->kind == TypeKind::UInt || ut->kind == TypeKind::SInt)) {
								int bw	= ut->width;
								int eff = (bw <= 0) ? types_.pointer_width() : bw;
								if (eff < 64) {
									if (ut->kind == TypeKind::UInt) {
										bool out_of_range = false;
										if (pat.expr->kind == ExprKind::IntLit ||
											pat.expr->kind == ExprKind::CharLit) {
											out_of_range = !fits_uint_width(pat.expr->int_val, eff);
										} else {
											uint64_t max = (uint64_t{1} << eff) - 1;
											int64_t	 sv	 = static_cast<int64_t>(uv);
											out_of_range = sv < 0 || static_cast<uint64_t>(sv) > max;
										}
										if (out_of_range) {
											log::report(
												log::Level::Error, pat.loc,
												"pattern value out of range for unsigned scrutinee");
										}
									} else {
										int64_t sv = static_cast<int64_t>(uv);
										int64_t lo = -(int64_t{1} << (eff - 1));
										int64_t hi = (int64_t{1} << (eff - 1)) - 1;
										if (sv < lo || sv > hi) {
											log::report(
												log::Level::Error, pat.loc,
												"pattern value out of range for signed scrutinee");
										}
									}
								} else if (ut->kind == TypeKind::UInt &&
										   pat.expr->kind != ExprKind::IntLit &&
										   pat.expr->kind != ExprKind::CharLit &&
										   static_cast<int64_t>(uv) < 0) {
									log::report(
										log::Level::Error, pat.loc,
										"pattern value out of range for unsigned scrutinee");
								}
							}
						}
					}
					continue;
				}

				// MatchPatternKind::Variant
				if (is_or_arm) {
					log::report(log::Level::Error, pat.loc,
								"bindings are not allowed in '|' or-patterns");
				}
				if (!enum_ty) {
					log::report(log::Level::Error, pat.loc,
								"variant pattern requires an enum scrutinee");
					continue;
				}
				std::string vname;
				if (pat.expr && pat.expr->kind == ExprKind::EnumMember) {
					vname = pat.expr->member;
				} else if (pat.expr && pat.expr->kind == ExprKind::Member) {
					vname = pat.expr->member;
					// Verify qualifier matches enum_ty when qualified (`Enum.Var(...)`).
					std::vector<std::string> qpath;
					if (flatten_member_path(*pat.expr, qpath) && qpath.size() >= 2) {
						std::vector<std::string> head(qpath.begin(), qpath.end() - 1);
						if (Symbol *qsym = resolve_path(head, m)) {
							if (qsym->kind == SymKind::Enum && qsym->type &&
								qsym->en && qsym->en->generic_params.empty() &&
								!eq_type(underlying_type(qsym->type), enum_ty)) {
								log::report(log::Level::Error, pat.loc,
											"variant pattern enum '" +
												type_name_for_diag(qsym->type) +
												"' does not match scrutinee type '" +
												type_name_for_diag(enum_ty) + "'");
							}
						}
					}
				}
				size_t vidx = SIZE_MAX;
				for (size_t vi = 0; vi < enum_ty->field_names.size(); ++vi) {
					if (enum_ty->field_names[vi] == vname) {
						vidx = vi;
						break;
					}
				}
				if (vidx == SIZE_MAX) {
					log::report(log::Level::Error, pat.loc,
								"enum '" + type_name_for_diag(enum_ty) + "' has no variant '" +
									vname + "'");
					continue;
				}
				pat.resolved_tag  = enum_ty->enum_values[vidx];
				pat.variant_index = vidx;
				if (!seen.insert(pat.resolved_tag & backing_mask).second) {
					log::report(log::Level::Error, pat.loc, "duplicate pattern in match");
				}
				if (!enum_ty->is_tagged_enum || vidx >= enum_ty->enum_variants.size() ||
					enum_ty->enum_variants[vidx].payload_kind == EnumPayloadKind::Unit) {
					log::report(log::Level::Error, pat.loc,
								"unit variant '" + vname + "' has no payload to destructure");
					continue;
				}
				const auto &vi = enum_ty->enum_variants[vidx];
				if (pat.is_struct_pattern && vi.payload_kind != EnumPayloadKind::Struct) {
					log::report(log::Level::Error, pat.loc,
								"variant '" + vname + "' is positional, not struct-style");
					continue;
				}
				if (!pat.is_struct_pattern && vi.payload_kind != EnumPayloadKind::Tuple) {
					log::report(log::Level::Error, pat.loc,
								"variant '" + vname + "' is struct-style, not positional");
					continue;
				}

				auto bind_var = [&](MatchBinding &b, StType *field_ty) {
					StType *bty = scrut_is_ptr ? types_.pointer(field_ty, scrut_ptr_mut) : field_ty;
					b.resolved	= bty;
					if (b.name == "_") {
						return;
					}
					if (scrut_is_ptr && b.is_mut) {
						log::report(log::Level::Error, b.loc,
									"'mut' binding modifier is only for by-value match bindings");
					}
					auto vd		   = std::make_unique<VarDeclInfo>();
					vd->name	   = b.name;
					vd->loc		   = b.loc;
					vd->is_mutable = !scrut_is_ptr && b.is_mut;
					vd->resolved   = bty;
					Symbol bsym;
					bsym.kind		= SymKind::Var;
					bsym.simple		= b.name;
					bsym.type		= bty;
					bsym.loc		= b.loc;
					bsym.usage		= &vd->used;
					bsym.local_decl = vd.get();
					if (!scope.define(b.name, bsym)) {
						log::report(log::Level::Error, b.loc,
									"duplicate binding '" + b.name + "' in match pattern");
					}
					b.var_decl = std::move(vd);
				};

				if (!pat.is_struct_pattern) {
					if (pat.bindings.size() != vi.field_types.size()) {
						log::report(log::Level::Error, pat.loc,
									"variant '" + vname + "' expects " +
										std::to_string(vi.field_types.size()) +
										" binding(s), got " + std::to_string(pat.bindings.size()));
					}
					for (size_t bi = 0; bi < pat.bindings.size() && bi < vi.field_types.size();
						 ++bi) {
						bind_var(pat.bindings[bi], vi.field_types[bi]);
					}
				} else {
					std::unordered_set<std::string> matched_fields;
					for (auto &b : pat.bindings) {
						if (!matched_fields.insert(b.field_name).second) {
							log::report(log::Level::Error, b.loc,
										"duplicate field '" + b.field_name + "' in match pattern");
						}
						size_t fi = SIZE_MAX;
						for (size_t k = 0; k < vi.field_names.size(); ++k) {
							if (vi.field_names[k] == b.field_name) {
								fi = k;
								break;
							}
						}
						if (fi == SIZE_MAX) {
							log::report(log::Level::Error, b.loc,
										"variant '" + vname + "' has no field '" + b.field_name +
											"'");
						} else {
							bind_var(b, vi.field_types[fi]);
						}
					}
					for (const auto &req : vi.field_names) {
						if (!matched_fields.count(req)) {
							log::report(log::Level::Error, pat.loc,
										"missing field '" + req + "' in struct pattern for '" +
											vname + "'");
						}
					}
				}
			}

			if (arm.body) {
				check_stmt(*arm.body, ctx, m, scope, ret);
			}
			scope.leave();
			match_arm_states.push_back(va_states_);
		}

		if (!has_wildcard && ok_kind) {
			if (enum_ty) {
				for (size_t vi = 0; vi < enum_ty->field_names.size(); ++vi) {
					if (!seen.count(enum_ty->enum_values[vi] & backing_mask)) {
						log::report(log::Level::Error, s.loc,
									"non-exhaustive match on enum '" +
										type_name_for_diag(enum_ty) + "': missing variant '" +
										enum_ty->field_names[vi] + "'");
					}
				}
			} else {
				log::report(log::Level::Error, s.loc,
							"non-exhaustive match on integer type: add a '_' wildcard arm");
			}
		}

		va_states_ = match_in_va_states;
		for (auto &arm_states : match_arm_states) {
			for (auto &[local, state] : va_states_) {
				if (arm_states.count(local)) {
					state |= arm_states[local];
				}
			}
		}
		return;
	}
	case StmtKind::Return: {
		if (defer_depth_ != 0) {
			log::report(log::Level::Error, s.loc, "return is not allowed in a defer block");
		}
		bool is_void = ret && underlying_type(ret)->kind == TypeKind::Void;
		if (has_attr(ctx.attrs, "must_tail") && !s.ret_val) {
			log::report(
				log::Level::Error, s.loc,
				"attribute '#[must_tail]' requires every reachable return to be a direct call");
		}
		if (s.ret_val) {
			if (is_void) {
				log::report(log::Level::Error, s.loc, "'return' with a value in a void function");
			} else {
				StType *t = check_expr_with_expected(*s.ret_val, ctx, m, scope, ret);
				if (s.ret_val->sema_invalid || is_poison(t) || is_poison(ret)) {
					return;
				}
				if (has_attr(ctx.attrs, "must_tail")) {
					bool valid = s.ret_val->kind == ExprKind::Call && s.ret_val->resolved_fn &&
								 s.ret_val->lhs && s.ret_val->lhs->kind == ExprKind::Ident &&
								 fn_signature_equal(ctx, *s.ret_val->resolved_fn);
					if (!valid) {
						log::report(log::Level::Error, s.loc,
									"attribute '#[must_tail]' requires every reachable return to "
									"be a direct call with an LLVM-compatible signature");
					}
				}
				// Typecheck the return value against the declared return type.
				// Typed values must match the declared return type exactly;
				// contextual typing applies only to literals and explicit `cast`
				// is required for other type changes.
				std::string mismatch = "return type mismatch: expected '" +
									   type_name_for_diag(ret) + "', got '" +
									   type_name_for_diag(t) + "'";
				if (s.ret_val->kind == ExprKind::Call && s.ret_val->resolved_fn) {
					FnDecl	   &callee	  = *s.ret_val->resolved_fn;
					std::string call_name = callee.name;
					if (s.ret_val->lhs && s.ret_val->lhs->kind == ExprKind::Ident) {
						call_name = join_path(s.ret_val->lhs->path);
					}
					std::string params;
					for (size_t i = 0; i < callee.params.size(); ++i) {
						if (i) {
							params += ", ";
						}
						params += type_name_for_diag(callee.params[i].resolved);
					}
					if (callee.is_variadic) {
						if (!params.empty()) {
							params += ", ";
						}
						params += "...";
					}
					mismatch = "return type mismatch: call to '" + call_name + "' selects '" +
							   callee.name + "(" + params + "): " + type_name_for_diag(t) +
							   "', which cannot be returned as '" + type_name_for_diag(ret) + "'";

					// Prefer the cause over the consequence for the common
					// overloaded-integer case.  A negative i64 literal is
					// intentionally rejected by an unsigned overload; if a
					// float overload then wins, reporting only its f64 return
					// obscures why the u64 overload was not selected.
					std::string rejected_overload;
					if (callee.sym) {
						for (size_t ai = 0;
							 ai < s.ret_val->args.size() && rejected_overload.empty(); ++ai) {
							Expr   &arg	   = *s.ret_val->args[ai];
							StType *arg_ty = underlying_type(arg.ty);
							bool	is_negative_signed_const =
								arg_ty && arg_ty->kind == TypeKind::SInt &&
								is_const_int_expr(arg) && eval_const_int(arg) < 0;
							if (!is_negative_signed_const) {
								continue;
							}
							for (FnDecl *candidate : callee.sym->overloads) {
								if (!candidate || ai >= candidate->params.size()) {
									continue;
								}
								StType *param_ty = underlying_type(candidate->params[ai].resolved);
								if (param_ty && param_ty->kind == TypeKind::UInt) {
									rejected_overload =
										candidate->name + "(" + type_name_for_diag(param_ty) + ")";
									break;
								}
							}
						}
					}
					if (!rejected_overload.empty()) {
						mismatch = "invalid type: negative 'i64' argument cannot match '" +
								   rejected_overload + "'";
					}
				}
				if (ret && t) {
					check_type_compat(ret, t, *s.ret_val, s.loc,
									  "return value not representable in '" +
										  type_name_for_diag(ret) + "'",
									  mismatch);
				}
			}
		} else if (!is_void) {
			log::report(log::Level::Error, s.loc,
						"'return' without a value in a non-void function");
		}
		return;
	}
	case StmtKind::Break:
		if (defer_depth_ != 0) {
			const size_t loop_base = defer_loop_bases_.back();
			const size_t switch_base = defer_switch_bases_.back();
			if (loop_stack_.size() <= loop_base && switch_stack_.size() <= switch_base) {
				log::report(log::Level::Error, s.loc,
							"break escaping a defer block is not allowed");
				return;
			}
		}
		if ((!in_loop() || !in_macro_loop()) && (!in_switch() || !in_macro_switch())) {
			log::report(log::Level::Error, s.loc, "'break' outside loop or switch");
		}
		return;
	case StmtKind::Continue:
		if (defer_depth_ != 0) {
			if (loop_stack_.size() <= defer_loop_bases_.back()) {
				log::report(log::Level::Error, s.loc,
							"continue escaping a defer block is not allowed");
				return;
			}
		}
		if (!in_loop() || !in_macro_loop()) {
			log::report(log::Level::Error, s.loc, "'continue' outside loop");
		}
		return;
	case StmtKind::Yield: {
		if (defer_depth_ != 0) {
			log::report(log::Level::Error, s.loc, "yield is not allowed in a defer block");
			return;
		}
		if (block_expr_depth_ == 0) {
			log::report(log::Level::Error, s.loc, "'yield' is only valid in a block expression");
			return;
		}
		if (!s.ret_val) {
			log::report(log::Level::Error, s.loc, "'yield' requires a value");
			return;
		}
		StType *yield_expected = block_expected_stack_.empty() ? nullptr : block_expected_stack_.back();
		block_yield_types_.push_back(yield_expected
			? check_expr_with_expected(*s.ret_val, ctx, m, scope, yield_expected)
			: check_expr(*s.ret_val, ctx, m, scope));
		return;
	}
	case StmtKind::Defer:
		if (has_attr(ctx.attrs, "must_tail")) {
			log::report(log::Level::Error, s.loc,
						"attribute '#[must_tail]' cannot be used with 'defer'");
		}
		if (defer_depth_ != 0) {
			log::report(log::Level::Error, s.loc, "nested defer is not allowed");
			return;
		}
		if (!s.defer_body) {
			log::report(log::Level::Error, s.loc, "defer requires a statement");
			return;
		}
		++defer_depth_;
		defer_loop_bases_.push_back(loop_stack_.size());
		defer_switch_bases_.push_back(switch_stack_.size());
		scope.enter();
		check_stmt(*s.defer_body, ctx, m, scope, ret);
		scope.leave();
		defer_loop_bases_.pop_back();
		defer_switch_bases_.pop_back();
		--defer_depth_;
		return;
	case StmtKind::Asm:
		for (auto &op : s.asm_outputs) {
			if (!op.operand) {
				log::report(log::Level::Error, s.loc,
							"inline assembly output requires an operand");
				continue;
			}
			check_expr(*op.operand, ctx, m, scope);
			if (!op.operand->is_lvalue) {
				log::report(log::Level::Error, op.operand->loc,
							"inline assembly output operand must be a writable lvalue");
			}
		}
		for (auto &op : s.asm_inputs) {
			if (!op.operand) {
				log::report(log::Level::Error, s.loc,
							"inline assembly input requires an operand");
				continue;
			}
			if (is_contextual_integer_expr(*op.operand)) {
				check_expr_with_expected(*op.operand, ctx, m, scope, types_.isize_ty());
			} else {
				check_expr(*op.operand, ctx, m, scope);
			}
			size_t constraint_index = 0;
			if (constraint_index < op.constraint.size() && op.constraint[constraint_index] == '*') {
				++constraint_index;
			}
			if (constraint_index < op.constraint.size() &&
				op.constraint[constraint_index] == 'm' && !op.operand->is_addressable) {
				log::report(log::Level::Error, op.operand->loc,
							"inline assembly memory input must be addressable");
			}
		}
		return;
	default:
		return;
	}
}

// Typecheck an expression in a known destination context.  Numeric literals
// are the one deliberately uncommitted value form: when their value fits the
// destination, record the destination type on the literal itself.  This is
// contextual typing, not a conversion of an already-typed value; variables
// and other expressions still have to match exactly or use `cast`.
//
// Array literals need this treatment recursively so `[1, 2, 3]` can be
// formed directly as a `[3]u32` literal without first becoming a
// `[3]i64` value and then being converted.
StType *Sema::check_expr_with_expected(Expr &e, FnDecl &ctx, Module &m, ScopeStack &scope,
									   StType *expected) {
	if (e.sema_invalid) {
		return e.ty ? e.ty : types_.poison_ty();
	}
	if (e.kind == ExprKind::SrcEmbed) {
		resolve_src_embed(e, ctx, m, scope);
	}
	if (is_poison(expected)) {
		if (!is_contextual_literal(e)) {
			check_expr(e, ctx, m, scope);
		}
		e.sema_invalid = true;
		e.ty		   = types_.poison_ty();
		return e.ty;
	}
	if (is_contextual_integer_expr(e)) {
		clear_contextual_integer_values(e);
	}
	if (e.kind == ExprKind::Unary || e.kind == ExprKind::Binary ||
		e.kind == ExprKind::Conditional) {
		clear_folded_value(e);
	}
	StType *et = underlying_type(expected);
	if (e.kind == ExprKind::EnumMember) {
		if (e.ty && underlying_type(e.ty) && underlying_type(e.ty)->kind == TypeKind::Enum) {
			return e.ty;
		}
		uint64_t enum_value = 0;
		if (enum_member_value(expected, e.member, enum_value)) {
			if (et && et->is_tagged_enum) {
				for (size_t vi = 0; vi < et->field_names.size() && vi < et->enum_variants.size();
					 ++vi) {
					if (et->field_names[vi] == e.member &&
						et->enum_variants[vi].payload_kind != EnumPayloadKind::Unit) {
						log::report(log::Level::Error, e.loc,
									"tagged enum variant '." + e.member +
										"' requires payload arguments");
						e.sema_invalid = true;
						break;
					}
				}
			}
			e.ty	  = expected;
			e.int_val = enum_value;
			return e.ty;
		}
		if (!et || et->kind != TypeKind::Enum) {
			log::report(log::Level::Error, e.loc,
						"enum member shorthand '." + e.member +
							"' requires an expected enum type");
		} else {
			log::report(log::Level::Error, e.loc,
						"enum '" + type_name_for_diag(expected) + "' has no member '" +
							e.member + "'");
		}
		e.ty = types_.void_ty();
		e.sema_invalid = true;
		return e.ty;
	}
	if (e.kind == ExprKind::Call && e.lhs && e.lhs->kind == ExprKind::EnumMember) {
		if (!et || et->kind != TypeKind::Enum) {
			log::report(log::Level::Error, e.loc,
						"enum variant shorthand '." + e.lhs->member +
							"(...)' requires an expected enum type");
			e.ty		   = types_.void_ty();
			e.sema_invalid = true;
			return e.ty;
		}
		size_t vidx = SIZE_MAX;
		for (size_t vi = 0; vi < et->field_names.size(); ++vi) {
			if (et->field_names[vi] == e.lhs->member) {
				vidx = vi;
				break;
			}
		}
		if (vidx == SIZE_MAX) {
			log::report(log::Level::Error, e.loc,
						"enum '" + type_name_for_diag(expected) + "' has no variant '" +
							e.lhs->member + "'");
			e.ty		   = types_.void_ty();
			e.sema_invalid = true;
			return e.ty;
		}
		const auto &vi = et->enum_variants[vidx];
		if (vi.payload_kind != EnumPayloadKind::Tuple) {
			log::report(log::Level::Error, e.loc,
						"variant '" + e.lhs->member + "' of enum '" +
							type_name_for_diag(expected) +
							"' cannot be constructed with positional arguments");
			e.ty		   = types_.void_ty();
			e.sema_invalid = true;
			return e.ty;
		}
		if (e.args.size() != vi.field_types.size()) {
			log::report(log::Level::Error, e.loc,
						"variant '" + e.lhs->member + "' expects " +
							std::to_string(vi.field_types.size()) + " argument(s), got " +
							std::to_string(e.args.size()));
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
		e.ty	  = expected;
		e.int_val = et->enum_values[vidx];
		e.lhs->ty = expected;
		return e.ty;
	}
	if (e.kind == ExprKind::VariantInit && !e.lhs) {
		if (!et || et->kind != TypeKind::Enum) {
			log::report(log::Level::Error, e.loc,
						"enum variant shorthand '." + e.member +
							" { ... }' requires an expected enum type");
			e.ty		   = types_.void_ty();
			e.sema_invalid = true;
			return e.ty;
		}
		size_t vidx = SIZE_MAX;
		for (size_t vi = 0; vi < et->field_names.size(); ++vi) {
			if (et->field_names[vi] == e.member) {
				vidx = vi;
				break;
			}
		}
		if (vidx == SIZE_MAX) {
			log::report(log::Level::Error, e.loc,
						"enum '" + type_name_for_diag(expected) + "' has no variant '" +
							e.member + "'");
			e.ty		   = types_.void_ty();
			e.sema_invalid = true;
			return e.ty;
		}
		const auto &vi = et->enum_variants[vidx];
		if (vi.payload_kind != EnumPayloadKind::Struct) {
			log::report(log::Level::Error, e.loc,
						"variant '" + e.member + "' of enum '" +
							type_name_for_diag(expected) +
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
		e.ty	  = expected;
		e.int_val = et->enum_values[vidx];
		return e.ty;
	}
	if (expected && e.kind == ExprKind::NullLit && et && et->kind == TypeKind::Pointer) {
		e.ty = expected;
		return e.ty;
	}
	if (expected && is_contextual_integer_expr(e) && et &&
		(et->kind == TypeKind::Bool || et->kind == TypeKind::UInt || et->kind == TypeKind::SInt ||
		 et->kind == TypeKind::Enum)) {
		FitResult fit = value_fits(expected, e);
		if (fit != FitResult::Fit) {
			log::report(log::Level::Error, e.loc,
						"integer expression value is not representable in '" +
							type_name_for_diag(expected) + "'");
			e.ty		   = types_.void_ty();
			e.sema_invalid = true;
			return e.ty;
		}
		apply_contextual_integer_type(e, expected);
		return check_expr(e, ctx, m, scope);
	}
	if (expected && generic_context_ && is_contextual_integer_expr(e) && et &&
		et->kind == TypeKind::Float && generic_integer_literal_fits_float(expected, e)) {
		apply_contextual_integer_type(e, expected);
		return check_expr(e, ctx, m, scope);
	}
	if (e.kind == ExprKind::StructLit) {
		if (!et || (et->kind != TypeKind::Struct && et->kind != TypeKind::Union)) {
			if (!defer_contextual_aggregate_) {
				log::report(log::Level::Error, e.loc,
							"aggregate literal requires an expected struct or union type");
			}
			for (auto &arg : e.args) {
				check_expr(*arg, ctx, m, scope);
			}
			e.ty = types_.void_ty();
			return e.ty;
		}
		if (et->is_opaque) {
			log::report(log::Level::Error, e.loc,
						"cannot construct opaque struct '" + et->name + "'");
			for (auto &arg : e.args) {
				check_expr(*arg, ctx, m, scope);
			}
			e.sema_invalid = true;
			e.ty		   = types_.poison_ty();
			return e.ty;
		}

		const Module &lit_module = e.binding_module ? *e.binding_module : m;

		if (et->kind == TypeKind::Struct) {
			for (size_t i = 0; i < et->field_names.size(); ++i) {
				if (!field_accessible(*et, i, lit_module)) {
					std::string f_name = et->field_names[i];
					std::string t_name = type_name_for_diag(const_cast<StType *>(et));
					log::error(e.loc, "cannot construct `" + t_name +
										  "` with a literal outside its declaring module: field `" +
										  f_name + "` is private")
						.code(log::DiagCode::PrivateMember)
						.phase("semantic")
						.help("mark the field `pub` in its declaration, or access it from the declaring module")
						.emit();
					for (auto &arg : e.args) {
						if (arg) {
							check_expr(*arg, ctx, m, scope);
						}
					}
					e.sema_invalid = true;
					e.ty		   = types_.poison_ty();
					return e.ty;
				}
			}
		}

		std::vector<bool> initialized(et->fields.size(), false);
		for (size_t i = 0; i < e.field_names.size(); ++i) {
			const std::string &name = e.field_names[i];
			if (i >= e.args.size()) {
				log::report(log::Level::Error, e.loc, "missing value for field '" + name + "'");
				continue;
			}
			size_t field = field_index(et, name);
			if (field == SIZE_MAX || field >= et->fields.size()) {
				log::report(log::Level::Error, e.args[i] ? e.args[i]->loc : e.loc,
							"aggregate '" + et->name + "' has no field named '" + name + "'");
				continue;
			}
			if (et->kind == TypeKind::Union && !field_accessible(*et, field, lit_module)) {
				std::string t_name = type_name_for_diag(const_cast<StType *>(et));
				log::error(e.args[i] ? e.args[i]->loc : e.loc,
						   "union variant `" + name + "` of `" + t_name + "` is private")
					.code(log::DiagCode::PrivateMember)
					.phase("semantic")
					.help("mark the field `pub` in its declaration, or access it from the declaring module")
					.emit();
				e.sema_invalid = true;
			}
			if (initialized[field]) {
				log::report(log::Level::Error, e.args[i] ? e.args[i]->loc : e.loc,
							"duplicate initializer for field '" + name + "'");
				continue;
			}
			initialized[field] = true;
			if (!e.args[i]) {
				log::report(log::Level::Error, e.loc, "missing value for field '" + name + "'");
				continue;
			}
			StType *field_ty = et->fields[field];
			StType *actual	 = check_expr_with_expected(*e.args[i], ctx, m, scope, field_ty);
			check_type_compat(field_ty, actual, *e.args[i], e.args[i]->loc,
							  "field '" + name + "' initializer value not representable in '" +
								  type_name_for_diag(field_ty) + "'",
							  "type mismatch for field '" + name + "': expected '" +
								  type_name_for_diag(field_ty) + "', got '" +
								  type_name_for_diag(e.args[i]->ty) + "'");
		}
		if (e.sema_invalid) {
			e.ty = types_.poison_ty();
			return e.ty;
		}
		if (et->kind == TypeKind::Union) {
			if (e.field_names.size() != 1) {
				log::report(log::Level::Error, e.loc,
							"union literal must initialize exactly one field");
			}
		} else {
			for (size_t i = 0; i < initialized.size(); ++i) {
				if (!initialized[i]) {
					log::report(log::Level::Error, e.loc,
								"missing initializer for field '" + et->field_names[i] + "'");
				}
			}
		}
		e.ty = expected;
		return e.ty;
	}

	if (e.kind == ExprKind::Conditional) {
		StType *expected_u = et;
		StType *ct		   = e.condition ? check_expr(*e.condition, ctx, m, scope) : nullptr;
		if (!e.condition || !e.condition->sema_invalid) {
			expect_bool_cond(e.condition ? e.condition->loc : e.loc, ct);
		}
		StType *tt = e.then_expr ? check_expr_with_expected(*e.then_expr, ctx, m, scope, expected_u)
								 : types_.void_ty();
		StType *et = e.else_expr ? check_expr_with_expected(*e.else_expr, ctx, m, scope, expected_u)
								 : types_.void_ty();
		if (!expected_u) {
			expected_u = tt;
		}
		if (!expected_u) {
			expected_u = et;
		}
		if (expected_u && underlying_type(expected_u)->kind != TypeKind::Void) {
			// The expected type is authoritative for literals, but a non-literal
			// arm still has to match it exactly.
			if (tt && !eq_type(tt, expected_u) && e.then_expr &&
				is_contextual_literal(*e.then_expr)) {
				tt = check_expr_with_expected(*e.then_expr, ctx, m, scope, expected_u);
			}
			if (et && !eq_type(et, expected_u) && e.else_expr &&
				is_contextual_literal(*e.else_expr)) {
				et = check_expr_with_expected(*e.else_expr, ctx, m, scope, expected_u);
			}
		}
		if (tt && et && !eq_type(tt, et)) {
			if (e.then_expr && is_contextual_literal(*e.then_expr) &&
				!is_contextual_literal(*e.else_expr)) {
				tt = check_expr_with_expected(*e.then_expr, ctx, m, scope, et);
			} else if (e.else_expr && is_contextual_literal(*e.else_expr) &&
					   !is_contextual_literal(*e.then_expr)) {
				et = check_expr_with_expected(*e.else_expr, ctx, m, scope, tt);
			}
		}
		if (tt && et && !eq_type(tt, et)) {
			log::report(log::Level::Error, e.loc,
						"conditional expression arms must have identical types ('" +
							type_name_for_diag(tt) + "' and '" + type_name_for_diag(et) + "')");
		}
		e.ty = tt ? tt : et;
		if (tt && et &&
			((underlying_type(tt)->kind == TypeKind::Void) != (underlying_type(et)->kind == TypeKind::Void))) {
			log::report(log::Level::Error, e.loc,
						"conditional expression cannot mix void and non-void arms");
		}
		fold_known_expr(e, types_.pointer_width());
		return e.ty;
	}
	if (et && et->kind == TypeKind::Array && e.kind == ExprKind::StringLit) {
		StType *elem = underlying_type(et->base);
		const size_t expected_len = e.str_val.size() + (e.is_cstring ? 1 : 0);
		if (elem && elem->kind == TypeKind::UInt && elem->width == 8 &&
			et->array_len == expected_len) {
			e.ty = expected;
			return e.ty;
		}
	}
	if (et && et->kind == TypeKind::Slice && e.kind == ExprKind::ArrayLit && e.args.empty()) {
		e.ty = expected;
		return e.ty;
	}
	if (et && et->kind == TypeKind::Array && e.kind == ExprKind::ArrayLit) {
		StType *elem_ty = nullptr;
		for (auto &arg : e.args) {
			StType *at = check_expr_with_expected(*arg, ctx, m, scope, et->base);
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

	StType *saved_block_expected = expected_block_expr_;
	expected_block_expr_ = (e.kind == ExprKind::Block) ? expected : nullptr;
	StType *saved_expected = expected_generic_result_;
	if (e.kind == ExprKind::Call) {
		expected_generic_result_ = expected;
	}
	StType *actual			 = check_expr(e, ctx, m, scope);
	expected_generic_result_ = saved_expected;
	expected_block_expr_ = saved_block_expected;
	if (e.kind == ExprKind::IntLit || e.kind == ExprKind::CharLit || e.kind == ExprKind::FloatLit ||
		e.kind == ExprKind::NullLit) {
		if (value_fits(expected, e) == FitResult::Fit) {
			e.ty = expected;
		}
	} else if (e.kind == ExprKind::Unary && e.lhs && (e.str_val == "-" || e.str_val == "~") &&
			   is_contextual_literal(*e.lhs) && value_fits(expected, e) == FitResult::Fit) {
		// `check_expr` has already validated the unary operator, but it checks
		// the operand without context. Revisit the literal operand and retag the
		// complete expression so `-44` can initialize an `i8` binding.
		check_expr_with_expected(*e.lhs, ctx, m, scope, expected);
		e.ty = expected;
	}
	return actual == e.ty ? actual : e.ty;
}

} // namespace stellac
