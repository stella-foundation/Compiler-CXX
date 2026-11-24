#pragma once

#include "intlit.hpp"
#include "mangle.hpp"
#include "prim_types.hpp"
#include "sema.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <limits>
#include <llvm/ADT/APSInt.h>
#include <llvm/ADT/APFloat.h>
#include <llvm/Support/Error.h>
#include <set>
#include <unordered_set>

namespace stellac {

template <typename Node>
class MacroDiagnosticContextGuard {
  public:
	 explicit MacroDiagnosticContextGuard(Node &node) : node_(node), original_loc_(node.loc) {
		active_ = node.binding_module != nullptr && !node.macro_argument &&
				  !node.invocation_loc.file.empty();
		if (active_) {
			log::push_macro_context(original_loc_, node.expansion_chain);
			node_.loc = node.invocation_loc;
		}
	}
	~MacroDiagnosticContextGuard() {
		if (active_) {
			node_.loc = original_loc_;
			log::pop_macro_context();
		}
	}
	MacroDiagnosticContextGuard(const MacroDiagnosticContextGuard &) = delete;
	MacroDiagnosticContextGuard &operator=(const MacroDiagnosticContextGuard &) = delete;

  private:
	Node &node_;
	SourceLoc original_loc_;
	bool active_ = false;
};

inline std::string source_key(const Module &m) {
	return m.fpath.empty() ? join_path(m.mod_path) : m.fpath;
}

inline std::vector<std::string> split_dotted_path(const std::string &s) {
	std::vector<std::string> parts;
	size_t start = 0;
	while (start < s.size()) {
		size_t dot = s.find('.', start);
		if (dot == std::string::npos) {
			parts.push_back(s.substr(start));
			break;
		}
		parts.push_back(s.substr(start, dot - start));
		start = dot + 1;
	}
	return parts;
}

struct ScopeGuard {
	ScopeStack *&target;
	ScopeStack  *prev;
	ScopeGuard(ScopeStack *&t, ScopeStack *next) : target(t), prev(t) {
		target = next;
	}
	~ScopeGuard() {
		target = prev;
	}
};

namespace {

// Bit size of a scalar (bitcast-compatible) type after underlying_type. Returns 0 for
// non-scalar / unsupported types (void, bool, struct, array, function). For
// usize/isize and pointers use the target pointer width. Enums report their
// backing integer width.
int scalar_size_bits(StType *t, int pointer_width) {
	if (!t) {
		return 0;
	}
	t = underlying_type(t);
	if (!t) {
		return 0;
	}
	switch (t->kind) {
	case TypeKind::UInt:
	case TypeKind::SInt:
		return t->width > 0 ? t->width : pointer_width;
	case TypeKind::Float:
		return t->width > 0 ? t->width : 0;
	case TypeKind::Pointer:
		return pointer_width;
	case TypeKind::Enum:
		return t->is_tagged_enum ? 0 : scalar_size_bits(t->base, pointer_width);
	default:
		return 0;
	}
}

// Is `t` a scalar (numeric, boolean, or pointer) that the `cast` operator can
// convert to/from? Aggregates, tagged enums, and void are excluded. `bitcast`
// has stricter rules and uses scalar_size_bits() instead.
inline bool is_cast_scalar(StType *t) {
	if (!t) {
		return false;
	}
	t = underlying_type(t);
	if (!t) {
		return false;
	}
	return t->kind == TypeKind::Bool || t->kind == TypeKind::UInt || t->kind == TypeKind::SInt ||
		   t->kind == TypeKind::Float || t->kind == TypeKind::Pointer ||
		   (t->kind == TypeKind::Enum && !t->is_tagged_enum);
}

inline bool eq_type(StType *a, StType *b) {
	if (!a || !b) {
		return false;
	}
	a = underlying_type(a);
	b = underlying_type(b);
	if (!a || !b) {
		return false;
	}
	if (a == b) {
		return true;
	}
	if (a->kind == TypeKind::Poison || b->kind == TypeKind::Poison) {
		return true;
	}
	if (a->kind != b->kind) {
		return false;
	}
	if (a->kind == TypeKind::Pointer || a->kind == TypeKind::Slice) {
		return a->is_mutable == b->is_mutable && a->is_volatile == b->is_volatile &&
			eq_type(a->base, b->base);
	}
	if (a->kind == TypeKind::Array) {
		return a->array_len == b->array_len && eq_type(a->base, b->base);
	}
	if (a->kind == TypeKind::Function) {
		if (a->fn_variadic != b->fn_variadic || a->fn_abi != b->fn_abi) {
			return false;
		}
		if (a->fn_params.size() != b->fn_params.size()) {
			return false;
		}
		for (size_t i = 0; i < a->fn_params.size(); ++i) {
			if (!eq_type(a->fn_params[i], b->fn_params[i])) {
				return false;
			}
		}
		return eq_type(a->fn_ret, b->fn_ret);
	}
	return a->name == b->name;
}

static inline bool storage_type_valid(StType *t, std::unordered_set<StType *> &active) {
	t = underlying_type(t);
	// Function values are represented as pointer-sized values in the ABI, so
	// `fn(...)` is a valid parameter/local/global storage type.
	if (!t || t->kind == TypeKind::Void || t->kind == TypeKind::Type) {
		return false;
	}
	if (t->kind == TypeKind::VaList) {
		return true; // opaque storage is supplied by the target va-list backend
	}
	if (t->kind == TypeKind::Slice) {
		return storage_type_valid(t->base, active);
	}
	if (t->kind == TypeKind::Array) {
		return t->array_len != 0 && storage_type_valid(t->base, active);
	}
	if (t->kind == TypeKind::Struct || t->kind == TypeKind::Union) {
		if (t->is_opaque) {
			return false;
		}
		if (t->fields.empty() || std::any_of(t->fields.begin(), t->fields.end(),
											 [](StType *field) { return field == nullptr; })) {
			// Aggregate fields are populated after function signatures are
			// resolved. Treat this as an incomplete-but-not-invalid type here;
			// validate_aggregate_layouts() performs the final storage check.
			return true;
		}
		if (!active.insert(t).second) {
			return true; // recursive cycles are diagnosed separately
		}
		for (auto *field : t->fields) {
			if (!storage_type_valid(field, active)) {
				active.erase(t);
				return false;
			}
		}
		active.erase(t);
	}
	return true;
}

static inline bool fn_signature_equal(const FnDecl &a, const FnDecl &b) {
	if (a.is_variadic != b.is_variadic || a.abi != b.abi ||
		a.params.size() != b.params.size()) {
		return false;
	}
	for (size_t i = 0; i < a.params.size(); ++i) {
		if (!eq_type(a.params[i].resolved, b.params[i].resolved)) {
			return false;
		}
	}
	return eq_type(a.fn_sig ? a.fn_sig->fn_ret : nullptr, b.fn_sig ? b.fn_sig->fn_ret : nullptr);
}

static inline AbiKind resolve_abi_attr(const AttributeList &attrs) {
	const Attribute *a = find_attr(attrs, "abi");
	if (a && !a->args.empty() && a->args[0] == "C") {
		return AbiKind::C;
	}
	return AbiKind::Stella;
}

static inline ReprKind resolve_repr_attr(const AttributeList &attrs) {
	const Attribute *a = find_attr(attrs, "repr");
	if (a && !a->args.empty() && a->args[0] == "C") {
		return ReprKind::C;
	}
	return ReprKind::Stella;
}

static inline bool is_ffi_safe_value_type(const StType *t) {
	t = underlying_type(t);
	if (!t) {
		return true;
	}
	switch (t->kind) {
	case TypeKind::Void:
	case TypeKind::Bool:
	case TypeKind::UInt:
	case TypeKind::SInt:
	case TypeKind::Float:
	case TypeKind::Pointer:
	case TypeKind::Function:
	case TypeKind::Enum:
		return !t->is_tagged_enum || t->repr == ReprKind::C;
	case TypeKind::VaList:
	case TypeKind::Poison:
		return true;
	case TypeKind::Array:
		return is_ffi_safe_value_type(t->base);
	case TypeKind::Struct:
	case TypeKind::Union:
		return t->repr == ReprKind::C;
	case TypeKind::Slice:
	case TypeKind::Opaque:
	case TypeKind::Type:
	case TypeKind::TypeBinding:
	case TypeKind::SelfType:
		return false;
	}
	return false;
}

static inline bool flatten_member_path(const Expr &e, std::vector<std::string> &out) {
	if (e.kind == ExprKind::Ident) {
		out = e.path;
		return !out.empty();
	}
	if (e.kind == ExprKind::Member && e.lhs && flatten_member_path(*e.lhs, out)) {
		out.push_back(e.member);
		return true;
	}
	return false;
}

// Resolve a contextual enum member without searching all visible enums. The
// destination enum is authoritative, just as it is for contextual literals.
static inline bool enum_member_value(StType *type, std::string_view member, uint64_t &value) {
	type = underlying_type(type);
	if (!type || type->kind != TypeKind::Enum) {
		return false;
	}
	for (size_t i = 0; i < type->field_names.size() && i < type->enum_values.size(); ++i) {
		if (type->field_names[i] == member) {
			value = type->enum_values[i];
			return true;
		}
	}
	return false;
}

static inline bool is_contextual_enum_variant_expr(const Expr &e) {
	return e.kind == ExprKind::EnumMember ||
		   (e.kind == ExprKind::Call && e.lhs && e.lhs->kind == ExprKind::EnumMember) ||
		   (e.kind == ExprKind::VariantInit && !e.lhs);
}

static inline std::string_view contextual_enum_variant_name(const Expr &e) {
	if (e.kind == ExprKind::Call && e.lhs && e.lhs->kind == ExprKind::EnumMember) {
		return e.lhs->member;
	}
	return e.member;
}

static inline bool has_unresolved_import(const Module					&module,
										 const std::vector<std::string> &path) {
	for (const auto &decl : module.decls) {
		if (!decl || decl->kind != TopKind::Import || !decl->imp || decl->imp->resolved) {
			continue;
		}
		std::vector<std::vector<std::string>> imports = decl->imp->group;
		if (imports.empty() && !decl->imp->path.empty()) {
			imports.push_back(decl->imp->path);
		}
		for (const auto &import : imports) {
			if (!import.empty() && import.size() <= path.size() &&
				(std::equal(import.begin(), import.end(), path.begin()) ||
				 import.front() == path.front())) {
				return true;
			}
		}
	}
	return false;
}

static inline std::string missing_child_module(ModuleResolver &resolver, Sema &sema, Module &from,
													   const std::vector<std::string> &path) {
	if (path.size() < 3) {
		return {};
	}
	// A namespace can be structurally spread across several explicitly
	// namespaced source files.  The old check below only searched the first
	// loaded fragment, which incorrectly reported a visible declaration from a
	// sibling fragment as a missing child module.  Let the requesting file's
	// filtered interface answer first; this helper is only a fallback for a
	// genuinely absent namespace prefix.
	for (size_t length = path.size() - 1; length >= 1; --length) {
		std::vector<std::string> prefix(path.begin(), path.begin() + length);
		if (sema.resolve_path(prefix, from)) {
			return {};
		}
	}
	for (size_t length = path.size() - 1; length >= 1; --length) {
		std::vector<std::string> prefix(path.begin(), path.begin() + length);
		Module					*parent = resolver.find_loaded(prefix);
		if (!parent) {
			continue;
		}
		const std::string		&child = path[length];
		std::vector<std::string> child_path(path.begin(), path.begin() + length + 1);
		if (!resolver.find_loaded(child_path) && !sema.lookup_in_module(*parent, child)) {
			return join_path(child_path);
		}
		break;
	}
	(void)from;
	return {};
}

static inline bool is_contextual_literal(const Expr &e) {
	switch (e.kind) {
	case ExprKind::IntLit:
	case ExprKind::CharLit:
	case ExprKind::FloatLit:
	case ExprKind::NullLit:
		return true;
	case ExprKind::EnumMember:
		return e.ty == nullptr;
	case ExprKind::Call:
		return e.ty == nullptr && e.lhs && e.lhs->kind == ExprKind::EnumMember;
	case ExprKind::VariantInit:
		return e.ty == nullptr && !e.lhs;
	case ExprKind::Unary:
		return (e.str_val == "-" || e.str_val == "~") && e.lhs && is_contextual_literal(*e.lhs);
	case ExprKind::Binary:
		if (!e.lhs || !e.rhs || !is_contextual_literal(*e.lhs) || !is_contextual_literal(*e.rhs)) {
			return false;
		}
		return e.str_val == "+" || e.str_val == "-" || e.str_val == "*" || e.str_val == "/" ||
			   e.str_val == "%" || e.str_val == "+%" || e.str_val == "-%" ||
			   e.str_val == "*%" || e.str_val == "+|" || e.str_val == "-|" ||
			   e.str_val == "*|" || e.str_val == "<<" || e.str_val == ">>" ||
			   e.str_val == "&" || e.str_val == "|" || e.str_val == "^";
	case ExprKind::ArrayLit:
		return std::all_of(e.args.begin(), e.args.end(),
						   [](const auto &arg) { return arg && is_contextual_literal(*arg); });
	case ExprKind::StructLit:
		return true;
	case ExprKind::Conditional:
		return e.then_expr && e.else_expr && is_contextual_literal(*e.then_expr) &&
			   is_contextual_literal(*e.else_expr);
	default:
		return false;
	}
}

// An integer expression made solely from literals has no type until an
// explicit integer destination supplies one. This is deliberately separate
// from variable/type inference: already-typed operands are never retagged.
static inline bool is_contextual_integer_expr(const Expr &e) {
	switch (e.kind) {
	case ExprKind::IntLit:
		return true;
	case ExprKind::Unary:
		return (e.str_val == "-" || e.str_val == "~") && e.lhs &&
			   is_contextual_integer_expr(*e.lhs);
	case ExprKind::Binary:
		return is_contextual_literal(e);
	default:
		return false;
	}
}

static inline void apply_contextual_integer_type(Expr &e, StType *type) {
	if (e.kind == ExprKind::IntLit) {
		e.ty = type;
		return;
	}
	if (e.kind == ExprKind::Unary && e.lhs) {
		apply_contextual_integer_type(*e.lhs, type);
	} else if (e.kind == ExprKind::Binary) {
		if (e.lhs) {
			apply_contextual_integer_type(*e.lhs, type);
		}
		if (e.rhs) {
			apply_contextual_integer_type(*e.rhs, type);
		}
	}
	e.ty = type;
}

static inline void clear_folded_value(Expr &e) {
	e.has_const_value = false;
	e.is_const_bool = false;
	e.const_bool_val = false;
	e.const_value_is_float = false;
	e.const_int_high_bits_ones = false;
	e.int_val = 0;
	e.flt_val = 0.0;
}

static inline void clear_contextual_integer_values(Expr &e) {
	if (e.kind == ExprKind::Unary || e.kind == ExprKind::Binary) {
		clear_folded_value(e);
		if (e.lhs) {
			clear_contextual_integer_values(*e.lhs);
		}
		if (e.kind == ExprKind::Binary && e.rhs) {
			clear_contextual_integer_values(*e.rhs);
		}
	}
}

// Fill each unsized array dimension from the matching initializer. Plain string
// literals omit the NUL terminator, while c"..." literals include it.
static inline bool infer_array_lengths(TypeRef &type, const Expr &init) {
	if (type.kind != TypeRef::Array) {
		return true;
	}

	if (type.array_len_inferred) {
		if (init.kind == ExprKind::ArrayLit) {
			type.array_len = init.args.size();
		} else if (init.kind == ExprKind::StringLit) {
			type.array_len = init.str_val.size() + (init.is_cstring ? 1 : 0);
		} else {
			return false;
		}
		type.array_len_inferred = false;
		type.array_len_expr.reset();
	}

	if (type.base && type.base->kind == TypeRef::Array && init.kind == ExprKind::ArrayLit) {
		for (const auto &element : init.args) {
			if (element && !infer_array_lengths(*type.base, *element)) {
				return false;
			}
		}
	}
	return true;
}

// Typed arguments must match exactly.  Literal contextual typing is handled
// separately, before this predicate is reached; this function must never
// perform a value conversion. The sole implicit typed conversion adds
// read-only/volatile qualifiers at the immediate pointee level.
static inline bool arg_type_compat(StType *param, StType *arg) {
	if (eq_type(param, arg)) {
		return true;
	}
	StType *dst = underlying_type(param);
	StType *src = underlying_type(arg);
	if (!dst || !src) {
		return false;
	}
	if (dst->kind == TypeKind::Pointer && src->kind == TypeKind::Pointer) {
		return (!dst->is_mutable || src->is_mutable) && (!src->is_volatile || dst->is_volatile) &&
			   eq_type(dst->base, src->base);
	}
	if (dst->kind == TypeKind::Slice && src->kind == TypeKind::Slice) {
		return (!dst->is_mutable || src->is_mutable) && (!src->is_volatile || dst->is_volatile) &&
			   eq_type(dst->base, src->base);
	}
	if (dst->kind == TypeKind::Slice && src->kind == TypeKind::Pointer && src->base &&
		src->base->kind == TypeKind::Array) {
		return (!dst->is_mutable || src->is_mutable) && (!src->is_volatile || dst->is_volatile) &&
			   eq_type(dst->base, src->base->base);
	}
	return false;
}

static inline bool supported_va_type(StType *t) {
	t = underlying_type(t);
	if (!t) {
		return false;
	}
	if (t->kind == TypeKind::Enum) {
		t = underlying_type(t->base);
	}
	return t &&
		   (t->kind == TypeKind::Bool || t->kind == TypeKind::UInt || t->kind == TypeKind::SInt ||
			t->kind == TypeKind::Float || t->kind == TypeKind::Pointer ||
			t->kind == TypeKind::Slice);
}

static inline std::string type_name_for_diag(StType *t);

static inline std::string type_ref_name_for_diag(const TypeRef &tr) {
	switch (tr.kind) {
	case TypeRef::Named:
		return tr.name;
	case TypeRef::SelfType:
		return "Self";
	case TypeRef::Pointer: {
		std::string s = "*";
		if (tr.is_mutable) {
			s += "mut ";
		}
		if (tr.is_volatile) {
			s += "volatile ";
		}
		s += tr.base ? type_ref_name_for_diag(*tr.base) : "void";
		return s;
	}
	case TypeRef::Array: {
		std::string s = "[" + (tr.array_len_inferred ? std::string("_") : std::to_string(tr.array_len)) + "]";
		s += tr.base ? type_ref_name_for_diag(*tr.base) : "void";
		return s;
	}
	case TypeRef::Slice: {
		std::string s = "[]";
		if (tr.is_mutable) {
			s += "mut ";
		}
		if (tr.is_volatile) {
			s += "volatile ";
		}
		s += tr.base ? type_ref_name_for_diag(*tr.base) : "void";
		return s;
	}
	case TypeRef::Function: {
		std::string s = "fn(";
		for (size_t i = 0; i < tr.fn_params.size(); ++i) {
			if (i > 0) {
				s += ", ";
			}
			if (tr.fn_params[i]) {
				s += type_ref_name_for_diag(*tr.fn_params[i]);
			}
		}
		if (tr.fn_variadic) {
			if (!tr.fn_params.empty()) {
				s += ", ";
			}
			s += "...";
		}
		s += ")";
		if (tr.fn_ret) {
			s += " -> " + type_ref_name_for_diag(*tr.fn_ret);
		}
		return s;
	}
	case TypeRef::TypeOf:
		return "@of.type(...)";
	}
	return "<type>";
}

static inline std::string type_name_for_diag(StType *t) {
	t = underlying_type(t);
	if (!t) {
		return "<null>";
	}
	// Report an enum by its backing integer type
	if (t->kind == TypeKind::Enum) {
		return type_name_for_diag(underlying_type(t->base));
	}
	if (t->kind == TypeKind::Pointer) {
		std::string result = "*";
		if (t->is_mutable) {
			result += "mut ";
		}
		if (t->is_volatile) {
			result += "volatile ";
		}
		result += t->base ? type_name_for_diag(t->base) : "void";
		return result;
	}
	if (t->kind == TypeKind::Array) {
		return "[" + std::to_string(t->array_len) + "]" + (t->base ? type_name_for_diag(t->base) : "void");
	}
	if (t->kind == TypeKind::Slice) {
		std::string result = "[]";
		if (t->is_mutable) {
			result += "mut ";
		}
		if (t->is_volatile) {
			result += "volatile ";
		}
		result += t->base ? type_name_for_diag(t->base) : "void";
		return result;
	}
	if (t->kind == TypeKind::Function) {
		std::string s = "fn(";
		for (size_t i = 0; i < t->fn_params.size(); ++i) {
			if (i > 0) {
				s += ", ";
			}
			s += type_name_for_diag(t->fn_params[i]);
		}
		if (t->fn_variadic) {
			if (!t->fn_params.empty()) {
				s += ", ";
			}
			s += "...";
		}
		s += ") -> ";
		s += t->fn_ret ? type_name_for_diag(t->fn_ret) : "void";
		return s;
	}
	// Strip the mangling of named types (struct/enum/type binding) so
	// diagnostics read `Foo`, not a mangled namespace path.
	if (t->generic_origin && !t->generic_arg_is_const.empty()) {
		std::string base_name = t->generic_origin->mangled.empty()
									? t->generic_origin->simple
									: t->generic_origin->mangled;
		if (is_mangled(base_name)) {
			auto segs = demangle(base_name);
			if (!segs.empty()) {
				base_name = (segs.size() == 2 && segs[0] == "main") ? segs.back() : join_path(segs);
			}
		}
		std::string s = base_name + "<";
		for (size_t i = 0; i < t->generic_arg_is_const.size(); ++i) {
			if (i > 0) {
				s += ", ";
			}
			if (t->generic_arg_is_const[i]) {
				s += std::to_string(t->generic_const_args[i]);
			} else {
				s += type_name_for_diag(t->generic_type_args[i]);
			}
		}
		s += ">";
		return s;
	}
	std::string n = t->name;
	if (is_mangled(n)) {
		auto segs = demangle(n);
		if (!segs.empty()) {
			if (segs.size() == 2 && segs[0] == "main") {
				return segs.back();
			}
			return join_path(segs);
		}
	}
	return n;
}

static inline std::string fn_signature_for_diag(const FnDecl &fn) {
	std::string result = fn.name + "(";
	for (size_t i = 0; i < fn.params.size(); ++i) {
		if (i > 0) {
			result += ", ";
		}
		if (fn.params[i].resolved) {
			result += type_name_for_diag(fn.params[i].resolved);
		} else if (fn.params[i].ty) {
			result += type_ref_name_for_diag(*fn.params[i].ty);
		} else {
			result += "<unknown>";
		}
	}
	if (fn.is_variadic) {
		if (!fn.params.empty()) {
			result += ", ";
		}
		result += "...";
	}
	result += ")";
	return result;
}

// True if the expression tree contains a layout intrinsic anywhere. `@of.size(T)`
// folds to a placeholder 0 in eval_const_int (the real size is only known at
// codegen), so we must not use it as a known constant for value-fit: a folded
// result of 0 would spuriously "fit" every narrow slot. We treat any
// expression that mentions a layout intrinsic as Unknown instead.
static inline bool contains_sizeof(const Expr &e) {
	if (e.kind == ExprKind::Sizeof || e.kind == ExprKind::Alignof || e.kind == ExprKind::Bitsof ||
		e.kind == ExprKind::Offsetof) {
		return true;
	}
	if (e.lhs && contains_sizeof(*e.lhs)) {
		return true;
	}
	if (e.rhs && contains_sizeof(*e.rhs)) {
		return true;
	}
	if (e.index_low && contains_sizeof(*e.index_low)) {
		return true;
	}
	if (e.condition && contains_sizeof(*e.condition)) {
		return true;
	}
	if (e.then_expr && contains_sizeof(*e.then_expr)) {
		return true;
	}
	if (e.else_expr && contains_sizeof(*e.else_expr)) {
		return true;
	}
	for (auto &a : e.args) {
		if (contains_sizeof(*a)) {
			return true;
		}
	}
	return false;
}

// Does `v` (unsigned 64-bit) fit an unsigned integer slot of `width` bits?
// width >= 64 means every uint64_t fits. Non-positive widths are invalid
// internal fallbacks and are treated conservatively as full-width.
static inline bool fits_uint_width(uint64_t v, int width) {
	if (width <= 0 || width >= 64) {
		return true;
	}
	return v <= ((uint64_t{1} << width) - 1);
}

// Does `sv` fit a signed integer slot of `width` bits (two's complement)?
// width >= 64 means any int64_t fits. Non-positive widths are invalid
// internal fallbacks and are treated conservatively as full-width.
static inline bool fits_sint_width(int64_t sv, int width) {
	if (width <= 0 || width >= 64) {
		return true;
	}
	int64_t hi = (int64_t{1} << (width - 1)) - 1;
	int64_t lo = -hi - 1;
	return sv >= lo && sv <= hi;
}

static inline bool enum_value_fits(StType *backing, uint64_t bits, const Expr *source = nullptr) {
	backing = underlying_type(backing);
	if (!backing) {
		return false;
	}
	if (backing->kind == TypeKind::SInt) {
		if (source && (source->kind == ExprKind::IntLit || source->kind == ExprKind::CharLit) &&
			source->int_val > static_cast<uint64_t>(INT64_MAX)) {
			return false;
		}
		return fits_sint_width(static_cast<int64_t>(bits), backing->width);
	}
	if (backing->kind == TypeKind::UInt) {
		if (source && (source->kind == ExprKind::IntLit || source->kind == ExprKind::CharLit)) {
			return fits_uint_width(source->int_val, backing->width);
		}
		return fits_uint_width(bits, backing->width);
	}
	return false;
}

static inline int64_t signed_bits(uint64_t value, int width);

// Check that a known integer value (as raw 64-bit bits) is representable in
// the target integer/bool/enum-backed slot described by `d`. `d` must be a
// UInt, SInt, Bool, or Enum after underlying_type. Returns Fit/NoFit.
static inline FitResult int_value_fits(StType *d, uint64_t vbits, bool source_is_negative = false) {
	if (!d) {
		return FitResult::Unknown;
	}
	switch (d->kind) {
	case TypeKind::Bool:
		return (vbits == 0 || vbits == 1) ? FitResult::Fit : FitResult::NoFit;
	case TypeKind::UInt:
		// `vbits` preserves the two's-complement representation of a signed
		// constant.  Do not mistake -9 (0xffff...fff7) for a valid u64
		// literal merely because every bit pattern fits a 64-bit unsigned
		// slot.  Apart from preventing an implicit signed-to-unsigned
		// conversion, this is essential for overload selection: `sqrt(-9)`
		// must not select `sqrt(u64)` over `sqrt(f64)`.
		if (source_is_negative) {
			return FitResult::NoFit;
		}
		return fits_uint_width(vbits, d->width) ? FitResult::Fit : FitResult::NoFit;
	case TypeKind::SInt: {
		// Every int64_t and every u64 value is representable by a signed
		// destination wider than 64 bits (65 bits already spans the full u64
		// range). The fit evaluator only stores up to 64 value bits.
		if (d->width > 64) {
			return FitResult::Fit;
		}
		if (!source_is_negative && vbits > static_cast<uint64_t>(INT64_MAX)) {
			return FitResult::NoFit;
		}
		int64_t sv = signed_bits(vbits, 64);
		return fits_sint_width(sv, d->width) ? FitResult::Fit : FitResult::NoFit;
	}
	case TypeKind::Enum:
		return int_value_fits(underlying_type(d->base), vbits, source_is_negative);
	default:
		return FitResult::Unknown;
	}
}

// Finite-range upper bound of a float slot. f16's largest finite is 65504;
// f32's is FLT_MAX (~3.402823466e38); f64's is DBL_MAX. NaN/inf literals are
// not produced by the lexer, so we treat any finite double as f64-exact.
[[maybe_unused]] static bool double_fits_float_width(double v, int width) {
	if (!std::isfinite(v)) {
		return false;
	}
	switch (width) {
	case 16: {
		// Largest finite f16 = 65504. Overflow out of finite range is NoFit.
		if (v > 65504.0 || v < -65504.0) {
			return false;
		}
		// Nearest-even f16 round-trip via a hand-rolled converter.
		// f16 = s (1) | exp (5, bias 15) | mantissa (10). Decompose v.
		if (v == 0.0) {
			return true; // +0 and -0 both exact
		}
		double a = v < 0.0 ? -v : v;
		int	   e;
		double m = std::frexp(a, &e); // a = m * 2^e, m in [0.5, 1)
		// Convert to f16: value = m * 2^e. f16 exponent bias 15, so store exp = e-1
		// (since mantissa is in [1,2) for f16). Reshape m into [1,2):
		m *= 2.0;
		--e; // now a = m * 2^e, m in [1,2), e is unbiased binary exponent.
		if (e > 15) {
			return false; // overflow (kept here for safety even with the 65504 gate)
		}
		if (e < -24) {
			// Sub-normals / underflow: not exactly representable as normal f16.
			return false;
		}
		// Round mantissa to 10 fractional bits (nearest-even): scale up, round,
		// scale down.
		double scaled = std::ldexp(m - 1.0, 10); // 10-bit fractional part in [0,1)
		double r	  = std::floor(scaled + 0.5);
		// Round-half-to-even: if scaled was exactly .5 of an even integer half, go
		// even.
		if (scaled - std::floor(scaled) == 0.5 && (static_cast<int64_t>(r) & 1)) {
			r -= 1.0;
		}
		double mfrac		 = std::ldexp(r, -10) + 1.0; // reconstructed mantissa
		double reconstructed = mfrac * std::ldexp(1.0, e);
		return reconstructed == a;
	}
	case 32: {
		// Exact iff the nearest f32 round-trips to the original double.
		float f = static_cast<float>(v);
		return static_cast<double>(f) == v;
	}
	case 64:
	default:
		// f64 from an f64 literal is always self-exact (and finite, checked above).
		return true;
	}
}

// 128-bit bit helpers for the decimal-exact float-literal check (the lexer's
// `digits.digits` form can carry up to ~38 significant digits before the value
// saturates an f64, and we want to reason about the exact rational value for
// as wide an input as practical).

// Exact-representability of a *decimal* float literal in a binary IEEE-754
// slot, judged on the real value the source text denotes (NOT on the lexer's
// nearest-f64 rounding). This is stella's strict float policy: a literal like
// `0.1` (whose real value is 1/10, not a dyadic rational) is NoFit for *every*
// float width -- f16, f32, AND f64 -- and must be written `0.1~` to
// opt into the lossy conversion. Only literals that already name a dyadic
// rational within the slot's finite range and precision pass without `~`.
//
// `text` is the raw lexer text of a FloatLit (form: `digits.digits`, optional `~` suffix, no sign,
// no exponent, no hex). Returns Fit/NoFit, or Unknown if the mantissa cannot
// be parsed exactly (e.g. > ~38 digits overflowing __int128).
static inline FitResult float_literal_fits(const std::string &text, int width) {
	const llvm::fltSemantics *sema = nullptr;
	if (width == 16) {
		sema = &llvm::APFloat::IEEEhalf();
	} else if (width == 32) {
		sema = &llvm::APFloat::IEEEsingle();
	} else if (width == 64) {
		sema = &llvm::APFloat::IEEEdouble();
	} else {
		return FitResult::Unknown;
	}

	llvm::APFloat f(*sema);
	std::string	  normalized  = normalize_numeric_literal(text);
	bool		  is_lossy	  = false;
	size_t		  tilde_pos   = normalized.find('~');
	if (tilde_pos != std::string::npos) {
		is_lossy = true;
		normalized.erase(tilde_pos, 1);
	}
	bool		  hex_literal = normalized.size() > 2 && normalized[0] == '0' &&
								(normalized[1] == 'x' || normalized[1] == 'X');
	bool has_float_suffix = !normalized.empty() &&
							(normalized.back() == 'f' || normalized.back() == 'F') &&
							(!hex_literal || normalized.find_first_of("pP") != std::string::npos);
	if (has_float_suffix) {
		normalized.pop_back();
	}
	auto status = f.convertFromString(normalized, llvm::APFloat::rmNearestTiesToEven);
	if (!status) {
		llvm::consumeError(status.takeError());
		return FitResult::Unknown;
	}
	auto st = status.get();
	if (st & llvm::APFloat::opOverflow) {
		return FitResult::NoFit;
	}
	if (st & llvm::APFloat::opUnderflow) {
		return FitResult::NoFit;
	}
	// A decimal literal denotes its exact source value, not the nearest
	// representable value.  Therefore an inexact conversion is a type error
	// unless explicit agreement for lossy rounding (trailing `~`, e.g. `0.1~`)
	// is provided: for example, 0.1 is 1/10 and is not representable by any binary IEEE-754
	// float.  `0.1~` is the explicit agreement to accept lossy rounding.
	if (!is_lossy && (st & llvm::APFloat::opInexact)) {
		return FitResult::NoFit;
	}
	return FitResult::Fit;
}

// Recursively check that every FloatLit leaf of `e` is exactly representable
// in a float slot of `width` bits (or has explicit `~` agreement). Returns:
//   NoFit   - at least one FloatLit leaf is non-dyadic without `~` for this width.
//   Unknown - the expression contains NO FloatLit leaf OR a leaf's text
//             cannot be parsed exactly.
//   Fit     - at least one FloatLit leaf is present AND all are representable.
static inline FitResult float_leaves_fit(const Expr &e, int width) {
	if (e.kind == ExprKind::Cast) {
		return e.lhs ? float_leaves_fit(*e.lhs, width) : FitResult::Unknown;
	}
	if (e.kind == ExprKind::FloatLit) {
		return float_literal_fits(e.str_val, width);
	}
	bool	  saw_leaf = false;
	FitResult worst	   = FitResult::Fit;
	if (e.lhs) {
		FitResult l = float_leaves_fit(*e.lhs, width);
		if (l == FitResult::NoFit) {
			return FitResult::NoFit;
		}
		if (l == FitResult::Unknown) {
			worst = FitResult::Unknown;
		} else {
			saw_leaf = true;
		}
	}
	if (e.rhs) {
		FitResult l = float_leaves_fit(*e.rhs, width);
		if (l == FitResult::NoFit) {
			return FitResult::NoFit;
		}
		if (l == FitResult::Unknown) {
			worst = FitResult::Unknown;
		} else {
			saw_leaf = true;
		}
	}
	for (const Expr *child : {e.condition.get(), e.then_expr.get(), e.else_expr.get()}) {
		if (!child) {
			continue;
		}
		FitResult l = float_leaves_fit(*child, width);
		if (l == FitResult::NoFit) {
			return FitResult::NoFit;
		}
		if (l == FitResult::Unknown) {
			worst = FitResult::Unknown;
		} else {
			saw_leaf = true;
		}
	}
	for (auto &a : e.args) {
		FitResult l = float_leaves_fit(*a, width);
		if (l == FitResult::NoFit) {
			return FitResult::NoFit;
		}
		if (l == FitResult::Unknown) {
			worst = FitResult::Unknown;
		} else {
			saw_leaf = true;
		}
	}
	return saw_leaf ? worst : FitResult::Unknown;
}

struct StaticValue {
	enum Kind { Bool, Int, Float } kind = Int;
	bool	b							= false;
	int64_t i							= 0;
	double	f							= 0;
};

using StaticConsts = std::unordered_map<std::string, const Expr *>;

static inline bool is_signed_integer_type(StType *t) {
	t = underlying_type(t);
	if (!t) {
		return false;
	}
	if (t->kind == TypeKind::SInt) {
		return true;
	}
	return t->kind == TypeKind::Enum && is_signed_integer_type(t->base);
}

static inline bool checked_add(int64_t lhs, int64_t rhs, int64_t &out) {
	if ((rhs > 0 && lhs > INT64_MAX - rhs) || (rhs < 0 && lhs < INT64_MIN - rhs)) {
		return false;
	}
	out = lhs + rhs;
	return true;
}

static inline bool checked_sub(int64_t lhs, int64_t rhs, int64_t &out) {
	if ((rhs > 0 && lhs < INT64_MIN + rhs) || (rhs < 0 && lhs > INT64_MAX + rhs)) {
		return false;
	}
	out = lhs - rhs;
	return true;
}

static inline bool checked_mul(int64_t lhs, int64_t rhs, int64_t &out) {
	if (lhs == 0 || rhs == 0) {
		out = 0;
		return true;
	}
	if (lhs == -1 && rhs == INT64_MIN) {
		return false;
	}
	if (rhs == -1 && lhs == INT64_MIN) {
		return false;
	}
	if (lhs > 0) {
		if (rhs > 0 && lhs > INT64_MAX / rhs) {
			return false;
		}
		if (rhs < 0 && rhs < INT64_MIN / lhs) {
			return false;
		}
	} else {
		if (rhs > 0 && lhs < INT64_MIN / rhs) {
			return false;
		}
		if (rhs < 0 && lhs < INT64_MAX / rhs) {
			return false;
		}
	}
	out = lhs * rhs;
	return true;
}

static inline bool checked_div(int64_t lhs, int64_t rhs, int64_t &out) {
	if (rhs == 0 || (lhs == INT64_MIN && rhs == -1)) {
		return false;
	}
	out = lhs / rhs;
	return true;
}

static inline bool checked_mod(int64_t lhs, int64_t rhs, int64_t &out) {
	if (rhs == 0 || (lhs == INT64_MIN && rhs == -1)) {
		return false;
	}
	out = lhs % rhs;
	return true;
}

static inline bool valid_shift_count(uint64_t count, int width) {
	return width > 0 && width <= 64 && count < static_cast<uint64_t>(width);
}

static inline int64_t arithmetic_shift_right(int64_t value, unsigned count) {
	if (count == 0) {
		return value;
	}
	uint64_t bits = static_cast<uint64_t>(value) >> count;
	if (value < 0) {
		bits |= ~uint64_t{0} << (64 - count);
	}
	return signed_bits(bits, 64);
}

static inline uint64_t width_mask(int width) {
	if (width <= 0 || width >= 64) {
		return UINT64_MAX;
	}
	return (uint64_t{1} << width) - 1;
}

static inline uint64_t normalize_int_bits(uint64_t value, int width) {
	return value & width_mask(width);
}

static inline int64_t signed_bits(uint64_t value, int width) {
	value = normalize_int_bits(value, width);
	if (width > 0 && width < 64 && (value & (uint64_t{1} << (width - 1)))) {
		value |= ~width_mask(width);
	}
	if (value <= static_cast<uint64_t>(INT64_MAX)) {
		return static_cast<int64_t>(value);
	}
	// Decode the two's-complement value without an out-of-range unsigned-to-
	// signed conversion, whose result is implementation-defined before C++20.
	return -1 - static_cast<int64_t>(~value);
}

// Enum values are stored as int64 bit patterns, which lets u64-backed enums
// retain values above INT64_MAX. Advance unsigned 64-bit values as raw bits so
// the valid successor of INT64_MAX is INT64_MIN's bit pattern; reject only
// UINT64_MAX, where incrementing would wrap the backing value to zero.
static inline bool next_enum_value(StType *backing, uint64_t bits, uint64_t &next) {
	backing = underlying_type(backing);
	if (!backing) {
		return false;
	}
	if (backing->kind == TypeKind::UInt) {
		if (backing->width >= 64) {
			if (bits == UINT64_MAX) return false;
			next = bits + 1;
			return true;
		}
		uint64_t max_u = (uint64_t{1} << backing->width) - 1;
		if (bits >= max_u) return false;
		next = bits + 1;
		return true;
	}
	if (backing->kind == TypeKind::SInt) {
		int64_t sv = static_cast<int64_t>(bits);
		int64_t max_s = (backing->width >= 64)
							? INT64_MAX
							: ((int64_t{1} << (backing->width - 1)) - 1);
		if (sv >= max_s) return false;
		next = static_cast<uint64_t>(sv + 1);
		return true;
	}
	return false;
}

static inline std::optional<uint64_t> valid_alignment_value(const Attribute &attribute) {
	if (attribute.args.size() != 1 || attribute.arg_is_string.size() != 1 ||
		attribute.arg_is_string[0]) {
		return std::nullopt;
	}
	ParsedInt parsed = parse_int_literal(attribute.args[0]);
	if (!parsed.ok || parsed.overflow || !is_power_of_two(parsed.value)) {
		return std::nullopt;
	}
	return parsed.value;
}

static inline std::optional<StaticValue>
eval_static_expr(const Expr &e, const std::unordered_map<std::string, bool> &flags,
				 const StaticConsts &consts, std::unordered_set<std::string> &active) {
	switch (e.kind) {
	case ExprKind::DefineRef: {
		auto it = flags.find(e.str_val);
		return StaticValue{StaticValue::Bool, it != flags.end() && it->second, 0, 0};
	}
	case ExprKind::BoolLit:
		return StaticValue{StaticValue::Bool, e.const_bool_val, 0, 0};
	case ExprKind::IntLit:
	case ExprKind::CharLit:
		return StaticValue{StaticValue::Int, false, signed_bits(e.int_val, 64), 0};
	case ExprKind::FloatLit:
		return StaticValue{StaticValue::Float, false, 0, e.flt_val};
	case ExprKind::Ident: {
		if (e.path.size() != 1) {
			return std::nullopt;
		}
		auto it = consts.find(e.path[0]);
		if (it == consts.end() || !it->second || !active.insert(e.path[0]).second) {
			return std::nullopt;
		}
		auto v = eval_static_expr(*it->second, flags, consts, active);
		active.erase(e.path[0]);
		return v;
	}
	case ExprKind::Unary: {
		if (!e.lhs) {
			return std::nullopt;
		}
		std::unordered_set<std::string> nested = active;
		auto							v	   = eval_static_expr(*e.lhs, flags, consts, nested);
		if (!v) {
			return std::nullopt;
		}
		if (e.str_val == "!" && v->kind == StaticValue::Bool) {
			return StaticValue{StaticValue::Bool, !v->b, 0, 0};
		}
		if (e.str_val == "-" && v->kind == StaticValue::Int) {
			int64_t result = 0;
			if (!checked_sub(0, v->i, result)) {
				return std::nullopt;
			}
			return StaticValue{StaticValue::Int, false, result, 0};
		}
		if (e.str_val == "~" && v->kind == StaticValue::Int) {
			return StaticValue{StaticValue::Int, false, ~v->i, 0};
		}
		return std::nullopt;
	}
	case ExprKind::Cast:
		if (!e.lhs) {
			return std::nullopt;
		}
		return eval_static_expr(*e.lhs, flags, consts, active);
	case ExprKind::Conditional: {
		if (!e.condition || !e.then_expr || !e.else_expr) {
			return std::nullopt;
		}
		auto c = eval_static_expr(*e.condition, flags, consts, active);
		if (!c || c->kind != StaticValue::Bool) {
			return std::nullopt;
		}
		return eval_static_expr(c->b ? *e.then_expr : *e.else_expr, flags, consts, active);
	}
	case ExprKind::Binary: {
		if (!e.lhs || !e.rhs) {
			return std::nullopt;
		}
		auto l = eval_static_expr(*e.lhs, flags, consts, active);
		auto r = eval_static_expr(*e.rhs, flags, consts, active);
		if (!l || !r) {
			return std::nullopt;
		}
		const auto &op = e.str_val;
		if ((op == "&&" || op == "||") && l->kind == StaticValue::Bool &&
			r->kind == StaticValue::Bool) {
			return StaticValue{StaticValue::Bool, op == "&&" ? (l->b && r->b) : (l->b || r->b), 0,
							   0};
		}
		if (l->kind == StaticValue::Bool && r->kind == StaticValue::Bool) {
			if (op == "==") {
				return StaticValue{StaticValue::Bool, l->b == r->b, 0, 0};
			}
			if (op == "!=") {
				return StaticValue{StaticValue::Bool, l->b != r->b, 0, 0};
			}
		}
		if (l->kind == StaticValue::Int && r->kind == StaticValue::Int) {
			int64_t result = 0;
			if (op == "+" && checked_add(l->i, r->i, result)) {
				return StaticValue{StaticValue::Int, false, result, 0};
			}
			if (op == "-" && checked_sub(l->i, r->i, result)) {
				return StaticValue{StaticValue::Int, false, result, 0};
			}
			if (op == "*" && checked_mul(l->i, r->i, result)) {
				return StaticValue{StaticValue::Int, false, result, 0};
			}
			if (op == "/" && checked_div(l->i, r->i, result)) {
				return StaticValue{StaticValue::Int, false, result, 0};
			}
			if (op == "%" && checked_mod(l->i, r->i, result)) {
				return StaticValue{StaticValue::Int, false, result, 0};
			}
			if (op == "<<" && r->i >= 0 && r->i < 64) {
				return StaticValue{StaticValue::Int, false,
								   signed_bits(static_cast<uint64_t>(l->i)
															<< static_cast<unsigned>(r->i),
												64),
								   0};
			}
			if (op == ">>" && r->i >= 0 && r->i < 64) {
				return StaticValue{StaticValue::Int, false,
								   arithmetic_shift_right(l->i, static_cast<unsigned>(r->i)), 0};
			}
			if (op == "&") {
				return StaticValue{StaticValue::Int, false, l->i & r->i, 0};
			}
			if (op == "|") {
				return StaticValue{StaticValue::Int, false, l->i | r->i, 0};
			}
			if (op == "^") {
				return StaticValue{StaticValue::Int, false, l->i ^ r->i, 0};
			}
			if (op == "==") {
				return StaticValue{StaticValue::Bool, l->i == r->i, 0, 0};
			}
			if (op == "!=") {
				return StaticValue{StaticValue::Bool, l->i != r->i, 0, 0};
			}
			if (op == "<") {
				return StaticValue{StaticValue::Bool, l->i < r->i, 0, 0};
			}
			if (op == "<=") {
				return StaticValue{StaticValue::Bool, l->i <= r->i, 0, 0};
			}
			if (op == ">") {
				return StaticValue{StaticValue::Bool, l->i > r->i, 0, 0};
			}
			if (op == ">=") {
				return StaticValue{StaticValue::Bool, l->i >= r->i, 0, 0};
			}
		}
		if (l->kind == StaticValue::Float && r->kind == StaticValue::Float) {
			if (op == "==") {
				return StaticValue{StaticValue::Bool, l->f == r->f, 0, 0};
			}
			if (op == "!=") {
				return StaticValue{StaticValue::Bool, l->f != r->f, 0, 0};
			}
			if (op == "<") {
				return StaticValue{StaticValue::Bool, l->f < r->f, 0, 0};
			}
			if (op == "<=") {
				return StaticValue{StaticValue::Bool, l->f <= r->f, 0, 0};
			}
			if (op == ">") {
				return StaticValue{StaticValue::Bool, l->f > r->f, 0, 0};
			}
			if (op == ">=") {
				return StaticValue{StaticValue::Bool, l->f >= r->f, 0, 0};
			}
		}
		return std::nullopt;
	}
	default:
		return std::nullopt;
	}
}

static inline const llvm::fltSemantics *float_semantics(StType *type) {
	type = underlying_type(type);
	if (!type || type->kind != TypeKind::Float) {
		return nullptr;
	}
	switch (type->width) {
	case 16:
		return &llvm::APFloat::IEEEhalf();
	case 32:
		return &llvm::APFloat::IEEEsingle();
	case 64:
		return &llvm::APFloat::IEEEdouble();
	default:
		return nullptr;
	}
}

static inline void fold_known_expr(Expr &e, int pointer_width) {
	if (e.kind == ExprKind::BoolLit || e.kind == ExprKind::DefineRef ||
		e.kind == ExprKind::IntLit || e.kind == ExprKind::CharLit || e.kind == ExprKind::FloatLit) {
		e.has_const_value = true;
		if (e.kind == ExprKind::BoolLit || e.kind == ExprKind::DefineRef) {
			e.is_const_bool = true;
		}
		return;
	}
	if (e.kind == ExprKind::Unary && e.lhs && e.lhs->has_const_value) {
		if (e.str_val == "!" && e.lhs->is_const_bool) {
			e.is_const_bool = e.has_const_value = true;
			e.const_bool_val					= !e.lhs->const_bool_val;
		} else if (e.str_val == "-" && e.lhs->const_value_is_float) {
			e.has_const_value	   = true;
			e.const_value_is_float = true;
			e.flt_val			   = -e.lhs->flt_val;
		} else if (e.str_val == "-" && !e.lhs->is_const_bool && !e.lhs->const_value_is_float) {
			int width = scalar_size_bits(e.ty ? e.ty : e.lhs->ty, pointer_width);
			e.has_const_value = true;
			if (width > 64) {
				llvm::APInt value(width, e.lhs->int_val);
				if (e.lhs->const_int_high_bits_ones) {
					value |= llvm::APInt::getHighBitsSet(width, width - 64);
				}
				value = -value;
				e.int_val = value.extractBitsAsZExtValue(64, 0);
				llvm::APInt high_bits = value.extractBits(width - 64, 64);
				if (high_bits.isAllOnes()) {
					e.const_int_high_bits_ones = true;
				} else if (high_bits.isZero()) {
					e.const_int_high_bits_ones = false;
				} else {
					e.has_const_value = false;
					return;
				}
			} else {
				e.int_val = normalize_int_bits(~e.lhs->int_val + 1, width);
				e.const_int_high_bits_ones = false;
			}
		} else if (e.str_val == "~" && !e.lhs->is_const_bool && !e.lhs->const_value_is_float) {
			int width = scalar_size_bits(e.ty ? e.ty : e.lhs->ty, pointer_width);
			e.has_const_value = true;
			if (width > 64) {
				llvm::APInt value(width, e.lhs->int_val);
				if (e.lhs->const_int_high_bits_ones) {
					value |= llvm::APInt::getHighBitsSet(width, width - 64);
				}
				value = ~value;
				e.int_val = value.extractBitsAsZExtValue(64, 0);
				llvm::APInt high_bits = value.extractBits(width - 64, 64);
				if (high_bits.isAllOnes()) {
					e.const_int_high_bits_ones = true;
				} else if (high_bits.isZero()) {
					e.const_int_high_bits_ones = false;
				} else {
					e.has_const_value = false;
					return;
				}
			} else {
				e.int_val = normalize_int_bits(~e.lhs->int_val, width);
				e.const_int_high_bits_ones = false;
			}
		}
		return;
	}
	if (e.kind == ExprKind::Cast && e.lhs && e.lhs->has_const_value) {
		StType *target_type = underlying_type(e.ty);
		if (target_type && target_type->kind == TypeKind::Bool) {
			bool value = false;
			if (e.lhs->is_const_bool) {
				value = e.lhs->const_bool_val;
			} else if (e.lhs->const_value_is_float) {
				// Match coerce(): unordered float comparison makes NaN truthy.
				value = e.lhs->flt_val != 0.0;
			} else {
				value = e.lhs->int_val != 0 || e.lhs->const_int_high_bits_ones;
			}
			e.has_const_value = true;
			e.is_const_bool = true;
			e.const_bool_val = value;
			e.const_value_is_float = false;
			e.const_int_high_bits_ones = false;
			e.int_val = value ? 1 : 0;
			e.flt_val = 0.0;
			return;
		}
		StType *source_type = underlying_type(e.lhs->ty);
		const bool source_integer = source_type &&
			(source_type->kind == TypeKind::UInt || source_type->kind == TypeKind::SInt ||
			 source_type->kind == TypeKind::Enum);
		const bool target_integer = target_type &&
			(target_type->kind == TypeKind::UInt || target_type->kind == TypeKind::SInt ||
			 target_type->kind == TypeKind::Enum);
		if (e.is_bitcast && source_type && source_type->kind == TypeKind::Float &&
			target_integer) {
			const llvm::fltSemantics *semantics = float_semantics(source_type);
			const int target_width = scalar_size_bits(target_type, pointer_width);
			if (semantics && target_width > 0 && target_width <= 64) {
				llvm::APFloat value(e.lhs->flt_val);
				bool loses_info = false;
				value.convert(*semantics, llvm::APFloat::rmNearestTiesToEven, &loses_info);
				const llvm::APInt raw_bits = value.bitcastToAPInt();
				uint64_t bits = raw_bits.getZExtValue();
				if (is_signed_integer_type(target_type) && target_width < 64) {
					bits = static_cast<uint64_t>(signed_bits(bits, target_width));
				}
				e.has_const_value = true;
				e.is_const_bool = false;
				e.const_bool_val = false;
				e.const_value_is_float = false;
				e.const_int_high_bits_ones = false;
				e.int_val = bits;
				e.flt_val = 0.0;
				return;
			}
		}
		if (e.is_bitcast && source_integer && target_type &&
			target_type->kind == TypeKind::Float) {
			const llvm::fltSemantics *semantics = float_semantics(target_type);
			const int source_width = scalar_size_bits(source_type, pointer_width);
			if (semantics && source_width > 0 && source_width <= 64) {
				llvm::APInt raw_bits(source_width, e.lhs->int_val);
				llvm::APFloat value(*semantics, raw_bits);
				e.has_const_value = true;
				e.is_const_bool = false;
				e.const_bool_val = false;
				e.const_value_is_float = true;
				e.const_int_high_bits_ones = false;
				e.int_val = 0;
				e.flt_val = value.convertToDouble();
				return;
			}
		}
		if (target_type && target_type->kind == TypeKind::Float) {
			const llvm::fltSemantics *semantics = float_semantics(target_type);
			if (semantics) {
				llvm::APFloat converted(*semantics);
				bool folded = false;
				if (e.lhs->const_value_is_float) {
					converted = llvm::APFloat(e.lhs->flt_val);
					bool loses_info = false;
					converted.convert(*semantics, llvm::APFloat::rmNearestTiesToEven,
									  &loses_info);
					folded = true;
				} else {
					StType *source_type = underlying_type(e.lhs->ty);
					int source_width =
						source_type && source_type->kind == TypeKind::Bool
							? 1
							: scalar_size_bits(source_type, pointer_width);
					if (source_width > 0 && source_width <= 64) {
						uint64_t bits =
							e.lhs->is_const_bool ? static_cast<uint64_t>(e.lhs->const_bool_val)
											 : e.lhs->int_val;
						llvm::APInt integer(source_width, bits);
						converted.convertFromAPInt(integer,
											   is_signed_integer_type(source_type),
											   llvm::APFloat::rmNearestTiesToEven);
						folded = true;
					}
				}
				if (folded) {
					e.has_const_value = true;
					e.is_const_bool = false;
					e.const_bool_val = false;
					e.const_value_is_float = true;
					e.const_int_high_bits_ones = false;
					e.int_val = 0;
					e.flt_val = converted.convertToDouble();
					return;
				}
			}
		}
		if (target_type && (target_type->kind == TypeKind::UInt ||
							target_type->kind == TypeKind::SInt ||
							target_type->kind == TypeKind::Enum)) {
			const int target_width = scalar_size_bits(target_type, pointer_width);
			if (target_width > 0) {
				uint64_t bits = 0;
				e.const_int_high_bits_ones = false;
				if (e.lhs->const_value_is_float) {
					if (target_width > 64) {
						return;
					}
					const bool is_signed = is_signed_integer_type(target_type);
					llvm::APSInt converted(target_width, !is_signed);
					llvm::APFloat source(e.lhs->flt_val);
					const auto status = source.convertToInteger(
						converted, llvm::APFloat::rmTowardZero, nullptr);
					if (status & llvm::APFloat::opInvalidOp) {
						e.has_const_value = false;
						e.is_const_bool = false;
						e.const_value_is_float = false;
						return;
					}
					bits = converted.getZExtValue();
				} else {
					StType *source_type = underlying_type(e.lhs->ty);
					const int source_width = source_type && source_type->kind == TypeKind::Bool
											 ? 1
											 : scalar_size_bits(source_type, pointer_width);
					if (source_width <= 0 || source_width > 64) {
						return;
					}
					bits = e.lhs->is_const_bool
							   ? static_cast<uint64_t>(e.lhs->const_bool_val)
							   : normalize_int_bits(e.lhs->int_val, source_width);
					const bool sign_extend = target_width > source_width &&
						is_signed_integer_type(source_type);
					if (sign_extend) {
						bits = static_cast<uint64_t>(signed_bits(bits, source_width));
					}
					if (target_width <= 64) {
						bits = normalize_int_bits(bits, target_width);
					}
					e.const_int_high_bits_ones = target_width > 64 && sign_extend &&
												 signed_bits(bits, 64) < 0;
				}
				if (is_signed_integer_type(target_type) && target_width < 64) {
					bits = static_cast<uint64_t>(signed_bits(bits, target_width));
				}
				e.has_const_value = true;
				e.is_const_bool = false;
				e.const_bool_val = false;
				e.const_value_is_float = false;
				e.int_val = bits;
				e.flt_val = 0.0;
				return;
			}
		}
		e.has_const_value	   = true;
		e.is_const_bool		   = e.lhs->is_const_bool;
		e.const_bool_val	   = e.lhs->const_bool_val;
		e.const_value_is_float = e.lhs->const_value_is_float;
		e.const_int_high_bits_ones = e.lhs->const_int_high_bits_ones;
		e.int_val			   = e.lhs->int_val;
		e.flt_val			   = e.lhs->flt_val;
		return;
	}
	if (e.kind == ExprKind::Conditional && e.condition && e.condition->is_const_bool) {
		const Expr *selected = e.condition->const_bool_val ? e.then_expr.get() : e.else_expr.get();
		if (selected && selected->has_const_value) {
			e.has_const_value	   = true;
			e.is_const_bool		   = selected->is_const_bool;
			e.const_bool_val	   = selected->const_bool_val;
			e.const_value_is_float = selected->const_value_is_float;
			e.const_int_high_bits_ones = selected->const_int_high_bits_ones;
			e.int_val			   = selected->int_val;
			e.flt_val			   = selected->flt_val;
		}
		return;
	}
	if (e.kind != ExprKind::Binary || !e.lhs || !e.rhs || !e.lhs->has_const_value ||
		!e.rhs->has_const_value) {
		return;
	}
	const auto &op = e.str_val;
	if ((op == "&&" || op == "||") && e.lhs->is_const_bool && e.rhs->is_const_bool) {
		e.is_const_bool = e.has_const_value = true;
		e.const_bool_val = op == "&&" ? (e.lhs->const_bool_val && e.rhs->const_bool_val)
									  : (e.lhs->const_bool_val || e.rhs->const_bool_val);
		return;
	}
	if (e.lhs->is_const_bool || e.rhs->is_const_bool) {
		return;
	}
	if (e.lhs->const_value_is_float || e.rhs->const_value_is_float) {
		if (!e.lhs->const_value_is_float || !e.rhs->const_value_is_float) {
			return;
		}
		const llvm::fltSemantics *semantics = float_semantics(e.lhs->ty);
		if (!semantics) {
			return;
		}
		llvm::APFloat lhs(e.lhs->flt_val);
		llvm::APFloat rhs(e.rhs->flt_val);
		bool loses_info = false;
		lhs.convert(*semantics, llvm::APFloat::rmNearestTiesToEven, &loses_info);
		rhs.convert(*semantics, llvm::APFloat::rmNearestTiesToEven, &loses_info);
		const double l = lhs.convertToDouble();
		const double r = rhs.convertToDouble();
		if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%") {
			if (op == "+") {
				lhs.add(rhs, llvm::APFloat::rmNearestTiesToEven);
			} else if (op == "-") {
				lhs.subtract(rhs, llvm::APFloat::rmNearestTiesToEven);
			} else if (op == "*") {
				lhs.multiply(rhs, llvm::APFloat::rmNearestTiesToEven);
			} else if (op == "/") {
				lhs.divide(rhs, llvm::APFloat::rmNearestTiesToEven);
			} else {
				lhs.mod(rhs);
			}
			e.has_const_value = true;
			e.is_const_bool = false;
			e.const_bool_val = false;
			e.const_value_is_float = true;
			e.int_val = 0;
			e.flt_val = lhs.convertToDouble();
			return;
		}
		if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=") {
			e.is_const_bool = e.has_const_value = true;
			if (op == "==") {
				e.const_bool_val = l == r;
			}
			if (op == "!=") {
				e.const_bool_val = l != r;
			}
			if (op == "<") {
				e.const_bool_val = l < r;
			}
			if (op == "<=") {
				e.const_bool_val = l <= r;
			}
			if (op == ">") {
				e.const_bool_val = l > r;
			}
			if (op == ">=") {
				e.const_bool_val = l >= r;
			}
		}
		return;
	}
	// Comparisons have bool result types; use the operand type for width and
	// signedness. Arithmetic expressions use the same type on both sides.
	StType *value_type = e.lhs->ty;
	int		width	   = scalar_size_bits(value_type, pointer_width);
	if (width <= 0 || width > 64) {
		return;
	}
	uint64_t lbits = normalize_int_bits(e.lhs->int_val, width);
	uint64_t rbits = normalize_int_bits(e.rhs->int_val, width);
	int64_t	 l = signed_bits(lbits, width), r = signed_bits(rbits, width);
	bool	 is_signed = is_signed_integer_type(value_type);
	const int64_t signed_min = width == 64
								 ? INT64_MIN
								 : -(int64_t{1} << (width - 1));
	const bool signed_division_overflow = is_signed && r == -1 && l == signed_min;
	if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=") {
		e.is_const_bool = e.has_const_value = true;
		if (op == "==") {
			e.const_bool_val = lbits == rbits;
		}
		if (op == "!=") {
			e.const_bool_val = lbits != rbits;
		}
		if (op == "<") {
			e.const_bool_val = is_signed ? l < r : lbits < rbits;
		}
		if (op == "<=") {
			e.const_bool_val = is_signed ? l <= r : lbits <= rbits;
		}
		if (op == ">") {
			e.const_bool_val = is_signed ? l > r : lbits > rbits;
		}
		if (op == ">=") {
			e.const_bool_val = is_signed ? l >= r : lbits >= rbits;
		}
		return;
	}
	uint64_t value	   = 0;
	bool	 supported = true;
	if (op == "+" || op == "+%") {
		value = lbits + rbits;
	} else if (op == "-" || op == "-%") {
		value = lbits - rbits;
	} else if (op == "+|" || op == "-|") {
		if (is_signed) {
			const llvm::APInt lhs(width, lbits), rhs(width, rbits);
			const llvm::APInt wide = op == "+|" ? lhs.sext(width + 1) + rhs.sext(width + 1)
										  : lhs.sext(width + 1) - rhs.sext(width + 1);
			const llvm::APInt minimum = llvm::APInt::getSignedMinValue(width).sext(width + 1);
			const llvm::APInt maximum = llvm::APInt::getSignedMaxValue(width).sext(width + 1);
			if (wide.slt(minimum)) value = llvm::APInt::getSignedMinValue(width).getZExtValue();
			else if (wide.sgt(maximum)) value = llvm::APInt::getSignedMaxValue(width).getZExtValue();
			else value = wide.trunc(width).getZExtValue();
		} else if (op == "+|") {
			value = (width == 64 && lbits > UINT64_MAX - rbits) ||
				(width < 64 && lbits + rbits > ((uint64_t{1} << width) - 1))
				? (width == 64 ? UINT64_MAX : ((uint64_t{1} << width) - 1))
				: lbits + rbits;
		} else {
			value = lbits < rbits ? 0 : lbits - rbits;
		}
	} else if (op == "*" || op == "*%") {
		value = lbits * rbits;
	} else if (op == "*|") {
		const llvm::APInt lhs(width, lbits), rhs(width, rbits);
		bool			  ov = false;
		if (is_signed) {
			const llvm::APInt prod = lhs.smul_ov(rhs, ov);
			if (ov) {
				value = (lhs.isNegative() != rhs.isNegative()
							 ? llvm::APInt::getSignedMinValue(width)
							 : llvm::APInt::getSignedMaxValue(width))
							.getZExtValue();
			} else {
				value = prod.getZExtValue();
			}
		} else {
			const llvm::APInt prod = lhs.umul_ov(rhs, ov);
			value = ov ? llvm::APInt::getMaxValue(width).getZExtValue() : prod.getZExtValue();
		}
	} else if (op == "/") {
		if (rbits == 0 || signed_division_overflow) {
			return;
		}
		value = is_signed ? static_cast<uint64_t>(l / r) : lbits / rbits;
	} else if (op == "%") {
		if (rbits == 0 || signed_division_overflow) {
			return;
		}
		value = is_signed ? static_cast<uint64_t>(l % r) : lbits % rbits;
	} else if (op == "<<") {
		if (!valid_shift_count(rbits, width)) {
			return;
		}
		value = lbits << static_cast<unsigned>(rbits);
	} else if (op == ">>") {
		if (!valid_shift_count(rbits, width)) {
			return;
		}
		value = is_signed
					? static_cast<uint64_t>(arithmetic_shift_right(l, static_cast<unsigned>(rbits)))
					: lbits >> static_cast<unsigned>(rbits);
	} else if (op == "&") {
		value = lbits & rbits;
	} else if (op == "|") {
		value = lbits | rbits;
	} else if (op == "^") {
		value = lbits ^ rbits;
	} else {
		supported = false;
	}
	if (supported) {
		e.has_const_value = true;
		e.int_val		  = normalize_int_bits(value, width);
	}
}

} // namespace

