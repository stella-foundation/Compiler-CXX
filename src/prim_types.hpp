#pragma once

#include "types.hpp"
#include <string>

namespace stellac {

StType *prim_from_name(TypeContext &tx, const std::string &n);
bool	is_prim_type_name(const std::string &n);
bool	is_generic_type_set_name(const std::string &n);

} // namespace stellac
