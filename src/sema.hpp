#pragma once

#include "ast.hpp"
#include "module_resolver.hpp"
#include "symbol.hpp"
#include "types.hpp"
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace stellac {

// Result of compile-time value-fit checking: does a known-at-compile-time
// source literal fit the target slot's representable range exactly?
//   - Fit:      value is exactly representable; accept (codegen may coerce).
//   - NoFit:    value is known and provably out of range; emit an error and
//               do NOT let codegen silently truncate.
//   - Unknown:  the expression is not a contextual literal or the slot is
//               not provably compatible; typed values are never converted.
enum class FitResult { Fit, NoFit, Unknown };

struct SemaOptions {
	// Macro flags defined via -D or derived from --target.
	std::unordered_map<std::string, bool> flags;
	// Target-derived flag enforcement
	std::string							  arch_flag; // e.g. arch_x86_64
	std::string							  os_flag;
	std::string							  bits_flag;
};

class Sema {
  public:
	Sema(ModuleResolver &resolver, TypeContext &types, SemaOptions opts)
		: resolver_(resolver), types_(types), opts_(std::move(opts)) {}

	// Run full semantic analysis across all modules reachable from root.
	// Returns false if any error occurred.
	bool		 run();

	TypeContext &types() {
		return types_;
	}
	const SemaOptions &opts() const {
		return opts_;
	}

	StType *resolve_type(TypeRef &tr, Module &m);
	// Resolve a type while retaining the substitutions of a specialized
	// generic function during code generation.
	StType *resolve_type_in_context(TypeRef &tr, Module &m, FnDecl *context);

	bool	ensure_const_value(Symbol &s);
	bool	eval_array_len_expr(const Expr &e, Module &m, uint64_t &out_value);
	bool	eval_comptime_int_expr(const Expr &e, Module &m, int64_t &out_signed,
								   uint64_t &out_unsigned, bool &is_unsigned);

	// Validate attribute list against the whitelist + conflict rules for the
	// given declaration kind. Emits diagnostics on failure.
	void	validate_attributes(AttributeList &attrs, TopKind target, const SourceLoc &decl_loc);
	void	validate_attribute_list(AttributeList &attrs, AttributeTarget attr_target,
									std::string_view kind_label, bool is_const = false);

	// Resolve a qualified call path to a Symbol across modules.
	Symbol *resolve_path(const std::vector<std::string> &path, Module &from);
	Symbol *lookup_in_module(Module &m, const std::string &simple);
	Symbol *lookup_pub_in_module(Module &m, const std::string &simple);
	const std::vector<FnDecl *> &generic_instances() const {
		return generic_instances_view_;
	}
	const std::vector<std::string> &embedded_files() const {
		return embedded_files_;
	}

	bool type_implements_trait(StType *type, TraitDecl *trait) const;
	ImplDecl *find_impl(TraitDecl *trait, StType *type) const;
	std::vector<FnDecl *> find_trait_methods_for_type(StType *type, const std::string &method_name);
	StType *substitute_self(StType *t, StType *concrete);
	void validate_trait_conformance();

  private:
	ModuleResolver	 &resolver_;
	TypeContext		 &types_;
	SemaOptions		  opts_;

	// Loop / switch enclosing-context stacks (sema-time break/continue legality).
	std::vector<bool> loop_stack_;
	std::vector<bool> switch_stack_;

	enum class VaState : unsigned char { Uninitialized = 1, Initialized = 2, Ended = 4 };

	FnDecl									   *active_fn_ = nullptr;
	std::unordered_map<VarDeclInfo *, unsigned> va_states_;
	std::unordered_set<VarDeclInfo *>			va_list_locals_;
	std::vector<VarDeclInfo *>					active_locals_;
	bool										checking_assignment_lhs_	= false;
	bool										checking_va_list_operand_	= false;
	bool										defer_contextual_aggregate_ = false;
	unsigned									defer_depth_ = 0;
	std::vector<size_t>									defer_loop_bases_;
	std::vector<size_t>									defer_switch_bases_;
	StType									   *expected_generic_result_	= nullptr;

	// On-demand generic specialization state.  Instances are owned here so
	// pointers stored in AST expressions and LLVM caches remain stable.
	std::vector<std::unique_ptr<FnDecl>>		generic_instances_owned_;
	std::vector<FnDecl *>						generic_instances_view_;
	std::unordered_map<std::string, FnDecl *>	generic_instance_cache_;
	std::unordered_set<FnDecl *>				generic_checked_;
	std::unordered_set<std::string>				generic_in_progress_;
	FnDecl									   *generic_context_ = nullptr;
	Expr									   *allowed_macro_call_ = nullptr;
	StType									   *expected_block_expr_ = nullptr;
	std::vector<StType *>						block_expected_stack_;
	unsigned									block_expr_depth_ = 0;
	std::vector<StType *>							block_yield_types_;
	unsigned									 macro_expansion_depth_ = 0;
	uint64_t								 generated_macro_nodes_ = 0;
	uint64_t								 macro_unique_id_ = 0;
	struct MacroControlBase {
		size_t loops;
		size_t switches;
	};
	std::vector<MacroControlBase> macro_control_bases_;
	std::string									last_generic_constraint_error_;
	std::unordered_set<Symbol *>				evaluating_consts_;
	ScopeStack								   *current_scope_ = nullptr;
	std::vector<std::string>					embedded_files_;

	// Per-module symbol tables: simple-name -> Symbol, for pub items.
	std::unordered_map<std::string, // mangled module path string ("std::io")
					   std::unordered_map<std::string, Symbol>>
				module_syms_;
	std::unordered_map<std::string, Symbol *> visible_symbol_cache_;
	std::string								  resolve_cache_key_buf_;
	std::deque<Symbol>						  merged_symbols_;
	bool									  visible_symbol_cache_ready_ = false;
	std::unordered_map<Module *, std::vector<Module *>>	   exported_interface_cache_;
	std::unordered_map<Module *, std::vector<Module *>>	   visible_interface_cache_;
	std::unordered_map<Module *, std::unordered_set<Module *>>	  marked_import_chains_;
	std::unordered_map<Module *, std::unordered_set<std::string>> marked_import_uses_;

	struct TraitImplEntry {
		TraitDecl *trait		  = nullptr;
		StType	  *type			  = nullptr;
		Symbol	  *generic_origin = nullptr;
		ImplDecl  *impl_decl	  = nullptr;
		Module	  *module		  = nullptr;
		SourceLoc  loc;
	};
	std::vector<TraitImplEntry>						   trait_impls_;
	std::unordered_map<std::string, std::vector<size_t>> trait_methods_by_name_;
	size_t												 trait_methods_indexed_count_ = 0;
	StType					   *active_self_type_ = nullptr;

	bool register_impl(ImplDecl &impl, Module &m);
	void validate_impl_signature(ImplDecl &impl, Module &m);
	bool check_orphan_rule(ImplDecl &impl, Module &m);

	// phase 1: register all top-level decls in every module
	void		register_module_symbols(Module &m);
	// Resolve a path without applying cross-module visibility, so callers can
	// report an inaccessible symbol instead of producing a codegen error.
	Symbol	   *resolve_path_any(const std::vector<std::string> &path, Module &from);
	// Phase 1b: resolve fn signatures, type binding targets, and var/const types
	// across every queued module. Deferred from register_module_symbols so
	// cross-module type references (e.g. `fd std.FileDescriptor` in a
	// root module's fn signature) see imported modules' registered symbols
	// even though modules are queued roots-first.
	void		resolve_module_types(Module &m);

	// phase 2: typecheck bodies
	void		check_module_bodies(Module &m);
	// Generic and macro bodies are checked later or in an expansion context;
	// record their direct function dependencies before unused-name diagnostics.
	void		mark_deferred_body_uses(Module &m);
	// Mark imports referenced from every parsed branch for diagnostics. This
	// deliberately includes target-disabled function-body branches, which are
	// skipped by semantic checking but remain valid source for other targets.
	void		mark_import_uses_in_source(Module &m);
	void		emit_unused_warnings(Module &m);
	void		prune_module_conditionals(Module &m);
	void		prune_decls(std::vector<TopDeclPtr> &decls, Module &m);
	StType		*resolve_type_binding(Symbol &symbol, Module &m);
	void		resolve_aggregate_fields(Module &m);
	void		validate_aggregate_layouts();
	const std::vector<Module *> &exported_interface(Module &m);
	const std::vector<Module *> &visible_interface(Module &m);
	void		validate_public_namespace_surface();
	void		check_fn(FnDecl &fn, Module &m);
	void		mark_import_use(const std::vector<std::string> &path, Module &from);
	// Resolve a function's parameter types and signature. Called from Phase 1
	// (register_module_symbols) so that every function's signature -- including
	// fn_sig, used to type function-pointer values and indirect calls -- is
	// available before any module body is typechecked in Phase 2. Modules are
	// checked in queue order (roots before the modules they import), so doing
	// this lazily in check_fn would leave an imported callee's params
	// unresolved when the importing root's call site is typechecked.
	void		resolve_fn_signature(FnDecl &fn, Module &m);
	FnDecl	   *instantiate_generic_fn(FnDecl &fn, Expr &call,
										   const std::vector<std::unique_ptr<Expr>> &args, Module &m,
										   FnDecl &caller);
	StmtPtr		expand_macro_call(Expr &call, FnDecl &caller, Module &m, ScopeStack &scope);
	void		validate_macro_body(FnDecl &macro);
	StType	   *instantiate_generic_type(Symbol &sym, TypeRef &tr, Module &m);
	bool		infer_generic_type(TypeRef &pattern, StType *actual, FnDecl &fn, Module &m);
	bool		bind_generic_arguments(FnDecl &fn, Expr &call,
									   const std::vector<std::unique_ptr<Expr>> &args, Module &m,
									   FnDecl &caller);
	void		validate_generic_params(std::vector<GenericParam> &params);
	bool		generic_type_allowed(const GenericParam &param, StType *supplied) const;
	std::string generic_constraint_text(const GenericParam &param) const;
	void		check_stmt(Stmt &s, FnDecl &ctx, Module &m, ScopeStack &scope, StType *ret);
	StType	   *check_layout_operand(Expr &operand, FnDecl &ctx, Module &m, ScopeStack &scope);
	StType	   *resolve_src_embed(Expr &e, FnDecl &ctx, Module &m, ScopeStack &scope);
	StType	   *check_expr(Expr &e, FnDecl &ctx, Module &m, ScopeStack &scope);
	StType	   *check_expr_with_expected(Expr &e, FnDecl &ctx, Module &m, ScopeStack &scope,
										 StType *expected);
	StType	   *check_indirect_call(Expr &e, FnDecl &ctx, Module &m, ScopeStack &scope);
	StType	   *resolve_ufcs_call(Expr &e, FnDecl &ctx, Module &m, ScopeStack &scope);
	std::string find_similar_identifier(const std::string &name, const ScopeStack &scope, const Module &m);
	std::string candidate_rejection_reason(FnDecl &cand, const Expr &call, Module &call_module,
										   bool cross_module);
	bool		is_dead_branch(Expr &cond);
	// Definite-return analysis: true if every path through s returns.
	bool		definite_return(Stmt &s);
	bool		definite_yield(Stmt &s);

	// Compile-time constant-integer predicate + evaluator (for `case` values).
	// Requires check_expr() to have run on `e` (populates enum int_val).
	bool		is_const_int_expr(Expr &e);
	int64_t		eval_const_int(Expr &e);

	// Compile-time value-fit test for contextual literals. Returns Fit when
	// the literal is exactly representable in `dst`, NoFit when it is
	// provably out of range or has an invalid aggregate shape, or Unknown for
	// typed/non-literal expressions.
	FitResult	value_fits(StType *dst, Expr &src);
	// Generic overloads may use a float-constrained T for more than one
	// parameter. An integer literal in a later T slot is contextually
	// converted after T has been bound by an earlier argument.
	bool		generic_integer_literal_fits_float(StType *dst, Expr &src);

	// Exact type check for assignments, initializers, return values, and call
	// arguments. Literal contextual typing must already have happened before
	// this method is called.
	void		check_type_compat(StType *dst, StType *src_ty, Expr &src_expr, const SourceLoc &loc,
								  const std::string &nofit_msg, const std::string &mismatch_msg);

	// Check that an expression evaluates to bool and emit an error if not.
	// Returns true when the type is bool (or untyped void_ fallback).
	bool		expect_bool_cond(const SourceLoc &loc, StType *ty);

	bool		field_accessible(const StType &agg, size_t idx, const Module &from) const;
	void		report_private_field(const SourceLoc &loc, const StType &agg, size_t idx) const;

	// Loop / switch context stack for break/continue legality.
	bool		in_loop() const {
		return !loop_stack_.empty();
	}
	bool in_switch() const {
		return !switch_stack_.empty();
	}
	bool in_macro_loop() const {
		// A caller's loop is not a valid target for control flow emitted by a
		// macro. During ordinary function checking there is no macro boundary.
		return macro_control_bases_.empty() ||
			loop_stack_.size() > macro_control_bases_.back().loops;
	}
	bool in_macro_switch() const {
		return macro_control_bases_.empty() ||
			switch_stack_.size() > macro_control_bases_.back().switches;
	}
	void enter_loop() {
		loop_stack_.push_back(true);
	}
	void leave_loop() {
		loop_stack_.pop_back();
	}
	void enter_switch() {
		switch_stack_.push_back(true);
	}
	void leave_switch() {
		switch_stack_.pop_back();
	}
};

} // namespace stellac