std::vector<std::string> split_scopes(const std::string &s);
Symbol *lookup_named_type(ModuleResolver &r, Sema &sema, Module &m, const std::string &single,
						  const std::string &full);
bool eval_generic_const_expr(const Expr &e, const std::unordered_map<std::string, int64_t> &values,
							 int64_t &out);

inline bool type_ref_contains_generic(const TypeRef &pattern, const FnDecl &fn) {
	if (pattern.kind == TypeRef::Named) {
		for (const auto &p : fn.generic_params) {
			if ((p.kind == GenericParam::Kind::Type || p.kind == GenericParam::Kind::Const) &&
				p.name == pattern.single) {
				return true;
			}
		}
		for (const auto &arg : pattern.generic_args) {
			if (arg.kind == GenericArg::Kind::Type && arg.type &&
				type_ref_contains_generic(*arg.type, fn)) {
				return true;
			}
			if (arg.kind == GenericArg::Kind::Value && arg.value &&
				arg.value->kind == ExprKind::Ident && arg.value->path.size() == 1) {
				for (const auto &p : fn.generic_params) {
					if (p.kind == GenericParam::Kind::Const && p.name == arg.value->path[0]) {
						return true;
					}
				}
			}
		}
		return false;
	}
	if (pattern.kind == TypeRef::Pointer || pattern.kind == TypeRef::Slice) {
		return pattern.base && type_ref_contains_generic(*pattern.base, fn);
	}
	if (pattern.kind == TypeRef::Array) {
		if (pattern.array_len_expr && pattern.array_len_expr->kind == ExprKind::Ident &&
			pattern.array_len_expr->path.size() == 1) {
			for (const auto &p : fn.generic_params) {
				if (p.kind == GenericParam::Kind::Const &&
					p.name == pattern.array_len_expr->path[0]) {
					return true;
				}
			}
		}
		return pattern.base && type_ref_contains_generic(*pattern.base, fn);
	}
	if (pattern.kind == TypeRef::Function) {
		for (const auto &param : pattern.fn_params) {
			if (param && type_ref_contains_generic(*param, fn)) {
				return true;
			}
		}
		return pattern.fn_ret && type_ref_contains_generic(*pattern.fn_ret, fn);
	}
	return false;
}

