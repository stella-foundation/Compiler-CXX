#include "internal.hpp"

#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace stellac {

namespace {

using ExprMap = std::unordered_map<std::string, const Expr *>;
using ShadowedNames = std::unordered_set<std::string>;
using NameScopes = std::vector<std::unordered_map<std::string, std::string>>;

static void rename_stmt(Stmt &s, NameScopes &names, uint64_t id, uint64_t &local_id);
static uint64_t stmt_nodes(const Stmt &s);
static void mark_macro_stmt(Stmt &s);
static void mark_macro_argument(Expr &e);
static StmtPtr expand_stmt(const Stmt &source, const ExprMap &fixed,
							  const std::vector<std::unique_ptr<Expr>> &variadic,
							  FnDecl &macro, const SourceLoc &invocation,
							  const std::vector<SourceLoc> &expansion_chain,
							  ShadowedNames shadowed = {});

static bool contains_variadic_splice(const Expr &e) {
	if (e.kind == ExprKind::VariadicSplice) {
		return true;
	}
	if ((e.lhs && contains_variadic_splice(*e.lhs)) ||
		(e.rhs && contains_variadic_splice(*e.rhs)) ||
		(e.condition && contains_variadic_splice(*e.condition)) ||
		(e.then_expr && contains_variadic_splice(*e.then_expr)) ||
		(e.else_expr && contains_variadic_splice(*e.else_expr))) {
		return true;
	}
	for (const auto &arg : e.args) {
		if (arg && contains_variadic_splice(*arg)) {
			return true;
		}
	}
	return false;
}

static std::unique_ptr<Expr> substitute_variadic_splice(const Expr &e, const Expr &replacement) {
	if (e.kind == ExprKind::VariadicSplice) {
		auto copy = clone_expr(replacement);
		mark_macro_argument(*copy);
		return copy;
	}
	auto out = clone_expr(e);
	if (out->lhs) out->lhs = substitute_variadic_splice(*out->lhs, replacement);
	if (out->rhs) out->rhs = substitute_variadic_splice(*out->rhs, replacement);
	if (out->condition) out->condition = substitute_variadic_splice(*out->condition, replacement);
	if (out->then_expr) out->then_expr = substitute_variadic_splice(*out->then_expr, replacement);
	if (out->else_expr) out->else_expr = substitute_variadic_splice(*out->else_expr, replacement);
	for (auto &arg : out->args) {
		if (arg) {
			arg = substitute_variadic_splice(*arg, replacement);
		}
	}
	return out;
}

static void mark_macro_argument(Expr &e) {
	e.macro_argument = true;
	if (e.lhs) mark_macro_argument(*e.lhs);
	if (e.rhs) mark_macro_argument(*e.rhs);
	if (e.condition) mark_macro_argument(*e.condition);
	if (e.then_expr) mark_macro_argument(*e.then_expr);
	if (e.else_expr) mark_macro_argument(*e.else_expr);
	for (auto &arg : e.args) if (arg) mark_macro_argument(*arg);
	for (auto &arg : e.generic_args) if (arg.value) mark_macro_argument(*arg.value);
	if (e.block) mark_macro_stmt(*e.block);
}

static void mark_macro_stmt(Stmt &s) {
	s.macro_argument = true;
	for (auto &child : s.body) if (child) mark_macro_stmt(*child);
	if (s.expr) mark_macro_argument(*s.expr);
	if (s.cond) mark_macro_argument(*s.cond);
	if (s.then_block) mark_macro_stmt(*s.then_block);
	if (s.else_block) mark_macro_stmt(*s.else_block);
	if (s.for_init) mark_macro_stmt(*s.for_init);
	if (s.for_cond) mark_macro_argument(*s.for_cond);
	if (s.for_step) mark_macro_argument(*s.for_step);
	if (s.loop_body) mark_macro_stmt(*s.loop_body);
	if (s.defer_body) mark_macro_stmt(*s.defer_body);
	if (s.scrutinee) mark_macro_argument(*s.scrutinee);
	if (s.ret_val) mark_macro_argument(*s.ret_val);
	for (auto &var : s.vars) {
		var.macro_argument = true;
		if (var.init) mark_macro_argument(*var.init);
	}
	for (auto &arm : s.arms) {
		for (auto &pat : arm.patterns) {
			if (pat.expr) mark_macro_argument(*pat.expr);
		}
		if (arm.body) mark_macro_stmt(*arm.body);
	}
	for (auto &op : s.asm_outputs) if (op.operand) mark_macro_argument(*op.operand);
	for (auto &op : s.asm_inputs) if (op.operand) mark_macro_argument(*op.operand);
}

static void rename_expr(Expr &e, NameScopes &names, uint64_t id, uint64_t &local_id) {
	if (e.macro_argument) return;
	if (e.kind == ExprKind::Ident && e.path.size() == 1) {
		for (auto it = names.rbegin(); it != names.rend(); ++it) {
			auto found = it->find(e.path[0]);
			if (found != it->end()) {
				e.path[0] = found->second;
				e.str_val = found->second;
				break;
			}
		}
	}
	if (e.lhs) rename_expr(*e.lhs, names, id, local_id);
	if (e.rhs) rename_expr(*e.rhs, names, id, local_id);
	if (e.condition) rename_expr(*e.condition, names, id, local_id);
	if (e.then_expr) rename_expr(*e.then_expr, names, id, local_id);
	if (e.else_expr) rename_expr(*e.else_expr, names, id, local_id);
	for (auto &arg : e.args) if (arg) rename_expr(*arg, names, id, local_id);
	for (auto &arg : e.generic_args) if (arg.value) rename_expr(*arg.value, names, id, local_id);
	if (e.block) rename_stmt(*e.block, names, id, local_id);
}

static void rename_stmt(Stmt &s, NameScopes &names, uint64_t id, uint64_t &local_id) {
	if (s.macro_argument) return;
	if (s.kind == StmtKind::Block) {
		names.emplace_back();
		for (auto &child : s.body) {
			if (child) rename_stmt(*child, names, id, local_id);
		}
		names.pop_back();
		return;
	}
	if (s.kind == StmtKind::For) {
		names.emplace_back();
		if (s.for_init) rename_stmt(*s.for_init, names, id, local_id);
		if (s.for_cond) rename_expr(*s.for_cond, names, id, local_id);
		if (s.for_step) rename_expr(*s.for_step, names, id, local_id);
		if (s.loop_body) rename_stmt(*s.loop_body, names, id, local_id);
		names.pop_back();
		return;
	}
	if (s.expr) rename_expr(*s.expr, names, id, local_id);
	if (s.cond) rename_expr(*s.cond, names, id, local_id);
	if (s.scrutinee) rename_expr(*s.scrutinee, names, id, local_id);
	if (s.ret_val) rename_expr(*s.ret_val, names, id, local_id);
	if (s.kind == StmtKind::VarDecl || s.kind == StmtKind::ConstDecl) {
		for (auto &var : s.vars) {
			if (var.init) rename_expr(*var.init, names, id, local_id);
			const std::string original_name = var.name;
			var.name = mangle({"macro", std::to_string(id), std::to_string(local_id++), original_name});
			if (names.empty()) names.emplace_back();
			names.back()[original_name] = var.name;
		}
		return;
	}
	for (auto &arm : s.arms) {
		for (auto &pat : arm.patterns) {
			if (pat.expr) rename_expr(*pat.expr, names, id, local_id);
		}
		if (arm.body) {
			auto arm_names = names;
			rename_stmt(*arm.body, arm_names, id, local_id);
		}
	}
	if (s.then_block) {
		auto branch_names = names;
		rename_stmt(*s.then_block, branch_names, id, local_id);
	}
	if (s.else_block) {
		auto branch_names = names;
		rename_stmt(*s.else_block, branch_names, id, local_id);
	}
	if (s.for_init) rename_stmt(*s.for_init, names, id, local_id);
	if (s.loop_body) {
		auto loop_names = names;
		rename_stmt(*s.loop_body, loop_names, id, local_id);
	}
	if (s.defer_body) {
		auto defer_names = names;
		rename_stmt(*s.defer_body, defer_names, id, local_id);
	}
}

static uint64_t expr_nodes(const Expr &e) {
	uint64_t n = 1;
	if (e.lhs) n += expr_nodes(*e.lhs);
	if (e.rhs) n += expr_nodes(*e.rhs);
	if (e.condition) n += expr_nodes(*e.condition);
	if (e.then_expr) n += expr_nodes(*e.then_expr);
	if (e.else_expr) n += expr_nodes(*e.else_expr);
	for (const auto &arg : e.args) if (arg) n += expr_nodes(*arg);
	for (const auto &arg : e.generic_args) if (arg.value) n += expr_nodes(*arg.value);
	if (e.block) n += stmt_nodes(*e.block);
	return n;
}

static uint64_t stmt_nodes(const Stmt &s) {
	uint64_t n = 1;
	for (const auto &child : s.body) if (child) n += stmt_nodes(*child);
	if (s.expr) n += expr_nodes(*s.expr);
	if (s.cond) n += expr_nodes(*s.cond);
	if (s.for_cond) n += expr_nodes(*s.for_cond);
	if (s.for_step) n += expr_nodes(*s.for_step);
	if (s.scrutinee) n += expr_nodes(*s.scrutinee);
	if (s.ret_val) n += expr_nodes(*s.ret_val);
	for (const auto &var : s.vars) if (var.init) n += expr_nodes(*var.init);
	for (const auto &arm : s.arms) {
		for (const auto &pat : arm.patterns) {
			if (pat.expr) n += expr_nodes(*pat.expr);
		}
		if (arm.body) n += stmt_nodes(*arm.body);
	}
	if (s.then_block) n += stmt_nodes(*s.then_block);
	if (s.else_block) n += stmt_nodes(*s.else_block);
	if (s.for_init) n += stmt_nodes(*s.for_init);
	if (s.loop_body) n += stmt_nodes(*s.loop_body);
	if (s.defer_body) n += stmt_nodes(*s.defer_body);
	return n;
}

static std::unique_ptr<Expr> expand_expr(const Expr &source, const ExprMap &fixed,
										 const std::vector<std::unique_ptr<Expr>> &variadic,
										 FnDecl &macro, const SourceLoc &invocation,
										 const std::vector<SourceLoc> &expansion_chain,
										 const ShadowedNames &shadowed,
										 bool preserve_splice = false) {
	if (!source.macro_argument && source.kind == ExprKind::Ident && source.path.size() == 1) {
		auto it = fixed.find(source.path[0]);
		if (!shadowed.count(source.path[0]) && it != fixed.end() && it->second) {
			auto result = clone_expr(*it->second);
			mark_macro_argument(*result);
			return result;
		}
		if (!shadowed.count(source.path[0])) {
			auto generic_int = macro.generic_const_subst.find(source.path[0]);
			if (generic_int != macro.generic_const_subst.end()) {
				const int64_t value = generic_int->second;
				if (value < 0) {
					// Keep a negative generic value as an expression so contextual
					// signed typing can distinguish it from a large u64 literal.
					auto operand = std::make_unique<Expr>();
					operand->kind = ExprKind::IntLit;
					operand->loc = source.loc;
					operand->int_val = value == INT64_MIN
						? static_cast<uint64_t>(INT64_MAX)
						: static_cast<uint64_t>(-value);
					operand->binding_module = macro.decl_module;
					operand->invocation_loc = invocation;
					operand->expansion_chain = expansion_chain;
					auto negative = std::make_unique<Expr>();
					negative->kind = ExprKind::Unary;
					negative->str_val = value == INT64_MIN ? "~" : "-";
					negative->lhs = std::move(operand);
					negative->loc = source.loc;
					negative->binding_module = macro.decl_module;
					negative->invocation_loc = invocation;
					negative->expansion_chain = expansion_chain;
					return negative;
				}
				auto result = clone_expr(source);
				result->kind = ExprKind::IntLit;
				result->int_val = static_cast<uint64_t>(value);
				result->str_val.clear();
				result->has_const_value = true;
				result->binding_module = macro.decl_module;
				result->macro_argument = false;
				result->invocation_loc = invocation;
				result->expansion_chain = expansion_chain;
				return result;
			}
		}
		if (!shadowed.count(source.path[0])) {
			auto generic_string = macro.generic_const_string_subst.find(source.path[0]);
			if (generic_string != macro.generic_const_string_subst.end()) {
				auto result = clone_expr(source);
				result->kind = ExprKind::StringLit;
				result->str_val = generic_string->second;
				result->binding_module = macro.decl_module;
				result->macro_argument = false;
				result->invocation_loc = invocation;
				result->expansion_chain = expansion_chain;
				return result;
			}
		}
	}
	auto result = clone_expr(source);
	result->binding_module = macro.decl_module;
	result->macro_argument = false;
	result->invocation_loc = invocation;
	result->expansion_chain = expansion_chain;
	if (result->kind == ExprKind::Stringify) {
		// Expand the operand first so a macro parameter is rendered as the
		// syntax supplied by the caller, rather than as the parameter name.
		if (result->args.size() == 1 && result->args.front()) {
			auto operand = expand_expr(*result->args.front(), fixed, variadic, macro,
												 invocation, expansion_chain, shadowed);
			result->kind = ExprKind::StringLit;
			result->str_val = stringify_expr(*operand);
			result->args.clear();
		}
		return result;
	}
	if (result->kind == ExprKind::Slice && result->lhs &&
		result->lhs->kind == ExprKind::ArrayLit && variadic.empty()) {
		bool had_splice = false;
		for (const auto &elem : result->lhs->args) {
			if (elem && contains_variadic_splice(*elem)) {
				had_splice = true;
				break;
			}
		}
		if (had_splice) {
			auto empty_arr = std::make_unique<Expr>();
			empty_arr->kind = ExprKind::ArrayLit;
			empty_arr->loc = invocation;
			empty_arr->invocation_loc = invocation;
			empty_arr->expansion_chain = expansion_chain;
			return empty_arr;
		}
	}
	if (result->lhs) result->lhs = expand_expr(*result->lhs, fixed, variadic, macro, invocation, expansion_chain, shadowed, preserve_splice);
	if (result->rhs) result->rhs = expand_expr(*result->rhs, fixed, variadic, macro, invocation, expansion_chain, shadowed);
	if (result->condition) result->condition = expand_expr(*result->condition, fixed, variadic, macro, invocation, expansion_chain, shadowed);
	if (result->then_expr) result->then_expr = expand_expr(*result->then_expr, fixed, variadic, macro, invocation, expansion_chain, shadowed);
	if (result->else_expr) result->else_expr = expand_expr(*result->else_expr, fixed, variadic, macro, invocation, expansion_chain, shadowed);
	if (result->kind == ExprKind::Call && !preserve_splice) {
		std::vector<std::unique_ptr<Expr>> args;
		for (const auto &arg : result->args) {
			if (arg && arg->kind == ExprKind::VariadicSplice) {
				for (const auto &value : variadic) {
					auto copy = clone_expr(*value);
					mark_macro_argument(*copy);
					args.push_back(std::move(copy));
				}
			} else if (arg) {
				args.push_back(expand_expr(*arg, fixed, variadic, macro, invocation, expansion_chain, shadowed));
			}
		}
		result->args = std::move(args);
	} else if (result->kind == ExprKind::ArrayLit) {
		std::vector<std::unique_ptr<Expr>> elems;
		for (const auto &elem : result->args) {
			if (elem && contains_variadic_splice(*elem)) {
				auto expanded_elem = expand_expr(*elem, fixed, variadic, macro, invocation,
												 expansion_chain, shadowed, true);
				for (const auto &value : variadic) {
					elems.push_back(substitute_variadic_splice(*expanded_elem, *value));
				}
			} else if (elem) {
				elems.push_back(expand_expr(*elem, fixed, variadic, macro, invocation,
											expansion_chain, shadowed));
			}
		}
		result->args = std::move(elems);
	} else {
		for (auto &arg : result->args) if (arg) arg = expand_expr(*arg, fixed, variadic, macro, invocation, expansion_chain, shadowed, preserve_splice);
	}
	for (auto &arg : result->generic_args) if (arg.value) arg.value = expand_expr(*arg.value, fixed, variadic, macro, invocation, expansion_chain, shadowed);
	if (result->block) {
		result->block = expand_stmt(*result->block, fixed, variadic, macro, invocation,
										expansion_chain, shadowed);
	}
	return result;
}

static StmtPtr expand_stmt(const Stmt &source, const ExprMap &fixed,
							   const std::vector<std::unique_ptr<Expr>> &variadic,
							   FnDecl &macro, const SourceLoc &invocation,
							   const std::vector<SourceLoc> &expansion_chain,
							   ShadowedNames shadowed) {
	auto result = clone_stmt(source);
	result->binding_module = macro.decl_module;
	result->macro_argument = false;
	result->invocation_loc = invocation;
	result->expansion_chain = expansion_chain;
	if (source.kind == StmtKind::Block) {
		for (size_t i = 0; i < result->body.size(); ++i) {
			if (result->body[i] && source.body[i]) {
				result->body[i] = expand_stmt(*source.body[i], fixed, variadic, macro, invocation,
											 expansion_chain, shadowed);
				if (source.body[i]->kind == StmtKind::VarDecl ||
					source.body[i]->kind == StmtKind::ConstDecl) {
					for (const auto &var : source.body[i]->vars) {
						shadowed.insert(var.name);
					}
				}
			}
		}
	} else if (result->kind == StmtKind::For) {
		if (source.for_init) {
			result->for_init = expand_stmt(*source.for_init, fixed, variadic, macro, invocation,
										   expansion_chain, shadowed);
			if (source.for_init->kind == StmtKind::VarDecl ||
				source.for_init->kind == StmtKind::ConstDecl) {
				for (const auto &var : source.for_init->vars) {
					shadowed.insert(var.name);
				}
			}
		}
		if (result->for_cond) result->for_cond = expand_expr(*result->for_cond, fixed, variadic, macro, invocation, expansion_chain, shadowed);
		if (result->for_step) result->for_step = expand_expr(*result->for_step, fixed, variadic, macro, invocation, expansion_chain, shadowed);
		if (source.loop_body) result->loop_body = expand_stmt(*source.loop_body, fixed, variadic, macro, invocation, expansion_chain, shadowed);
	}
	if (result->expr) result->expr = expand_expr(*result->expr, fixed, variadic, macro, invocation, expansion_chain, shadowed);
	if (result->cond) result->cond = expand_expr(*result->cond, fixed, variadic, macro, invocation, expansion_chain, shadowed);
	if (result->scrutinee) result->scrutinee = expand_expr(*result->scrutinee, fixed, variadic, macro, invocation, expansion_chain, shadowed);
	if (result->ret_val) result->ret_val = expand_expr(*result->ret_val, fixed, variadic, macro, invocation, expansion_chain, shadowed);
	for (auto &var : result->vars) {
		var.binding_module = macro.decl_module;
		if (var.init) var.init = expand_expr(*var.init, fixed, variadic, macro, invocation, expansion_chain, shadowed);
		shadowed.insert(var.name);
	}
	for (auto &arm : result->arms) {
		for (auto &pat : arm.patterns) {
			if (pat.expr) pat.expr = expand_expr(*pat.expr, fixed, variadic, macro, invocation, expansion_chain, shadowed);
		}
		if (arm.body) arm.body = expand_stmt(*arm.body, fixed, variadic, macro, invocation, expansion_chain, shadowed);
	}
	if (result->then_block) result->then_block = expand_stmt(*source.then_block, fixed, variadic, macro, invocation, expansion_chain, shadowed);
	if (result->else_block) result->else_block = expand_stmt(*source.else_block, fixed, variadic, macro, invocation, expansion_chain, shadowed);
	if (source.kind != StmtKind::For && result->for_init) result->for_init = expand_stmt(*source.for_init, fixed, variadic, macro, invocation, expansion_chain, shadowed);
	if (source.kind != StmtKind::For && result->loop_body) result->loop_body = expand_stmt(*source.loop_body, fixed, variadic, macro, invocation, expansion_chain, shadowed);
	if (result->defer_body) result->defer_body = expand_stmt(*source.defer_body, fixed, variadic, macro, invocation, expansion_chain, shadowed);
	for (auto &op : result->asm_outputs) if (op.operand) op.operand = expand_expr(*op.operand, fixed, variadic, macro, invocation, expansion_chain, shadowed);
	for (auto &op : result->asm_inputs) if (op.operand) op.operand = expand_expr(*op.operand, fixed, variadic, macro, invocation, expansion_chain, shadowed);
	return result;
}

} // namespace

