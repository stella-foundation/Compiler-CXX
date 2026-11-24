#pragma once

#include "log.hpp"
#include "mangle.hpp"
#include "parser.hpp"
#include "prim_types.hpp"
#include <algorithm>
#include <cstring>
#include <sstream>

namespace stellac {

inline bool builtin_member_path(const Expr &e, std::string_view &module, std::string_view &member) {
	if (e.kind != ExprKind::Member || !e.lhs || e.lhs->kind != ExprKind::Ident ||
		 e.lhs->path.size() != 1 ||
		(e.lhs->path[0] != "@of" && e.lhs->path[0] != "@va" && e.lhs->path[0] != "@src" &&
		 e.lhs->path[0] != "@syn" && e.lhs->path[0] != "@diag")) {
		return false;
	}
	module = e.lhs->path[0];
	member = e.member;
	return true;
}

inline bool reject_reserved_name(const SourceLoc &loc, const std::string &nm, const char *what) {
	if (is_prim_type_name(nm)) {
		log::report(log::Level::Error, loc,
					std::string("reserved builtin type name '") + nm + "' cannot be used as a " +
						what);
		return true;
	}
	// `u` and `i` are constraint markers only in generic type-set position, so
	// keep them usable as ordinary identifiers (especially loop variable `i`).
	// They still cannot name a declared type, where bare type lookup is reserved.
	const bool type_set_is_type_name = std::strcmp(what, "struct name") == 0 ||
		std::strcmp(what, "union name") == 0 || std::strcmp(what, "enum name") == 0 ||
		std::strcmp(what, "type binding name") == 0 || std::strcmp(what, "trait name") == 0;
	if (is_generic_type_set_name(nm) && (nm == "f" || type_set_is_type_name)) {
		log::report(log::Level::Error, loc,
					std::string("generic-only type-set name '") + nm + "' cannot be used as a " +
						what);
		return true;
	}
	return false;
}

} // namespace stellac
