#pragma once

#include "log.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace stellac {

struct Symbol;
struct Module;

enum class AbiKind {
	Stella,
	C,
};

enum class ReprKind {
	Stella,
	C,
};

enum class TypeKind {
	Void,
	Bool,
	UInt,  // u<N>: unsigned integer of N bits (arbitrary width); usize == -1
	SInt,  // i<N>: signed integer of N bits (arbitrary width); isize == -1
	Float, // f16/f32/f64
	Pointer,
	Array,
	Slice,
	Struct,
	Union,
	Function, // function ptr (we model via void* but keep for typechecking)
	Type,	  // compile-time type value (`type`)
	TypeBinding, // compile-time type binding, transparent
	Opaque,	  // forward-declared / unresolved type name
	Enum,	  // enum: distinct integer type with named constants
	VaList,	  // compiler-owned opaque variadic cursor
	Poison,	  // error sentinel type to suppress cascading diagnostics
	SelfType, // contextual trait/impl Self type
};

struct StType;
enum class EnumPayloadKind { Unit, Tuple, Struct };

struct EnumVariantInfo {
	EnumPayloadKind			 payload_kind = EnumPayloadKind::Unit;
	std::vector<StType *>	 field_types;
	std::vector<std::string> field_names;
};

struct StType {
	TypeKind				 kind;
	// For ints/floats: bit width. usize/isize are materialized at the target
	// pointer width by TypeContext.
	int						 width = 0;
	std::string				 name;			 // canonical spelling (u32, *i32, struct name, ...)
	StType					*base = nullptr; // pointer base / type binding target / array element type
	size_t					 array_len = 0;	 // for Array: element count
	std::vector<StType *>	 fields;		 // struct element types
	std::vector<std::string> field_names;
	std::vector<bool>		 field_is_pub;   // struct/union field visibility (parallel to field_names)
	bool					 is_mutable = false; // true if this pointer type is mutable (*mut T)
	bool	 is_volatile			  = false; // true if accesses through this pointer are volatile
	bool	 is_pub					  = false;
	bool	 is_opaque				  = false; // true for opaque struct declaration (struct Name;)
	ReprKind repr					  = ReprKind::Stella;
	uint64_t explicit_align			  = 0; // #[align(N)] on struct: storage alignment, 0 = natural
	std::optional<uint64_t> layout_size;
	std::optional<uint64_t> layout_align;
	SourceLoc layout_loc;
	// Enum: backing integer type in `base`, member names in `field_names`,
	//       member discriminant bit-patterns in `enum_values` (parallel to field_names).
	std::vector<uint64_t>		 enum_values;
	bool						 is_tagged_enum = false;
	std::vector<EnumVariantInfo> enum_variants;
	// Function signature (when kind == TypeKind::Function). A function VALUE
	// is modelled as a pointer at the LLVM (opaque-ptr) level: the `fn(...)`
	// type IS the value type; there is no separate Pointer<Function>.
	std::vector<StType *> fn_params;
	StType				 *fn_ret	  = nullptr;
	bool				  fn_variadic = false;
	AbiKind				  fn_abi	  = AbiKind::Stella;
	// Generic nominal instantiation metadata (for Struct/Union/Enum/TypeBinding).
	Symbol					*generic_origin = nullptr;
	Module					*decl_module	= nullptr;
	std::vector<std::string> scope_path;
	std::vector<bool>		 generic_arg_is_const;
	std::vector<StType *>	 generic_type_args;
	std::vector<int64_t>	 generic_const_args;
};

// Peel type-binding chains. Returns the first non-TypeBinding type (or nullptr).
StType		 *underlying_type(StType *t);
const StType *underlying_type(const StType *t);

inline bool is_poison(const StType *t) {
	if (!t) return false;
	const StType *u = underlying_type(t);
	return u && u->kind == TypeKind::Poison;
}

// Index of a named struct field, or SIZE_MAX if not found.
size_t		  field_index(const StType *st, std::string_view name);