void Sema::validate_macro_body(FnDecl &macro) {
	if (!macro.body) return;
	bool has_yield = false;
	std::function<void(const Stmt &, unsigned, unsigned)> visit =
		[&](const Stmt &stmt, unsigned loop_depth, unsigned breakable_depth) {
			if (stmt.kind == StmtKind::Yield) {
				has_yield = true;
			}
			if (stmt.kind == StmtKind::Return) {
				log::report(log::Level::Error, stmt.loc,
							"return is not allowed in a macro body");
			} else if (stmt.kind == StmtKind::Break && breakable_depth == 0) {
				log::report(log::Level::Error, stmt.loc,
							"break in a macro must target a loop or switch in the same macro expansion");
			} else if (stmt.kind == StmtKind::Continue && loop_depth == 0) {
				log::report(log::Level::Error, stmt.loc,
							"continue in a macro must target a loop in the same macro expansion");
			}
			for (const auto &child : stmt.body) {
				if (child) visit(*child, loop_depth, breakable_depth);
			}
			if (stmt.then_block) visit(*stmt.then_block, loop_depth, breakable_depth);
			if (stmt.else_block) visit(*stmt.else_block, loop_depth, breakable_depth);
			if (stmt.for_init) visit(*stmt.for_init, loop_depth, breakable_depth);
			if (stmt.loop_body) visit(*stmt.loop_body, loop_depth + 1, breakable_depth + 1);
			if (stmt.defer_body) visit(*stmt.defer_body, loop_depth, breakable_depth);
			for (const auto &arm : stmt.arms) {
				if (arm.body) visit(*arm.body, loop_depth, breakable_depth + 1);
			}
		};
	visit(*macro.body, 0, 0);
	macro.macro_returns_value = macro.macro_returns_value || has_yield;
}

