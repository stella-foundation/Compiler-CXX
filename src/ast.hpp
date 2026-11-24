#pragma once

#include "token.hpp"
#include "types.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace stellac {

struct Symbol; // forward decl
struct FnDecl;
struct Module;
struct TopDecl;
using TopDeclPtr = std::unique_ptr<TopDecl>;

// Attributes
struct Attribute {
	std::string				 name;			  // canonical base name, e.g. "section"
	bool					 negated = false; // source spelling was #[!name]
	std::vector<std::string> args;			  // positional args: [".text.boot"], [8], ["address"]
	std::vector<bool>		 arg_is_string;	  // true if parsed as StringLit
	SourceLoc				 loc;
};

struct AttributeList {
	std::vector<Attribute> attrs;
	SourceLoc			   loc;
};

enum class AttributeTarget : uint32_t {
	Function  = 1u << 0,
	Struct	  = 1u << 1,
	Union	  = 1u << 2,
	Global	  = 1u << 3,
	TypeBinding = 1u << 4,
	Enum	  = 1u << 5,
	Import	  = 1u << 6,
	Parameter	 = 1u << 7,
	Local		 = 1u << 8,
	FunctionType = 1u << 9,
};

enum class AttributePolarity { PositiveOnly, NegativeOnly, Either };
enum class AttributeArgKind { None, String, OptionalString, Integer, Layout, SanitizerSelector };

struct AttributeSpec {
	std::string_view  name;
	uint32_t		  targets;
	AttributePolarity polarity;
	AttributeArgKind  arg_kind;
};

// Attribute specifications and lookup helpers (single source of truth).
const AttributeSpec *lookup_attribute_spec(std::string_view name);
bool				 attribute_target_allowed(const AttributeSpec &spec, AttributeTarget target);
const Attribute		*find_attr(const AttributeList &al, std::string_view name);
const Attribute		*find_negated_attr(const AttributeList &al, std::string_view name);
bool				 has_attr(const AttributeList &al, std::string_view name);
bool				 has_negated_attr(const AttributeList &al, std::string_view name);
std::string attr_string_or(const AttributeList &al, std::string_view name, std::string def = {});
std::string attr_diag_prefix(const Attribute &a); // e.g. "attribute '#[!return]'"

// Define-condition helpers (single source of truth). A define condition is an
// expression built from #flag references, unary !, and binary && / ||.
struct Expr;
struct VarDeclInfo;
bool is_define_cond(const Expr &e);
bool eval_define_cond(const Expr &e, const std::unordered_map<std::string, bool> &flags);

// Render an expression as canonical Stella source for syntax metaprogramming.
std::string stringify_expr(const Expr &e);

struct TypeRef;

// Generic arguments stay in source form until semantic analysis validates
// their kind and resolves/evaluates them in the current module.
struct GenericArg {
	enum class Kind { Type, Value } kind = Kind::Type;
	std::unique_ptr<TypeRef> type;
	std::unique_ptr<Expr>	 value;
	SourceLoc				 loc;
};

struct GenericParam {
	enum class Kind { Type, Const } kind = Kind::Type;
	struct TypeSetMember {
		enum class Kind { Exact, Sint, Uint, Float } kind = Kind::Exact;
		// Source spelling before semantic normalization; for Exact members
		// this is retained as the canonical primitive spelling for diagnostics.
		std::string name;
		// Normalized exact primitive identity. Categories leave these unset.
		TypeKind	exact_type	= TypeKind::Void;
		int			exact_width = 0;
		SourceLoc	loc;
	};
	std::string				   name;
	std::unique_ptr<TypeRef>   const_type;
	std::vector<TypeSetMember> type_set;
	std::vector<std::unique_ptr<TypeRef>> trait_bounds;
	SourceLoc				   loc;
};