inline bool type_ref_contains_unbound_generic(const TypeRef &pattern, const FnDecl &fn) {
	if (pattern.kind == TypeRef::Named) {
		for (const auto &p : fn.generic_params) {
			if (p.kind == GenericParam::Kind::Type && p.name == pattern.single) {
				return !fn.generic_type_subst.count(p.name);
			}
			if (p.kind == GenericParam::Kind::Const && p.name == pattern.single) {
				return !fn.generic_const_subst.count(p.name) &&
					   !fn.generic_const_string_subst.count(p.name);
			}
		}
		for (const auto &arg : pattern.generic_args) {
			if (arg.kind == GenericArg::Kind::Type && arg.type &&
				type_ref_contains_unbound_generic(*arg.type, fn)) {
				return true;
			}
			if (arg.kind == GenericArg::Kind::Value && arg.value &&
				arg.value->kind == ExprKind::Ident && arg.value->path.size() == 1) {
				for (const auto &p : fn.generic_params) {
					if (p.kind == GenericParam::Kind::Const && p.name == arg.value->path[0] &&
						!fn.generic_const_subst.count(p.name) &&
						!fn.generic_const_string_subst.count(p.name)) {
						return true;
					}
				}
			}
		}
		return false;
	}
	if (pattern.kind == TypeRef::Pointer || pattern.kind == TypeRef::Slice) {
		return pattern.base && type_ref_contains_unbound_generic(*pattern.base, fn);
	}
	if (pattern.kind == TypeRef::Array) {
		if (pattern.array_len_expr && pattern.array_len_expr->kind == ExprKind::Ident &&
			pattern.array_len_expr->path.size() == 1) {
			for (const auto &p : fn.generic_params) {
				if (p.kind == GenericParam::Kind::Const &&
					p.name == pattern.array_len_expr->path[0] &&
					!fn.generic_const_subst.count(p.name)) {
					return true;
				}
			}
		}
		return pattern.base && type_ref_contains_unbound_generic(*pattern.base, fn);
	}
	if (pattern.kind == TypeRef::Function) {
		for (const auto &param : pattern.fn_params) {
			if (param && type_ref_contains_unbound_generic(*param, fn)) {
				return true;
			}
		}
		return pattern.fn_ret && type_ref_contains_unbound_generic(*pattern.fn_ret, fn);
	}
	return false;
}

