#pragma once

#include "ast.hpp"
#include "ctx.hpp"
#include "sema.hpp"
#include "types.hpp"

namespace stellac {

class Codegen {
  public:
	Codegen(LLVMCtx &lctx, TypeContext &types, Sema &sema) : l_(lctx), t_(types), sema_(sema) {}

	// Emit IR for every module in sema's resolver (root + imports).
	bool emit_all(ModuleResolver &resolver);

	// When true, emit the complete safe dependency closure of imported public
	// functions. Public entry points use available_externally linkage, while
	// cloned private helpers remain internal. Mutable globals stay declarations
	// of their canonical owner-side objects. Default off keeps -O0 imports as
	// normal runtime declarations.
	void set_inline_imports(bool v) {
		inline_imports_ = v;
	}

  private:
	LLVMCtx		&l_;
	TypeContext &t_;
	Sema		&sema_;
	bool		 inline_imports_ = false;
	uint64_t static_local_counter_ = 0;

	struct LocalVal {
		llvm::Value *val	 = nullptr;
		StType		*ty		 = nullptr;
		bool		 is_addr = false;
	};

	// cache: mangled name -> llvm::Function*
	std::unordered_map<std::string, llvm::Function *>		fns_;
	// cache: mangled struct name -> llvm::StructType*
	std::unordered_map<std::string, llvm::StructType *>		structs_;
	// cache: mangled global name -> llvm::GlobalValue*
	std::unordered_map<std::string, llvm::GlobalVariable *> globals_;
	std::unordered_map<std::string, llvm::Function *> tls_init_functions_;
	std::unordered_map<std::string, llvm::GlobalVariable *> tls_init_guards_;
	std::vector<std::pair<GlobalDecl *, Module *>> tls_dynamic_initializers_;
	// dedupe cache: string literal content -> gep constant into the global
	std::unordered_map<std::string, llvm::Constant *>		string_lits_;
	std::unordered_map<std::string, llvm::Constant *>		slice_string_lits_;
	// type mapping cache
	std::unordered_map<StType *, llvm::Type *>				type_cache_;

	llvm::Type											   *map_type(StType *t);
	llvm::Type											   *map_abi_type(StType *t);
	llvm::Type											   *map_named_struct(StType *t);
	llvm::Type											   *map_named_union(StType *t);
	llvm::Type											   *map_tagged_enum(StType *t);

	struct TaggedEnumLayout {
		llvm::StructType				 *outer_type		 = nullptr;
		llvm::Type						 *tag_type			 = nullptr;
		unsigned						  tag_field_idx		 = 0;
		unsigned						  payload_field_idx	 = 0;
		bool							  has_payload		 = false;
		std::vector<llvm::StructType *>	  variant_types;
		std::vector<std::vector<unsigned>> variant_field_map;
	};
	std::unordered_map<StType *, TaggedEnumLayout> tagged_enum_layouts_;
	const TaggedEnumLayout						  &get_tagged_enum_layout(StType *t);
	// StType::Function -> llvm::FunctionType for the call site. Function
	// values themselves are opaque pointers at LLVM level (see map_type).
	llvm::FunctionType									   *map_fn_type(StType *t);
	llvm::Value	   *to_abi_value(llvm::Value *v, llvm::Type *dst);
	llvm::Value	   *from_abi_value(llvm::Value *v, llvm::Type *dst);
	Expr		   *abi_result_expr_ = nullptr;
	llvm::Value	   *sret_ptr_		 = nullptr;

	struct CAbiArgInfo {
		enum class Kind { Direct, SRet, ByVal, Indirect, Ignore };
		Kind		kind		 = Kind::Direct;
		llvm::Type *storage_type = nullptr;
		llvm::Type *coerced_type = nullptr;
		uint64_t	align		 = 0;
	};
	struct CAbiSignature {
		CAbiArgInfo				  ret;
		std::vector<CAbiArgInfo>  params;
		llvm::FunctionType		 *fn_type = nullptr;
	};
	CAbiArgInfo	  classify_c_abi_type(StType *t, bool is_return);
	CAbiSignature compute_c_abi_signature(const std::vector<StType *> &param_types,
										  StType *ret_type, bool is_variadic);
	void		  apply_c_abi_attrs(llvm::Function *fn, const CAbiSignature &sig);
	void		  apply_c_abi_call_attrs(llvm::CallInst *call, const CAbiSignature &sig);
	llvm::Value	 *coerce_abi_via_memory(llvm::IRBuilder<> &B, llvm::Value *v, llvm::Type *dst);

