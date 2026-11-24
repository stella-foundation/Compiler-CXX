#include "internal.hpp"

namespace stellac {

std::unique_ptr<Expr> Parser::parse_expr() {
	return parse_assign();
}

std::unique_ptr<Expr> Parser::parse_assign() {
	auto lhs = parse_conditional();
	if (at(TokenKind::Assign)) {
		auto e	= std::make_unique<Expr>();
		e->kind = ExprKind::Assign;
		e->loc	= peek().loc;
		consume();
		auto rhs = parse_assign();
		e->lhs	 = std::move(lhs);
		e->rhs	 = std::move(rhs);
		return e;
	}
	// Compound assignments (`+=` `-=` `*=` `/=` `%=` `&=` `|=` `^=` `<<=` `>>=`)
	// are lexed as a single Operator token with compound_assignment=true; here
	// they desugar to `lhs = lhs <op> rhs`. The rhs is a Binary node carrying
	// the plain operator string so sema/codegen need no compound-aware logic.
	if (peek().kind == TokenKind::Operator && peek().compound_assignment) {
		SourceLoc	oploc = peek().loc;
		const char *op	  = op_text(peek().op);
		consume();
		auto rhs	 = parse_assign();
		auto bin	 = std::make_unique<Expr>();
		bin->kind	 = ExprKind::Binary;
		bin->loc	 = oploc;
		bin->str_val = op;
		bin->lhs	 = clone_expr(*lhs);
		bin->rhs	 = std::move(rhs);
		auto e		 = std::make_unique<Expr>();
		e->kind		 = ExprKind::Assign;
		e->loc		 = oploc;
		e->is_compound_assignment = true;
		e->lhs		 = std::move(lhs);
		e->rhs		 = std::move(bin);
		return e;
	}
	return lhs;
}

std::unique_ptr<Expr> Parser::parse_conditional() {
	auto condition = parse_logic_or();
	if (!accept(TokenKind::Question)) {
		return condition;
	}

	auto e		 = std::make_unique<Expr>();
	e->kind		 = ExprKind::Conditional;
	e->loc		 = condition ? condition->loc : peek().loc;
	e->condition = std::move(condition);
	// The middle operand is a full expression, as in C. The ':' token
	// terminates it naturally, while the final operand is conditional so
	// nesting associates from right to left.
	e->then_expr = parse_expr();
	expect(TokenKind::Colon, "':' in conditional expression");
	e->else_expr = parse_conditional();
	return e;
}

std::unique_ptr<Expr> Parser::parse_logic_or() {
	auto lhs = parse_logic_and();
	while (at_binop(Op::LogicalOr)) {
		auto e	   = std::make_unique<Expr>();
		e->kind	   = ExprKind::Binary;
		e->loc	   = peek().loc;
		e->str_val = "||";
		consume();
		e->lhs = std::move(lhs);
		e->rhs = parse_logic_and();
		lhs	   = std::move(e);
	}
	return lhs;
}
std::unique_ptr<Expr> Parser::parse_logic_and() {
	auto lhs = parse_bitor();
	while (at_binop(Op::LogicalAnd)) {
		auto e	   = std::make_unique<Expr>();
		e->kind	   = ExprKind::Binary;
		e->loc	   = peek().loc;
		e->str_val = "&&";
		consume();
		e->lhs = std::move(lhs);
		e->rhs = parse_bitor();
		lhs	   = std::move(e);
	}
	return lhs;
}
std::unique_ptr<Expr> Parser::parse_bitor() {
	auto lhs = parse_bitxor();
	while (at_binop(Op::BitOr)) {
		auto e	   = std::make_unique<Expr>();
		e->kind	   = ExprKind::Binary;
		e->loc	   = peek().loc;
		e->str_val = "|";
		consume();
		e->lhs = std::move(lhs);
		e->rhs = parse_bitxor();
		lhs	   = std::move(e);
	}
	return lhs;
}
std::unique_ptr<Expr> Parser::parse_bitxor() {
	auto lhs = parse_bitand();
	while (at_binop(Op::BitXor)) {
		auto e	   = std::make_unique<Expr>();
		e->kind	   = ExprKind::Binary;
		e->loc	   = peek().loc;
		e->str_val = "^";
		consume();
		e->lhs = std::move(lhs);
		e->rhs = parse_bitand();
		lhs	   = std::move(e);
	}
	return lhs;
}
std::unique_ptr<Expr> Parser::parse_bitand() {
	auto lhs = parse_equality();
	while (at_binop(Op::BitAnd)) {
		auto e	   = std::make_unique<Expr>();
		e->kind	   = ExprKind::Binary;
		e->loc	   = peek().loc;
		e->str_val = "&";
		consume();
		e->lhs = std::move(lhs);
		e->rhs = parse_equality();
		lhs	   = std::move(e);
	}
	return lhs;
}
std::unique_ptr<Expr> Parser::parse_equality() {
	auto lhs = parse_relational();
	while (at_binop(Op::Eq) || at_binop(Op::Ne)) {
		auto e	   = std::make_unique<Expr>();
		e->kind	   = ExprKind::Binary;
		e->loc	   = peek().loc;
		e->str_val = op_text(peek().op);
		consume();
		e->lhs = std::move(lhs);
		e->rhs = parse_relational();
		lhs	   = std::move(e);
	}
	return lhs;
}
std::unique_ptr<Expr> Parser::parse_relational() {
	auto lhs = parse_shift();
	while (at_binop(Op::Lt) ||
		   ((!parsing_generic_arg_ || generic_arg_nesting_ != 0) && at_binop(Op::Gt)) ||
		   at_binop(Op::Le) || at_binop(Op::Ge)) {
		auto e	   = std::make_unique<Expr>();
		e->kind	   = ExprKind::Binary;
		e->loc	   = peek().loc;
		e->str_val = op_text(peek().op);
		consume();
		e->lhs = std::move(lhs);
		e->rhs = parse_shift();
		lhs	   = std::move(e);
	}
	return lhs;
}
std::unique_ptr<Expr> Parser::parse_shift() {
	auto lhs = parse_additive();
	while (at_binop(Op::Shl) || at_binop(Op::Shr)) {
		auto e	   = std::make_unique<Expr>();
		e->kind	   = ExprKind::Binary;
		e->loc	   = peek().loc;
		e->str_val = op_text(peek().op);
		consume();
		e->lhs = std::move(lhs);
		e->rhs = parse_additive();
		lhs	   = std::move(e);
	}
	return lhs;
}
std::unique_ptr<Expr> Parser::parse_additive() {
	auto lhs = parse_multiplicative();
	while (at_binop(Op::Add) || at_binop(Op::Sub) || at_binop(Op::AddWrap) ||
		   at_binop(Op::SubWrap) || at_binop(Op::AddSat) || at_binop(Op::SubSat)) {
		auto e	   = std::make_unique<Expr>();
		e->kind	   = ExprKind::Binary;
		e->loc	   = peek().loc;
		e->str_val = op_text(peek().op);
		consume();
		e->lhs = std::move(lhs);
		e->rhs = parse_multiplicative();
		lhs	   = std::move(e);
	}
	return lhs;
}
std::unique_ptr<Expr> Parser::parse_multiplicative() {
	auto lhs = parse_unary();
	while (at_binop(Op::Mul) || at_binop(Op::MulWrap) || at_binop(Op::MulSat) ||
		   at_binop(Op::Div) || at_binop(Op::Mod)) {
		auto e	   = std::make_unique<Expr>();
		e->kind	   = ExprKind::Binary;
		e->loc	   = peek().loc;
		e->str_val = op_text(peek().op);
		consume();
		e->lhs = std::move(lhs);
		e->rhs = parse_unary();
		lhs	   = std::move(e);
	}
	return lhs;
}
std::unique_ptr<Expr> Parser::parse_unary() {
	if (at_binop(Op::Mul)) {
		auto e	= std::make_unique<Expr>();
		e->kind = ExprKind::Deref;
		e->loc	= peek().loc;
		consume();
		e->lhs = parse_unary();
		return e;
	}
	if (at_binop(Op::BitAnd)) {
		auto e	= std::make_unique<Expr>();
		e->kind = ExprKind::AddrOf;
		e->loc	= peek().loc;
		consume();
		e->lhs = parse_unary();
		return e;
	}
	if (at(TokenKind::Bang)) {
		auto e	   = std::make_unique<Expr>();
		e->kind	   = ExprKind::Unary;
		e->str_val = "!";
		e->loc	   = peek().loc;
		consume();
		e->lhs = parse_unary();
		return e;
	}
	if (at(TokenKind::Tilde)) {
		auto e	   = std::make_unique<Expr>();
		e->kind	   = ExprKind::Unary;
		e->str_val = "~";
		e->loc	   = peek().loc;
		consume();
		e->lhs = parse_unary();
		return e;
	}
	if (at_binop(Op::Sub)) {
		auto e	   = std::make_unique<Expr>();
		e->kind	   = ExprKind::Unary;
		e->str_val = "-";
		e->loc	   = peek().loc;
		consume();
		e->lhs = parse_unary();
		return e;
	}
	return parse_postfix();
}

std::unique_ptr<Expr> Parser::parse_builtin_call(std::string_view module, std::string_view member,
												 SourceLoc loc) {
	auto e = std::make_unique<Expr>();
	e->loc = loc;
	if (module == "@of" && member == "size") {
		e->kind = ExprKind::Sizeof;
	} else if (module == "@of" && member == "align") {
		e->kind = ExprKind::Alignof;
	} else if (module == "@of" && member == "bits") {
		e->kind = ExprKind::Bitsof;
	} else if (module == "@of" && member == "offset") {
		e->kind = ExprKind::Offsetof;
	} else if (module == "@va" && member == "start") {
		e->kind = ExprKind::VaStart;
	} else if (module == "@va" && member == "copy") {
		e->kind = ExprKind::VaCopy;
	} else if (module == "@va" && member == "end") {
		e->kind = ExprKind::VaEnd;
	} else if (module == "@va" && member == "arg") {
		e->kind = ExprKind::VaArg;
	} else if (module == "@src" && member == "file") {
		e->kind = ExprKind::SrcFile;
	} else if (module == "@src" && member == "line") {
		e->kind = ExprKind::SrcLine;
	} else if (module == "@src" && member == "fn") {
		e->kind = ExprKind::SrcFn;
	} else if (module == "@src" && member == "func") {
		e->kind = ExprKind::SrcFn;
	} else if (module == "@src" && member == "column") {
		e->kind = ExprKind::SrcColumn;
	} else if (module == "@src" && member == "embed") {
		e->kind = ExprKind::SrcEmbed;
	} else if (module == "@syn" && member == "stringify") {
		e->kind = ExprKind::Stringify;
	} else if (module == "@diag" && member == "warn") {
		e->kind = ExprKind::DiagWarn;
	} else if (module == "@diag" && member == "err") {
		e->kind = ExprKind::DiagErr;
	} else {
		log::report(log::Level::Error, loc,
					"unknown member '" + std::string(member) + "' in built-in module '" +
						std::string(module) + "'");
		return e;
	}
	consume(); // '('
	if (e->kind == ExprKind::Stringify) {
		if (at(TokenKind::RParen)) {
			log::report(log::Level::Error, peek().loc,
						"@syn.stringify expects one syntax argument");
		} else {
			e->args.push_back(parse_expr());
		}
		expect(TokenKind::RParen, "')' after '@syn.stringify'");
		return e;
	}
	if (e->kind == ExprKind::SrcEmbed) {
		if (at(TokenKind::RParen)) {
			log::report(log::Level::Error, peek().loc,
						"@src.embed expects one file path argument");
		} else {
			e->args.push_back(parse_expr());
		}
		expect(TokenKind::RParen, "')' after '@src.embed'");
		return e;
	}
	if (e->kind == ExprKind::DiagWarn || e->kind == ExprKind::DiagErr) {
		const char *builtin_name = e->kind == ExprKind::DiagErr ? "@diag.err" : "@diag.warn";
		if (at(TokenKind::RParen)) {
			log::error(peek().loc, std::string(builtin_name) + " expects two arguments: (description, message)")
				.code(log::DiagCode::ParseExpectedToken);
		} else {
			e->args.push_back(parse_expr());
			if (accept(TokenKind::Comma)) {
				if (!at(TokenKind::RParen) && !at(TokenKind::Eof)) {
					e->args.push_back(parse_expr());
				} else {
					log::error(peek().loc, std::string(builtin_name) + " expects two arguments: (description, message)")
						.code(log::DiagCode::ParseExpectedToken);
				}
			} else {
				expect(TokenKind::Comma, (std::string("',' between ") + builtin_name + " arguments").c_str());
			}
		}
		expect(TokenKind::RParen, (std::string("')' after '") + builtin_name + "'").c_str());
		return e;
	}
	if (e->kind == ExprKind::SrcFile || e->kind == ExprKind::SrcLine ||
		e->kind == ExprKind::SrcFn || e->kind == ExprKind::SrcColumn) {
		expect(TokenKind::RParen, "')' after source builtin");
		return e;
	}
	bool layout_call_recovered = false;
	auto recover_layout_call   = [&](const std::string &message) {
		log::report(log::Level::Error, peek().loc, message);
		// A malformed layout operand is one syntactic unit. Consume the rest of
		// that unit here so the closing parenthesis remains available to the
		// caller and does not trigger unrelated statement-level diagnostics.
		while (!at(TokenKind::RParen) && !at(TokenKind::Eof)) {
			consume();
		}
		if (at(TokenKind::RParen)) {
			consume();
		}
		layout_call_recovered = true;
	};
	auto parse_layout_operand = [&]() {
		if (!looks_like_compound_type_operand()) {
			return parse_expr();
		}

		auto operand		 = std::make_unique<Expr>();
		operand->kind		 = ExprKind::TypeValue;
		SourceLoc type_loc	 = peek().loc;
		operand->layout_type = parse_type();
		operand->loc		 = type_loc;
		return operand;
	};
	if (e->kind == ExprKind::Sizeof || e->kind == ExprKind::Alignof ||
		e->kind == ExprKind::Bitsof) {
		// Layout queries inspect the static type of their operand. Parsing the
		// operand as an expression lets callers use values directly, including
		// lvalues, calls, indexing, and member access. Compound array types are
		// parsed as a TypeRef only when their element type follows the closing
		// bracket, as in `[66]u8` or `[]T`.
		e->lhs = parse_layout_operand();
	} else if (e->kind == ExprKind::Offsetof) {
		e->lhs = parse_layout_operand();
		if (!accept(TokenKind::Comma)) {
			recover_layout_call("expected ',' after layout operand in '@of.offset'");
		} else if (!accept(TokenKind::Dot)) {
			recover_layout_call("expected '.' before field name in '@of.offset'");
		} else if (at(TokenKind::Ident)) {
			e->offsetof_member = consume().text;
		} else {
			recover_layout_call("expected a field after '.' in '@of.offset'");
		}
	} else if (e->kind == ExprKind::VaArg) {
		e->args.push_back(parse_expr());
		expect(TokenKind::Comma, "',' between @va.arg list and type");
		e->va_arg_ty = parse_type();
	} else if (e->kind == ExprKind::VaCopy) {
		e->args.push_back(parse_expr());
		expect(TokenKind::Comma, "',' between @va.copy destination and source");
		e->args.push_back(parse_expr());
	} else {
		e->args.push_back(parse_expr());
	}
	if (!layout_call_recovered &&
		(e->kind == ExprKind::Sizeof || e->kind == ExprKind::Alignof ||
		 e->kind == ExprKind::Bitsof) &&
		!at(TokenKind::RParen) && !at(TokenKind::Eof)) {
		recover_layout_call("expected ')' after layout operand");
	}
	if (!layout_call_recovered) {
		expect(TokenKind::RParen, "')' after built-in module member");
	}
	return e;
}

bool Parser::looks_like_compound_type_operand() const {
	if (!at(TokenKind::LBracket)) {
		return false;
	}

	// Locate the matching bracket before deciding whether this is an array
	// type. In particular, `[value][index]` must remain an expression operand,
	// while `[N][M]T` is a nested array type.
	auto matching_rbracket = [&](size_t open) {
		size_t depth = 0;
		for (size_t i = open; i < toks_.size(); ++i) {
			if (toks_[i].kind == TokenKind::LBracket) {
				++depth;
			} else if (toks_[i].kind == TokenKind::RBracket) {
				if (depth == 0) {
					return toks_.size();
				}
				--depth;
				if (depth == 0) {
					return i;
				}
			}
		}
		return toks_.size();
	};

	// This recognizes the type starters accepted by parse_type(). Recurse
	// through nested array types so postfix indexing is not misclassified as a
	// type suffix. Named types do not need further validation here; parse_type()
	// performs their complete parsing and semantic resolution.
	auto starts_type = [&](auto &&self, size_t pos) -> bool {
		while (pos < toks_.size() && toks_[pos].kind == TokenKind::Operator &&
			   toks_[pos].op == Op::Mul && !toks_[pos].compound_assignment) {
			++pos;
			while (pos < toks_.size() && (toks_[pos].kind == TokenKind::KwMut ||
										  toks_[pos].kind == TokenKind::KwVolatile)) {
				++pos;
			}
		}
		if (pos >= toks_.size()) {
			return false;
		}
		if (toks_[pos].kind == TokenKind::LBracket) {
			size_t close = matching_rbracket(pos);
			return close < toks_.size() && self(self, close + 1);
		}
		if (toks_[pos].kind == TokenKind::Ident || toks_[pos].kind == TokenKind::KwFn ||
			toks_[pos].kind == TokenKind::KwType || toks_[pos].kind == TokenKind::KwSelf) {
			return true;
		}
		return toks_[pos].kind == TokenKind::BuiltinModuleRef && toks_[pos].text == "of" &&
			   pos + 3 < toks_.size() && toks_[pos + 1].kind == TokenKind::Dot &&
			   toks_[pos + 2].kind == TokenKind::Ident && toks_[pos + 2].text == "type" &&
			   toks_[pos + 3].kind == TokenKind::LParen;
	};

	size_t close = matching_rbracket(idx_);
	return close < toks_.size() && starts_type(starts_type, close + 1);
}

std::unique_ptr<Expr> Parser::parse_postfix() {
	auto base = parse_primary();
	while (true) {
		if (generic_call_suffix()) {
			parse_generic_args(base->generic_args);
		} else if (at(TokenKind::LParen)) {
			std::string_view module;
			std::string_view member;
			if (builtin_member_path(*base, module, member)) {
				SourceLoc builtin_loc = base->lhs ? base->lhs->loc : base->loc;
				if (base->lhs && base->loc.line == base->lhs->loc.line &&
					base->loc.column >= base->lhs->loc.column) {
					builtin_loc.length = (base->loc.column + base->loc.length) - base->lhs->loc.column;
				}
				base = parse_builtin_call(module, member, builtin_loc);
				continue;
			}
			auto e	= std::make_unique<Expr>();
			e->kind = ExprKind::Call;
			e->loc	= peek().loc;
			e->lhs	= std::move(base);
			if (parsing_generic_arg_) {
				++generic_arg_nesting_;
			}
			consume();
			while (!at(TokenKind::RParen) && !at(TokenKind::Eof)) {
				e->args.push_back(parse_expr());
				if (!at(TokenKind::RParen)) {
					expect(TokenKind::Comma, "comma or ')'");
				}
			}
			expect(TokenKind::RParen, "')'");
			if (parsing_generic_arg_) {
				--generic_arg_nesting_;
			}
			base = std::move(e);
		} else if (allow_struct_init_ && at(TokenKind::LBrace) &&
				   (base->kind == ExprKind::EnumMember || base->kind == ExprKind::Member)) {
			auto e				 = std::make_unique<Expr>();
			e->kind				 = ExprKind::VariantInit;
			e->loc				 = peek().loc;
			e->is_struct_variant = true;
			if (base->kind == ExprKind::Member) {
				e->lhs	  = std::move(base->lhs);
				e->member = std::move(base->member);
				e->generic_args = std::move(base->generic_args);
			} else {
				e->member = std::move(base->member);
			}
			consume(); // '{'
			while (!at(TokenKind::RBrace) && !at(TokenKind::Eof)) {
				if (!at(TokenKind::Ident)) {
					log::report(log::Level::Error, peek().loc, "expected field name");
					break;
				}
				std::string fname = consume().text;
				expect(TokenKind::Colon, "':'");
				auto val = parse_expr();
				e->field_names.push_back(std::move(fname));
				e->args.push_back(std::move(val));
				if (!accept(TokenKind::Comma)) {
					break;
				}
			}
			expect(TokenKind::RBrace, "'}'");
			base = std::move(e);
		} else if (at(TokenKind::Dot)) {
			consume();
			bool src_fn_member = at(TokenKind::KwFn) && base && base->kind == ExprKind::Ident &&
									 base->path.size() == 1 && base->path[0] == "@src";
			if (!at(TokenKind::Ident) && !src_fn_member) {
				log::report(log::Level::Error, peek().loc, "expected member name after '.'");
				break;
			}
			auto e	  = std::make_unique<Expr>();
			e->kind	  = ExprKind::Member;
			e->loc	  = peek().loc;
			if (base && (base->kind == ExprKind::Ident || base->kind == ExprKind::Member) &&
				!base->generic_args.empty()) {
				for (const auto &ga : base->generic_args) {
					e->generic_args.push_back(clone_generic_arg(ga));
				}
			}
			e->lhs	  = std::move(base);
			if (src_fn_member) {
				consume();
				e->member = "fn";
			} else {
				e->member = consume().text;
			}
			std::string_view builtin_module;
			std::string_view builtin_member;
			if (builtin_member_path(*e, builtin_module, builtin_member) &&
				builtin_module == "@src" && at(TokenKind::LParen)) {
				base = parse_builtin_call(builtin_module, builtin_member, e->loc);
			} else if (builtin_member_path(*e, builtin_module, builtin_member) &&
					   builtin_module == "@src" &&
					   (builtin_member == "file" || builtin_member == "line" ||
						builtin_member == "fn" || builtin_member == "func" ||
						builtin_member == "column")) {
				if (builtin_member == "file") e->kind = ExprKind::SrcFile;
				else if (builtin_member == "line") e->kind = ExprKind::SrcLine;
				else if (builtin_member == "fn" || builtin_member == "func") e->kind = ExprKind::SrcFn;
				else e->kind = ExprKind::SrcColumn;
				base = std::move(e);
			} else {
				base = std::move(e);
			}
		} else if (at(TokenKind::LBracket)) {
			SourceLoc lloc = peek().loc;
			consume();
			if (accept(TokenKind::DotDot)) {
				std::unique_ptr<Expr> end;
				if (!at(TokenKind::RBracket)) {
					end = parse_expr();
				}
				expect(TokenKind::RBracket, "']'");
				auto e		 = std::make_unique<Expr>();
				e->kind		 = ExprKind::Slice;
				e->loc		 = lloc;
				e->lhs		 = std::move(base);
				e->index_low = std::move(end);
				base		 = std::move(e);
			} else {
				auto idx = parse_expr();
				if (accept(TokenKind::DotDot)) {
					std::unique_ptr<Expr> end;
					if (!at(TokenKind::RBracket)) {
						end = parse_expr();
					}
					expect(TokenKind::RBracket, "']'");
					auto e		 = std::make_unique<Expr>();
					e->kind		 = ExprKind::Slice;
					e->loc		 = idx ? idx->loc : lloc;
					e->lhs		 = std::move(base);
					e->rhs		 = std::move(idx);
					e->index_low = std::move(end);
					base		 = std::move(e);
				} else {
					std::unique_ptr<Expr> low;
					if (accept(TokenKind::Colon)) {
						low = parse_expr();
					}
					expect(TokenKind::RBracket, "']'");
					auto e			= std::make_unique<Expr>();
					e->kind			= ExprKind::Index;
					e->loc			= idx ? idx->loc : lloc;
					e->lhs			= std::move(base);
					e->rhs			= std::move(idx);
					e->is_bit_range = static_cast<bool>(low);
					e->index_low	= std::move(low);
					base			= std::move(e);
				}
			}
		} else if (at(TokenKind::KwCast) || at(TokenKind::KwBitcast)) {
			// Cast postfix operators: `expr cast Type` (normal value/type
			// conversion) and `expr bitcast Type` (bit reinterpretation,
			// same-size only). Both bind tighter than the binary operators
			// (parse_mul/parse_add/...) since they sit in the postfix chain,
			// mirroring Rust's `as`. Repeated `e cast T1 cast T2` works
			// left-to-right via this loop.
			SourceLoc	op_loc = peek().loc;
			bool		is_bit = at(TokenKind::KwBitcast);
			const char *kw	   = is_bit ? "bitcast" : "cast";
			consume(); // 'cast' or 'bitcast'
			auto ty = parse_type();
			if (!ty) {
				log::report(log::Level::Error, op_loc,
							std::string("expected type after '") + kw + "'");
				break;
			}
			auto e		  = std::make_unique<Expr>();
			e->kind		  = ExprKind::Cast;
			e->loc		  = op_loc;
			e->is_bitcast = is_bit;
			e->lhs		  = std::move(base);
			e->cast_type  = std::move(ty);
			base		  = std::move(e);
		} else if (at(TokenKind::Tilde)) {
			if (base && base->kind == ExprKind::FloatLit) {
				SourceLoc op_loc = peek().loc;
				consume();
				if (base->str_val.find('~') != std::string::npos) {
					log::report(log::Level::Error, op_loc,
								"duplicate '~' suffix on floating-point literal");
				} else {
					base->str_val.push_back('~');
				}
			} else {
				break;
			}
		} else {
			break;
		}
	}
	return base;
}
std::unique_ptr<Expr> Parser::parse_primary() {
	const Token &t = peek();
	switch (t.kind) {
	case TokenKind::IntLit: {
		auto e	   = std::make_unique<Expr>();
		e->kind	   = ExprKind::IntLit;
		e->loc	   = t.loc;
		e->int_val = t.int_val;
		e->str_val = t.text;
		consume();
		return e;
	}
	case TokenKind::FloatLit: {
		auto e	   = std::make_unique<Expr>();
		e->kind	   = ExprKind::FloatLit;
		e->loc	   = t.loc;
		e->flt_val = t.flt_val;
		e->str_val = t.text;
		consume();
		return e;
	}
	case TokenKind::StringLit: {
		auto e		  = std::make_unique<Expr>();
		e->kind		  = ExprKind::StringLit;
		e->loc		  = t.loc;
		e->str_val	  = t.text;
		e->is_cstring = t.is_cstring;
		consume();
		return e;
	}
	case TokenKind::CharLit: {
		auto e	   = std::make_unique<Expr>();
		e->kind	   = ExprKind::CharLit;
		e->loc	   = t.loc;
		e->int_val = t.char_val;
		consume();
		return e;
	}
	case TokenKind::KwTrue: {
		auto e			  = std::make_unique<Expr>();
		e->kind			  = ExprKind::BoolLit;
		e->loc			  = t.loc;
		e->const_bool_val = true;
		e->is_const_bool  = true;
		consume();
		return e;
	}
	case TokenKind::KwFalse: {
		auto e			  = std::make_unique<Expr>();
		e->kind			  = ExprKind::BoolLit;
		e->loc			  = t.loc;
		e->const_bool_val = false;
		e->is_const_bool  = true;
		consume();
		return e;
	}
	case TokenKind::KwNull: {
		auto e	= std::make_unique<Expr>();
		e->kind = ExprKind::NullLit;
		e->loc	= t.loc;
		consume();
		return e;
	}
	case TokenKind::DotDotDot: {
		auto e = std::make_unique<Expr>();
		e->kind = ExprKind::VariadicSplice;
		e->loc = t.loc;
		consume();
		return e;
	}
	case TokenKind::Dot: {
		consume();
		if (!at(TokenKind::Ident)) {
			log::report(log::Level::Error, peek().loc,
						"expected enum member name after '.'");
			return std::make_unique<Expr>();
		}
		auto e = std::make_unique<Expr>();
		e->kind = ExprKind::EnumMember;
		e->loc = t.loc;
		e->member = consume().text;
		return e;
	}
	case TokenKind::KwType: {
		auto e	= std::make_unique<Expr>();
		e->kind = ExprKind::TypeValue;
		e->loc	= t.loc;
		consume();
		return e;
	}
	case TokenKind::BuiltinModuleRef: {
		auto e	= std::make_unique<Expr>();
		e->kind = ExprKind::Ident;
		e->loc	= t.loc;
		e->path = {"@" + t.text};
		consume();
		return e;
	}
	case TokenKind::DefineRef: {
		auto e	   = std::make_unique<Expr>();
		e->kind	   = ExprKind::DefineRef;
		e->loc	   = t.loc;
		e->str_val = t.text;
		e->path	   = {t.text};
		consume();
		return e;
	}
	case TokenKind::LParen: {
		consume();
		bool prev_struct_init = allow_struct_init_;
		allow_struct_init_	  = true;
		if (parsing_generic_arg_) {
			++generic_arg_nesting_;
		}
		auto e = parse_expr();
		expect(TokenKind::RParen, "')'");
		if (e) e->parenthesized = true;
		if (parsing_generic_arg_) {
			--generic_arg_nesting_;
		}
		allow_struct_init_ = prev_struct_init;
		return e;
	}
	case TokenKind::LBracket: {
		// Array literal: [expr, expr, ...]
		consume(); // [
		auto e	= std::make_unique<Expr>();
		e->kind = ExprKind::ArrayLit;
		e->loc	= t.loc;
		if (!at(TokenKind::RBracket)) {
			e->args.push_back(parse_expr());
			while (at(TokenKind::Comma)) {
				consume();
				if (at(TokenKind::RBracket)) {
					break; // trailing comma
				}
				e->args.push_back(parse_expr());
			}
		}
		expect(TokenKind::RBracket, "']'");
		return e;
	}
	case TokenKind::KwIf:
	case TokenKind::KwFor:
	case TokenKind::KwMatch:
		return parse_block_expr();
	case TokenKind::LBrace: {
		// Aggregate literals start with `.field =`; any other brace in
		// expression position is a block expression whose value comes from yield.
		if (!(peek(1).kind == TokenKind::Dot && peek(3).kind == TokenKind::Assign)) {
			return parse_block_expr();
		}
		consume();
		auto e	= std::make_unique<Expr>();
		e->kind = ExprKind::StructLit;
		e->loc	= t.loc;
		while (!at(TokenKind::RBrace) && !at(TokenKind::Eof)) {
			if (!expect(TokenKind::Dot, "'.' in aggregate literal")) {
				break;
			}
			if (!at(TokenKind::Ident)) {
				log::report(log::Level::Error, peek().loc, "expected field name");
				break;
			}
			e->field_names.push_back(consume().text);
			if (!expect(TokenKind::Assign, "'=' in aggregate literal")) {
				break;
			}
			e->args.push_back(parse_expr());
			if (!at(TokenKind::RBrace) && !expect(TokenKind::Comma, "Comma or '}'")) {
				break;
			}
		}
		expect(TokenKind::RBrace, "'}' in aggregate literal");
		return e;
	}
	case TokenKind::KwSelf:
	case TokenKind::Ident: {
		std::vector<std::string> segs;
		segs.push_back(consume().text);
		std::vector<GenericArg> generic_args;
		if (generic_call_suffix()) {
			parse_generic_args(generic_args);
		}
		auto e			= std::make_unique<Expr>();
		e->kind			= ExprKind::Ident;
		e->loc			= t.loc;
		e->path			= segs;
		e->str_val		= stellac::join_path(segs);
		e->generic_args = std::move(generic_args);
		return e;
	}
	default:
		log::report(log::Level::Error, t.loc,
					std::string("unexpected token '") + t.text + "' in expression");
		consume();
		return std::make_unique<Expr>();
	}
}

std::unique_ptr<Expr> Parser::parse_block_expr() {
	SourceLoc loc = peek().loc;
	auto e = std::make_unique<Expr>();
	e->kind = ExprKind::Block;
	e->loc = loc;
	switch (peek().kind) {
	case TokenKind::KwIf:
		e->block = parse_if();
		break;
	case TokenKind::KwFor:
		e->block = parse_for();
		break;
	case TokenKind::KwMatch:
		e->block = parse_match();
		break;
	default:
		e->block = parse_block();
		break;
	}
	return e;
}

} // namespace stellac
