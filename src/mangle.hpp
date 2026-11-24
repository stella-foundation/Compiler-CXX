#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace stellac {

struct Module;
struct StType;
struct FnDecl;
struct GlobalDecl;

// Returns true if `sym` carries the Stella mangling prefix (`st$`).
bool is_mangled(std::string_view sym);

// Mangle a fully-qualified path as `st$seg.seg`.
std::string mangle(const std::vector<std::string> &segments);

// Demangle a `st$` symbol into its path segments, ignoring any trailing
// generic arguments (`<...>` / `!<...>`), parameter list (`(...)`), or
// private-module suffix (`#priv:...`). Returns empty on non-mangled input.
std::vector<std::string> demangle(std::string_view sym);

// Join a path with "." (no prefix). Used as the canonical module-key and
// diagnostic display-path separator.
std::string join_path(const std::vector<std::string> &segs);

// Split a "."-separated module key (e.g. `std.io`) into path segments.
std::vector<std::string> split_path(std::string_view dotted);

// Mangle a top-level symbol `name` declared in module `m`: appends `name` to
// `m.mod_path` then calls mangle(). Convenience for decl kinds without an
// owner-path (Struct/Union/TypeBinding/Enum/Trait).
std::string mangle_in(const Module &m, std::string_view name);

// Mangle type encoding into a readable, injective string.
std::string mangle_type(const StType *t);

struct GenericMangleArg {
	enum class Kind { Type, ConstInt, ConstString };
	Kind		  kind	  = Kind::Type;
	const StType *type	  = nullptr;
	int64_t		  int_val = 0;
	std::string	  str_val;
};

// Append a generic specialization suffix (`<...>` or `!<...>` for macros) to
// `base_mangled`.
std::string mangle_generic(std::string_view					   base_mangled,
						   const std::vector<GenericMangleArg> &args,
						   bool								   is_macro = false);

// Mangle a function path, appending `(<params>[,...])`.
std::string mangle_fn(const Module &m, const FnDecl &f);

// Linker/IR symbol for a global. Private globals receive a deterministic
// source-module scope (`#priv:<id>`) and use local linkage in their owning object.
std::string mangle_global(const Module &m, const GlobalDecl &g);
std::string private_global_scope_suffix(const Module &m);

} // namespace stellac