inline void substitute_self_in_typeref(std::unique_ptr<TypeRef> &tr, const TypeRef &target_type);
inline void substitute_self_in_expr(std::unique_ptr<Expr> &ep, const TypeRef &target_type);
inline void substitute_self_in_stmt(Stmt &s, const TypeRef &target_type);

inline void substitute_self_in_typeref(std::unique_ptr<TypeRef> &tr, const TypeRef &target_type) {
	if (!tr) {
		return;
	}
	if (tr->kind == TypeRef::SelfType ||
		(tr->kind == TypeRef::Named && tr->single == "Self" && tr->generic_args.empty())) {
		SourceLoc loc = tr->loc;
		tr = clone_typeref(target_type);
		if (tr) {
			tr->loc = loc;
		}
		return;
	}
	substitute_self_in_typeref(tr->base, target_type);
	substitute_self_in_expr(tr->typeof_expr, target_type);
	substitute_self_in_expr(tr->array_len_expr, target_type);
	for (auto &p : tr->fn_params) {
		substitute_self_in_typeref(p, target_type);
	}
	substitute_self_in_typeref(tr->fn_ret, target_type);
	for (auto &ga : tr->generic_args) {
		if (ga.kind == GenericArg::Kind::Type) {
			substitute_self_in_typeref(ga.type, target_type);
		} else if (ga.kind == GenericArg::Kind::Value) {
			substitute_self_in_expr(ga.value, target_type);
		}
	}
}

