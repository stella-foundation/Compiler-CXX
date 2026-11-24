#pragma once

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace stellac {

struct ParsedInt {
	bool	 ok		  = false;
	bool	 overflow = false;
	uint64_t value	  = 0;
};

// Parse an integer literal supporting 0x / 0b / decimal prefixes. Separators
// are accepted only between digits in a component. Returns ok=false on any
// malformed input (empty, bad digits, non-numeric chars). On overflow, sets
// `overflow = true` and saturates `value` to UINT64_MAX.
inline ParsedInt parse_int_literal(std::string_view s) {
	ParsedInt r;
	if (s.empty()) {
		return r;
	}
	uint64_t v			  = 0;
	auto	 is_hex_digit = [](unsigned char c) { return std::isxdigit(c) != 0; };
	auto	 is_dec_digit = [](unsigned char c) { return std::isdigit(c) != 0; };
	auto	 is_bin_digit = [](unsigned char c) { return c == '0' || c == '1'; };

	auto	 parse_digits = [&](size_t begin, auto is_digit, unsigned base) {
		bool had_digit = false;
		for (size_t k = begin; k < s.size(); ++k) {
			unsigned char c = (unsigned char)s[k];
			if (is_digit(c)) {
				uint64_t d =
					std::isdigit(c) ? (uint64_t)(c - '0') : (uint64_t)(std::tolower(c) - 'a' + 10);
				if (v > (UINT64_MAX / base)) {
					r.overflow = true;
					v		   = UINT64_MAX;
					had_digit  = true;
					continue;
				}
				uint64_t nv = v * base + d;
				if (nv < v) {
					r.overflow = true;
					v		   = UINT64_MAX;
				} else {
					v = nv;
				}
				had_digit = true;
				continue;
			}
			if (c == '_') {
				// A separator belongs to this component only when it is
				// between two digits of the component.
				if (!had_digit || k + 1 >= s.size() || !is_digit((unsigned char)s[k + 1])) {
					return false;
				}
				continue;
			}
			return false;
		}
		return had_digit;
	};

	if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		if (!parse_digits(2, is_hex_digit, 16)) {
			return r;
		}
	} else if (s.size() > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
		if (!parse_digits(2, is_bin_digit, 2)) {
			return r;
		}
	} else {
		if (!parse_digits(0, is_dec_digit, 10)) {
			return r;
		}
	}
	r.ok	= true;
	r.value = v;
	return r;
}

// Remove separators after the lexer has validated their placement.
inline std::string normalize_numeric_literal(std::string_view s) {
	std::string out;
	out.reserve(s.size());
	for (char c : s) {
		if (c != '_') {
			out.push_back(c);
		}
	}
	return out;
}

inline bool is_int_literal(std::string_view s) {
	return parse_int_literal(s).ok;
}

inline bool is_power_of_two(uint64_t v) {
	return v != 0 && (v & (v - 1)) == 0;
}

} // namespace stellac