// Type references (as written in source)
struct TypeRef {
	enum Kind { Named, Pointer, Array, Slice, Function, TypeOf, SelfType };
	Kind					 kind = Named;
	std::string				 name;	 // for Named (may include :: separators baked-in as text)
	std::string				 single; // for Named with no separators (bareword)
	std::unique_ptr<TypeRef> base;	 // for Pointer, Array
	size_t					 array_len = 0;				 // for Array: element count
	std::unique_ptr<Expr>	 array_len_expr;			 // expression form, for const generics
	bool					 array_len_inferred = false; // `[]T`: infer from the initializer
	bool					 is_mutable			= false; // for Pointer: *mut T (false = immutable *T)
	bool					 is_volatile		= false; // for Pointer: *volatile T
	// Function type syntax: `fn(p0, p1, ...) ret`
	std::vector<std::unique_ptr<TypeRef>> fn_params;
	std::unique_ptr<TypeRef>			  fn_ret;
	bool								  fn_variadic = false;
	AttributeList						  fn_attrs;
	std::vector<GenericArg>				  generic_args; // named generic application
	std::unique_ptr<Expr>				  typeof_expr;	// for TypeOf: @of.type(expr)
	bool	  sema_invalid = false; // suppress cascading diagnostics after a resolution error
	SourceLoc loc;
};

// Expressions
struct Stmt;
enum class ExprKind {
	IntLit,
	FloatLit,
	StringLit,
	CharLit,
	BoolLit,
	NullLit,
	Ident,	   // bare or qualified path "a::b::c" stored in name
	EnumMember, // contextual enum member shorthand, e.g. `.Info`
	VariantInit, // tagged enum struct-variant init: `Enum.Var { ... }` or `.Var { ... }`
	DefineRef, // single #flag
	Unary,
	Binary,
	Assign,
	Conditional,
	Call,
	Member,
	Index,
	Slice,
	TypeValue, // compile-time type value, e.g. `u32`
	StructLit,
	ArrayLit,
	Cast, // explicit cast: `expr cast Type` (normal) or `expr bitcast Type` (bit reinterpret);
		  // `is_bitcast` discriminates the two
	AddrOf,
	Deref,
	Sizeof,
	Alignof,
	Bitsof,
	Offsetof, // type/layout intrinsics
	VaStart,
	VaCopy,
	VaEnd,
	VaArg, // explicit varargs builtins
	VariadicSplice, // macro variadic AST capture splice (`...`)
	Block, // block expression; its value is produced by `yield`
	SrcFile,
	SrcLine,
	SrcFn,
	SrcColumn,
	SrcEmbed,  // @src.embed("FILE")
	Stringify, // @syn.stringify(syntax)
	DiagWarn,  // @diag.warn(description, message)
	DiagErr,   // @diag.err(description, message)
};

struct Expr {
	~Expr();
	Module *binding_module = nullptr;
	bool macro_argument = false;
	SourceLoc invocation_loc;
	std::vector<SourceLoc> expansion_chain;
	ExprKind						   kind = ExprKind::IntLit;
	SourceLoc						   loc;
	bool							   parenthesized = false;
	// Literals
	uint64_t						   int_val = 0;
	double							   flt_val = 0;
	std::string						   str_val;
	bool							   is_cstring = false; // true for c"..."
	uint32_t						   char_val = 0;
	// Ident / DefineRef / member name
	std::vector<std::string>		   path;   // for Ident: e.g. {"std","io","print"}
	std::string						   member; // for Member
	// children
	std::unique_ptr<Expr>			   lhs;
	std::unique_ptr<Expr>			   rhs;
	std::unique_ptr<Expr>			   index_low; // Index: inclusive bit range low bound
	uint64_t							 bit_range_high = 0;
	uint64_t							 bit_range_low = 0;
	bool								 is_bit_range = false;
	// Conditional: condition ? then_expr : else_expr
	std::unique_ptr<Expr>			   condition;
	std::unique_ptr<Expr>			   then_expr;
	std::unique_ptr<Expr>			   else_expr;
	std::vector<std::unique_ptr<Expr>> args;		  // Call / StructLit values
	std::vector<std::string>		   field_names;	  // StructLit named fields
	std::vector<GenericArg>			   generic_args;  // explicit call arguments
	std::unique_ptr<TypeRef>		   cast_type;	  // Cast target
	std::unique_ptr<TypeRef>		   layout_type;	  // Compound layout type operand
	bool					 is_bitcast = false;	  // Cast: true => bitcast (bit reinterpret)
	bool					 is_compound_assignment = false;
	bool					 is_struct_variant = false; // VariantInit / Call: true for struct-style `{ ... }`
	std::string				 offsetof_member;		  // Offsetof member name
	std::unique_ptr<TypeRef> va_arg_ty;				  // VaArg target type
	VarDeclInfo				*va_list_decl  = nullptr; // resolved local va-list operand
	bool					 va_list_param = false;	  // resolved va-list parameter operand
	// resolved (filled by sema)
	StType					*ty			   = nullptr;
	StType					*type_value	   = nullptr; // payload for a compile-time type value
	FnDecl					*resolved_fn   = nullptr;
	bool sema_invalid		  = false; // suppress cascading diagnostics after a prior error
	bool is_lvalue			  = false;
	bool is_addressable		  = false;
	bool is_const_bool		  = false;
	bool const_bool_val		  = false;
	bool has_const_value	  = false;
	bool const_value_is_float = false;
	bool const_int_high_bits_ones = false; // bits above int_val are all ones
	std::unique_ptr<Stmt> block; // block-expression body