inline void substitute_self_in_expr(std::unique_ptr<Expr> &ep, const TypeRef &target_type) {
	if (!ep) {
		return;
	}
	if (ep->kind == ExprKind::Member && ep->lhs && ep->lhs->kind == ExprKind::Ident &&
		ep->lhs->path.size() == 1 && ep->lhs->path[0] == "Self") {
		if (target_type.kind == TypeRef::Named) {
			const std::string &tname =
				target_type.name.empty() ? target_type.single : target_type.name;
			ep->lhs->path	 = split_dotted_path(tname);
			ep->lhs->str_val = tname;
			if (!target_type.generic_args.empty()) {
				ep->lhs->generic_args.clear();
				std::vector<GenericArg> combined;
				for (const auto &ga : target_type.generic_args) {
					ep->lhs->generic_args.push_back(clone_generic_arg(ga));
					combined.push_back(clone_generic_arg(ga));
				}
				for (auto &ga : ep->generic_args) {
					combined.push_back(std::move(ga));
				}
				ep->generic_args = std::move(combined);
			}
		}
	} else if (ep->kind == ExprKind::Ident && ep->path.size() == 1 && ep->path[0] == "Self") {
		ep->kind		= ExprKind::TypeValue;
		ep->layout_type = clone_typeref(target_type);
		if (ep->layout_type) {
			ep->layout_type->loc = ep->loc;
		}
		return;
	}
	substitute_self_in_expr(ep->lhs, target_type);
	substitute_self_in_expr(ep->rhs, target_type);
	substitute_self_in_expr(ep->index_low, target_type);
	substitute_self_in_expr(ep->condition, target_type);
	substitute_self_in_expr(ep->then_expr, target_type);
	substitute_self_in_expr(ep->else_expr, target_type);
	for (auto &a : ep->args) {
		substitute_self_in_expr(a, target_type);
	}
	for (auto &ga : ep->generic_args) {
		if (ga.kind == GenericArg::Kind::Type) {
			substitute_self_in_typeref(ga.type, target_type);
		} else if (ga.kind == GenericArg::Kind::Value) {
			substitute_self_in_expr(ga.value, target_type);
		}
	}
	substitute_self_in_typeref(ep->cast_type, target_type);
	substitute_self_in_typeref(ep->layout_type, target_type);
	substitute_self_in_typeref(ep->va_arg_ty, target_type);
	if (ep->block) {
		substitute_self_in_stmt(*ep->block, target_type);
	}
}