StmtPtr Sema::expand_macro_call(Expr &call, FnDecl &caller, Module &m, ScopeStack &) {
	FnDecl *macro = call.resolved_fn;
	if (!macro || !macro->is_macro || !macro->body) return nullptr;
	const SourceLoc diagnostic_loc = call.invocation_loc.file.empty()
		? (call.lhs ? call.lhs->loc : call.loc)
		: call.invocation_loc;
	if (macro_expansion_depth_ >= 128) {
		log::report(log::Level::Error, diagnostic_loc,
					"macro expansion exceeds the maximum depth of 128");
		return nullptr;
	}
	ExprMap fixed;
	for (size_t i = 0; i < macro->params.size() && i < call.args.size(); ++i) {
		fixed.emplace(macro->params[i].name, call.args[i].get());
	}
	std::vector<std::unique_ptr<Expr>> variadic;
	for (size_t i = macro->params.size(); i < call.args.size(); ++i) {
		if (call.args[i]->kind == ExprKind::VariadicSplice) {
			log::report(log::Level::Error, call.args[i]->loc,
						"'...' cannot be used as a macro invocation argument");
			continue;
		}
		variadic.push_back(clone_expr(*call.args[i]));
	}
	SourceLoc invocation = diagnostic_loc;
	auto expansion_chain = call.expansion_chain;
	expansion_chain.push_back(invocation);
	auto result = expand_stmt(*macro->body, fixed, variadic, *macro, invocation,
								  expansion_chain);
	uint64_t id = ++macro_unique_id_;
	NameScopes local_names;
	uint64_t local_id = 0;
	rename_stmt(*result, local_names, id, local_id);
	uint64_t nodes = stmt_nodes(*result);
	if (nodes > 1000000 - generated_macro_nodes_) {
		log::report(log::Level::Error, call.loc,
					"macro expansion exceeds the generated-node budget of 1,000,000");
		return nullptr;
	}
	generated_macro_nodes_ += nodes;
	(void)caller;
	(void)m;
	return result;
}

} // namespace stellac