	Expr() = default;
	Expr(Expr &&) = default;
	Expr &operator=(Expr &&) = default;
	Expr(const Expr &) = delete;
	Expr &operator=(const Expr &) = delete;
};

// Statements
struct Stmt;
using StmtPtr = std::unique_ptr<Stmt>;

enum class StmtKind {
	Block,
	ExprStmt,
	VarDecl,
	ConstDecl,
	If,
	For,
	Match,
	Return,
	Unreachable,
	Break,
	Continue,
	Defer,
	Yield,
	Asm,
	Import, // resolved at parse; kept for raw listing
};

enum class MatchPatternKind { Wildcard, Value, Variant };

struct MatchBinding {
	std::string					 name;		 // "_" to ignore
	std::string					 field_name; // for struct-style `.Var { field: local }`
	bool						 is_mut = false;
	SourceLoc					 loc;
	StType						*resolved = nullptr; // filled by sema
	std::unique_ptr<VarDeclInfo> var_decl;			 // filled by sema for codegen
};

struct MatchPattern {
	MatchPatternKind		  kind = MatchPatternKind::Value;
	std::unique_ptr<Expr>	  expr;		   // Value expr or Enum.Var / .Var target
	std::vector<MatchBinding> bindings;	   // for Variant
	bool					  is_struct_pattern = false;
	uint64_t				  resolved_tag		= 0; // filled by sema
	size_t					  variant_index		= 0; // filled by sema
	SourceLoc				  loc;
};

struct MatchArm {
	std::vector<MatchPattern> patterns; // >1 for `|` or-patterns
	StmtPtr					  body;		// always a Block
	SourceLoc				  loc;
};

struct VarDeclInfo {
	Module *binding_module = nullptr;
	bool macro_argument = false;
	std::string				 name;
	std::unique_ptr<TypeRef> ty;
	std::unique_ptr<Expr>	 init; // optional
	SourceLoc				 loc;
	bool					 is_const = false;
	bool					 is_mutable = false;
	bool					 is_static = false;
	bool					 is_thread_local = false;
	StType					*resolved = nullptr;
	AttributeList			 attrs;
	bool					 used = false;
};

struct AsmOperand {
	std::string			  constraint;
	std::unique_ptr<Expr> operand; // null for clobber entries
	bool				  is_clobber = false;
};

struct Stmt {
	~Stmt();
	Module *binding_module = nullptr;
	bool macro_argument = false;
	SourceLoc invocation_loc;
	std::vector<SourceLoc> expansion_chain;
	StmtKind							  kind = StmtKind::Block;
	SourceLoc							  loc;

	// Block
	std::vector<StmtPtr>				  body;
	// ExprStmt
	std::unique_ptr<Expr>				  expr;
	// VarDecl/ConstDecl. A single source declaration may bind several names.
	std::vector<VarDeclInfo>			  vars;
	// If: cond, then, else
	std::unique_ptr<Expr>				  cond;
	StmtPtr								  then_block;
	StmtPtr								  else_block;
	// For: init, cond, step + body, modeled via cond + extra exprs
	StmtPtr								  for_init;
	std::unique_ptr<Expr>				  for_cond;
	std::unique_ptr<Expr>				  for_step;
	StmtPtr								  loop_body;
	// Match: scrutinee + arms
	std::unique_ptr<Expr>				  scrutinee;
	std::vector<MatchArm>				  arms;
	// Return
	std::unique_ptr<Expr>				  ret_val;
	// Defer: body is executed on every exit from the enclosing scope after
	// control reaches this statement.
	StmtPtr								  defer_body;
	// Asm
	std::string							  asm_code;
	std::vector<AsmOperand>				  asm_outputs;
	std::vector<AsmOperand>				  asm_inputs;
	std::vector<std::string>			  asm_clobbers;
	bool								  asm_volatile = false;
	// Import (raw); kept but resolved before sema
	std::vector<std::string>			  import_path;	// e.g. {"std","io"}
	std::vector<std::vector<std::string>> import_group; // for grouped imports
};