inline void substitute_self_in_stmt(Stmt &s, const TypeRef &target_type) {
	for (auto &child : s.body) {
		if (child) {
			substitute_self_in_stmt(*child, target_type);
		}
	}
	substitute_self_in_expr(s.expr, target_type);
	for (auto &v : s.vars) {
		substitute_self_in_typeref(v.ty, target_type);
		substitute_self_in_expr(v.init, target_type);
	}
	substitute_self_in_expr(s.cond, target_type);
	if (s.then_block) {
		substitute_self_in_stmt(*s.then_block, target_type);
	}
	if (s.else_block) {
		substitute_self_in_stmt(*s.else_block, target_type);
	}
	if (s.for_init) {
		substitute_self_in_stmt(*s.for_init, target_type);
	}
	substitute_self_in_expr(s.for_cond, target_type);
	substitute_self_in_expr(s.for_step, target_type);
	if (s.loop_body) {
		substitute_self_in_stmt(*s.loop_body, target_type);
	}
	if (s.defer_body) {
		substitute_self_in_stmt(*s.defer_body, target_type);
	}
	substitute_self_in_expr(s.scrutinee, target_type);
	for (auto &arm : s.arms) {
		for (auto &pat : arm.patterns) {
			substitute_self_in_expr(pat.expr, target_type);
		}
		if (arm.body) {
			substitute_self_in_stmt(*arm.body, target_type);
		}
	}
	for (auto &op : s.asm_outputs) {
		substitute_self_in_expr(op.operand, target_type);
	}
	for (auto &op : s.asm_inputs) {
		substitute_self_in_expr(op.operand, target_type);
	}
	substitute_self_in_expr(s.ret_val, target_type);
}

inline void substitute_self_in_fn(FnDecl &fn, const TypeRef &target_type) {
	for (auto &p : fn.params) {
		substitute_self_in_typeref(p.ty, target_type);
	}
	substitute_self_in_typeref(fn.ret, target_type);
	if (fn.body) {
		substitute_self_in_stmt(*fn.body, target_type);
	}
}

} // namespace stellac
