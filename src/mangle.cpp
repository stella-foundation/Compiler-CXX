#include "mangle.hpp"
#include "ast.hpp"
#include "symbol.hpp"

namespace stellac {

namespace {

void append_escaped_string(std::string &out, std::string_view s) {
	static constexpr char hex[] = "0123456789abcdef";
	out.push_back('"');
	for (unsigned char c : s) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (c < 0x20 || c == 0x7f) {
				out += "\\x";
				out.push_back(hex[c >> 4]);
				out.push_back(hex[c & 0x0f]);
			} else {
				out.push_back(static_cast<char>(c));
			}
			break;
		}
	}
	out.push_back('"');
}

} // namespace

bool is_mangled(std::string_view sym) {
	return sym.size() >= 3 && sym[0] == 's' && sym[1] == 't' && sym[2] == '$';
}

std::string join_path(const std::vector<std::string> &segs) {
	std::string r;
	for (size_t i = 0; i < segs.size(); ++i) {
		if (i) {
			r += ".";
		}
		r += segs[i];
	}
	return r;
}

std::vector<std::string> split_path(std::string_view dotted) {
	std::vector<std::string> out;
	size_t					 begin = 0;
	while (begin < dotted.size()) {
		size_t end = dotted.find('.', begin);
		if (end == std::string_view::npos) {
			end = dotted.size();
		}
		if (end > begin) {
			out.emplace_back(dotted.substr(begin, end - begin));
		}
		if (end == dotted.size()) {
			break;
		}
		begin = end + 1;
	}
	return out;
}

static std::vector<std::string> identity_prefix(const Module &m) {
	if (!m.mod_path.empty()) {
		return m.mod_path;
	}
	// A headerless source file contributes to the global namespace. Its
	// filesystem path only controls import resolution and must not become part
	// of the externally visible symbol identity.
	return {};
}

std::string mangle(const std::vector<std::string> &segs) {
	return "st$" + join_path(segs);
}

std::string mangle_in(const Module &m, std::string_view name) {
	std::vector<std::string> segs = identity_prefix(m);
	segs.emplace_back(name);
	return mangle(segs);
}

std::vector<std::string> demangle(std::string_view sym) {
	if (!is_mangled(sym)) {
		return {};
	}
	std::string_view body = sym.substr(3);
	const size_t	 end  = body.find_first_of("<!(#");
	if (end != std::string_view::npos) {
		body = body.substr(0, end);
	}
	return split_path(body);
}

std::string mangle_type(const StType *t) {
	t = underlying_type(t);
	if (!t) {
		return "unk";
	}
	switch (t->kind) {
	case TypeKind::Void:
		return "void";
	case TypeKind::Bool:
		return "bool";
	case TypeKind::UInt:
		return t->name == "usize" ? "usize" : "u" + std::to_string(t->width);
	case TypeKind::SInt:
		return t->name == "isize" ? "isize" : "i" + std::to_string(t->width);
	case TypeKind::Float:
		return "f" + std::to_string(t->width);
	case TypeKind::Pointer: {
		std::string r = "*";
		if (t->is_mutable || t->is_volatile) {
			if (t->is_mutable) {
				r.push_back('m');
			}
			if (t->is_volatile) {
				r.push_back('v');
			}
			r.push_back(':');
		}
		r += mangle_type(t->base);
		return r;
	}
	case TypeKind::Array:
		return "[" + std::to_string(t->array_len) + "]" + mangle_type(t->base);
	case TypeKind::Slice: {
		std::string r = "[]";
		if (t->is_mutable || t->is_volatile) {
			if (t->is_mutable) {
				r.push_back('m');
			}
			if (t->is_volatile) {
				r.push_back('v');
			}
			r.push_back(':');
		}
		r += mangle_type(t->base);
		return r;
	}
	case TypeKind::Struct:
	case TypeKind::Union:
	case TypeKind::Enum:
	case TypeKind::Opaque:
		return is_mangled(t->name) ? t->name.substr(3) : t->name;
	case TypeKind::TypeBinding:
		return t->base ? mangle_type(t->base) : "unk";
	case TypeKind::VaList:
		return "@va.list";
	case TypeKind::Poison:
		return "poison";
	case TypeKind::Function: {
		std::string r = t->fn_abi == AbiKind::C ? "abi(C)fn(" : "fn(";
		for (size_t i = 0; i < t->fn_params.size(); ++i) {
			if (i > 0) {
				r.push_back(',');
			}
			r += mangle_type(t->fn_params[i]);
		}
		if (t->fn_variadic) {
			if (!t->fn_params.empty()) {
				r.push_back(',');
			}
			r += "...";
		}
		r += ")->" + mangle_type(t->fn_ret);
		return r;
	}
	case TypeKind::Type:
		return "type";
	case TypeKind::SelfType:
		return "Self";
	}
	return "unk";
}