	void			declare_struct(StructDecl &s, Module &m);
	void			declare_union(UnionDecl &u, Module &m);
	void			declare_fn(FnDecl &f, Module &m);
	void			emit_fn_body(FnDecl &f, Module &m);
	void			emit_global(GlobalDecl &g, Module &m);
	void			declare_global(GlobalDecl &g, Module &m);
	void			emit_tls_dynamic_initializers();
	void			ensure_tls_initialized(llvm::IRBuilder<> &builder, const std::string &name);
	// Lower a compile-time global initializer, including constant casts,
	// unary/binary expressions, and aggregate literals. Returns nullptr when
	// the expression is not representable as an LLVM constant.
	llvm::Constant *lower_global_init(GlobalDecl &g, Module &m, StType *ty, llvm::Type *&lt,
									  const std::string &nm);
	llvm::Constant *lower_const_expr(Expr &e, Module &m, StType *expected_ty,
									 llvm::Type *expected_lt, const std::string &nm);

	void			apply_fn_attrs(llvm::Function *fn, AttributeList &attrs);
	void			apply_struct_attrs(llvm::StructType *st, StructDecl &s);
	void			apply_global_attrs(llvm::GlobalVariable *gv, AttributeList &attrs);
	std::string		fn_symbol_name(FnDecl &f);

	void			emit_stmt(Stmt &s, FnDecl &ctx, Module &m);
	llvm::Value	   *emit_expr(Expr &e, FnDecl &ctx, Module &m);
	llvm::Value	   *emit_lvalue(Expr &e, FnDecl &ctx, Module &m);
	llvm::Value	   *emit_call(Expr &e, FnDecl &ctx, Module &m);
	llvm::Value	   *emit_struct_lit(Expr &e, FnDecl &ctx, Module &m);
	llvm::Value	   *emit_union_lit(Expr &e, FnDecl &ctx, Module &m);
	void			emit_asm(Stmt &s, FnDecl &ctx, Module &m);

	void			br_or_absorb(llvm::IRBuilder<> &B, llvm::BasicBlock *target);
	llvm::Value	   *coerce_cond(llvm::IRBuilder<> &B, llvm::Value *c);
	llvm::Constant *cstring_global(llvm::StringRef s, const std::string &nm);
	llvm::Constant *slice_string_global(llvm::StringRef s, const std::string &nm);

	llvm::Value	   *alloca_in_entry(llvm::Type *ty, const char *name);
	llvm::Value	   *alloca_in_entry_typed(StType *t, llvm::Type *lt, const char *name);
	llvm::Align		struct_align(StType *t);
	llvm::Align		storage_align(StType *t, llvm::Type *lt);
	llvm::Align		pointer_align(llvm::Value *val, StType *st, llvm::Type *lt);
	llvm::Value	   *load_value(llvm::Value *ptr, StType *ty, const SourceLoc &loc);
	void			store_value(llvm::Value *ptr, llvm::Value *val, StType *ty, StType *src_st,
								const SourceLoc &loc);

	llvm::Function *cur_fn(FnDecl &ctx);
	LocalVal	   *find_local(const std::string &n);

	// per-function emission state
	std::vector<std::unordered_map<std::string, LocalVal>> local_stack_;
	llvm::IRBuilder<>									  *builder_ = nullptr;
	FnDecl												  *cur_fn_	= nullptr;
	llvm::Function	 *cur_llvm_fn_ = nullptr; // cached for cur_fn() fast path
	llvm::BasicBlock *sink_block_  = nullptr; // shared dead-pool BB (opt 7)
	const Expr		  *compound_lhs_expr_ = nullptr;
	llvm::Value	  *compound_lhs_value_ = nullptr;
	// Source va-list locals are mapped explicitly to target ABI storage.
	std::unordered_map<VarDeclInfo *, llvm::Value *> va_list_storage_;
	llvm::StructType								*va_list_llvm_type();
	llvm::Value					   *va_list_operand(Expr &operand, FnDecl &ctx, Module &m);
	bool							target_supports_va_lists() const;
	llvm::Value *emit_va_arg(llvm::Value *cursor, StType *type);
	struct DeferScope {
		std::vector<Stmt *> active;
	};
	struct ControlTarget {
		llvm::BasicBlock *target = nullptr;
		size_t defer_scope_depth = 0;
	};
	std::vector<DeferScope> defer_scopes_;
	std::vector<ControlTarget> break_stack_;
	std::vector<ControlTarget> continue_stack_;
	void emit_active_defers(size_t retain_depth, FnDecl &ctx, Module &m);
	struct BlockExprState {
		llvm::BasicBlock *merge = nullptr;
		size_t defer_scope_depth = 0;
		std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> values;
	};
	std::vector<BlockExprState> block_expr_stack_;
};

} // namespace stellac
