#include "internal.hpp"

namespace stellac {

StmtPtr Parser::parse_block() {
	auto s	= std::make_unique<Stmt>();
	s->kind = StmtKind::Block;
	s->loc	= peek().loc;
	expect(TokenKind::LBrace, "'{'");
	bool prev_struct_init = allow_struct_init_;
	allow_struct_init_	  = true;
	while (!at(TokenKind::RBrace) && !at(TokenKind::Eof)) {
		auto st = parse_stmt();
		if (st) {
			s->body.push_back(std::move(st));
		} else if (!at(TokenKind::RBrace) && !at(TokenKind::Eof)) {
			consume();
		}
	}
	allow_struct_init_ = prev_struct_init;
	expect(TokenKind::RBrace, "'}'");
	return s;
}

StmtPtr Parser::try_parse_stmt_keyword_typo(AttributeList &attrs) {
	if (peek().kind != TokenKind::Ident) {
		return nullptr;
	}
	// Check for C-style variable declaration (e.g. `u8 x = 1;`)
	if (is_prim_type_name(peek(0).text) && peek(1).kind == TokenKind::Ident) {
		SourceLoc loc = peek().loc;
		std::string ty_name = peek(0).text;
		std::string var_name = peek(1).text;
		auto diag = log::error(loc, "unexpected type name '" + ty_name + "' at start of statement")
						.code("E_PARSE_EXPECTED_TOKEN")
						.phase("parser")
						.help("declare variables using `let` or `let mut` (e.g. `let " + var_name + ": " + ty_name + " = ...;`)");
		diag.emit();
		auto s = std::make_unique<Stmt>();
		s->kind = StmtKind::VarDecl;
		s->loc = loc;
		consume(); // type name
		std::string name = consume().text; // var name
		std::unique_ptr<Expr> init;
		if (accept(TokenKind::Assign)) {
			init = parse_expr();
		}
		expect(TokenKind::Semicolon, "';'");
		VarDeclInfo var;
		var.name = name;
		var.loc = loc;
		var.is_const = false;
		var.is_mutable = false;
		var.attrs = std::move(attrs);
		auto ty = std::make_unique<TypeRef>();
		ty->name = ty_name;
		ty->loc = loc;
		var.ty = std::move(ty);
		var.init = std::move(init);
		s->vars.push_back(std::move(var));
		return s;
	}

	const Token &t0 = peek(0);
	const Token &t1 = peek(1);
	std::string_view text = t0.text;

	TokenKind best_kw = TokenKind::Unknown;
	size_t best_dist = 999;
	for (TokenKind kw : statement_keywords) {
		const char *kw_name = token_kind_name(kw);
		size_t dist = levenshtein_distance(text, kw_name);
		size_t threshold = (std::strlen(kw_name) <= 3) ? 1 : 2;
		if (dist <= threshold && dist < best_dist) {
			best_dist = dist;
			best_kw = kw;
		}
	}
	if (best_kw == TokenKind::Unknown) {
		return nullptr;
	}

	bool is_match = false;
	if (best_kw == TokenKind::KwLet || best_kw == TokenKind::KwConst) {
		if (t1.kind == TokenKind::Ident || t1.kind == TokenKind::KwStatic || t1.kind == TokenKind::KwMut ||
			t1.kind == TokenKind::KwThreadLocal) {
			is_match = true;
		}
	} else if (best_kw == TokenKind::KwReturn) {
		if (t1.kind == TokenKind::Ident || t1.kind == TokenKind::IntLit ||
			t1.kind == TokenKind::FloatLit || t1.kind == TokenKind::StringLit ||
			t1.kind == TokenKind::CharLit || t1.kind == TokenKind::KwTrue ||
			t1.kind == TokenKind::KwFalse || t1.kind == TokenKind::KwNull ||
			t1.kind == TokenKind::Bang || t1.kind == TokenKind::Tilde ||
			t1.kind == TokenKind::Semicolon) {
			is_match = true;
		}
	} else if (best_kw == TokenKind::KwFor) {
		if (t1.kind == TokenKind::KwConst ||
			t1.kind == TokenKind::KwLet || t1.kind == TokenKind::Ident) {
			is_match = true;
		}
	} else if (best_kw == TokenKind::KwIf || best_kw == TokenKind::KwMatch) {
		if (t1.kind == TokenKind::LParen || t1.kind == TokenKind::Ident ||
			t1.kind == TokenKind::IntLit || t1.kind == TokenKind::StringLit) {
			is_match = true;
		}
	} else if (best_kw == TokenKind::KwDefer) {
		if (t1.kind == TokenKind::LBrace || t1.kind == TokenKind::Ident ||
			t1.kind == TokenKind::KwIf || t1.kind == TokenKind::KwFor ||
			t1.kind == TokenKind::KwMatch || t1.kind == TokenKind::KwAsm) {
			is_match = true;
		}
	} else if (best_kw == TokenKind::KwYield) {
		if (t1.kind == TokenKind::Ident || t1.kind == TokenKind::IntLit ||
			t1.kind == TokenKind::FloatLit || t1.kind == TokenKind::StringLit) {
			is_match = true;
		}
	} else if (best_kw == TokenKind::KwBreak || best_kw == TokenKind::KwContinue ||
			   best_kw == TokenKind::KwUnreachable) {
		if (t1.kind == TokenKind::Semicolon) {
			is_match = true;
		}
	}

	if (!is_match) {
		return nullptr;
	}

	const char *kw_name = token_kind_name(best_kw);
	log::error(t0.loc, "unknown keyword '" + t0.text + "'")
		.code("E_PARSE_EXPECTED_TOKEN")
		.phase("parser")
		.help("did you mean `" + std::string(kw_name) + "`?")
		.fixit(t0.loc, kw_name, "replace with `" + std::string(kw_name) + "`")
		.emit();

	toks_[idx_].kind = best_kw;
	switch (best_kw) {
	case TokenKind::KwLet:
	case TokenKind::KwConst:
		return parse_var_or_const(best_kw == TokenKind::KwConst, std::move(attrs));
	case TokenKind::KwReturn:
		return parse_return();
	case TokenKind::KwIf:
		return parse_if();
	case TokenKind::KwFor:
		return parse_for();
	case TokenKind::KwMatch:
		return parse_match();
	case TokenKind::KwDefer:
		return parse_defer();
	case TokenKind::KwYield:
		return parse_yield();
	case TokenKind::KwBreak: {
		SourceLoc loc = peek().loc;
		consume();
		expect(TokenKind::Semicolon, "';'");
		auto s = std::make_unique<Stmt>();
		s->kind = StmtKind::Break;
		s->loc = loc;
		return s;
	}
	case TokenKind::KwContinue: {
		SourceLoc loc = peek().loc;
		consume();
		expect(TokenKind::Semicolon, "';'");
		auto s = std::make_unique<Stmt>();
		s->kind = StmtKind::Continue;
		s->loc = loc;
		return s;
	}
	case TokenKind::KwUnreachable: {
		SourceLoc loc = peek().loc;
		consume();
		expect(TokenKind::Semicolon, "';'");
		auto s = std::make_unique<Stmt>();
		s->kind = StmtKind::Unreachable;
		s->loc = loc;
		return s;
	}
	default:
		return nullptr;
	}
}

StmtPtr Parser::parse_stmt(bool expect_semicolon) {
	AttributeList attrs;
	parse_attributes(attrs);
	if (auto typo_stmt = try_parse_stmt_keyword_typo(attrs)) {
		return typo_stmt;
	}
	if (!attrs.attrs.empty() && peek().kind != TokenKind::KwConst && peek().kind != TokenKind::KwLet) {
		log::report(log::Level::Error, attrs.attrs.front().loc,
					attr_diag_prefix(attrs.attrs.front()) + " is not valid on this statement");
	}
	switch (peek().kind) {
	case TokenKind::LBrace:
		return parse_block();
	case TokenKind::KwIf:
		return parse_if();
	case TokenKind::KwFor:
		return parse_for();
	case TokenKind::KwMatch:
		return parse_match();
	case TokenKind::KwReturn:
		return parse_return(expect_semicolon);
	case TokenKind::KwUnreachable: {
		auto s = std::make_unique<Stmt>();
		s->kind = StmtKind::Unreachable;
		s->loc = peek().loc;
		consume();
		if (expect_semicolon) {
			expect(TokenKind::Semicolon, "';'");
		}
		return s;
	}
	case TokenKind::KwBreak:
		{
			SourceLoc loc = peek().loc;
			consume();
			if (expect_semicolon) {
				expect(TokenKind::Semicolon, "';'");
			}
			auto s	= std::make_unique<Stmt>();
			s->kind = StmtKind::Break;
			s->loc = loc;
			return s;
		}
	case TokenKind::KwContinue:
		{
			SourceLoc loc = peek().loc;
			consume();
			if (expect_semicolon) {
				expect(TokenKind::Semicolon, "';'");
			}
			auto s	= std::make_unique<Stmt>();
			s->kind = StmtKind::Continue;
			s->loc = loc;
			return s;
		}
	case TokenKind::KwDefer:
		return parse_defer(expect_semicolon);
	case TokenKind::KwYield:
		return parse_yield(expect_semicolon);
	case TokenKind::KwConst:
	case TokenKind::KwLet:
		return parse_var_or_const(peek().kind == TokenKind::KwConst, std::move(attrs),
								  expect_semicolon);
	case TokenKind::KwAsm:
		return parse_asm(expect_semicolon);
	case TokenKind::Semicolon:
		consume();
		return nullptr;
	default:
		return parse_expr_stmt_or_decl(expect_semicolon);
	}
}

StmtPtr Parser::parse_var_or_const(bool is_const, AttributeList attrs, bool expect_semicolon) {
	auto s	= std::make_unique<Stmt>();
	s->kind = is_const ? StmtKind::ConstDecl : StmtKind::VarDecl;
	s->loc	= peek().loc;
	consume(); // let or const
	bool is_mutable = false;
	bool is_static = false;
	bool is_thread_local = false;
	while (at(TokenKind::KwMut) || at(TokenKind::KwStatic) || at(TokenKind::KwThreadLocal)) {
		if (accept(TokenKind::KwMut)) {
			if (is_const) {
				log::report(log::Level::Error, peek().loc,
							"'mut' cannot be used with 'const'");
			}
			is_mutable = true;
		} else if (accept(TokenKind::KwStatic)) {
			is_static = true;
		} else if (accept(TokenKind::KwThreadLocal)) {
			is_thread_local = true;
		}
	}
	if (!at(TokenKind::Ident)) {
		log::report(log::Level::Error, peek().loc, "expected binding name");
		return s;
	}
	std::vector<std::pair<std::string, SourceLoc>> names;
	while (true) {
		SourceLoc	vloc = peek().loc;
		std::string name = consume().text;
		reject_reserved_name(vloc, name, "binding name");
		names.emplace_back(std::move(name), vloc);
		if (!accept(TokenKind::Comma)) {
			break;
		}
		if (!at(TokenKind::Ident)) {
			log::report(log::Level::Error, peek().loc, "expected binding name after ','");
			break;
		}
	}
	expect(TokenKind::Colon, "':'");
	auto				  ty = parse_type();
	std::unique_ptr<Expr> init;
	if (accept(TokenKind::Assign)) {
		init = parse_expr();
	}
	if (expect_semicolon) {
		expect(TokenKind::Semicolon, "';'");
	}
	for (auto &[name, loc] : names) {
		VarDeclInfo var;
		var.name	 = name;
		var.loc		 = loc;
		var.is_const = is_const;
		var.is_mutable = is_mutable;
		var.is_static = is_static;
		var.is_thread_local = is_thread_local;
		var.attrs	 = attrs;
		if (ty) {
			var.ty = clone_typeref(*ty);
		}
		if (init) {
			var.init = clone_expr(*init);
		}
		s->vars.push_back(std::move(var));
	}
	return s;
}

StmtPtr Parser::parse_if() {
	auto s	= std::make_unique<Stmt>();
	s->kind = StmtKind::If;
	s->loc	= peek().loc;
	consume(); // if
	{
		bool prev_struct_init = allow_struct_init_;
		allow_struct_init_	  = false;
		s->cond				  = parse_expr();
		allow_struct_init_	  = prev_struct_init;
	}
	s->then_block = parse_block();
	if (accept(TokenKind::KwElse)) {
		if (at(TokenKind::KwIf)) {
			s->else_block = parse_if();
		} else {
			s->else_block = parse_block();
		}
	}
	return s;
}

StmtPtr Parser::parse_for() {
	auto s	= std::make_unique<Stmt>();
	s->kind = StmtKind::For;
	s->loc	= peek().loc;
	consume();
	// init: binding decl (no parens -- `for let mut i: T = e; cond; step { }`)
	AttributeList init_attrs;
	parse_attributes(init_attrs);
	if (at(TokenKind::KwLet) || at(TokenKind::KwConst)) {
		s->for_init = parse_var_or_const(at(TokenKind::KwConst), std::move(init_attrs));
	} else {
		if (!init_attrs.attrs.empty()) {
			log::report(log::Level::Error, init_attrs.attrs.front().loc,
						"attributes are only valid on a for-loop binding");
		}
		if (!at(TokenKind::LBrace) && !at(TokenKind::Semicolon)) {
			bool prev_struct_init = allow_struct_init_;
			allow_struct_init_	  = false;
			s->for_cond			  = parse_expr();
			allow_struct_init_	  = prev_struct_init;
		}
		if (!at(TokenKind::LBrace)) {
			expect(TokenKind::Semicolon, "';'");
			if (!at(TokenKind::LBrace)) {
				s->for_step = parse_expr();
			}
		}
		s->loop_body = parse_block();
		return s;
	}
	if (!at(TokenKind::Semicolon)) {
		if (!at(TokenKind::LBrace) && !at(TokenKind::Semicolon)) {
			bool prev_struct_init = allow_struct_init_;
			allow_struct_init_	  = false;
			s->for_cond			  = parse_expr();
			allow_struct_init_	  = prev_struct_init;
		}
		expect(TokenKind::Semicolon, "';'");
	} else {
		consume();
	}
	if (!at(TokenKind::LBrace)) {
		s->for_step = parse_expr();
	}
	s->loop_body = parse_block();
	return s;
}

MatchPattern Parser::parse_match_pattern() {
	MatchPattern pat;
	pat.loc = peek().loc;

	if (at(TokenKind::Ident) && peek().text == "_") {
		consume();
		pat.kind = MatchPatternKind::Wildcard;
		return pat;
	}

	// Check if this starts a variant pattern with payload destructuring:
	// `.Variant(...)` / `.Variant { ... }` or `Path.Variant(...)` / `Path.Variant { ... }`.
	auto looks_like_variant_head = [&]() -> size_t {
		size_t p = idx_;
		if (p < toks_.size() && toks_[p].kind == TokenKind::Dot) {
			if (p + 1 < toks_.size() && toks_[p + 1].kind == TokenKind::Ident) {
				return p + 2;
			}
			return SIZE_MAX;
		}
		if (p < toks_.size() && toks_[p].kind == TokenKind::Ident) {
			++p;
			while (p < toks_.size()) {
				if (toks_[p].kind == TokenKind::Dot && p + 1 < toks_.size() &&
					toks_[p + 1].kind == TokenKind::Ident) {
					p += 2;
				} else if (toks_[p].is_binop(Op::Lt)) {
					// Skip balanced `<...>` generic args if present.
					int depth = 1;
					size_t q  = p + 1;
					while (q < toks_.size() && depth > 0) {
						if (toks_[q].is_binop(Op::Lt)) ++depth;
						else if (toks_[q].is_binop(Op::Gt)) --depth;
						++q;
					}
					if (depth != 0) return SIZE_MAX;
					p = q;
				} else {
					break;
				}
			}
			return p;
		}
		return SIZE_MAX;
	};

	auto is_struct_pattern_brace = [&](size_t brace_pos) -> bool {
		if (brace_pos >= toks_.size() || toks_[brace_pos].kind != TokenKind::LBrace) {
			return false;
		}
		int depth = 1;
		size_t q  = brace_pos + 1;
		while (q < toks_.size() && depth > 0) {
			if (toks_[q].kind == TokenKind::LBrace) ++depth;
			else if (toks_[q].kind == TokenKind::RBrace) --depth;
			++q;
		}
		if (depth != 0 || q >= toks_.size()) return false;
		return toks_[q].kind == TokenKind::FatArrow || toks_[q].is_binop(Op::BitOr);
	};

	size_t after_head = looks_like_variant_head();
	if (after_head != SIZE_MAX && after_head < toks_.size() &&
		(toks_[after_head].kind == TokenKind::LParen || is_struct_pattern_brace(after_head))) {
		// Parse the variant target expression (`.Var` or `Enum<...>.Var`).
		bool prev_struct_init = allow_struct_init_;
		allow_struct_init_	  = false;
		if (at(TokenKind::Dot)) {
			pat.expr = parse_primary();
		} else {
			pat.expr = parse_primary();
			while (at(TokenKind::Dot) || generic_call_suffix()) {
				if (generic_call_suffix()) {
					parse_generic_args(pat.expr->generic_args);
				} else {
					consume(); // '.'
					if (!at(TokenKind::Ident)) {
						log::report(log::Level::Error, peek().loc, "expected variant name after '.'");
						break;
					}
					auto m	  = std::make_unique<Expr>();
					m->kind	  = ExprKind::Member;
					m->loc	  = peek().loc;
					m->lhs	  = std::move(pat.expr);
					m->member = consume().text;
					pat.expr  = std::move(m);
				}
			}
		}
		allow_struct_init_ = prev_struct_init;
		pat.kind		   = MatchPatternKind::Variant;

		if (accept(TokenKind::LParen)) {
			pat.is_struct_pattern = false;
			while (!at(TokenKind::RParen) && !at(TokenKind::Eof)) {
				MatchBinding b;
				b.loc = peek().loc;
				if (accept(TokenKind::KwMut)) {
					b.is_mut = true;
				}
				if (!at(TokenKind::Ident)) {
					log::report(log::Level::Error, peek().loc, "expected binding name or '_'");
					break;
				}
				b.name = consume().text;
				if (b.name == "_" && b.is_mut) {
					log::report(log::Level::Error, b.loc, "'_' wildcard binding cannot be 'mut'");
				}
				pat.bindings.push_back(std::move(b));
				if (!accept(TokenKind::Comma)) {
					break;
				}
			}
			expect(TokenKind::RParen, "')'");
		} else if (accept(TokenKind::LBrace)) {
			pat.is_struct_pattern = true;
			while (!at(TokenKind::RBrace) && !at(TokenKind::Eof)) {
				bool leading_mut = accept(TokenKind::KwMut);
				if (!at(TokenKind::Ident)) {
					log::report(log::Level::Error, peek().loc, "expected field name");
					break;
				}
				MatchBinding b;
				b.loc		 = peek().loc;
				b.field_name = consume().text;
				if (accept(TokenKind::Colon)) {
					if (leading_mut) {
						log::report(log::Level::Error, b.loc,
									"'mut' must be placed on the binding name after ':'");
					}
					if (accept(TokenKind::KwMut)) {
						b.is_mut = true;
					}
					if (!at(TokenKind::Ident)) {
						log::report(log::Level::Error, peek().loc, "expected binding name or '_'");
						break;
					}
					b.name = consume().text;
					if (b.name == "_" && b.is_mut) {
						log::report(log::Level::Error, b.loc, "'_' wildcard binding cannot be 'mut'");
					}
				} else {
					b.is_mut = leading_mut;
					b.name	 = b.field_name;
				}
				pat.bindings.push_back(std::move(b));
				if (!accept(TokenKind::Comma)) {
					break;
				}
			}
			expect(TokenKind::RBrace, "'}'");
		}
		return pat;
	}

	bool prev_struct_init = allow_struct_init_;
	allow_struct_init_	  = false;
	pat.kind			  = MatchPatternKind::Value;
	pat.expr			  = parse_shift();
	allow_struct_init_	  = prev_struct_init;
	return pat;
}

StmtPtr Parser::parse_match() {
	auto s	= std::make_unique<Stmt>();
	s->kind = StmtKind::Match;
	s->loc	= peek().loc;
	consume(); // 'match'
	{
		bool prev_struct_init = allow_struct_init_;
		allow_struct_init_	  = false;
		s->scrutinee		  = parse_expr();
		allow_struct_init_	  = prev_struct_init;
	}
	expect(TokenKind::LBrace, "'{' after match expression");
	while (!at(TokenKind::RBrace) && !at(TokenKind::Eof)) {
		MatchArm arm;
		arm.loc = peek().loc;
		arm.patterns.push_back(parse_match_pattern());
		while (at_binop(Op::BitOr)) {
			consume(); // '|'
			arm.patterns.push_back(parse_match_pattern());
		}
		expect(TokenKind::FatArrow, "'=>' after match pattern");
		if (at(TokenKind::LBrace)) {
			arm.body = parse_block();
			accept(TokenKind::Comma);
		} else {
			auto block	= std::make_unique<Stmt>();
			block->kind = StmtKind::Block;
			block->loc	= peek().loc;
			if (auto st = parse_stmt(false)) {
				block->body.push_back(std::move(st));
			}
			arm.body = std::move(block);
			if (!at(TokenKind::RBrace)) {
				expect(TokenKind::Comma, "','");
			} else {
				accept(TokenKind::Comma);
			}
		}
		s->arms.push_back(std::move(arm));
	}
	expect(TokenKind::RBrace, "'}'");
	return s;
}

StmtPtr Parser::parse_return(bool expect_semicolon) {
	auto s	= std::make_unique<Stmt>();
	s->kind = StmtKind::Return;
	s->loc	= peek().loc;
	consume();
	bool has_val = expect_semicolon
					   ? !at(TokenKind::Semicolon)
					   : (!at(TokenKind::Comma) && !at(TokenKind::RBrace) && !at(TokenKind::Eof));
	if (has_val) {
		s->ret_val = parse_expr();
	}
	if (expect_semicolon) {
		expect(TokenKind::Semicolon, "';'");
	}
	return s;
}

StmtPtr Parser::parse_yield(bool expect_semicolon) {
	auto s = std::make_unique<Stmt>();
	s->kind = StmtKind::Yield;
	s->loc = peek().loc;
	consume();
	bool has_val = expect_semicolon
					   ? !at(TokenKind::Semicolon)
					   : (!at(TokenKind::Comma) && !at(TokenKind::RBrace) && !at(TokenKind::Eof));
	if (has_val) {
		s->ret_val = parse_expr();
	}
	if (expect_semicolon) {
		expect(TokenKind::Semicolon, "';'");
	}
	return s;
}

StmtPtr Parser::parse_defer(bool expect_semicolon) {
	auto s = std::make_unique<Stmt>();
	s->kind = StmtKind::Defer;
	s->loc = peek().loc;
	consume();
	if (at(TokenKind::Semicolon) || at(TokenKind::RBrace) || at(TokenKind::Eof)) {
		log::report(log::Level::Error, peek().loc, "defer requires a statement");
		if (at(TokenKind::Semicolon)) {
			consume();
		}
		return s;
	}
	s->defer_body = parse_stmt(expect_semicolon);
	return s;
}

StmtPtr Parser::parse_asm(bool expect_semicolon) {
	auto s	= std::make_unique<Stmt>();
	s->kind = StmtKind::Asm;
	s->loc	= peek().loc;
	consume(); // asm
	if (at(TokenKind::KwVolatile)) {
		s->asm_volatile = true;
		consume();
	}
	expect(TokenKind::LBrace, "'{'");
	// code string (one or more concatenated string literals allowed)
	if (!at(TokenKind::StringLit)) {
		log::report(log::Level::Error, peek().loc, "expected asm code string");
		return s;
	}
	s->asm_code = consume().text;
	while (at(TokenKind::StringLit)) {
		s->asm_code += consume().text;
	}

	// Helper: parse an operand list. Caller has already consumed the leading ':'.
	auto parse_operand_list = [&](std::vector<AsmOperand> &dest, bool allow_clobber) {
		while (true) {
			if (!at(TokenKind::StringLit)) {
				log::report(log::Level::Error, peek().loc,
							"expected constraint string in asm operand list");
				return;
			}
			std::string c = consume().text;
			if (at(TokenKind::LParen)) {
				consume();
				AsmOperand aop;
				aop.constraint = c;
				aop.operand	   = parse_expr();
				expect(TokenKind::RParen, "')'");
				dest.push_back(std::move(aop));
			} else if (allow_clobber) {
				AsmOperand aop;
				aop.constraint = c;
				aop.is_clobber = true;
				dest.push_back(std::move(aop));
			} else {
				log::report(log::Level::Error, peek().loc,
							"bare clobber string not allowed in output/input list");
			}
			if (!at(TokenKind::Comma)) {
				break;
			}
			consume();
		}
	};

	// outputs: ':' <operands>
	if (accept(TokenKind::Colon)) {
		if (at(TokenKind::StringLit)) {
			parse_operand_list(s->asm_outputs, false);
		}
		// inputs: ':' <operands>
		if (accept(TokenKind::Colon)) {
			if (at(TokenKind::StringLit)) {
				parse_operand_list(s->asm_inputs, false);
			}
			// clobbers: ':' <clobber strings>
			if (accept(TokenKind::Colon)) {
				while (at(TokenKind::StringLit)) {
					s->asm_clobbers.push_back(consume().text);
					if (!at(TokenKind::Comma)) {
						break;
					}
					consume();
				}
			}
		}
	}
	expect(TokenKind::RBrace, "'}'");
	if (expect_semicolon) {
		expect(TokenKind::Semicolon, "';'");
	}
	return s;
}

StmtPtr Parser::parse_expr_stmt_or_decl(bool expect_semicolon) {
	auto s	= std::make_unique<Stmt>();
	s->kind = StmtKind::ExprStmt;
	s->loc	= peek().loc;
	s->expr = parse_expr();
	if (expect_semicolon) {
		expect(TokenKind::Semicolon, "';'");
	}
	return s;
}

// expressions

// Deep-clone an expression. Used only during compound-assignment desugaring
// (`x += y` -> `x = x + y`), which happens before sema fills in resolved
// type pointers -- so we only need to copy the syntactic payload.

} // namespace stellac
