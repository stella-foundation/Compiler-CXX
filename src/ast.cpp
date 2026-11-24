#include "ast.hpp"

#include <initializer_list>

namespace stellac {

Expr::~Expr() = default;
Stmt::~Stmt() = default;

namespace {

constexpr uint32_t target(AttributeTarget t) {
	return static_cast<uint32_t>(t);
}

constexpr uint32_t targets(std::initializer_list<AttributeTarget> values) {
	uint32_t result = 0;
	for (AttributeTarget value : values) {
		result |= target(value);
	}
	return result;
}

const AttributeSpec kAttributeSpecs[] = {
	{"link_name", targets({AttributeTarget::Function, AttributeTarget::Global}),
	 AttributePolarity::PositiveOnly, AttributeArgKind::OptionalString},
	{"abi", targets({AttributeTarget::Function, AttributeTarget::FunctionType}),
	 AttributePolarity::PositiveOnly, AttributeArgKind::String},
	{"repr", targets({AttributeTarget::Struct, AttributeTarget::Union, AttributeTarget::Enum}),
	 AttributePolarity::PositiveOnly, AttributeArgKind::String},
	{"inline", targets({AttributeTarget::Function}), AttributePolarity::Either,
	 AttributeArgKind::None},
	{"cold", targets({AttributeTarget::Function}), AttributePolarity::PositiveOnly,
	 AttributeArgKind::None},
	{"hot", targets({AttributeTarget::Function}), AttributePolarity::PositiveOnly,
	 AttributeArgKind::None},
	{"naked", targets({AttributeTarget::Function}), AttributePolarity::PositiveOnly,
	 AttributeArgKind::None},
	{"section", targets({AttributeTarget::Function, AttributeTarget::Global}),
	 AttributePolarity::PositiveOnly, AttributeArgKind::String},
	{"used", targets({AttributeTarget::Function, AttributeTarget::Global}),
	 AttributePolarity::PositiveOnly, AttributeArgKind::None},
	{"must_tail", targets({AttributeTarget::Function}), AttributePolarity::PositiveOnly,
	 AttributeArgKind::None},
	{"align", targets({AttributeTarget::Struct, AttributeTarget::Union, AttributeTarget::Enum, AttributeTarget::Global}),
	 AttributePolarity::PositiveOnly, AttributeArgKind::Integer},
	{"layout", targets({AttributeTarget::Struct, AttributeTarget::Enum}), AttributePolarity::PositiveOnly,
	 AttributeArgKind::Layout},
	{"return", targets({AttributeTarget::Function}), AttributePolarity::NegativeOnly,
	 AttributeArgKind::None},
	{"alias", targets({AttributeTarget::Parameter}), AttributePolarity::NegativeOnly,
	 AttributeArgKind::None},
	{"sanitize", targets({AttributeTarget::Function}), AttributePolarity::NegativeOnly,
	 AttributeArgKind::SanitizerSelector},
};

} // namespace

const AttributeSpec *lookup_attribute_spec(std::string_view name) {
	for (const auto &spec : kAttributeSpecs) {
		if (spec.name == name) {
			return &spec;
		}
	}
	return nullptr;
}

bool attribute_target_allowed(const AttributeSpec &spec, AttributeTarget target_kind) {
	return (spec.targets & static_cast<uint32_t>(target_kind)) != 0;
}

const Attribute *find_attr(const AttributeList &al, std::string_view name) {
	for (const auto &a : al.attrs) {
		if (!a.negated && a.name == name) {
			return &a;
		}
	}
	return nullptr;
}

const Attribute *find_negated_attr(const AttributeList &al, std::string_view name) {
	for (const auto &a : al.attrs) {
		if (a.negated && a.name == name) {
			return &a;
		}
	}
	return nullptr;
}

bool has_attr(const AttributeList &al, std::string_view name) {
	return find_attr(al, name) != nullptr;
}

bool has_negated_attr(const AttributeList &al, std::string_view name) {
	return find_negated_attr(al, name) != nullptr;
}

std::string attr_string_or(const AttributeList &al, std::string_view name, std::string def) {
	const Attribute *a = find_attr(al, name);
	if (!a || a->args.empty()) {
		return def;
	}
	return a->args[0];
}

std::string attr_diag_prefix(const Attribute &a) {
	return std::string("attribute '#[") + (a.negated ? "!" : "") + a.name + "]'";
}

bool is_define_cond(const Expr &e) {
	if (e.kind == ExprKind::DefineRef) {
		return true;
	}
	if (e.kind == ExprKind::Unary && e.str_val == "!") {
		return e.lhs && is_define_cond(*e.lhs);
	}
	if (e.kind == ExprKind::Binary && (e.str_val == "&&" || e.str_val == "||")) {
		return e.lhs && is_define_cond(*e.lhs) && e.rhs && is_define_cond(*e.rhs);
	}
	return false;
}

bool eval_define_cond(const Expr &e, const std::unordered_map<std::string, bool> &flags) {
	if (e.kind == ExprKind::DefineRef) {
		auto it = flags.find(e.str_val);
		return it != flags.end() ? it->second : false;
	}
	if (e.kind == ExprKind::Unary && e.str_val == "!") {
		return e.lhs && !eval_define_cond(*e.lhs, flags);
	}
	if (e.kind == ExprKind::Binary) {
		bool l = e.lhs && eval_define_cond(*e.lhs, flags);
		bool r = e.rhs && eval_define_cond(*e.rhs, flags);
		if (e.str_val == "&&") {
			return l && r;
		}
		if (e.str_val == "||") {
			return l || r;
		}
	}
	return false;
}

} // namespace stellac