std::string mangle_generic(std::string_view					   base_mangled,
						   const std::vector<GenericMangleArg> &args,
						   bool								   is_macro) {
	std::string out(base_mangled);
	out += is_macro ? "!<" : "<";
	for (size_t i = 0; i < args.size(); ++i) {
		if (i > 0) {
			out.push_back(',');
		}
		const auto &arg = args[i];
		switch (arg.kind) {
		case GenericMangleArg::Kind::Type:
			out += mangle_type(arg.type);
			break;
		case GenericMangleArg::Kind::ConstInt:
			out += std::to_string(arg.int_val);
			break;
		case GenericMangleArg::Kind::ConstString:
			append_escaped_string(out, arg.str_val);
			break;
		}
	}
	out.push_back('>');
	return out;
}

std::string mangle_fn(const Module &m, const FnDecl &f) {
	const Attribute *ln		   = find_attr(f.attrs, "link_name");
	std::string		 link_name = ln && !ln->args.empty() ? ln->args[0] : std::string{};
	if (ln) {
		return link_name.empty() ? f.name : link_name;
	}

	std::vector<std::string> segs = identity_prefix(m);
	if (f.full_path.empty() || m.mod_path.empty()) {
		for (auto &o : f.owner_path) {
			segs.push_back(o);
		}
		segs.push_back(f.name);
	} else {
		segs = f.full_path;
	}
	std::string base = mangle(segs);

	// Every function includes its parameter list `(...)` so functions are
	// visually distinct from types/globals and overloads never collide:
	//
	//     st$std.math.pow(f64,f64)
	//     st$main.init()
	base.push_back('(');
	for (size_t i = 0; i < f.params.size(); ++i) {
		if (i > 0) {
			base.push_back(',');
		}
		base += mangle_type(f.params[i].resolved);
	}
	if (f.is_variadic) {
		if (!f.params.empty()) {
			base.push_back(',');
		}
		base += "...";
	}
	base.push_back(')');
	if (!f.is_pub && !f.is_extern && !f.is_generic_instance) {
		base += private_global_scope_suffix(m);
	}
	return base;
}

std::string mangle_global(const Module &m, const GlobalDecl &g) {
	const Attribute *ln = find_attr(g.attrs, "link_name");
	if (ln) {
		return ln->args.empty() ? g.name : ln->args[0];
	}
	std::vector<std::string> segs = identity_prefix(m);
	segs.insert(segs.end(), g.owner_path.begin(), g.owner_path.end());
	segs.push_back(g.name);
	std::string result = mangle(segs);
	if (!g.is_pub && !g.is_extern) {
		// Private globals can be cloned into an importing LLVM module for
		// optimization. Scope them by the owning source file's path relative to
		// the entry source directory, not by resolver order or checkout location.
		// This keeps same-named declarations distinct in one LLVM module while
		// preserving symbol names when the source tree moves.
		result += private_global_scope_suffix(m);
	}
	return result;
}

std::string private_global_scope_suffix(const Module &m) {
	return "#priv:" + m.private_symbol_module_id;
}

} // namespace stellac