// Top-level declarations
struct Param {
	std::string				 name;
	std::unique_ptr<TypeRef> ty;
	SourceLoc				 loc;
	StType					*resolved = nullptr; // resolved param type (sema), cached for codegen
	AttributeList			 attrs;				 // per-parameter attributes (e.g. #[!alias])
	bool					 used		 = false;
	bool					 is_comptime = false; // `T type`: erased compile-time type parameter
	bool					 is_mutable	 = false; // `mut name: T`: binding may be reassigned in the body
};

enum class TopKind { Error, Mod, Fn, Struct, Union, TypeBinding, Var, Const, Import, Enum, Conditional, Trait, Impl };

struct ModDecl {
	std::string				 name;
	std::vector<std::string> owner_path;
	std::vector<TopDeclPtr>	 body;
	bool					 is_pub = false;
	SourceLoc				 loc;
	AttributeList			 attrs;
};

struct FnDecl {
	bool is_macro = false;
	bool macro_returns_value = false;
	std::vector<std::string> owner_path; // for method: {"Foo"} // parsed as prefix
	std::string				 name;		 // last segment of fn name
	std::vector<std::string> full_path;	 // module path + owner + name
	std::vector<Param>		 params;
	std::unique_ptr<TypeRef> ret;
	StmtPtr					 body;
	bool					 is_pub		 = false;
	bool					 is_method	 = false; // had Foo:: prefix
	bool					 is_variadic = false;
	bool		  is_extern = false; // `extern fn` -- declaration only, no body
	AbiKind		  abi		= AbiKind::Stella;
	SourceLoc	  loc;
	AttributeList attrs;
	std::vector<GenericParam> generic_params;
	size_t					  impl_generic_param_count = 0;
	// resolved
	StType					 *fn_sig = nullptr;
	Symbol					 *sym	 = nullptr; // back-pointer to the registered Symbol
	std::string				  mangled;			// mangled function symbol name
	bool					  used				  = false;
	Module					 *decl_module		  = nullptr;
	bool					  is_generic_instance = false;
	FnDecl					 *generic_template	  = nullptr;
	std::unordered_map<std::string, StType *>	 generic_type_subst;
	std::unordered_map<std::string, int64_t>	 generic_const_subst;
	std::unordered_map<std::string, std::string> generic_const_string_subst;
};

struct StructDecl {
	std::string							  name;
	std::vector<std::string>			  field_names;
	std::vector<std::unique_ptr<TypeRef>> field_types;
	std::vector<bool>					  field_is_pub; // parallel to field_names; false = private
	bool								  is_pub = false;
	SourceLoc							  loc;
	AttributeList						  attrs;
	std::vector<GenericParam>			  generic_params;
	bool								  is_opaque = false;
	StType								 *resolved = nullptr;
};

struct UnionDecl {
	std::string							  name;
	std::vector<std::string>			  field_names;
	std::vector<std::unique_ptr<TypeRef>> field_types;
	std::vector<bool>					  field_is_pub; // parallel to field_names; false = private
	bool								  is_pub = false;
	SourceLoc							  loc;
	AttributeList						  attrs;
	std::vector<GenericParam>			  generic_params;
	StType								 *resolved = nullptr;
};

struct TypeBindingDecl {
	std::string				  name;
	std::unique_ptr<TypeRef>  target;
	bool					  is_pub		= false;
	SourceLoc				  loc;
	AttributeList			  attrs;
	std::vector<GenericParam> generic_params;
	StType					 *resolved = nullptr;
	bool					  resolving = false;
	bool					  sema_invalid = false;
};

struct GlobalDecl {
	std::string				 name;
	std::vector<std::string> owner_path;
	std::unique_ptr<TypeRef> ty;
	std::unique_ptr<Expr>	 init;
	bool					 is_const = false;
	bool					 is_mutable = false;
	bool					 is_static = false;
	bool					 is_thread_local = false;
	bool					 is_pub	  = false;
	bool					 is_extern = false;
	SourceLoc				 loc;
	AttributeList			 attrs;
	bool					 used = false;
};

