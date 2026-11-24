#include "ast.hpp"

#include <iomanip>
#include <sstream>

namespace stellac {

namespace {

std::string quote_string(std::string_view value, char quote) {
	std::string out;
	out.push_back(quote);
	for (unsigned char c : value) {
		switch (c) {
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\t':
			out += "\\t";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\0':
			out += "\\0";
			break;
		case '\a':
			out += "\\a";
			break;
		case '\b':
			out += "\\b";
			break;
		case '\f':
			out += "\\f";
			break;
		case '\v':
			out += "\\v";
			break;
		default:
			if (c == static_cast<unsigned char>(quote)) {
				out.push_back('\\');
			}
			if (c < 0x20 || c == 0x7f) {
				std::ostringstream hex;
				hex << "\\x" << std::hex << std::setw(2) << std::setfill('0')
					<< static_cast<unsigned>(c);
				out += hex.str();
			} else {
				out.push_back(static_cast<char>(c));
			}
			break;
		}
	}
	out.push_back(quote);
	return out;
}

std::string stringify_type(const TypeRef &type);
std::string stringify_expr_impl(const Expr &expr, int parent_precedence = 0,
								bool right_child = false);
std::string stringify_stmt_impl(const Stmt &stmt);

std::string stringify_generic_args(const std::vector<GenericArg> &args) {
	if (args.empty()) {
		return {};
	}
	std::string out = "<";
	for (size_t i = 0; i < args.size(); ++i) {
		if (i != 0) {
			out += ", ";
		}
		if (args[i].type) {
			out += stringify_type(*args[i].type);
		} else if (args[i].value) {
			out += stringify_expr_impl(*args[i].value);
		}
	}
	out += ">";
	return out;
}

std::string stringify_type(const TypeRef &type) {
	switch (type.kind) {
	case TypeRef::Named:
		return (type.name.empty() ? type.single : type.name) +
			stringify_generic_args(type.generic_args);
	case TypeRef::SelfType:
		return "Self";
	case TypeRef::Pointer: {
		std::string out = "*";
		if (type.is_mutable) {
			out += "mut ";
		}
		if (type.is_volatile) {
			out += "volatile ";
		}
		return out + (type.base ? stringify_type(*type.base) : "void");
	}
	case TypeRef::Array: {
		std::string out = "[";
		if (type.array_len_inferred) {
			out += "_]";
		} else if (type.array_len_expr) {
			out += stringify_expr_impl(*type.array_len_expr) + "]";
		} else {
			out += std::to_string(type.array_len) + "]";
		}
		return out + (type.base ? stringify_type(*type.base) : "void");
	}
	case TypeRef::Slice: {
		std::string out = "[]";
		if (type.is_mutable) {
			out += "mut ";
		}
		if (type.is_volatile) {
			out += "volatile ";
		}
		return out + (type.base ? stringify_type(*type.base) : "void");
	}
	case TypeRef::Function: {
		std::string out = "fn(";
		for (size_t i = 0; i < type.fn_params.size(); ++i) {
			if (i != 0) {
				out += ", ";
			}
			if (type.fn_params[i]) {
				out += stringify_type(*type.fn_params[i]);
			}
		}
		if (type.fn_variadic) {
			if (!type.fn_params.empty()) {
				out += ", ";
			}
			out += "...";
		}
		out += ")";
		if (type.fn_ret) {
			out += " -> " + stringify_type(*type.fn_ret);
		}
		return out;
	}
	case TypeRef::TypeOf:
		return "@of.type(" +
			   (type.typeof_expr ? stringify_expr_impl(*type.typeof_expr) : std::string{}) + ")";
	}
	return {};
}

std::string stringify_attributes(const AttributeList &attrs) {
	if (attrs.attrs.empty()) {
		return {};
	}
	std::string out = "#[";
	for (size_t i = 0; i < attrs.attrs.size(); ++i) {
		if (i != 0) {
			out += ", ";
		}
		const Attribute &attribute = attrs.attrs[i];
		if (attribute.negated) {
			out += '!';
		}
		out += attribute.name;
		if (!attribute.args.empty()) {
			out += '(';
			for (size_t arg = 0; arg < attribute.args.size(); ++arg) {
				if (arg != 0) {
					out += ", ";
				}
				if (arg < attribute.arg_is_string.size() && attribute.arg_is_string[arg]) {
					out += quote_string(attribute.args[arg], '"');
				} else {
					out += attribute.args[arg];
				}
			}
			out += ')';
		}
	}
	return out + "] ";
}

std::string stringify_var_decl(const VarDeclInfo &var, bool semicolon) {
	std::string out = stringify_attributes(var.attrs);
	if (var.is_const) {
		out += "const ";
	} else {
		out += var.is_mutable ? "let mut " : "let ";
	}
	if (var.is_static) {
		out += "static ";
	}
	if (var.is_thread_local) {
		out += "thread_local ";
	}
	out += var.name;
	if (var.ty) {
		out += ": " + stringify_type(*var.ty);
	}
	if (var.init) {
		out += " = " + stringify_expr_impl(*var.init);
	}
	if (semicolon) {
		out += ';';
	}
	return out;
}

std::string stringify_asm_operand(const AsmOperand &operand) {
	if (operand.is_clobber) {
		return quote_string(operand.constraint, '"');
	}
	return quote_string(operand.constraint, '"') + "(" +
		   (operand.operand ? stringify_expr_impl(*operand.operand) : std::string{}) + ")";
}

std::string stringify_stmt_impl(const Stmt &stmt) {
	std::string out;
	switch (stmt.kind) {
	case StmtKind::Block:
		out = "{";
		for (const auto &child : stmt.body) {
			if (child) {
				out += "\n" + stringify_stmt_impl(*child);
			}
		}
		if (!stmt.body.empty()) {
			out += '\n';
		}
		out += '}';
		break;
	case StmtKind::ExprStmt:
		out = stmt.expr ? stringify_expr_impl(*stmt.expr) : std::string{};
		out += ';';
		break;
	case StmtKind::VarDecl:
	case StmtKind::ConstDecl:
		for (size_t i = 0; i < stmt.vars.size(); ++i) {
			if (i != 0) {
				out += '\n';
			}
			out += stringify_var_decl(stmt.vars[i], true);
		}
		break;
	case StmtKind::If:
		out = "if " +
			   (stmt.cond ? stringify_expr_impl(*stmt.cond) : std::string{}) + " " +
			   (stmt.then_block ? stringify_stmt_impl(*stmt.then_block) : "{}");
		if (stmt.else_block) {
			out += " else ";
			out += stringify_stmt_impl(*stmt.else_block);
		}
		break;
	case StmtKind::For:
		out = "for ";
		if (stmt.for_init && !stmt.for_init->vars.empty()) {
			out += stringify_var_decl(stmt.for_init->vars.front(), false) + ";";
			if (stmt.for_cond) {
				out += " " + stringify_expr_impl(*stmt.for_cond) + ";";
			} else if (stmt.for_step) {
				out += " ;";
			}
			if (stmt.for_step) {
				out += " " + stringify_expr_impl(*stmt.for_step);
			}
		} else {
			if (stmt.for_cond) {
				out += stringify_expr_impl(*stmt.for_cond);
			}
			if (stmt.for_step) {
				out += "; " + stringify_expr_impl(*stmt.for_step);
			}
		}
		out += " " + (stmt.loop_body ? stringify_stmt_impl(*stmt.loop_body) : "{}");
		break;
	case StmtKind::Match:
		out = "match " +
			   (stmt.scrutinee ? stringify_expr_impl(*stmt.scrutinee) : std::string{}) + " {";
		for (const MatchArm &arm : stmt.arms) {
			out += "\n";
			for (size_t pi = 0; pi < arm.patterns.size(); ++pi) {
				if (pi != 0) out += " | ";
				const MatchPattern &pat = arm.patterns[pi];
				if (pat.kind == MatchPatternKind::Wildcard) {
					out += "_";
				} else if (pat.kind == MatchPatternKind::Value) {
					out += pat.expr ? stringify_expr_impl(*pat.expr) : "";
				} else {
					out += pat.expr ? stringify_expr_impl(*pat.expr) : "";
					if (pat.is_struct_pattern) {
						out += " { ";
						for (size_t bi = 0; bi < pat.bindings.size(); ++bi) {
							if (bi != 0) out += ", ";
							const auto &b = pat.bindings[bi];
							out += b.field_name;
							if (!b.field_name.empty() && b.name != b.field_name) {
								out += ": ";
								if (b.is_mut) out += "mut ";
								out += b.name;
							} else if (b.is_mut) {
								out += ": mut " + b.name;
							}
						}
						out += " }";
					} else {
						out += "(";
						for (size_t bi = 0; bi < pat.bindings.size(); ++bi) {
							if (bi != 0) out += ", ";
							const auto &b = pat.bindings[bi];
							if (b.is_mut) out += "mut ";
							out += b.name;
						}
						out += ")";
					}
				}
			}
			out += " => ";
			out += arm.body ? stringify_stmt_impl(*arm.body) : "{}";
		}
		if (!stmt.arms.empty()) {
			out += '\n';
		}
		out += '}';
		break;
	case StmtKind::Return:
		out = "return";
		if (stmt.ret_val) {
			out += " " + stringify_expr_impl(*stmt.ret_val);
		}
		out += ';';
		break;
	case StmtKind::Unreachable:
		out = "unreachable;";
		break;
	case StmtKind::Break:
		out = "break;";
		break;
	case StmtKind::Continue:
		out = "continue;";
		break;
	case StmtKind::Defer:
		out = "defer " + (stmt.defer_body ? stringify_stmt_impl(*stmt.defer_body) : "{}");
		break;
	case StmtKind::Yield:
		out = "yield";
		if (stmt.ret_val) {
			out += " " + stringify_expr_impl(*stmt.ret_val);
		}
		out += ';';
		break;
	case StmtKind::Asm:
		out = stmt.asm_volatile ? "asm volatile { " : "asm { ";
		out += quote_string(stmt.asm_code, '"');
		if (!stmt.asm_outputs.empty() || !stmt.asm_inputs.empty() || !stmt.asm_clobbers.empty()) {
			out += " : ";
			for (size_t i = 0; i < stmt.asm_outputs.size(); ++i) {
				if (i != 0) out += ", ";
				out += stringify_asm_operand(stmt.asm_outputs[i]);
			}
		}
		if (!stmt.asm_inputs.empty() || !stmt.asm_clobbers.empty()) {
			out += " : ";
			for (size_t i = 0; i < stmt.asm_inputs.size(); ++i) {
				if (i != 0) out += ", ";
				out += stringify_asm_operand(stmt.asm_inputs[i]);
			}
		}
		if (!stmt.asm_clobbers.empty()) {
			out += " : ";
			for (size_t i = 0; i < stmt.asm_clobbers.size(); ++i) {
				if (i != 0) out += ", ";
				out += quote_string(stmt.asm_clobbers[i], '"');
			}
		}
		out += " };";
		break;
	case StmtKind::Import:
		out = "import ";
		if (!stmt.import_group.empty()) {
			out += '{';
			for (size_t i = 0; i < stmt.import_group.size(); ++i) {
				if (i != 0) out += ", ";
				for (size_t j = 0; j < stmt.import_group[i].size(); ++j) {
					if (j != 0) out += '.';
					out += stmt.import_group[i][j];
				}
			}
			out += '}';
		} else {
			for (size_t i = 0; i < stmt.import_path.size(); ++i) {
				if (i != 0) out += '.';
				out += stmt.import_path[i];
			}
		}
		out += ';';
		break;
	}
	return out;
}

int precedence(const Expr &expr) {
	if (expr.kind == ExprKind::Assign) {
		return 1;
	}
	if (expr.kind == ExprKind::Conditional) {
		return 2;
	}
	if (expr.kind != ExprKind::Binary) {
		return 14;
	}
	if (expr.str_val == "||") {
		return 3;
	}
	if (expr.str_val == "&&") {
		return 4;
	}
	if (expr.str_val == "|") {
		return 5;
	}
	if (expr.str_val == "^") {
		return 6;
	}
	if (expr.str_val == "&") {
		return 7;
	}
	if (expr.str_val == "==" || expr.str_val == "!=") {
		return 8;
	}
	if (expr.str_val == "<" || expr.str_val == "<=" || expr.str_val == ">" ||
		expr.str_val == ">=") {
		return 9;
	}
	if (expr.str_val == "<<" || expr.str_val == ">>") {
		return 10;
	}
	if (expr.str_val == "+" || expr.str_val == "-") {
		return 11;
	}
	if (expr.str_val == "*" || expr.str_val == "/" || expr.str_val == "%") {
		return 12;
	}
	return 14;
}

std::string parenthesize_if_needed(const Expr &expr, std::string value, int parent,
								   bool right_child) {
	int current = precedence(expr);
	if (current < parent || (right_child && current == parent &&
							 (expr.kind == ExprKind::Binary || expr.kind == ExprKind::Assign))) {
		return "(" + value + ")";
	}
	return value;
}

std::string stringify_expr_impl(const Expr &e, int parent_precedence, bool right_child) {
	std::string out;
	switch (e.kind) {
	case ExprKind::IntLit:
		out = e.str_val.empty() ? std::to_string(e.int_val) : e.str_val;
		break;
	case ExprKind::FloatLit:
		out = e.str_val.empty() ? std::to_string(e.flt_val) : e.str_val;
		break;
	case ExprKind::StringLit:
		out = (e.is_cstring ? std::string("c") : std::string()) + quote_string(e.str_val, '"');
		break;
	case ExprKind::CharLit:
		out = quote_string(std::string(1, static_cast<char>(e.char_val)), '\'');
		break;
	case ExprKind::BoolLit:
		out = e.const_bool_val ? "true" : "false";
		break;
	case ExprKind::NullLit:
		out = "null";
		break;
	case ExprKind::Ident:
		for (size_t i = 0; i < e.path.size(); ++i) {
			if (i != 0) {
				out += ".";
			}
			out += e.path[i];
		}
		out += stringify_generic_args(e.generic_args);
		break;
	case ExprKind::EnumMember:
		out = "." + e.member;
		break;
	case ExprKind::DefineRef:
		out = "#" + e.str_val;
		break;
	case ExprKind::TypeValue:
		out = "type";
		break;
	case ExprKind::Unary:
		out = e.str_val + (e.lhs ? stringify_expr_impl(*e.lhs, 13) : std::string{});
		break;
	case ExprKind::AddrOf:
		out = "&" + (e.lhs ? stringify_expr_impl(*e.lhs, 13) : std::string{});
		break;
	case ExprKind::Deref:
		out = "*" + (e.lhs ? stringify_expr_impl(*e.lhs, 13) : std::string{});
		break;
	case ExprKind::Binary:
		out = (e.lhs ? stringify_expr_impl(*e.lhs, precedence(e)) : std::string{}) + " " +
			  e.str_val + " " +
			  (e.rhs ? stringify_expr_impl(*e.rhs, precedence(e), true) : std::string{});
		break;
	case ExprKind::Assign:
		out = (e.lhs ? stringify_expr_impl(*e.lhs, precedence(e)) : std::string{}) +
			  (e.is_compound_assignment && e.rhs && e.rhs->kind == ExprKind::Binary
				   ? " " + e.rhs->str_val + "= "
				   : " = ") +
			  (e.is_compound_assignment && e.rhs && e.rhs->kind == ExprKind::Binary && e.rhs->rhs
				   ? stringify_expr_impl(*e.rhs->rhs, precedence(e), true)
				   : (e.rhs ? stringify_expr_impl(*e.rhs, precedence(e), true) : std::string{}));
		break;
	case ExprKind::Conditional:
		out =
			(e.condition ? stringify_expr_impl(*e.condition, precedence(e)) : std::string{}) +
			" ? " + (e.then_expr ? stringify_expr_impl(*e.then_expr) : std::string{}) + " : " +
			(e.else_expr ? stringify_expr_impl(*e.else_expr, precedence(e), true) : std::string{});
		break;
	case ExprKind::Member:
		out = (e.lhs ? stringify_expr_impl(*e.lhs, 14) : std::string{}) + "." + e.member;
		break;
	case ExprKind::Index:
		out = (e.lhs ? stringify_expr_impl(*e.lhs, 14) : std::string{}) + "[" +
			  (e.rhs ? stringify_expr_impl(*e.rhs) : std::string{});
		if (e.is_bit_range) {
			out += ":" + (e.index_low ? stringify_expr_impl(*e.index_low) : std::string{});
		}
		out += "]";
		break;
	case ExprKind::Slice:
		out = (e.lhs ? stringify_expr_impl(*e.lhs, 14) : std::string{}) + "[" +
			  (e.rhs ? stringify_expr_impl(*e.rhs) : std::string{}) + ".." +
			  (e.index_low ? stringify_expr_impl(*e.index_low) : std::string{}) + "]";
		break;
	case ExprKind::Call:
		out = e.lhs ? stringify_expr_impl(*e.lhs, 14) : std::string{};
		out += stringify_generic_args(e.generic_args) + "(";
		for (size_t i = 0; i < e.args.size(); ++i) {
			if (i != 0) {
				out += ", ";
			}
			if (e.args[i]) {
				out += stringify_expr_impl(*e.args[i]);
			}
		}
		out += ")";
		break;
	case ExprKind::ArrayLit:
		out = "[";
		for (size_t i = 0; i < e.args.size(); ++i) {
			if (i != 0) {
				out += ", ";
			}
			if (e.args[i]) {
				out += stringify_expr_impl(*e.args[i]);
			}
		}
		out += "]";
		break;
	case ExprKind::VariantInit:
		out = (e.lhs ? stringify_expr_impl(*e.lhs) : std::string{}) + "." + e.member + " {";
		for (size_t i = 0; i < e.args.size() && i < e.field_names.size(); ++i) {
			if (i != 0) out += ", ";
			out += " " + e.field_names[i] + ": " +
				   (e.args[i] ? stringify_expr_impl(*e.args[i]) : std::string{});
		}
		out += " }";
		break;
	case ExprKind::StructLit:
		out = "{";
		for (size_t i = 0; i < e.args.size() && i < e.field_names.size(); ++i) {
			if (i != 0) {
				out += ", ";
			}
			out += "." + e.field_names[i] + " = ";
			if (e.args[i]) {
				out += stringify_expr_impl(*e.args[i]);
			}
		}
		out += "}";
		break;
	case ExprKind::Cast:
		out = (e.lhs ? stringify_expr_impl(*e.lhs, 14) : std::string{}) +
			  (e.is_bitcast ? " bitcast " : " cast ") +
			  (e.cast_type ? stringify_type(*e.cast_type) : std::string{});
		break;
	case ExprKind::Sizeof:
	case ExprKind::Alignof:
	case ExprKind::Bitsof:
	case ExprKind::Offsetof: {
		const char *name =
			e.kind == ExprKind::Sizeof
				? "size"
				: (e.kind == ExprKind::Alignof ? "align"
											   : (e.kind == ExprKind::Bitsof ? "bits" : "offset"));
		out = std::string("@of.") + name + "(" +
			  (e.lhs ? stringify_expr_impl(*e.lhs) : std::string{});
		if (e.kind == ExprKind::Offsetof) {
			out += ", ." + e.offsetof_member;
		}
		out += ")";
		break;
	}
	case ExprKind::VaStart:
		out =
			"@va.start(" + (e.args.empty() ? std::string{} : stringify_expr_impl(*e.args[0])) + ")";
		break;
	case ExprKind::VaCopy:
		out = "@va.copy(" + (e.args.empty() ? std::string{} : stringify_expr_impl(*e.args[0])) +
			  ", " + (e.args.size() < 2 ? std::string{} : stringify_expr_impl(*e.args[1])) + ")";
		break;
	case ExprKind::VaEnd:
		out = "@va.end(" + (e.args.empty() ? std::string{} : stringify_expr_impl(*e.args[0])) + ")";
		break;
	case ExprKind::VaArg:
		out = "@va.arg(" + (e.args.empty() ? std::string{} : stringify_expr_impl(*e.args[0])) +
			  ", " + (e.va_arg_ty ? stringify_type(*e.va_arg_ty) : std::string{}) + ")";
		break;
	case ExprKind::VariadicSplice:
		out = "...";
		break;
	case ExprKind::SrcFile:
		out = "@src.file";
		break;
	case ExprKind::SrcLine:
		out = "@src.line";
		break;
	case ExprKind::SrcFn:
		out = "@src.fn";
		break;
	case ExprKind::SrcColumn:
		out = "@src.column";
		break;
	case ExprKind::SrcEmbed:
		out = "@src.embed(" +
			  (e.args.empty() || !e.args.front() ? std::string{} : stringify_expr_impl(*e.args.front())) +
			  ")";
		break;
	case ExprKind::Stringify:
		out = "@syn.stringify(" +
			  (e.args.empty() ? std::string{} : stringify_expr_impl(*e.args.front())) + ")";
		break;
	case ExprKind::DiagWarn:
	case ExprKind::DiagErr: {
		const char *name = e.kind == ExprKind::DiagErr ? "@diag.err" : "@diag.warn";
		out = std::string(name) + "(";
		for (size_t i = 0; i < e.args.size(); ++i) {
			if (i != 0) {
				out += ", ";
			}
			if (e.args[i]) {
				out += stringify_expr_impl(*e.args[i]);
			}
		}
		out += ")";
		break;
	}
	case ExprKind::Block:
		out = e.block ? stringify_stmt_impl(*e.block) : "{}";
		break;
	}
	if (e.parenthesized) {
		return "(" + out + ")";
	}
	return parenthesize_if_needed(e, std::move(out), parent_precedence, right_child);
}

} // namespace

std::string stringify_expr(const Expr &e) {
	return stringify_expr_impl(e);
}

} // namespace stellac