// Owned, interned type pool. Keeps Type* stable across the compilation.
class TypeContext {
  public:
	explicit TypeContext(int pointer_width = 64);

	// Raw members point into pool_; copying/moving would leave the new
	// instance's raw pointers dangling into the (destroyed) source pool. The
	// TypeContext is always passed by reference (main.cpp constructs a single
	// instance), so disable both copy and move to surface misuse at compile time.
	TypeContext(const TypeContext &)			= delete;
	TypeContext &operator=(const TypeContext &) = delete;
	TypeContext(TypeContext &&)					= delete;
	TypeContext &operator=(TypeContext &&)		= delete;

	StType		*void_ty() const {
		return void_;
	}
	StType *bool_ty() const {
		return bool_;
	}
	StType *type_ty() const {
		return type_;
	}
	StType *va_list_ty() const {
		return va_list_;
	}
	StType *poison_ty() const {
		return poison_;
	}
	StType *self_ty() const {
		return self_;
	}

	// Integer/float scalar of the given bit width. Integers accept any
	// positive width and are interned per width on first request --
	// there is no fixed u8..u64 table. Floats are interned likewise;
	// the language surface only names f16/f32/f64, but TypeContext
	// itself is width-agnostic.
	StType *uint(int width);
	StType *sint(int width);
	StType *float_ty(int width);
	StType *usize_ty() const {
		return usize_;
	}
	StType *isize_ty() const {
		return isize_;
	}
	int pointer_width() const {
		return pointer_width_;
	}

	StType *pointer(StType *base, bool is_mutable = false, bool is_volatile = false);
	StType *array_of(StType *elem, size_t len);
	StType *slice(StType *elem, bool is_mutable = false, bool is_volatile = false);
	StType *struct_of(std::string name, std::vector<std::string> field_names,
					  std::vector<StType *> field_types, bool is_pub,
					  std::vector<bool> field_is_pub = {});
	StType *union_of(std::string name, std::vector<std::string> field_names,
					 std::vector<StType *> field_types, bool is_pub,
					 std::vector<bool> field_is_pub = {});

	StType *type_binding_of(std::string name, StType *target, bool is_pub);
	StType *enum_of(std::string name, StType *backing, std::vector<std::string> member_names,
					std::vector<uint64_t> member_values, bool is_pub,
					bool is_tagged = false, std::vector<EnumVariantInfo> variants = {});

	// Function type (value = pointer at the LLVM level). Interned by a
	// canonical signature key so eq_type can use pointer identity.
	StType *function_of(std::vector<StType *> params, StType *ret, bool variadic,
						AbiKind abi = AbiKind::Stella);

	// Look up struct/type binding by mangled name (already-created).
	StType *find_named(const std::string &name) const;

	bool	is_int_like(StType *t) const;
	bool	is_arith(StType *t) const;
	bool	is_pointer(StType *t) const;

  private:
	std::vector<std::unique_ptr<StType>>	  pool_;
	StType									 *void_;
	StType									 *bool_;
	StType									 *usize_;
	StType									 *isize_;
	StType									 *va_list_;
	StType									 *type_;
	StType									 *poison_;
	StType									 *self_;
	int										  pointer_width_ = 64;
	// Interned-by-width scalar caches: width -> StType. Arbitrary integer
	// widths (u<N>/i<N>) are created on demand; usize_/isize_ live in their
	// own slots above, never here.
	std::unordered_map<int, StType *>		  uints_;
	std::unordered_map<int, StType *>		  sints_;
	std::unordered_map<int, StType *>		  floats_;
	std::unordered_map<std::string, StType *> ptr_cache_;
	std::unordered_map<std::string, StType *> slice_cache_;
	std::unordered_map<std::string, StType *> named_;
	// array cache: key = "elemName[N]"
	std::unordered_map<std::string, StType *> arr_cache_;
	// function-type cache: key = "(p0,p1,...)->ret[...]"
	std::unordered_map<std::string, StType *> fn_cache_;
};

} // namespace stellac