struct ImportDecl {
	std::vector<std::string>			  path;	 // {"std","io"}
	std::vector<std::vector<std::string>> group; // for braces
	// Exact source spelling and the canonical file selected by the resolver.
	std::string						 written_path;
	std::string						 resolved_path;
	Module							 *target = nullptr;
	std::vector<Module *> targets;
	std::vector<std::string> resolved_paths;
	bool								 is_pub = false;
	SourceLoc							  loc;
	AttributeList						  attrs;
	bool								  used	   = false;
	bool								  resolved = false;
};

struct EnumVariantField {
	std::string				 name; // empty for Tuple payloads
	std::unique_ptr<TypeRef> ty;
	SourceLoc				 loc;
};

struct EnumMember {
	std::string					  name;
	EnumPayloadKind				  payload_kind = EnumPayloadKind::Unit;
	std::vector<EnumVariantField> payload_fields;
	std::unique_ptr<Expr>		  value;		// optional explicit initializer
	uint64_t					  resolved = 0; // filled by sema: final discriminant bit-pattern
	SourceLoc					  loc;
};

struct EnumDecl {
	std::string				  name;
	std::vector<EnumMember>	  members;
	bool					  is_pub = false;
	SourceLoc				  loc;
	AttributeList			  attrs;
	std::unique_ptr<TypeRef>  backing; // mandatory backing integer type (`enum E T`)
	std::vector<GenericParam> generic_params;
	StType					 *resolved = nullptr; // enum StType (kind == TypeKind::Enum)
};

struct ConditionalArm {
	std::unique_ptr<Expr>	cond; // null for the final `else` arm
	std::vector<TopDeclPtr> body;
	SourceLoc				loc;
};

struct ConditionalDecl {
	std::vector<ConditionalArm> arms;
	SourceLoc					loc;
};

struct TraitDecl {
	std::string							 name;
	bool								 is_pub = false;
	SourceLoc							 loc;
	AttributeList						 attrs;
	std::vector<std::unique_ptr<FnDecl>> methods;
	Module								*decl_module = nullptr;
	std::string							 mangled;
};

struct ImplDecl {
	bool									 is_inherent = false;
	std::string								 trait_name;
	std::vector<std::string>				 trait_path;
	std::vector<std::string>				 owner_path;
	std::vector<GenericParam>				 generic_params;
	std::unique_ptr<TypeRef>				 target_type;
	SourceLoc								 loc;
	AttributeList							 attrs;
	std::vector<std::unique_ptr<FnDecl>>	 methods;
	std::vector<std::unique_ptr<GlobalDecl>> consts;
	Module									*decl_module		  = nullptr;
	TraitDecl								*resolved_trait		  = nullptr;
	StType									*resolved_target_type = nullptr;
};

struct TopDecl {
	~TopDecl()							  = default;
	TopKind							 kind = TopKind::Error;
	std::unique_ptr<ModDecl>		 mod_decl;
	std::unique_ptr<FnDecl>			 fn;
	std::unique_ptr<StructDecl>		 st;
	std::unique_ptr<UnionDecl>		 un;
	std::unique_ptr<TypeBindingDecl>	 tb;
	std::unique_ptr<GlobalDecl>		 glob;
	std::unique_ptr<ImportDecl>		 imp;
	std::unique_ptr<EnumDecl>		 en;
	std::unique_ptr<ConditionalDecl> conditional;
	std::unique_ptr<TraitDecl>		 trait_decl;
	std::unique_ptr<ImplDecl>		 impl_decl;
};

// Module
struct Module {
	std::string				 fpath;		 // filesystem path
	std::string private_symbol_module_id; // stable source-tree-relative owner identity
	std::vector<std::string> mod_path;	 // file-level namespace path from `mod <qpath>;`
	std::vector<std::string> lexical_path;
	AttributeList			 file_attrs; // `#[...]` preceding the file-level `mod;`
	SourceLoc				 mod_loc;	 // location of the file-level `mod;` header
	std::vector<TopDeclPtr>	 decls;		 // imports first, then top-level declarations
	// resolved exported/own symbols after sema (by mangled name)
};

std::unique_ptr<Expr>	 clone_expr(const Expr &e);
StmtPtr					 clone_stmt(const Stmt &s);
std::unique_ptr<TypeRef> clone_typeref(const TypeRef &t);
GenericArg				 clone_generic_arg(const GenericArg &a);
GenericParam			 clone_generic_param(const GenericParam &p);

} // namespace stellac
