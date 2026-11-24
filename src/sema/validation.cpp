#include "internal.hpp"

namespace stellac {

void Sema::validate_attributes(AttributeList &attrs, TopKind target, const SourceLoc &) {
	AttributeTarget attr_target = AttributeTarget::Function;
	std::string		kind_label;
	switch (target) {
	case TopKind::Fn:
		attr_target = AttributeTarget::Function;
		kind_label	= "function declarations";
		break;
	case TopKind::Struct:
		attr_target = AttributeTarget::Struct;
		kind_label	= "struct declarations";
		break;
	case TopKind::Union:
		attr_target = AttributeTarget::Union;
		kind_label	= "union declarations";
		break;
	case TopKind::Var:
	case TopKind::Const:
		attr_target = AttributeTarget::Global;
		kind_label	= "global declarations";
		break;
	case TopKind::TypeBinding:
		attr_target = AttributeTarget::TypeBinding;
		kind_label	= "type binding declarations";
		break;
	case TopKind::Enum:
		attr_target = AttributeTarget::Enum;
		kind_label	= "enum declarations";
		break;
	case TopKind::Import:
		attr_target = AttributeTarget::Import;
		kind_label	= "import declarations";
		break;
	case TopKind::Mod:
		kind_label = "module declarations";
		break;
	default:
		kind_label = "this declaration";
		break;
	}

	validate_attribute_list(attrs, attr_target, kind_label, target == TopKind::Const);
}

void Sema::validate_attribute_list(AttributeList &attrs, AttributeTarget attr_target,
								   std::string_view kind_label, bool is_const) {
	std::set<std::pair<std::string, bool>> seen;
	std::set<std::string>				   sanitizer_selectors;
	for (const auto &a : attrs.attrs) {
		const AttributeSpec *spec = lookup_attribute_spec(a.name);
		if (!spec) {
			log::report(log::Level::Error, a.loc, "unknown " + attr_diag_prefix(a));
			continue;
		}
		if (!attribute_target_allowed(*spec, attr_target)) {
			log::report(log::Level::Error, a.loc,
						attr_diag_prefix(a) + " is not valid on " + std::string(kind_label));
			continue;
		}
		if (a.name == "link_name" && is_const) {
			log::report(log::Level::Error, a.loc,
						attr_diag_prefix(a) + " is not valid on compile-time const declarations");
			continue;
		}
		bool polarity_ok = spec->polarity == AttributePolarity::Either ||
						   (spec->polarity == AttributePolarity::PositiveOnly && !a.negated) ||
						   (spec->polarity == AttributePolarity::NegativeOnly && a.negated);
		if (!polarity_ok) {
			log::report(log::Level::Error, a.loc,
						attr_diag_prefix(a) +
							(a.negated ? " does not support negation" : " requires negation"));
			continue;
		}
		if (a.name != "sanitize" && !seen.insert({a.name, a.negated}).second) {
			log::report(log::Level::Error, a.loc,
						attr_diag_prefix(a) + " is specified more than once");
		}
		switch (spec->arg_kind) {
		case AttributeArgKind::None:
			if (!a.args.empty()) {
				log::report(log::Level::Error, a.loc, attr_diag_prefix(a) + " takes no arguments");
			}
			break;
		case AttributeArgKind::OptionalString:
			if (a.args.size() > 1 ||
				(a.args.size() == 1 && (a.arg_is_string.size() != 1 || !a.arg_is_string[0]))) {
				log::report(log::Level::Error, a.loc,
							attr_diag_prefix(a) + " takes at most one string argument");
			}
			break;
		case AttributeArgKind::String:
			if (a.args.size() != 1 || a.arg_is_string.size() != 1 || !a.arg_is_string[0]) {
				log::report(log::Level::Error, a.loc,
							attr_diag_prefix(a) + " expects exactly one string argument");
			} else if (a.name == "abi" && a.args[0] != "Stella" && a.args[0] != "C") {
				log::report(log::Level::Error, a.loc,
							attr_diag_prefix(a) + " unsupported ABI '" + a.args[0] +
								"' (expected \"Stella\" or \"C\")");
			} else if (a.name == "repr" && a.args[0] != "Stella" && a.args[0] != "C") {
				log::report(log::Level::Error, a.loc,
							attr_diag_prefix(a) + " unsupported representation '" + a.args[0] +
								"' (expected \"Stella\" or \"C\")");
			}
			break;
		case AttributeArgKind::Integer:
			if (a.args.size() != 1 || a.arg_is_string.size() != 1 || a.arg_is_string[0]) {
				log::report(log::Level::Error, a.loc,
							attr_diag_prefix(a) + " expects exactly one integer argument");
			} else {
				ParsedInt pi = parse_int_literal(a.args[0]);
				if (!pi.ok) {
					log::report(log::Level::Error, a.loc,
								attr_diag_prefix(a) + " expects exactly one integer argument");
				} else if (pi.overflow) {
					log::report(log::Level::Error, a.loc,
								attr_diag_prefix(a) + " integer argument is out of range");
				} else if (!is_power_of_two(pi.value)) {
					log::report(log::Level::Error, a.loc,
								std::string("alignment must be a power of two, got ") + a.args[0]);
				}
			}
			break;
		case AttributeArgKind::Layout: {
			if (a.args.size() != 2 || a.arg_is_string.size() != a.args.size()) {
				log::report(log::Level::Error, a.loc,
							attr_diag_prefix(a) + " expects exactly two integer arguments: size and alignment");
				break;
			}
			for (size_t i = 0; i < a.args.size(); ++i) {
				if (a.arg_is_string[i]) {
					log::report(log::Level::Error, a.loc,
								attr_diag_prefix(a) + " arguments must be integers");
					continue;
				}
				ParsedInt pi = parse_int_literal(a.args[i]);
				if (!pi.ok || pi.overflow) {
					log::report(log::Level::Error, a.loc,
								attr_diag_prefix(a) + " arguments must be in-range integers");
				} else if (i == 1 && !is_power_of_two(pi.value)) {
					log::report(log::Level::Error, a.loc,
								"layout alignment must be a power of two, got " + a.args[i]);
				}
			}
			break;
		}
		case AttributeArgKind::SanitizerSelector:
			if (a.args.size() != 1 || a.arg_is_string.size() != 1 || !a.arg_is_string[0]) {
				log::report(log::Level::Error, a.loc,
							attr_diag_prefix(a) + " expects exactly one string argument");
			} else if (a.args[0] != "address" && a.args[0] != "memory" && a.args[0] != "thread") {
				log::report(log::Level::Error, a.loc,
							attr_diag_prefix(a) +
								" selector must be 'address', 'memory', or 'thread'");
			} else if (!sanitizer_selectors.insert(a.args[0]).second) {
				log::report(log::Level::Error, a.loc,
							attr_diag_prefix(a) + " repeats sanitizer selector '" + a.args[0] +
								"'");
			}
			break;
		}
	}
	if (has_attr(attrs, "inline") && has_negated_attr(attrs, "inline")) {
		log::report(log::Level::Error, attrs.loc,
					"conflicting attributes '#[inline]' and '#[!inline]'");
	}
	if (has_attr(attrs, "inline") && has_attr(attrs, "naked")) {
		log::report(log::Level::Error, attrs.loc, "naked functions cannot be inlined");
	}
	if (has_attr(attrs, "cold") && has_attr(attrs, "hot")) {
		log::report(log::Level::Error, attrs.loc, "conflicting attributes '#[cold]' and '#[hot]'");
	}
}

// Compile-time value-fit test used only for contextual literal typing and
// overload selection. It must not be used to convert an already-typed value.
//
// Value scope (decided with the user): literals + const-fold, *excluding*
// `@of.size(T)` (whose folded value is a placeholder 0 and whose real size is
// only known at codegen); `contains_sizeof` filters that out.
//
// Type scope: all scalars -- integers (incl. enum-backed + usize), bool,
// pointers, and floats. Float policy: exact representability (NOT rounding),
// so `0.1` into f32 is NoFit and must be written `0.1~`.
void Sema::check_type_compat(StType *dst, StType *src_ty, Expr &src_expr, const SourceLoc &loc,
							 const std::string &nofit_msg, const std::string &mismatch_msg) {
	if (src_expr.sema_invalid || is_poison(src_ty) || is_poison(dst)) {
		return;
	}
	// A float literal is initially typed as f64. Type equality alone therefore
	// cannot tell whether its source value is exactly representable in the
	// destination (notably for `0.1` assigned to f64). Preserve the strict
	// literal policy at typed sinks, requiring explicit agreement (`~`) to opt
	// into rounding.
	StType *dst_underlying = underlying_type(dst);
	if (dst_underlying && dst_underlying->kind == TypeKind::Float &&
		value_fits(dst, src_expr) == FitResult::NoFit) {
		log::report(log::Level::Error, loc, nofit_msg);
		return;
	}
	if (!arg_type_compat(dst, src_ty)) {
		std::string msg = mismatch_msg;
		StType *dst_p = underlying_type(dst);
		StType *src_p = underlying_type(src_ty);
		if (dst_p && src_p && dst_p->kind == TypeKind::Pointer && src_p->kind == TypeKind::Pointer &&
			eq_type(dst_p->base, src_p->base)) {
			if (!src_p->is_mutable && dst_p->is_mutable && msg.find("pointer") == std::string::npos) {
				msg += " (cannot convert immutable pointer to mutable pointer)";
			} else if (src_p->is_volatile && !dst_p->is_volatile && msg.find("qualifier") == std::string::npos) {
				msg += " (discards 'volatile' qualifier)";
			}
		}
		log::report(log::Level::Error, loc, msg, "E_TYPE_MISMATCH", "sema");
	}
}

namespace {

// Evaluate an untyped integer expression using the width and unsigned
// arithmetic of its contextual destination. Keeping intermediates at that
// width avoids both incorrect wraparound decisions and shifts wider than 64
// bits invoking undefined behavior in the host compiler.
static bool eval_unsigned_contextual_expr(const Expr &e, unsigned width, llvm::APInt &value) {
	if (e.kind == ExprKind::IntLit || e.kind == ExprKind::CharLit) {
		if (width < 64 && !fits_uint_width(e.int_val, static_cast<int>(width))) {
			return false;
		}
		value = llvm::APInt(width, e.int_val);
		return true;
	}

	if (e.kind == ExprKind::Unary && e.lhs) {
		llvm::APInt operand(width, 0);
		if (!eval_unsigned_contextual_expr(*e.lhs, width, operand)) {
			return false;
		}
		if (e.str_val == "-") {
			value = -operand;
			return true;
		}
		if (e.str_val == "~") {
			value = ~operand;
			return true;
		}
		return false;
	}

	if (e.kind != ExprKind::Binary || !e.lhs || !e.rhs) {
		return false;
	}

	llvm::APInt lhs(width, 0);
	llvm::APInt rhs(width, 0);
	if (!eval_unsigned_contextual_expr(*e.lhs, width, lhs) ||
		!eval_unsigned_contextual_expr(*e.rhs, width, rhs)) {
		return false;
	}

	if (e.str_val == "/" || e.str_val == "%") {
		if (rhs.isZero()) {
			return false;
		}
		value = e.str_val == "/" ? lhs.udiv(rhs) : lhs.urem(rhs);
		return true;
	}
	if (e.str_val == "<<" || e.str_val == ">>") {
		const uint64_t shift = rhs.getLimitedValue(width);
		if (shift >= width) {
			return false;
		}
		value = e.str_val == "<<" ? lhs.shl(static_cast<unsigned>(shift))
											 : lhs.lshr(static_cast<unsigned>(shift));
		return true;
	}

	if (e.str_val == "+" || e.str_val == "+%") {
		value = lhs + rhs;
	} else if (e.str_val == "-" || e.str_val == "-%") {
		value = lhs - rhs;
	} else if (e.str_val == "+|") {
		llvm::APInt extended = lhs.zext(width + 1) + rhs.zext(width + 1);
		value = extended[width] ? llvm::APInt::getMaxValue(width) : extended.trunc(width);
	} else if (e.str_val == "-|") {
		value = lhs.ult(rhs) ? llvm::APInt(width, 0) : lhs - rhs;
	} else if (e.str_val == "*" || e.str_val == "*%") {
		value = lhs * rhs;
	} else if (e.str_val == "*|") {
		bool			  ov   = false;
		const llvm::APInt prod = lhs.umul_ov(rhs, ov);
		value				   = ov ? llvm::APInt::getMaxValue(width) : prod;
	} else if (e.str_val == "&") {
		value = lhs & rhs;
	} else if (e.str_val == "|") {
		value = lhs | rhs;
	} else if (e.str_val == "^") {
		value = lhs ^ rhs;
	} else {
		return false;
	}
	return true;
}

} // namespace

bool Sema::expect_bool_cond(const SourceLoc &loc, StType *ty) {
	StType *underlying = underlying_type(ty);
	if (ty && (!underlying || underlying->kind != TypeKind::Bool)) {
		log::report(log::Level::Error, loc, "condition must be of type bool");
		return false;
	}
	return true;
}

FitResult Sema::value_fits(StType *dst, Expr &src) {
	if (!dst) {
		return FitResult::Unknown;
	}
	StType *d = underlying_type(dst);
	if (!d) {
		return FitResult::Unknown;
	}
	if (d->kind == TypeKind::UInt && !src.ty && is_contextual_integer_expr(src)) {
		const int width = scalar_size_bits(d, types_.pointer_width());
		if (width <= 0) {
			return FitResult::Unknown;
		}
		llvm::APInt value(static_cast<unsigned>(width), 0);
		return eval_unsigned_contextual_expr(src, width, value) ? FitResult::Fit : FitResult::NoFit;
	}

	// Aggregate literals are contextually typed by the candidate aggregate
	// type. This probe is deliberately diagnostic-free; the selected candidate
	// is checked authoritatively by check_expr_with_expected().
	if (src.kind == ExprKind::StructLit &&
		(d->kind == TypeKind::Struct || d->kind == TypeKind::Union)) {
		if (d->kind == TypeKind::Union && (src.field_names.size() != 1 || src.args.size() != 1)) {
			return FitResult::NoFit;
		}
		if (d->kind == TypeKind::Struct && src.field_names.size() != d->fields.size()) {
			return FitResult::NoFit;
		}

		std::vector<bool> initialized(d->fields.size(), false);
		FitResult		  result = FitResult::Fit;
		for (size_t i = 0; i < src.field_names.size(); ++i) {
			size_t field = field_index(d, src.field_names[i]);
			if (field == SIZE_MAX || field >= d->fields.size() || initialized[field]) {
				return FitResult::NoFit;
			}
			initialized[field] = true;
			if (!src.args[i]) {
				return FitResult::NoFit;
			}
			FitResult fit = FitResult::Unknown;
			if (is_contextual_literal(*src.args[i])) {
				fit = value_fits(d->fields[field], *src.args[i]);
			} else if (src.args[i]->ty && arg_type_compat(d->fields[field], src.args[i]->ty)) {
				fit = FitResult::Fit;
			}
			if (fit == FitResult::NoFit) {
				return fit;
			}
			if (fit == FitResult::Unknown) {
				result = FitResult::Unknown;
			}
		}
		if (d->kind == TypeKind::Struct && std::any_of(initialized.begin(), initialized.end(),
													   [](bool value) { return !value; })) {
			return FitResult::NoFit;
		}
		return result;
	}

	// Array literals are contextually typed element-by-element.
	if (d->kind == TypeKind::Array && src.kind == ExprKind::ArrayLit) {
		if (src.args.size() != d->array_len) {
			return FitResult::NoFit;
		}
		for (auto &arg : src.args) {
			if (!arg || !is_contextual_literal(*arg)) {
				return FitResult::Unknown;
			}
			FitResult fit = value_fits(d->base, *arg);
			if (fit != FitResult::Fit) {
				return fit;
			}
		}
		return FitResult::Fit;
	}
	if (d->kind == TypeKind::Array && src.kind == ExprKind::StringLit) {
		StType *elem = underlying_type(d->base);
		const size_t expected_len = src.str_val.size() + (src.is_cstring ? 1 : 0);
		return elem && elem->kind == TypeKind::UInt && elem->width == 8 &&
					   d->array_len == expected_len
				   ? FitResult::Fit
				   : FitResult::NoFit;
	}
	if (d->kind == TypeKind::Slice && src.kind == ExprKind::ArrayLit && src.args.empty()) {
		return FitResult::Fit;
	}

	// NullLit into a pointer slot is the C-style null pointer constant; into
	// any other slot it stays Unknown so the site's type-compat rule decides.
	if (src.kind == ExprKind::NullLit) {
		return (d->kind == TypeKind::Pointer) ? FitResult::Fit : FitResult::Unknown;
	}

	// Try to obtain a compile-time-known integer value as raw 64-bit bits.
	// BoolLit/CharLit/IntLit are read from the AST directly; everything else
	// const-foldable (Unary, Binary, enum-member Ident) goes through
	// eval_const_int. Sizeof-containing expressions are rejected so no folded
	// 0 placeholder ever reaches a fit decision.
	bool	 have_int = false;
	uint64_t vbits	  = 0;
	if (!contains_sizeof(src) && is_const_int_expr(src)) {
		if (src.kind == ExprKind::IntLit) {
			vbits	 = src.int_val;
			have_int = true;
		} else if (src.kind == ExprKind::CharLit) {
			vbits	 = src.int_val;
			have_int = true;
		} else if (src.kind == ExprKind::BoolLit) {
			vbits	 = src.const_bool_val ? 1u : 0u;
			have_int = true;
		} else {
			int64_t ev = eval_const_int(src);
			vbits	   = static_cast<uint64_t>(ev);
			have_int   = true;
		}
	}

	// FloatLit carries a compile-time-known double (lexer parses to nearest f64).
	bool have_float = (src.kind == ExprKind::FloatLit);

	switch (d->kind) {
	case TypeKind::Pointer:
		// Only NullLit is contextually typable as a pointer; integer zero is
		// still an integer literal and requires an explicit cast.
		return FitResult::Unknown;
	case TypeKind::Bool:
	case TypeKind::UInt:
	case TypeKind::SInt:
		if (have_int) {
			// Constants keep their value in `vbits` so that signed and
			// unsigned 64-bit values can both be represented.  Preserve the
			// source signedness for the fit test: a negative i64 expression
			// is not implicitly representable by any unsigned destination.
			StType *source_ty = underlying_type(src.ty);
			bool	source_is_negative = false;
			if (source_ty && is_signed_integer_type(source_ty) &&
				src.kind != ExprKind::IntLit && src.kind != ExprKind::CharLit) {
				int source_width = scalar_size_bits(source_ty, types_.pointer_width());
				int64_t signed_value = signed_bits(vbits, source_width);
				source_is_negative = signed_value < 0;
				vbits = static_cast<uint64_t>(signed_value);
			}
			// An untyped IntLit stores its value as raw uint64 bits. Its high
			// bit does not indicate a negative source value; a leading '-' is
			// represented by a separate Unary expression.
			if (!source_ty && src.kind != ExprKind::IntLit && src.kind != ExprKind::CharLit &&
				signed_bits(vbits, 64) < 0) {
				source_is_negative = true;
			}
			return int_value_fits(d, vbits, source_is_negative);
		}
		// Non-literal integer source is not contextually typed.
		return FitResult::Unknown;
	case TypeKind::Enum:
		// Enum members are already enum-typed; bare integers are not.
		return FitResult::Unknown;
	case TypeKind::Float: {
		if (have_float) {
			// Bare FloatLit: judge by the real value its source text denotes.
			return float_literal_fits(src.str_val, d->width);
		}
		// Integer-to-float is a conversion, even for an integer literal.
		if (have_int) {
			return FitResult::Unknown;
		}
		// Non-constant source: walk the expression for FloatLit leaves and
		// require each to be representable in this slot's width (or opt in with `~`).
		// When NoFit, the caller can emit a single slot-width error.
		return float_leaves_fit(src, d->width);
	}
	default:
		// void/struct/array/function are not scalar literals.
		return FitResult::Unknown;
	}
}

bool Sema::generic_integer_literal_fits_float(StType *dst, Expr &src) {
	StType *d = underlying_type(dst);
	if (!d || d->kind != TypeKind::Float || contains_sizeof(src) || !is_const_int_expr(src)) {
		return false;
	}

	uint64_t magnitude = 0;
	if (src.kind == ExprKind::IntLit || src.kind == ExprKind::CharLit) {
		magnitude = src.int_val;
	} else {
		StType *source_type = underlying_type(src.ty);
		if (source_type && source_type->kind == TypeKind::Enum) {
			source_type = underlying_type(source_type->base);
		}
		if (source_type && source_type->kind == TypeKind::UInt) {
			// eval_const_int returns int64_t, so a u64 result with its top bit
			// set looks negative there. Use the folded raw bits for unsigned
			// expressions rather than measuring the apparent signed magnitude.
			if (!src.has_const_value || src.is_const_bool || src.const_value_is_float) {
				return false;
			}
			const int width = scalar_size_bits(source_type, types_.pointer_width());
			if (width <= 0 || width > 64) {
				return false;
			}
			magnitude = normalize_int_bits(src.int_val, width);
		} else {
			const int64_t value = eval_const_int(src);
			magnitude = value < 0 ? static_cast<uint64_t>(-(value + 1)) + 1u
									 : static_cast<uint64_t>(value);
		}
	}

	// f16, f32, and f64 have 11, 24, and 53 significant binary bits.
	// An integer is exactly representable when its significant bits fit, or
	// when discarded low bits are all zero. This avoids depending on the host
	// floating-point format and avoids rounding large integer literals.
	unsigned precision = 0;
	switch (d->width) {
	case 16:
		precision = 11;
		break;
	case 32:
		precision = 24;
		break;
	case 64:
		precision = 53;
		break;
	default:
		return false;
	}
	if (magnitude == 0) {
		return true;
	}

	unsigned bits = 0;
	for (uint64_t n = magnitude; n != 0; n >>= 1) {
		++bits;
	}
	if (bits <= precision) {
		return true;
	}

	unsigned trailing_zeroes = 0;
	for (uint64_t n = magnitude; (n & 1u) == 0; n >>= 1) {
		++trailing_zeroes;
	}
	return trailing_zeroes >= bits - precision;
}

bool Sema::is_const_int_expr(Expr &e) {
	switch (e.kind) {
	case ExprKind::IntLit:
	case ExprKind::CharLit:
		return true;
	case ExprKind::EnumMember:
		return true;
	case ExprKind::Sizeof:
	case ExprKind::Alignof:
	case ExprKind::Bitsof:
	case ExprKind::Offsetof:
		return true;
	case ExprKind::Ident:
		// Enum member access: requires resolved enum (check_expr populated int_val
		// and type=enum). A bare local const binding is NOT folded here -- keep the
		// surface tight.
		if (e.path.size() >= 2 && e.ty && underlying_type(e.ty) && underlying_type(e.ty)->kind == TypeKind::Enum) {
			return true;
		}
		// Local `const` binding: fold only if its initializer is const-foldable.
		// (Common case: top-level `const X = 5;` referenced from a `case`.)
		if (e.path.size() == 1 && e.ty && underlying_type(e.ty) &&
			(underlying_type(e.ty)->kind == TypeKind::SInt || underlying_type(e.ty)->kind == TypeKind::UInt)) {
			// We don't reliably have the binding's initializer value here without
			// extra plumbing; treat enum path above as the supported route. Code
			// that wants a named constant in a case should use an `enum` or a
			// literal. Reject to keep diagnostics honest.
		}
		return e.has_const_value && !e.is_const_bool && !e.const_value_is_float;
	case ExprKind::Unary:
		if (e.str_val == "-" || e.str_val == "~") {
			return e.lhs && is_const_int_expr(*e.lhs);
		}
		return false;
	case ExprKind::Binary: {
		const std::string &op = e.str_val;
		if (op == "+" || op == "-" || op == "+%" || op == "-%" || op == "*%" || op == "+|" ||
			op == "-|" || op == "*|" || op == "*" || op == "/" || op == "%" || op == "<<" ||
			op == ">>" || op == "&" || op == "|" || op == "^") {
			return e.lhs && e.rhs && is_const_int_expr(*e.lhs) && is_const_int_expr(*e.rhs);
		}
		return false;
	}
	case ExprKind::Conditional: {
		if (!e.condition || !e.condition->is_const_bool) {
			return false;
		}
		Expr *selected = e.condition->const_bool_val ? e.then_expr.get() : e.else_expr.get();
		return selected && is_const_int_expr(*selected);
	}
	default:
		return e.has_const_value && !e.is_const_bool && !e.const_value_is_float;
	}
}

int64_t Sema::eval_const_int(Expr &e) {
	if (e.has_const_value && !e.is_const_bool && !e.const_value_is_float) {
		StType *value_type = underlying_type(e.ty);
		if (value_type && value_type->kind == TypeKind::Enum) {
			value_type = underlying_type(value_type->base);
		}
		const int width = scalar_size_bits(value_type, types_.pointer_width());
		const bool cached_unsigned_operation =
			value_type && value_type->kind == TypeKind::UInt &&
			(e.kind == ExprKind::Unary || e.kind == ExprKind::Binary);
		const bool cached_signed_narrow_value =
			is_signed_integer_type(value_type) && width > 0 && width < 64;
		if (width > 0 &&
			(e.kind == ExprKind::Cast || cached_unsigned_operation || cached_signed_narrow_value)) {
			const uint64_t bits = normalize_int_bits(e.int_val, width);
			if (value_type && value_type->kind == TypeKind::UInt && width < 64) {
				return static_cast<int64_t>(bits);
			}
			return signed_bits(bits, std::min(width, 64));
		}
	}
	switch (e.kind) {
	case ExprKind::IntLit:
	case ExprKind::CharLit:
		return signed_bits(e.int_val, 64);
	case ExprKind::EnumMember:
		return signed_bits(e.int_val, 64);
	case ExprKind::Sizeof:
	case ExprKind::Alignof:
	case ExprKind::Bitsof:
	case ExprKind::Offsetof:
		// Sema does not have the target DataLayout, so this placeholder must not
		// be used for value-sensitive checks. Layout-dependent switch cases are
		// evaluated from their expressions in codegen instead.
		return 0;
	case ExprKind::Ident:
		if (e.path.size() >= 2 && e.ty && underlying_type(e.ty) && underlying_type(e.ty)->kind == TypeKind::Enum) {
			return signed_bits(e.int_val, 64);
		}
		return e.has_const_value ? signed_bits(e.int_val, 64) : 0;
	case ExprKind::Cast:
		return 0; // A valid integer cast was handled by the cached constant above.
	case ExprKind::Unary: {
		int64_t v = e.lhs ? eval_const_int(*e.lhs) : 0;
		if (e.str_val == "-") {
			// The positive token magnitude in the spelling `-2^63` is not
			// representable as int64_t, but the complete negative literal is.
			if (e.lhs && e.lhs->kind == ExprKind::IntLit &&
				e.lhs->int_val == (uint64_t{1} << 63)) {
				return INT64_MIN;
			}
			if (v == INT64_MIN) {
				log::report(log::Level::Error, e.loc,
							"overflow in constant expression: unary negation");
				return 0;
			}
			return -v;
		}
		if (e.str_val == "~") {
			return ~v;
		}
		return v;
	}
	case ExprKind::Binary: {
		int64_t			   l  = e.lhs ? eval_const_int(*e.lhs) : 0;
		int64_t			   r  = e.rhs ? eval_const_int(*e.rhs) : 0;
		int			   width = e.lhs && e.lhs->ty
											? scalar_size_bits(e.lhs->ty, types_.pointer_width())
											: 64;
		if (width <= 0 || width > 64) {
			width = 64;
		}
		int64_t		   signed_min = width >= 64
									 ? INT64_MIN
									 : -(int64_t{1} << (width - 1));
		const bool signed_type = is_signed_integer_type(e.lhs ? e.lhs->ty : nullptr);
		const std::string &op = e.str_val;
		if (op == "+") {
			if ((r > 0 && l > INT64_MAX - r) || (r < 0 && l < INT64_MIN - r)) {
				log::report(log::Level::Error, e.loc, "overflow in constant expression: addition");
				return 0;
			}
			return l + r;
		}
		if (op == "-") {
			if ((r > 0 && l < INT64_MIN + r) || (r < 0 && l > INT64_MAX + r)) {
				log::report(log::Level::Error, e.loc,
							"overflow in constant expression: subtraction");
				return 0;
			}
			return l - r;
		}
		if (op == "*") {
			if (l != 0 && r != 0) {
				if (l > 0 && r > 0 && l > INT64_MAX / r) {
					log::report(log::Level::Error, e.loc,
								"overflow in constant expression: multiplication");
					return 0;
				}
				if (l > 0 && r < 0 && r < INT64_MIN / l) {
					log::report(log::Level::Error, e.loc,
								"overflow in constant expression: multiplication");
					return 0;
				}
				if (l < 0 && r > 0 && l < INT64_MIN / r) {
					log::report(log::Level::Error, e.loc,
								"overflow in constant expression: multiplication");
					return 0;
				}
				if (l < 0 && r < 0 && l < INT64_MAX / r) {
					log::report(log::Level::Error, e.loc,
								"overflow in constant expression: multiplication");
					return 0;
				}
			}
			return l * r;
		}
		if (op == "/") {
			if (r == 0) {
				log::report(log::Level::Error, e.loc, "division by zero in constant expression");
				return 0;
			}
			// A signed minimum divided by -1 is not representable at any width.
			if (signed_type && l == signed_min && r == -1) {
				log::report(log::Level::Error, e.loc,
							"overflow in constant expression: signed division");
				return 0;
			}
			return l / r;
		}
		if (op == "%") {
			if (r == 0) {
				log::report(log::Level::Error, e.loc, "modulo by zero in constant expression");
				return 0;
			}
			// LLVM makes signed remainder poison for the same minimum/-1 pair.
			if (signed_type && l == signed_min && r == -1) {
				log::report(log::Level::Error, e.loc,
							"overflow in constant expression: signed remainder");
				return 0;
			}
			return l % r;
		}
		if (op == "<<") {
			int width = e.lhs && e.lhs->ty ? scalar_size_bits(e.lhs->ty, types_.pointer_width())
											   : types_.pointer_width();
			if (r < 0 || r >= width) {
				log::report(log::Level::Error, e.loc,
							"invalid constant shift count " + std::to_string(r) + " for " +
								std::to_string(width) + "-bit operand");
				return 0;
			}
			if (r >= 64) {
				log::report(log::Level::Error, e.loc,
							"constant shift exceeds the 64-bit compile-time evaluation range");
				return 0;
			}
			return signed_bits(static_cast<uint64_t>(l) << static_cast<unsigned>(r), 64);
		}
		if (op == ">>") {
			int width = e.lhs && e.lhs->ty ? scalar_size_bits(e.lhs->ty, types_.pointer_width())
										   : types_.pointer_width();
			if (r < 0 || r >= width) {
				log::report(log::Level::Error, e.loc,
							"invalid constant shift count " + std::to_string(r) + " for " +
								std::to_string(width) + "-bit operand");
				return 0;
			}
			if (r >= 64) {
				log::report(log::Level::Error, e.loc,
							"constant shift exceeds the 64-bit compile-time evaluation range");
				return 0;
			}
			return arithmetic_shift_right(l, static_cast<unsigned>(r));
		}
		if (op == "&") {
			return l & r;
		}
		if (op == "|") {
			return l | r;
		}
		if (op == "^") {
			return l ^ r;
		}
		return 0;
	}
	case ExprKind::Conditional: {
		if (!e.condition || !e.condition->is_const_bool) {
			return 0;
		}
		Expr *selected = e.condition->const_bool_val ? e.then_expr.get() : e.else_expr.get();
		return selected ? eval_const_int(*selected) : 0;
	}
	default:
		return 0;
	}
}

StType *Sema::substitute_self(StType *t, StType *concrete) {
	if (!t || !concrete) return t;
	t = underlying_type(t);
	if (t == types_.self_ty() || t->kind == TypeKind::SelfType) {
		return concrete;
	}
	if (t->kind == TypeKind::Pointer) {
		StType *new_base = substitute_self(t->base, concrete);
		return types_.pointer(new_base, t->is_mutable, t->is_volatile);
	}
	if (t->kind == TypeKind::Slice) {
		StType *new_base = substitute_self(t->base, concrete);
		return types_.slice(new_base, t->is_mutable, t->is_volatile);
	}
	if (t->kind == TypeKind::Array) {
		StType *new_base = substitute_self(t->base, concrete);
		return types_.array_of(new_base, t->array_len);
	}
	if (t->kind == TypeKind::Function) {
		std::vector<StType *> new_params;
		new_params.reserve(t->fn_params.size());
		for (auto *p : t->fn_params) {
			new_params.push_back(substitute_self(p, concrete));
		}
		StType *new_ret = substitute_self(t->fn_ret, concrete);
		return types_.function_of(std::move(new_params), new_ret, t->fn_variadic, t->fn_abi);
	}
	return t;
}

bool Sema::type_implements_trait(StType *type, TraitDecl *trait) const {
	if (!type || !trait) return false;
	type = underlying_type(type);
	auto matches_entry = [this](const TraitImplEntry &entry, StType *t) {
		if (!t) return false;
		if (entry.type && eq_type(entry.type, t)) {
			return true;
		}
		if (entry.generic_origin && t->generic_origin &&
			(entry.generic_origin == t->generic_origin ||
			 entry.generic_origin->mangled == t->generic_origin->mangled)) {
			if (entry.impl_decl && !entry.impl_decl->generic_params.empty()) {
				for (size_t i = 0; i < entry.impl_decl->generic_params.size(); ++i) {
					const auto &gp = entry.impl_decl->generic_params[i];
					if (gp.kind == GenericParam::Kind::Type && i < t->generic_type_args.size() &&
						t->generic_type_args[i]) {
						StType *arg_ty = t->generic_type_args[i];
						if (!gp.type_set.empty() && !generic_type_allowed(gp, arg_ty)) {
							return false;
						}
						if (!gp.trait_bounds.empty() && entry.module) {
							auto *self = const_cast<Sema *>(this);
							for (const auto &bound_ref : gp.trait_bounds) {
								if (!bound_ref) continue;
								std::string tname =
									bound_ref->name.empty() ? bound_ref->single : bound_ref->name;
								Symbol *tsym =
									self->resolve_path(split_scopes(tname), *entry.module);
								if (!tsym) {
									tsym = self->resolve_path_any(split_scopes(tname),
																  *entry.module);
								}
								if (!tsym || tsym->kind != SymKind::Trait || !tsym->trait_decl ||
									!type_implements_trait(arg_ty, tsym->trait_decl)) {
									return false;
								}
							}
						}
					}
				}
			}
			return true;
		}
		return false;
	};
	for (const auto &entry : trait_impls_) {
		if (entry.trait == trait && matches_entry(entry, type)) {
			return true;
		}
	}
	while (type && type->kind == TypeKind::Pointer) {
		type = underlying_type(type->base);
		for (const auto &entry : trait_impls_) {
			if (entry.trait == trait && matches_entry(entry, type)) {
				return true;
			}
		}
	}
	return false;
}

ImplDecl *Sema::find_impl(TraitDecl *trait, StType *type) const {
	if (!type || !trait) return nullptr;
	type = underlying_type(type);
	auto matches_entry = [this](const TraitImplEntry &entry, StType *t) {
		if (!t) return false;
		if (entry.type && eq_type(entry.type, t)) {
			return true;
		}
		if (entry.generic_origin && t->generic_origin &&
			(entry.generic_origin == t->generic_origin ||
			 entry.generic_origin->mangled == t->generic_origin->mangled)) {
			if (entry.impl_decl && !entry.impl_decl->generic_params.empty()) {
				for (size_t i = 0; i < entry.impl_decl->generic_params.size(); ++i) {
					const auto &gp = entry.impl_decl->generic_params[i];
					if (gp.kind == GenericParam::Kind::Type && i < t->generic_type_args.size() &&
						t->generic_type_args[i]) {
						StType *arg_ty = t->generic_type_args[i];
						if (!gp.type_set.empty() && !generic_type_allowed(gp, arg_ty)) {
							return false;
						}
						if (!gp.trait_bounds.empty() && entry.module) {
							auto *self = const_cast<Sema *>(this);
							for (const auto &bound_ref : gp.trait_bounds) {
								if (!bound_ref) continue;
								std::string tname =
									bound_ref->name.empty() ? bound_ref->single : bound_ref->name;
								Symbol *tsym =
									self->resolve_path(split_scopes(tname), *entry.module);
								if (!tsym) {
									tsym = self->resolve_path_any(split_scopes(tname),
																  *entry.module);
								}
								if (!tsym || tsym->kind != SymKind::Trait || !tsym->trait_decl ||
									!type_implements_trait(arg_ty, tsym->trait_decl)) {
									return false;
								}
							}
						}
					}
				}
			}
			return true;
		}
		return false;
	};
	for (const auto &entry : trait_impls_) {
		if (entry.trait == trait && matches_entry(entry, type)) {
			return entry.impl_decl;
		}
	}
	while (type && type->kind == TypeKind::Pointer) {
		type = underlying_type(type->base);
		for (const auto &entry : trait_impls_) {
			if (entry.trait == trait && matches_entry(entry, type)) {
				return entry.impl_decl;
			}
		}
	}
	return nullptr;
}

std::vector<FnDecl *> Sema::find_trait_methods_for_type(StType *type, const std::string &method_name) {
	std::vector<FnDecl *> result;
	if (!type) return result;
	if (trait_methods_indexed_count_ < trait_impls_.size()) {
		for (size_t idx = trait_methods_indexed_count_; idx < trait_impls_.size(); ++idx) {
			const auto &entry = trait_impls_[idx];
			if (!entry.impl_decl) {
				continue;
			}
			for (const auto &m : entry.impl_decl->methods) {
				if (m) {
					trait_methods_by_name_[m->name].push_back(idx);
				}
			}
		}
		trait_methods_indexed_count_ = trait_impls_.size();
	}
	auto by_name = trait_methods_by_name_.find(method_name);
	if (by_name == trait_methods_by_name_.end()) {
		return result;
	}
	type = underlying_type(type);
	auto search_type = [&](StType *t) {
		if (!t) return;
		for (size_t idx : by_name->second) {
			const auto &entry	= trait_impls_[idx];
			bool		matched = (entry.type && eq_type(entry.type, t)) ||
						   (entry.generic_origin && t->generic_origin &&
							(entry.generic_origin == t->generic_origin ||
							 entry.generic_origin->mangled == t->generic_origin->mangled));
			if (matched && entry.impl_decl) {
				for (const auto &m : entry.impl_decl->methods) {
					if (m && m->name == method_name) {
						result.push_back(m.get());
					}
				}
			}
		}
	};
	search_type(type);
	while (type && type->kind == TypeKind::Pointer) {
		type = underlying_type(type->base);
		search_type(type);
	}
	return result;
}

bool Sema::check_orphan_rule(ImplDecl &impl, Module &m) {
	if (!impl.resolved_trait || !impl.resolved_target_type) {
		return true;
	}
	Module *trait_mod = impl.resolved_trait->decl_module;
	bool trait_is_local = trait_mod && (trait_mod == &m || resolver_.same_source_file(m, *trait_mod));

	bool type_is_local = false;
	if (impl.target_type) {
		Symbol *s = resolve_path(split_scopes(impl.target_type->name.empty() ? impl.target_type->single : impl.target_type->name), m);
		if (!s) {
			s = resolve_path_any(split_scopes(impl.target_type->name.empty() ? impl.target_type->single : impl.target_type->name), m);
		}
		if (s && s->owner_module) {
			type_is_local = (s->owner_module == &m || resolver_.same_source_file(m, *s->owner_module));
		}
	}

	if (!trait_is_local && !type_is_local) {
		log::error(impl.loc, "cannot implement foreign trait '" + impl.trait_name +
								 "' for foreign type '" + type_name_for_diag(impl.resolved_target_type) + "'")
			.code("E_ORPHAN_RULE")
			.phase("sema")
			.note(impl.resolved_trait->loc, "trait '" + impl.trait_name + "' is defined in another module")
			.emit();
		return false;
	}
	return true;
}

bool Sema::register_impl(ImplDecl &impl, Module &m) {
	if (!impl.resolved_trait || !impl.resolved_target_type) {
		return false;
	}
	StType *target = underlying_type(impl.resolved_target_type);
	for (const auto &entry : trait_impls_) {
		if (entry.trait == impl.resolved_trait && entry.type && eq_type(entry.type, target)) {
			log::error(impl.loc, "duplicate implementation of trait '" + impl.trait_name +
									 "' for type '" + type_name_for_diag(target) + "'")
				.code("E_DUPLICATE_IMPL")
				.phase("sema")
				.note(entry.loc, "previous implementation was here")
				.emit();
			return false;
		}
	}
	if (!check_orphan_rule(impl, m)) {
		return false;
	}
	trait_impls_.push_back({impl.resolved_trait, target, nullptr, &impl, &m, impl.loc});
	return true;
}

void Sema::validate_impl_signature(ImplDecl &impl, Module &m) {
	(void)m;
	if (!impl.resolved_trait || !impl.resolved_target_type) {
		return;
	}
	StType *concrete = underlying_type(impl.resolved_target_type);

	// 1. Missing methods
	for (const auto &proto : impl.resolved_trait->methods) {
		if (!proto) continue;
		bool found = false;
		for (const auto &mdecl : impl.methods) {
			if (mdecl && mdecl->name == proto->name) {
				found = true;
				break;
			}
		}
		if (!found) {
			log::error(impl.loc, "missing implementation for trait method '" + proto->name +
									 "' of trait '" + impl.trait_name + "'")
				.code("E_TRAIT_MISSING_METHOD")
				.phase("sema")
				.note(proto->loc, "trait method declared here")
				.emit();
		}
	}

	// 2. Extraneous methods
	for (const auto &mdecl : impl.methods) {
		if (!mdecl) continue;
		FnDecl *matching_proto = nullptr;
		for (const auto &proto : impl.resolved_trait->methods) {
			if (proto && proto->name == mdecl->name) {
				matching_proto = proto.get();
				break;
			}
		}
		if (!matching_proto) {
			log::error(mdecl->loc, "method '" + mdecl->name + "' is not declared in trait '" +
									   impl.trait_name + "'")
				.code("E_TRAIT_UNKNOWN_METHOD")
				.phase("sema")
				.note(impl.resolved_trait->loc, "trait '" + impl.trait_name + "' defined here")
				.emit();
			continue;
		}

		// 3. Signature verification
		if (!mdecl->body) {
			log::error(mdecl->loc, "method '" + mdecl->name + "' in 'impl' block must have a body")
				.code("E_TRAIT_MISSING_BODY")
				.phase("sema")
				.emit();
		}

		if (matching_proto->params.size() != mdecl->params.size()) {
			log::error(mdecl->loc, "method '" + mdecl->name + "' expects " +
									   std::to_string(matching_proto->params.size()) +
									   " parameter(s), got " + std::to_string(mdecl->params.size()))
				.code("E_TRAIT_SIGNATURE_MISMATCH")
				.phase("sema")
				.note(matching_proto->loc, "trait method declared here")
				.emit();
			continue;
		}

		for (size_t i = 0; i < matching_proto->params.size(); ++i) {
			StType *expected = substitute_self(matching_proto->params[i].resolved, concrete);
			StType *actual = mdecl->params[i].resolved;
			if (!expected || !actual || !eq_type(expected, actual)) {
				log::error(mdecl->params[i].loc,
						   "parameter " + std::to_string(i + 1) + " of method '" + mdecl->name +
							   "' has mismatched type: expected '" + type_name_for_diag(expected) +
							   "', got '" + type_name_for_diag(actual) + "'")
					.code("E_TRAIT_SIGNATURE_MISMATCH")
					.phase("sema")
					.note(matching_proto->params[i].loc, "trait parameter declared here")
					.emit();
			}
		}

		StType *expected_ret = substitute_self(
			matching_proto->fn_sig ? matching_proto->fn_sig->fn_ret : types_.void_ty(), concrete);
		StType *actual_ret = mdecl->fn_sig ? mdecl->fn_sig->fn_ret : types_.void_ty();
		if (!expected_ret || !actual_ret || !eq_type(expected_ret, actual_ret)) {
			log::error(mdecl->loc, "method '" + mdecl->name + "' has mismatched return type: expected '" +
									   type_name_for_diag(expected_ret) + "', got '" +
									   type_name_for_diag(actual_ret) + "'")
				.code("E_TRAIT_SIGNATURE_MISMATCH")
				.phase("sema")
				.note(matching_proto->loc, "trait method declared here")
				.emit();
		}
	}
}

void Sema::validate_trait_conformance() {
	for (Module *m : resolver_.all()) {
		if (!m) continue;
		std::string mkey = source_key(*m);
		auto &tbl = module_syms_[mkey];

		std::function<void(const std::vector<TopDeclPtr> &, std::vector<std::string>)>
			validate_decls;
		validate_decls = [&](const std::vector<TopDeclPtr> &decls,
							 std::vector<std::string>		prefix) {
			for (auto &td : decls) {
				if (!td) continue;
				if (td->kind == TopKind::Mod && td->mod_decl) {
					std::vector<std::string> child = prefix;
					child.push_back(td->mod_decl->name);
					validate_decls(td->mod_decl->body, std::move(child));
					continue;
				}
				if (td->kind != TopKind::Impl || !td->impl_decl || td->impl_decl->is_inherent) {
					continue;
				}
				ImplDecl &im = *td->impl_decl;
				auto saved_path = m->lexical_path;
				m->lexical_path = m->mod_path;
				m->lexical_path.insert(m->lexical_path.end(), prefix.begin(), prefix.end());

				Symbol *sym = resolve_path(im.trait_path, *m);
				if (!sym) {
					sym = resolve_path_any(im.trait_path, *m);
				}
				if (!sym || sym->kind != SymKind::Trait || !sym->trait_decl) {
					log::error(im.loc, "unknown trait '" + im.trait_name + "'")
						.code("E_UNDEFINED_TYPE")
						.phase("sema")
						.emit();
					m->lexical_path = std::move(saved_path);
					continue;
				}
				im.resolved_trait = sym->trait_decl;

				Symbol *target_sym = nullptr;
				if (im.target_type && im.target_type->kind == TypeRef::Named) {
					target_sym = lookup_named_type(resolver_, *this, *m, im.target_type->single,
												   im.target_type->name);
					if (im.generic_params.empty() && !im.target_type->generic_args.empty() &&
						target_sym) {
						const std::vector<GenericParam> *decl_gp = nullptr;
						if (target_sym->kind == SymKind::Struct && target_sym->st) {
							decl_gp = &target_sym->st->generic_params;
						} else if (target_sym->kind == SymKind::Union && target_sym->un) {
							decl_gp = &target_sym->un->generic_params;
						} else if (target_sym->kind == SymKind::Enum && target_sym->en) {
							decl_gp = &target_sym->en->generic_params;
						}
						if (decl_gp && im.target_type->generic_args.size() == decl_gp->size()) {
							bool all_unbound = true;
							for (const auto &ga : im.target_type->generic_args) {
								if (ga.kind != GenericArg::Kind::Type || !ga.type ||
									ga.type->kind != TypeRef::Named ||
									!ga.type->generic_args.empty() ||
									is_prim_type_name(ga.type->single) ||
									tbl.count(ga.type->single) != 0) {
									all_unbound = false;
									break;
								}
							}
							if (all_unbound) {
								for (size_t i = 0; i < decl_gp->size(); ++i) {
									GenericParam gp = clone_generic_param((*decl_gp)[i]);
									gp.name			= im.target_type->generic_args[i].type->single;
									gp.loc			= im.target_type->generic_args[i].loc;
									im.generic_params.push_back(std::move(gp));
								}
							}
						}
					} else if (!im.generic_params.empty() && target_sym) {
						const std::vector<GenericParam> *decl_gp = nullptr;
						if (target_sym->kind == SymKind::Struct && target_sym->st) {
							decl_gp = &target_sym->st->generic_params;
						} else if (target_sym->kind == SymKind::Union && target_sym->un) {
							decl_gp = &target_sym->un->generic_params;
						} else if (target_sym->kind == SymKind::Enum && target_sym->en) {
							decl_gp = &target_sym->en->generic_params;
						}
						if (decl_gp && im.target_type->generic_args.empty() &&
							im.generic_params.size() == decl_gp->size()) {
							for (const auto &gp : im.generic_params) {
								GenericArg ga;
								ga.kind			= GenericArg::Kind::Type;
								ga.loc			= gp.loc;
								ga.type			= std::make_unique<TypeRef>();
								ga.type->kind	= TypeRef::Named;
								ga.type->name	= gp.name;
								ga.type->single = gp.name;
								ga.type->loc	= gp.loc;
								im.target_type->generic_args.push_back(std::move(ga));
							}
						}
					}
				}

				if (!im.generic_params.empty()) {
					validate_generic_params(im.generic_params);
					if (!target_sym ||
						(target_sym->kind != SymKind::Struct && target_sym->kind != SymKind::Union &&
						 target_sym->kind != SymKind::Enum)) {
						log::error(im.loc, "unknown generic target type in trait implementation")
							.code("E_UNDEFINED_TYPE")
							.phase("sema")
							.emit();
						m->lexical_path = std::move(saved_path);
						continue;
					}
					Module *trait_mod = im.resolved_trait->decl_module;
					bool trait_is_local =
						trait_mod && (trait_mod == m || resolver_.same_source_file(*m, *trait_mod));
					bool type_is_local =
						target_sym->owner_module &&
						(target_sym->owner_module == m ||
						 resolver_.same_source_file(*m, *target_sym->owner_module));
					if (!trait_is_local && !type_is_local) {
						log::error(im.loc, "cannot implement foreign trait '" + im.trait_name +
											   "' for foreign type '" + target_sym->simple + "'")
							.code("E_ORPHAN_RULE")
							.phase("sema")
							.note(im.resolved_trait->loc,
								  "trait '" + im.trait_name + "' is defined in another module")
							.emit();
						m->lexical_path = std::move(saved_path);
						continue;
					}
					bool dup = false;
					for (const auto &entry : trait_impls_) {
						if (entry.trait == im.resolved_trait &&
							entry.generic_origin == target_sym) {
							log::error(im.loc, "duplicate implementation of trait '" +
												   im.trait_name + "' for type '" +
												   target_sym->simple + "'")
								.code("E_DUPLICATE_IMPL")
								.phase("sema")
								.note(entry.loc, "previous implementation was here")
								.emit();
							dup = true;
							break;
						}
					}
					if (dup) {
						m->lexical_path = std::move(saved_path);
						continue;
					}
					trait_impls_.push_back(
						{im.resolved_trait, nullptr, target_sym, &im, m, im.loc});

					for (const auto &proto : im.resolved_trait->methods) {
						if (!proto) continue;
						bool found = false;
						for (const auto &mdecl : im.methods) {
							if (mdecl && mdecl->name == proto->name) {
								found = true;
								break;
							}
						}
						if (!found) {
							log::error(im.loc, "missing implementation for trait method '" +
												   proto->name + "' of trait '" + im.trait_name +
												   "'")
								.code("E_TRAIT_MISSING_METHOD")
								.phase("sema")
								.note(proto->loc, "trait method declared here")
								.emit();
						}
					}
					const std::string type_name = target_sym->simple;
					const bool is_trait_pub = im.resolved_trait && im.resolved_trait->is_pub;
					const bool is_target_pub = target_sym->is_pub;
					for (auto &method : im.methods) {
						if (!method) continue;
						FnDecl *matching_proto = nullptr;
						for (const auto &proto : im.resolved_trait->methods) {
							if (proto && proto->name == method->name) {
								matching_proto = proto.get();
								break;
							}
						}
						if (!matching_proto) {
							log::error(method->loc, "method '" + method->name +
														"' is not declared in trait '" +
														im.trait_name + "'")
								.code("E_TRAIT_UNKNOWN_METHOD")
								.phase("sema")
								.note(im.resolved_trait->loc,
									  "trait '" + im.trait_name + "' defined here")
								.emit();
							continue;
						}
						if (!method->body) {
							log::error(method->loc, "method '" + method->name +
														"' in 'impl' block must have a body")
								.code("E_TRAIT_MISSING_BODY")
								.phase("sema")
								.emit();
						}
						if (matching_proto->params.size() != method->params.size()) {
							log::error(method->loc,
									   "method '" + method->name + "' expects " +
										   std::to_string(matching_proto->params.size()) +
										   " parameter(s), got " +
										   std::to_string(method->params.size()))
								.code("E_TRAIT_SIGNATURE_MISMATCH")
								.phase("sema")
								.note(matching_proto->loc, "trait method declared here")
								.emit();
						}
						substitute_self_in_fn(*method, *im.target_type);
						std::vector<GenericParam> combined;
						size_t					  prepended = 0;
						for (const auto &gp : im.generic_params) {
							bool exists = false;
							for (const auto &mp : method->generic_params) {
								if (mp.name == gp.name) {
									exists = true;
									break;
								}
							}
							if (!exists) {
								combined.push_back(clone_generic_param(gp));
								++prepended;
							}
						}
						for (auto &mp : method->generic_params) {
							combined.push_back(std::move(mp));
						}
						method->generic_params			 = std::move(combined);
						method->impl_generic_param_count = prepended;
						method->owner_path				 = prefix;
						method->owner_path.push_back(im.trait_name);
						method->owner_path.push_back(type_name);
						method->decl_module				 = m;
						const bool is_method_pub =
							method->is_pub || (is_trait_pub && is_target_pub);
						method->is_pub = is_method_pub;
						std::vector<std::string> full = m->mod_path;
						full.insert(full.end(), method->owner_path.begin(),
									method->owner_path.end());
						full.push_back(method->name);
						method->full_path = std::move(full);

						std::vector<std::string> base_keys = {
							type_name + "." + method->name,
							im.trait_name + "." + type_name + "." + method->name,
							im.trait_name + "." + method->name};
						std::vector<std::string> keys = base_keys;
						if (!prefix.empty()) {
							const std::string pfx = join_path(prefix) + ".";
							for (const auto &bk : base_keys) {
								keys.push_back(pfx + bk);
							}
						}
						for (const auto &k : keys) {
							auto it = tbl.find(k);
							if (it != tbl.end() && it->second.kind == SymKind::Fn) {
								it->second.overloads.push_back(method.get());
								it->second.is_pub = it->second.is_pub || is_method_pub;
							} else if (it == tbl.end()) {
								Symbol sym_entry;
								sym_entry.owner_module = m;
								sym_entry.kind		   = SymKind::Fn;
								sym_entry.simple	   = method->name;
								sym_entry.fn		   = method.get();
								sym_entry.overloads.push_back(method.get());
								sym_entry.is_pub	  = is_method_pub;
								sym_entry.loc		  = method->loc;
								sym_entry.module_path = m->mod_path;
								auto res			  = tbl.emplace(k, sym_entry);
								if (method->sym == nullptr) {
									method->sym = &res.first->second;
								}
							}
						}
					}
					m->lexical_path = std::move(saved_path);
					continue;
				}

				im.resolved_target_type =
					im.target_type ? resolve_type(*im.target_type, *m) : nullptr;
				if (!im.resolved_target_type || is_poison(im.resolved_target_type)) {
					m->lexical_path = std::move(saved_path);
					continue;
				}

				if (register_impl(im, *m)) {
					std::string type_name;
					if (im.target_type && !im.target_type->single.empty()) {
						type_name = im.target_type->single;
					} else if (im.target_type && !im.target_type->name.empty()) {
						type_name = im.target_type->name;
					} else if (im.resolved_target_type) {
						type_name = im.resolved_target_type->name;
						if (is_mangled(type_name)) {
							auto segs = demangle(type_name);
							type_name = segs.empty() ? "" : segs.back();
						}
					}

					active_self_type_ = im.resolved_target_type;
					auto is_type_public = [](const StType *ty) -> bool {
						if (!ty) return false;
						if (ty->kind == TypeKind::TypeBinding && !ty->is_pub) return false;
						const StType *base = underlying_type(ty);
						while (base &&
							   (base->kind == TypeKind::Pointer || base->kind == TypeKind::Array ||
								base->kind == TypeKind::Slice)) {
							base = underlying_type(base->base);
						}
						if (!base) return false;
						switch (base->kind) {
						case TypeKind::Struct:
						case TypeKind::Union:
						case TypeKind::Enum:
							return base->is_pub;
						default:
							return true;
						}
					};
					const bool is_trait_pub = im.resolved_trait && im.resolved_trait->is_pub;
					const bool is_target_pub = is_type_public(im.resolved_target_type);
					for (auto &method : im.methods) {
						if (!method) continue;
						if (im.target_type) {
							substitute_self_in_fn(*method, *im.target_type);
						}
						method->owner_path = prefix;
						method->owner_path.push_back(im.trait_name);
						method->owner_path.push_back(type_name);
						method->decl_module = m;
						const bool is_method_pub =
							method->is_pub || (is_trait_pub && is_target_pub);
						method->is_pub = is_method_pub;
						std::vector<std::string> full = m->mod_path;
						full.insert(full.end(), method->owner_path.begin(),
									method->owner_path.end());
						full.push_back(method->name);
						method->full_path = std::move(full);
						resolve_fn_signature(*method, *m);
						method->mangled = mangle_fn(*m, *method);

						std::vector<std::string> base_keys = {
							type_name + "." + method->name,
							im.trait_name + "." + type_name + "." + method->name,
							im.trait_name + "." + method->name};
						std::vector<std::string> keys = base_keys;
						if (!prefix.empty()) {
							const std::string pfx = join_path(prefix) + ".";
							for (const auto &bk : base_keys) {
								keys.push_back(pfx + bk);
							}
						}
						for (const auto &k : keys) {
							auto it = tbl.find(k);
							if (it != tbl.end() && it->second.kind == SymKind::Fn) {
								it->second.overloads.push_back(method.get());
								it->second.is_pub = it->second.is_pub || is_method_pub;
							} else if (it == tbl.end()) {
								Symbol sym_entry;
								sym_entry.owner_module = m;
								sym_entry.kind = SymKind::Fn;
								sym_entry.simple = method->name;
								sym_entry.fn = method.get();
								sym_entry.overloads.push_back(method.get());
								sym_entry.is_pub = is_method_pub;
								sym_entry.loc = method->loc;
								sym_entry.module_path = m->mod_path;
								sym_entry.mangled = method->mangled;
								auto res = tbl.emplace(k, sym_entry);
								if (method->sym == nullptr) {
									method->sym = &res.first->second;
								}
							}
						}
					}
					validate_impl_signature(im, *m);
					active_self_type_ = nullptr;
				}
				m->lexical_path = std::move(saved_path);
			}
		};
		validate_decls(m->decls, {});
	}
}

} // namespace stellac
