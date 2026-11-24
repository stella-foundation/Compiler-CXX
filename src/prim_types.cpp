#include "prim_types.hpp"
#include <llvm/IR/DerivedTypes.h>
#include <cctype>
#include <climits>

namespace stellac {

namespace {

// Parse an arbitrary-width integer type name. Returns the bit width (>0) for
// "u<N>" / "i<N>"; returns -1 for the pointer-sized "usize" / "isize";
// returns 0 when the spelling is not an integer primitive. "u" / "i" with
// no digits, a leading zero, or non-digit suffix are rejected (0) so they
// remain available as ordinary identifiers.
int parse_int_type_width(const std::string &n) {
	if (n == "usize" || n == "isize") {
		return -1;
	}
	const char *p = nullptr;
	if (n.compare(0, 1, "u") == 0) {
		p = n.c_str() + 1;
	} else if (n.compare(0, 1, "i") == 0) {
		p = n.c_str() + 1;
	} else {
		return 0;
	}
	if (*p == '\0' || *p == '0') {
		return 0;
	}
	unsigned long w = 0;
	for (const char *q = p; *q; ++q) {
		if (!std::isdigit((unsigned char)*q)) {
			return 0;
		}
		const unsigned long digit = static_cast<unsigned long>(*q - '0');
		if (w > (static_cast<unsigned long>(INT_MAX) - digit) / 10) {
			return 0;
		}
		w = w * 10 + digit;
	}
	if (w > llvm::IntegerType::MAX_INT_BITS) {
		// Keep unsupported widths out of the type graph: Codegen maps these
		// types directly to LLVM IntegerType, which asserts above this limit.
		return 0;
	}
	return static_cast<int>(w);
}

} // namespace

StType *prim_from_name(TypeContext &tx, const std::string &n) {
	if (n == "@va.list") {
		return tx.va_list_ty();
	}
	if (n == "void") {
		return tx.void_ty();
	}
	if (n == "bool") {
		return tx.bool_ty();
	}
	if (n == "f16") {
		return tx.float_ty(16);
	}
	if (n == "f32") {
		return tx.float_ty(32);
	}
	if (n == "f64") {
		return tx.float_ty(64);
	}
	int w = parse_int_type_width(n);
	if (w > 0) {
		return n[0] == 'u' ? tx.uint(w) : tx.sint(w);
	}
	if (w < 0) {
		return n[0] == 'u' ? tx.usize_ty() : tx.isize_ty();
	}
	return nullptr;
}

bool is_prim_type_name(const std::string &n) {
	if (n == "void" || n == "bool" || n == "f16" || n == "f32" || n == "f64") {
		return true;
	}
	return parse_int_type_width(n) != 0;
}

bool is_generic_type_set_name(const std::string &n) {
	return n == "u" || n == "i" || n == "f";
}

} // namespace stellac
