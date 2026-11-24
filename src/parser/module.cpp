#include "internal.hpp"

namespace stellac {

Parser::Parser(std::vector<Token> toks, std::string fname, std::vector<std::string> mod_path)
	: toks_(std::move(toks)), fname_(std::move(fname)), mod_path_(std::move(mod_path)) {}

const Token &Parser::peek(size_t off) const {
	size_t i = idx_ + off;
	if (i >= toks_.size()) {
		return toks_.back();
	}
	return toks_[i];
}
const Token &Parser::consume() {
	const Token &t = toks_[idx_];
	if (idx_ < toks_.size() - 1) {
		++idx_;
	}
	return t;
}

bool Parser::at(TokenKind k) const {
	return peek().kind == k;
}
bool Parser::at_binop(Op o) const {
	return peek().is_binop(o);
}
bool Parser::is_generic_type_param_name(const std::string &name) const {
	return std::find(generic_type_params_.begin(), generic_type_params_.end(), name) !=
		   generic_type_params_.end();
}
bool Parser::accept(TokenKind k) {
	if (peek().kind == k) {
		consume();
		return true;
	}
	return false;
}
bool Parser::expect(TokenKind k, const char *what) {
	if (accept(k)) {
		return true;
	}
	auto diag = log::error(peek().loc,
						   std::string("expected ") + what + " but got '" + peek().text + "'")
					.code("E_PARSE_EXPECTED_TOKEN")
					.phase("parser");
	if (what && what[0] != '\0') {
		std::string token_str;
		if (what[0] == '\'') {
			const char *end = std::strchr(what + 1, '\'');
			if (end) {
				token_str = std::string(what + 1, end);
			}
		}
		if (token_str.empty()) {
			token_str = what;
		}
		SourceSpan ins_span;
		ins_span.start = peek().loc;
		ins_span.start.length = 0;
		ins_span.end = ins_span.start;
		diag.fixit(ins_span, token_str, "insert `" + token_str + "`");
	}
	diag.emit();
	// Consume the offending token to guarantee forward progress in callers
	// that loop on expect() (e.g. comma-separated lists). Never consume Eof
	// -- advancing past Eof would just spin on the same sentinel forever.
	if (peek().kind != TokenKind::Eof) {
		consume();
	}
	return false;
}

static bool is_attribute_name_token(TokenKind kind) {
	switch (kind) {
	case TokenKind::Ident:
	case TokenKind::KwTrue:
	case TokenKind::KwFalse:
	case TokenKind::KwNull:
	case TokenKind::KwMod:
	case TokenKind::KwImport:
	case TokenKind::KwPub:
	case TokenKind::KwExtern:
	case TokenKind::KwFn:
	case TokenKind::KwLet:
	case TokenKind::KwMut:
	case TokenKind::KwConst:
	case TokenKind::KwStruct:
	case TokenKind::KwUnion:
	case TokenKind::KwType:
	case TokenKind::KwEnum:
	case TokenKind::KwIf:
	case TokenKind::KwElse:
	case TokenKind::KwFor:
	case TokenKind::KwReturn:
	case TokenKind::KwBreak:
	case TokenKind::KwContinue:
	case TokenKind::KwDefer:
	case TokenKind::KwYield:
	case TokenKind::KwMatch:
	case TokenKind::KwAsm:
	case TokenKind::KwVolatile:
	case TokenKind::KwCast:
	case TokenKind::KwBitcast:
		return true;
	default:
		return false;
	}
}
bool Parser::parse_module(Module &out) {
	out.fpath	 = fname_;
	out.mod_path = mod_path_;

	// File-level namespace header: optional `#[...]` attributes, then
	// `mod <dotted-path>;` -- must be the first top-level construct (and at
	// most one per file).
	//
	// Do not parse attributes eagerly here. A headerless file is allowed to put
	// attributes on its first declaration, so consuming `#[export]` before
	// checking for the namespace header would lose the attribute and report a
	// misleading file-level diagnostic.
	auto starts_with_namespace_header = [&]() {
		size_t scan = idx_;
		while (scan + 1 < toks_.size() && toks_[scan].kind == TokenKind::Hash &&
			   toks_[scan + 1].kind == TokenKind::LBracket) {
			int square_depth = 0;
			do {
				if (toks_[scan].kind == TokenKind::LBracket) {
					++square_depth;
				} else if (toks_[scan].kind == TokenKind::RBracket) {
					--square_depth;
				}
				++scan;
			} while (scan < toks_.size() && square_depth > 0);
			if (square_depth != 0) {
				return false;
			}
		}
		if (scan >= toks_.size() || toks_[scan].kind != TokenKind::KwMod) {
			return false;
		}
		++scan;
		if (scan >= toks_.size() || toks_[scan].kind != TokenKind::Ident) {
			return false;
		}
		++scan;
		while (scan + 1 < toks_.size() && toks_[scan].kind == TokenKind::Dot &&
			   toks_[scan + 1].kind == TokenKind::Ident) {
			scan += 2;
		}
		return scan < toks_.size() && toks_[scan].kind == TokenKind::Semicolon;
	};

	AttributeList file_attrs;
	const bool has_header = starts_with_namespace_header();
	if (has_header) {
		parse_attributes(file_attrs);
	}
	if (!has_header) {
		while (!at(TokenKind::Eof)) {
			TopKind tk = parse_top_into(out.decls, false);
			if (tk == TopKind::Error && !at(TokenKind::Eof)) {
				consume();
			}
		}
		return !log::has_error();
	}
	consume(); // mod
	std::vector<std::string> segs;
	SourceLoc				 mod_loc = peek().loc;
	if (!parse_dotted_path(segs)) {
		log::report(log::Level::Error, peek().loc, "expected module path after 'mod'");
		return !log::has_error();
	}
	for (const auto &s : segs) {
		reject_reserved_name(mod_loc, s, "module name");
	}
	out.mod_path   = segs;
	out.mod_loc	   = mod_loc;
	out.file_attrs = std::move(file_attrs);

	if (accept(TokenKind::Semicolon)) {
		while (!at(TokenKind::Eof)) {
			TopKind tk = parse_top_into(out.decls, false);
			if (tk == TopKind::Error) {
				// error recovery: skip to next top-level token
				consume();
			}
			if (log::has_error() && peek().kind == TokenKind::Eof) {
				break;
			}
		}
		return !log::has_error();
	}

	log::report(log::Level::Error, peek().loc, "expected ';' after module name");
	return false;
}

TopKind Parser::parse_top(Module &out) {
	return parse_top_into(out.decls, false);
}

TopKind Parser::parse_top_into(std::vector<TopDeclPtr> &body, bool inside_block) {
	AttributeList attrs;
	parse_attributes(attrs);
	bool is_pub = accept(TokenKind::KwPub);
	switch (peek().kind) {
	case TokenKind::KwMod:
		parse_mod_into(body, is_pub, std::move(attrs), inside_block);
		seen_non_import_ = true;
		return TopKind::Mod;
	case TokenKind::KwImport:
		if (seen_non_import_) {
			log::report(log::Level::Error, peek().loc,
						"'import' must appear before any other declaration");
			return TopKind::Error;
		}
		parse_import_into(body, is_pub, std::move(attrs));
		return TopKind::Import;
	case TokenKind::KwIf:
		if (!attrs.attrs.empty() || is_pub) {
			log::report(log::Level::Error, peek().loc,
						"attributes and 'pub' are not valid on top-level conditionals");
		}
		seen_non_import_ = true;
		parse_top_if(body);
		return TopKind::Conditional;
	case TokenKind::KwFn:
		parse_fn_into(body, is_pub, std::move(attrs));
		seen_non_import_ = true;
		return TopKind::Fn;
	case TokenKind::KwMacro:
		parse_macro_into(body, is_pub, std::move(attrs));
		seen_non_import_ = true;
		return TopKind::Fn;
	case TokenKind::KwExtern:
		if (peek(1).kind == TokenKind::KwMacro) {
			log::report(log::Level::Error, peek().loc, "extern macro declarations are invalid");
			consume();
			parse_macro_into(body, is_pub, std::move(attrs));
			seen_non_import_ = true;
			return TopKind::Fn;
		}
		seen_non_import_ = true;
		if (peek(1).kind == TokenKind::KwLet) {
			parse_global_into(body, is_pub, false, std::move(attrs), true);
			return TopKind::Var;
		}
		if (peek(1).kind == TokenKind::KwConst) {
			parse_global_into(body, is_pub, true, std::move(attrs), true);
			return TopKind::Const;
		}
		parse_fn_into(body, is_pub, std::move(attrs), true);
		return TopKind::Fn;
	case TokenKind::KwStruct:
		parse_struct_into(body, is_pub, std::move(attrs));
		seen_non_import_ = true;
		return TopKind::Struct;
	case TokenKind::KwUnion:
		parse_union_into(body, is_pub, std::move(attrs));
		seen_non_import_ = true;
		return TopKind::Union;
	case TokenKind::KwEnum:
		parse_enum_into(body, is_pub, std::move(attrs));
		seen_non_import_ = true;
		return TopKind::Enum;
	case TokenKind::KwTrait:
		parse_trait_into(body, is_pub, std::move(attrs));
		seen_non_import_ = true;
		return TopKind::Trait;
	case TokenKind::KwImpl:
		if (is_pub) {
			log::report(log::Level::Error, peek().loc, "'pub' is not valid on 'impl' blocks");
		}
		parse_impl_into(body, std::move(attrs));
		seen_non_import_ = true;
		return TopKind::Impl;
	case TokenKind::KwLet:
		parse_global_into(body, is_pub, false, std::move(attrs));
		seen_non_import_ = true;
		return TopKind::Var;
	case TokenKind::KwConst:
		parse_global_into(body, is_pub, true, std::move(attrs));
		seen_non_import_ = true;
		return TopKind::Const;
	case TokenKind::KwPub:
		log::report(log::Level::Error, peek().loc, "duplicate 'pub'");
		return TopKind::Error;
	default: {
		if (peek().kind == TokenKind::Ident) {
			const Token &t0	   = peek();
			TokenKind	 best_kw   = TokenKind::Unknown;
			size_t		 best_dist = 999;
			for (TokenKind kw : top_level_keywords) {
				const char *kw_name  = token_kind_name(kw);
				size_t		dist	 = levenshtein_distance(t0.text, kw_name);
				size_t		threshold = (std::strlen(kw_name) <= 3) ? 1 : 2;
				if (dist <= threshold && dist < best_dist) {
					best_dist = dist;
					best_kw	  = kw;
				}
			}
			if (best_kw != TokenKind::Unknown) {
				const char *kw_name = token_kind_name(best_kw);
				log::error(t0.loc, "unknown top-level keyword '" + t0.text + "'")
					.code("E_PARSE_EXPECTED_TOKEN")
					.phase("parser")
					.help("did you mean `" + std::string(kw_name) + "`?")
					.fixit(t0.loc, kw_name, "replace with `" + std::string(kw_name) + "`")
					.emit();
				toks_[idx_].kind = best_kw;
				switch (best_kw) {
				case TokenKind::KwMod:
					parse_mod_into(body, is_pub, std::move(attrs), inside_block);
					seen_non_import_ = true;
					return TopKind::Mod;
				case TokenKind::KwFn:
					parse_fn_into(body, is_pub, std::move(attrs));
					seen_non_import_ = true;
					return TopKind::Fn;
				case TokenKind::KwMacro:
					parse_macro_into(body, is_pub, std::move(attrs));
					seen_non_import_ = true;
					return TopKind::Fn;
				case TokenKind::KwStruct:
					parse_struct_into(body, is_pub, std::move(attrs));
					seen_non_import_ = true;
					return TopKind::Struct;
				case TokenKind::KwUnion:
					parse_union_into(body, is_pub, std::move(attrs));
					seen_non_import_ = true;
					return TopKind::Union;
				case TokenKind::KwEnum:
					parse_enum_into(body, is_pub, std::move(attrs));
					seen_non_import_ = true;
					return TopKind::Enum;
				case TokenKind::KwTrait:
					parse_trait_into(body, is_pub, std::move(attrs));
					seen_non_import_ = true;
					return TopKind::Trait;
				case TokenKind::KwImpl:
					parse_impl_into(body, std::move(attrs));
					seen_non_import_ = true;
					return TopKind::Impl;
				case TokenKind::KwLet:
					parse_global_into(body, is_pub, false, std::move(attrs));
					seen_non_import_ = true;
					return TopKind::Var;
				case TokenKind::KwConst:
					parse_global_into(body, is_pub, true, std::move(attrs));
					seen_non_import_ = true;
					return TopKind::Const;
				case TokenKind::KwImport:
					parse_import_into(body, is_pub, std::move(attrs));
					return TopKind::Import;
				case TokenKind::KwExtern:
					seen_non_import_ = true;
					if (peek(1).kind == TokenKind::KwLet) {
						parse_global_into(body, is_pub, false, std::move(attrs), true);
						return TopKind::Var;
					}
					if (peek(1).kind == TokenKind::KwConst) {
						parse_global_into(body, is_pub, true, std::move(attrs), true);
						return TopKind::Const;
					}
					parse_fn_into(body, is_pub, std::move(attrs), true);
					return TopKind::Fn;
				case TokenKind::KwIf:
					seen_non_import_ = true;
					parse_top_if(body);
					return TopKind::Conditional;
				case TokenKind::KwPub:
					consume();
					return parse_top_into(body, true);
				default:
					break;
				}
			}
		}
		log::report(log::Level::Error, peek().loc,
					std::string("unexpected top-level token '") + peek().text + "' (" +
						token_kind_name(peek().kind) + ")");
		return TopKind::Error;
	}
	}
}

bool Parser::parse_top_if(std::vector<TopDeclPtr> &body) {
	auto cd = std::make_unique<ConditionalDecl>();
	cd->loc = peek().loc;
	while (true) {
		ConditionalArm arm;
		arm.loc = peek().loc;
		expect(TokenKind::KwIf, "'if'");
		arm.cond = parse_expr();
		expect(TokenKind::LBrace, "'{' after top-level condition");
		bool saved_seen	 = seen_non_import_;
		seen_non_import_ = false;
		while (!at(TokenKind::RBrace) && !at(TokenKind::Eof)) {
			TopKind tk = parse_top_into(arm.body, true);
			if (tk == TopKind::Error && !at(TokenKind::Eof)) {
				consume();
			}
		}
		expect(TokenKind::RBrace, "'}' after top-level conditional body");
		seen_non_import_ = saved_seen;
		cd->arms.push_back(std::move(arm));
		if (!accept(TokenKind::KwElse)) {
			break;
		}
		if (!at(TokenKind::KwIf)) {
			ConditionalArm else_arm;
			else_arm.loc = peek().loc;
			expect(TokenKind::LBrace, "'{' after 'else'");
			bool saved		 = seen_non_import_;
			seen_non_import_ = false;
			while (!at(TokenKind::RBrace) && !at(TokenKind::Eof)) {
				TopKind tk = parse_top_into(else_arm.body, true);
				if (tk == TopKind::Error && !at(TokenKind::Eof)) {
					consume();
				}
			}
			expect(TokenKind::RBrace, "'}' after top-level else body");
			seen_non_import_ = saved;
			cd->arms.push_back(std::move(else_arm));
			break;
		}
		// The next loop iteration consumes `else if`.
	}
	auto td			= std::make_unique<TopDecl>();
	td->kind		= TopKind::Conditional;
	td->conditional = std::move(cd);
	body.push_back(std::move(td));
	return true;
}

void Parser::parse_attributes(AttributeList &out) {
	while (at(TokenKind::Hash) && peek(1).kind == TokenKind::LBracket) {
		if (out.attrs.empty()) {
			out.loc = peek().loc;
		}
		SourceLoc hash_loc = peek().loc;
		consume(); // #
		consume(); // [
		while (!at(TokenKind::RBracket) && !at(TokenKind::Eof)) {
			// Attribute groups may be nested for macro-generated source, for
			// example `#[cold, #[section(".text")]]`.  The AST deliberately
			// stores a flat list, so recursively reading a nested group gives
			// callers the same semantics as spelling all items in one group.
			if (at(TokenKind::Hash) && peek(1).kind == TokenKind::LBracket) {
				parse_attributes(out);
			} else {
				parse_attribute_item(out.attrs, out.loc);
			}
			if (at(TokenKind::Comma)) {
				consume();
			}
		}
		if (!at(TokenKind::RBracket)) {
			log::report(log::Level::Error, hash_loc, "expected ']' to close attribute list");
			return;
		}
		consume(); // ]
	}
}

void Parser::parse_attribute_item(std::vector<Attribute> &out, SourceLoc &list_loc) {
	(void)list_loc;
	SourceLoc start	  = peek().loc;
	bool	  negated = accept(TokenKind::Bang);
	if (!is_attribute_name_token(peek().kind)) {
		log::report(log::Level::Error, peek().loc, "expected attribute name");
		return;
	}
	Attribute a;
	a.loc		  = start;
	a.negated	  = negated;
	a.name		  = consume().text;
	SourceLoc end = toks_[idx_ - 1].loc;
	if (at(TokenKind::LParen)) {
		consume(); // (
		while (!at(TokenKind::RParen) && !at(TokenKind::Eof)) {
			// Accept a literal token; record its raw text.
			if (at(TokenKind::StringLit) || at(TokenKind::IntLit) || at(TokenKind::Ident)) {
				a.arg_is_string.push_back(at(TokenKind::StringLit));
				a.args.push_back(consume().text);
			} else {
				log::report(log::Level::Error, peek().loc,
							"expected literal (string, integer, or "
							"identifier) in attribute arguments");
				return;
			}
			if (at(TokenKind::Comma)) {
				consume();
			}
		}
		if (!at(TokenKind::RParen)) {
			log::report(log::Level::Error, peek().loc, "expected ')' to close attribute arguments");
			return;
		}
		const Token &end_tok = consume(); // )
		end					 = end_tok.loc;
	}
	if (end.line == a.loc.line) {
		a.loc.length = std::max(1, end.column + std::max(1, end.length) - a.loc.column);
	} else {
		a.loc.length = std::max(1, a.loc.length);
	}
	out.push_back(std::move(a));
}

bool Parser::parse_dotted_path(std::vector<std::string> &out) {
	if (!at(TokenKind::Ident)) {
		return false;
	}
	out.push_back(consume().text);
	while (at(TokenKind::Dot)) {
		consume();
		if (!at(TokenKind::Ident)) {
			log::report(log::Level::Error, peek().loc, "expected identifier after '.'");
			return false;
		}
		out.push_back(consume().text);
	}
	return true;
}

void Parser::parse_generic_params(std::vector<GenericParam> &out) {
	// Generic declarations and applications use angle brackets. Square
	// brackets remain the spelling for array types and indexing.
	if (!at_binop(Op::Lt)) {
		return;
	}
	consume();
	while (!at_binop(Op::Gt) && !at(TokenKind::Eof)) {
		if (!at(TokenKind::Ident)) {
			log::report(log::Level::Error, peek().loc, "expected generic parameter name");
			break;
		}
		GenericParam p;
		p.loc  = peek().loc;
		p.name = consume().text;
		// A bare name is an unconstrained type parameter (<T>). A name
		// followed by ':' has constraints (<T: u | i>, <T: u | i + Trait>)
		// or a const generic type (<N: usize>).
		bool has_colon = accept(TokenKind::Colon);
		if (!has_colon && !at(TokenKind::Comma) && !at_binop(Op::Gt)) {
			has_colon = expect(TokenKind::Colon, "':'");
		}
		if (has_colon) {
			const bool single_prim_const =
				at(TokenKind::Ident) && is_prim_type_name(peek().text) &&
				(peek(1).kind == TokenKind::Comma ||
				 (peek(1).kind == TokenKind::Operator && peek(1).op == Op::Gt));
			if (single_prim_const) {
				p.kind		 = GenericParam::Kind::Const;
				p.const_type = parse_type();
			} else if (!at(TokenKind::Comma) && !at_binop(Op::Gt) && !at(TokenKind::Eof)) {
				p.kind = GenericParam::Kind::Type;
				while (!at(TokenKind::Comma) && !at_binop(Op::Gt) && !at(TokenKind::Eof)) {
					if (at(TokenKind::Ident) &&
						(is_generic_type_set_name(peek().text) || is_prim_type_name(peek().text))) {
						GenericParam::TypeSetMember member;
						member.loc	= peek().loc;
						member.name = consume().text;
						p.type_set.push_back(std::move(member));
					} else {
						auto tr = parse_type();
						if (!tr) {
							break;
						}
						p.trait_bounds.push_back(std::move(tr));
					}
					if (at_binop(Op::Add) || at_binop(Op::BitOr)) {
						consume();
						if (at(TokenKind::Comma) || at_binop(Op::Gt) || at(TokenKind::Eof)) {
							log::report(log::Level::Error, peek().loc,
										"expected type or trait constraint after separator");
							break;
						}
					} else {
						break;
					}
				}
			} else {
				log::report(log::Level::Error, peek().loc, "expected type or constraint after ':'");
			}
		}
		out.push_back(std::move(p));
		if (!accept(TokenKind::Comma)) {
			break;
		}
	}
	if (!at_binop(Op::Gt)) {
		log::report(log::Level::Error, peek().loc,
					"expected '>' after generic parameter list but got '" + peek().text + "'");
		if (!at(TokenKind::Eof)) {
			consume();
		}
	} else {
		consume();
	}
}

void Parser::parse_generic_args(std::vector<GenericArg> &out) {
	if (!at_binop(Op::Lt)) {
		log::report(log::Level::Error, peek().loc,
					"expected '<' before generic arguments but got '" + peek().text + "'");
		return;
	}
	consume();
	while (!at_binop(Op::Gt) && !at(TokenKind::Eof)) {
		GenericArg a;
		a.loc	   = peek().loc;
		bool value = at(TokenKind::IntLit) || at(TokenKind::FloatLit) || at(TokenKind::StringLit) ||
					 at(TokenKind::CharLit) || at(TokenKind::KwTrue) || at(TokenKind::KwFalse) ||
					 at(TokenKind::KwNull) || at(TokenKind::LParen) || at_binop(Op::Sub) ||
					 at(TokenKind::Tilde) ||
					 (at(TokenKind::Ident) &&
					  ((peek(1).kind == TokenKind::Operator && peek(1).op != Op::Gt) ||
					   peek(1).kind == TokenKind::Assign ||
					   peek(1).kind == TokenKind::Question));
		if (value) {
			a.kind	= GenericArg::Kind::Value;
			bool was_parsing_generic_arg = parsing_generic_arg_;
			parsing_generic_arg_ = true;
			a.value = parse_expr();
			parsing_generic_arg_ = was_parsing_generic_arg;
		} else {
			a.kind = GenericArg::Kind::Type;
			a.type = parse_type();
		}
		out.push_back(std::move(a));
		if (!accept(TokenKind::Comma)) {
			break;
		}
	}
	if (!at_binop(Op::Gt)) {
		log::report(log::Level::Error, peek().loc,
					"expected '>' after generic argument list but got '" + peek().text + "'");
		if (!at(TokenKind::Eof)) {
			consume();
		}
	} else {
		consume();
	}
}

bool Parser::generic_call_suffix() const {
	if (!at_binop(Op::Lt)) {
		return false;
	}
	int	 depth					 = 0;
	int	 paren_depth			 = 0;
	bool saw_comparison_or_logic = false;
	for (size_t i = idx_; i < toks_.size(); ++i) {
		if (depth == 1 && paren_depth == 0 && (toks_[i].kind == TokenKind::Semicolon ||
						   toks_[i].kind == TokenKind::LBrace ||
						   toks_[i].kind == TokenKind::RBrace ||
						   toks_[i].kind == TokenKind::RParen)) {
			return false;
		}
		if (depth == 1 && paren_depth == 0) {
			if (toks_[i].kind == TokenKind::Assign ||
				(toks_[i].kind == TokenKind::Operator &&
				 (toks_[i].op == Op::LogicalAnd || toks_[i].op == Op::LogicalOr ||
				  toks_[i].op == Op::Eq || toks_[i].op == Op::Ne || toks_[i].op == Op::Le ||
				  toks_[i].op == Op::Ge))) {
				saw_comparison_or_logic = true;
			}
		}
		if (toks_[i].kind == TokenKind::LParen) {
			++paren_depth;
		} else if (toks_[i].kind == TokenKind::RParen && paren_depth > 0) {
			--paren_depth;
		}
		if (paren_depth == 0 && toks_[i].is_binop(Op::Lt)) {
			++depth;
		} else if (paren_depth == 0 && toks_[i].is_binop(Op::Gt) && --depth == 0) {
			if (i + 1 < toks_.size() && toks_[i + 1].kind == TokenKind::LParen) {
				return true;
			}
			if (!saw_comparison_or_logic && i + 2 < toks_.size() &&
				toks_[i + 1].kind == TokenKind::Dot && toks_[i + 2].kind == TokenKind::Ident) {
				return true;
			}
			return false;
		}
	}
	return false;
}

bool Parser::parse_import(Module &out, bool is_pub, AttributeList attrs) {
	return parse_import_into(out.decls, is_pub, std::move(attrs));
}
bool Parser::parse_import_into(std::vector<TopDeclPtr> &body, bool is_pub, AttributeList attrs) {
	auto imp   = std::make_unique<ImportDecl>();
	imp->loc   = peek().loc;
	imp->attrs = std::move(attrs);
	imp->is_pub = is_pub;
	expect(TokenKind::KwImport, "'import'");
	if (!at(TokenKind::StringLit)) {
		expect(TokenKind::StringLit, "string literal import path");
		return true;
	}
	imp->written_path = consume().text;
	expect(TokenKind::Semicolon, "';'");
	auto td	 = std::make_unique<TopDecl>();
	td->kind = TopKind::Import;
	td->imp	 = std::move(imp);
	body.push_back(std::move(td));
	return true;
}

std::unique_ptr<TypeRef> Parser::parse_type() {
	// Possible forms: bareword / `type` / a.b.B / *T / *mut T /
	// *volatile T / [N]T. Qualifiers belong to the pointer introduced by '*'.
	struct PointerQualifiers {
		SourceLoc loc;
		bool	  is_mutable  = false;
		bool	  is_volatile = false;
	};
	std::vector<PointerQualifiers> ptrs;
	while (at_binop(Op::Mul)) {
		SourceLoc ploc = peek().loc;
		consume();
		PointerQualifiers qualifiers;
		qualifiers.loc = ploc;
		// Accept either order so both `*mut volatile T` and
		// `*volatile mut T` remain unambiguous and compose naturally.
		while (at(TokenKind::KwMut) || at(TokenKind::KwVolatile)) {
			if (accept(TokenKind::KwMut)) {
				qualifiers.is_mutable = true;
			} else {
				consume();
				qualifiers.is_volatile = true;
			}
		}
		ptrs.push_back(qualifiers);
	}

	AttributeList type_attrs;
	if (at(TokenKind::Hash)) {
		parse_attributes(type_attrs);
		if (!at(TokenKind::KwFn)) {
			log::report(log::Level::Error, type_attrs.loc,
						"attributes on type references are only allowed on function types 'fn(...)'");
		}
	}

	std::unique_ptr<TypeRef> base;
	if (at(TokenKind::KwFn)) {
		// Function type: `fn(p0, p1, ...) -> ret` or `#[abi("C")] fn(p0, p1, ...) -> ret`
		SourceLoc loc = peek().loc;
		consume(); // fn
		expect(TokenKind::LParen, "'('");
		auto r		= std::make_unique<TypeRef>();
		r->kind		= TypeRef::Function;
		r->loc		= loc;
		r->fn_attrs = std::move(type_attrs);
		while (!at(TokenKind::RParen) && !at(TokenKind::Eof)) {
			if (accept(TokenKind::DotDotDot)) {
				r->fn_variadic = true;
				break;
			}
			auto p = parse_type();
			if (p) {
				r->fn_params.push_back(std::move(p));
			}
			if (!at(TokenKind::RParen)) {
				expect(TokenKind::Comma, "comma or ')'");
			}
		}
		expect(TokenKind::RParen, "')'");
		expect(TokenKind::Arrow, "'->'");
		r->fn_ret = parse_type();
		r->name	  = "fn";
		r->single = "fn";
		base	  = std::move(r);
	} else if (at(TokenKind::LBracket)) {
		// Slice type: []T / []mut T / []volatile T
		// Or array type: [N]T, or [_]T with a length inferred from an initializer.
		SourceLoc lloc = peek().loc;
		consume(); // [
		if (accept(TokenKind::RBracket)) {
			bool is_mutable	 = false;
			bool is_volatile = false;
			while (at(TokenKind::KwMut) || at(TokenKind::KwVolatile)) {
				if (accept(TokenKind::KwMut)) {
					is_mutable = true;
				} else {
					consume();
					is_volatile = true;
				}
			}
			auto elem	   = parse_type();
			auto r		   = std::make_unique<TypeRef>();
			r->kind		   = TypeRef::Slice;
			r->is_mutable  = is_mutable;
			r->is_volatile = is_volatile;
			r->base		   = std::move(elem);
			r->loc		   = lloc;
			base		   = std::move(r);
		} else {
			std::unique_ptr<Expr> len_expr;
			size_t				  len	   = 0;
			bool				  inferred = false;
			if (at(TokenKind::Ident) && peek().text == "_") {
				consume();
				inferred = true;
			} else {
				len_expr = parse_expr();
				len		 = (len_expr && len_expr->kind == ExprKind::IntLit)
							   ? static_cast<size_t>(len_expr->int_val)
							   : 0;
			}
			expect(TokenKind::RBracket, "']'");
			auto elem			  = parse_type();
			auto r				  = std::make_unique<TypeRef>();
			r->kind				  = TypeRef::Array;
			r->array_len		  = len;
			r->array_len_expr	  = std::move(len_expr);
			r->array_len_inferred = inferred;
			r->base				  = std::move(elem);
			r->loc				  = lloc;
			base				  = std::move(r);
		}
	} else if (at(TokenKind::BuiltinModuleRef) && peek().text == "of") {
		SourceLoc loc = peek().loc;
		consume();
		expect(TokenKind::Dot, "'.' after built-in module '@of'");
		if (!at(TokenKind::Ident) || peek().text != "type") {
			log::report(log::Level::Error, peek().loc,
						"expected member '@of.type' in a type reference");
			return nullptr;
		}
		consume();
		expect(TokenKind::LParen, "'(' after '@of.type'");
		auto r		   = std::make_unique<TypeRef>();
		r->kind		   = TypeRef::TypeOf;
		r->typeof_expr = parse_expr();
		r->loc		   = loc;
		expect(TokenKind::RParen, "')' after '@of.type'");
		base = std::move(r);
	} else if (at(TokenKind::KwType)) {
		auto r	  = std::make_unique<TypeRef>();
		r->kind	  = TypeRef::Named;
		r->loc	  = peek().loc;
		r->name	  = "type";
		r->single = "type";
		consume();
		base = std::move(r);
	} else if (at(TokenKind::KwSelf)) {
		auto r	  = std::make_unique<TypeRef>();
		r->kind	  = TypeRef::SelfType;
		r->loc	  = peek().loc;
		r->name	  = "Self";
		r->single = "Self";
		consume();
		base = std::move(r);
	} else if (at(TokenKind::Ident) || (at(TokenKind::BuiltinModuleRef) && peek().text == "va")) {
		// dotted path type -- primitive names (void/u32/...) lex as Ident now;
		// sema's prim_from_name resolves them. User-defined types also flow
		// through here.
		auto r	= std::make_unique<TypeRef>();
		r->kind = TypeRef::Named;
		r->loc	= peek().loc;
		std::vector<std::string> segs;
		if (at(TokenKind::BuiltinModuleRef)) {
			SourceLoc module_loc = consume().loc;
			expect(TokenKind::Dot, "'.' after built-in module '@va'");
			if (!at(TokenKind::Ident) || peek().text != "list") {
				log::report(log::Level::Error, peek().loc,
							"expected member '@va.list' in a type reference");
				return nullptr;
			}
			consume();
			segs.push_back("@va.list");
			r->loc = module_loc;
		} else {
			parse_dotted_path(segs);
		}
		r->name	  = stellac::join_path(segs);
		r->single = segs.back();
		base	  = std::move(r);
	} else {
		log::report(log::Level::Error, peek().loc,
					std::string("expected type, got '") + peek().text + "'");
		return nullptr;
	}
	if (base && base->kind == TypeRef::Named && at_binop(Op::Lt)) {
		parse_generic_args(base->generic_args);
	}

	// wrap pointers from right-to-left
	for (auto it = ptrs.rbegin(); it != ptrs.rend(); ++it) {
		auto p		   = std::make_unique<TypeRef>();
		p->kind		   = TypeRef::Pointer;
		p->loc		   = it->loc;
		p->is_mutable  = it->is_mutable;
		p->is_volatile = it->is_volatile;
		p->base		   = std::move(base);
		base		   = std::move(p);
	}
	return base;
}

bool Parser::parse_fn(Module &out, bool is_pub, AttributeList attrs, bool is_extern) {
	return parse_fn_into(out.decls, is_pub, std::move(attrs), is_extern);
}
bool Parser::parse_fn_into(std::vector<TopDeclPtr> &body, bool is_pub, AttributeList attrs,
						   bool is_extern) {
	auto f		 = std::make_unique<FnDecl>();
	f->is_pub	 = is_pub;
	f->is_extern = is_extern;
	f->loc		 = peek().loc;
	f->attrs	 = std::move(attrs);
	if (is_extern) {
		expect(TokenKind::KwExtern, "'extern'");
	}
	expect(TokenKind::KwFn, "'fn'");
	if (!at(TokenKind::Ident)) {
		log::report(log::Level::Error, peek().loc, "expected function name");
		return true;
	}
	f->name = consume().text;
	reject_reserved_name(f->loc, f->name, "function name");
	parse_generic_params(f->generic_params);
	size_t generic_type_params_start = generic_type_params_.size();
	for (const auto &param : f->generic_params) {
		if (param.kind == GenericParam::Kind::Type) {
			generic_type_params_.push_back(param.name);
		}
	}
	expect(TokenKind::LParen, "'('");
	while (!at(TokenKind::RParen) && !at(TokenKind::Eof)) {
		if (accept(TokenKind::DotDotDot)) {
			f->is_variadic = true;
			break;
		}
		Param p;
		parse_attributes(p.attrs); // per-parameter: #[noalias], etc.
		SourceLoc mut_loc = peek().loc;
		if (accept(TokenKind::KwMut)) {
			if (is_extern) {
				log::report(log::Level::Error, mut_loc,
							"'mut' is not allowed on 'extern fn' parameters");
			} else {
				p.is_mutable = true;
			}
		}
		if (!at(TokenKind::Ident)) {
			log::report(log::Level::Error, peek().loc, "expected parameter name");
			break;
		}
		SourceLoc ploc = peek().loc;
		p.name		   = consume().text;
		reject_reserved_name(ploc, p.name, "parameter name");
		expect(TokenKind::Colon, "':'");
		p.ty  = parse_type();
		p.loc = ploc;
		if (p.ty && p.ty->kind == TypeRef::Named && p.ty->single == "type") {
			p.is_comptime		  = true;
			if (p.is_mutable) {
				log::report(log::Level::Error, mut_loc,
							"'mut' is not allowed on comptime 'type' parameters");
				p.is_mutable = false;
			}
			bool already_declared = false;
			for (const auto &gp : f->generic_params) {
				if (gp.kind == GenericParam::Kind::Type && gp.name == p.name) {
					already_declared = true;
					break;
				}
			}
			if (!already_declared) {
				GenericParam gp;
				gp.kind = GenericParam::Kind::Type;
				gp.name = p.name;
				gp.loc	= p.loc;
				f->generic_params.push_back(std::move(gp));
				generic_type_params_.push_back(p.name);
			}
		}
		f->params.push_back(std::move(p));
		if (!at(TokenKind::RParen)) {
			expect(TokenKind::Comma, "Comma or ')'");
		}
	}
	expect(TokenKind::RParen, "')'");
	expect(TokenKind::Arrow, "'->'");
	f->ret = parse_type();
	if (!f->ret) {
		// Synthesize an explicit `void` return so downstream sema/codegen never
		// dereference a null ret TypeRef (e.g. resolve_fn_signature does
		// `resolve_type(*fn.ret, m)`).
		auto v	  = std::make_unique<TypeRef>();
		v->kind	  = TypeRef::Named;
		v->name	  = "void";
		v->single = "void";
		v->loc	  = peek().loc;
		f->ret	  = std::move(v);
	}
	if (is_extern) {
		expect(TokenKind::Semicolon, "';'");
		// extern fn is a declaration only -- no body.
	} else {
		f->body = parse_block();
	}
	generic_type_params_.resize(generic_type_params_start);
	auto td	 = std::make_unique<TopDecl>();
	td->kind = TopKind::Fn;
	td->fn	 = std::move(f);
	body.push_back(std::move(td));
	return true;
}

bool Parser::parse_macro_into(std::vector<TopDeclPtr> &body, bool is_pub, AttributeList attrs) {
	auto f = std::make_unique<FnDecl>();
	f->is_macro = true;
	f->is_pub = is_pub;
	f->loc = peek().loc;
	f->attrs = std::move(attrs);
	expect(TokenKind::KwMacro, "'macro'");
	if (!at(TokenKind::Ident)) {
		log::report(log::Level::Error, peek().loc, "expected macro name");
		return true;
	}
	f->name = consume().text;
	reject_reserved_name(f->loc, f->name, "macro name");
	parse_generic_params(f->generic_params);
	size_t generic_type_params_start = generic_type_params_.size();
	for (const auto &param : f->generic_params) {
		if (param.kind == GenericParam::Kind::Type) {
			generic_type_params_.push_back(param.name);
		}
	}
	expect(TokenKind::LParen, "'('");
	while (!at(TokenKind::RParen) && !at(TokenKind::Eof)) {
		if (accept(TokenKind::DotDotDot)) {
			f->is_variadic = true;
			if (!at(TokenKind::RParen)) {
				log::report(log::Level::Error, peek().loc,
							"macro variadic capture '...' must be the final parameter");
			}
			break;
		}
		Param p;
		parse_attributes(p.attrs);
		SourceLoc mut_loc = peek().loc;
		if (accept(TokenKind::KwMut)) {
			log::report(log::Level::Error, mut_loc,
						"'mut' is not allowed on macro parameters");
		}
		if (!at(TokenKind::Ident)) {
			log::report(log::Level::Error, peek().loc, "expected macro parameter name");
			break;
		}
		SourceLoc ploc = peek().loc;
		p.name = consume().text;
		reject_reserved_name(ploc, p.name, "macro parameter name");
		expect(TokenKind::Colon, "':'");
		p.ty = parse_type();
		p.loc = ploc;
		if (p.ty && p.ty->kind == TypeRef::Named && p.ty->single == "type") {
			p.is_comptime = true;
			bool already_declared = false;
			for (const auto &gp : f->generic_params) {
				if (gp.kind == GenericParam::Kind::Type && gp.name == p.name) {
					already_declared = true;
					break;
				}
			}
			if (!already_declared) {
				GenericParam gp;
				gp.kind = GenericParam::Kind::Type;
				gp.name = p.name;
				gp.loc = p.loc;
				f->generic_params.push_back(std::move(gp));
				generic_type_params_.push_back(p.name);
			}
		}
		f->params.push_back(std::move(p));
		if (!at(TokenKind::RParen)) {
			expect(TokenKind::Comma, "comma or ')'");
		}
	}
	expect(TokenKind::RParen, "')'");
	// A macro may be value-producing. Omitting the return type preserves the
	// statement macro form; a block containing `yield` can still infer its
	// value type at the call site.
	if (!at(TokenKind::LBrace)) {
		expect(TokenKind::Arrow, "'->'");
		f->ret = parse_type();
		f->macro_returns_value = true;
	} else {
		f->ret = std::make_unique<TypeRef>();
		f->ret->kind = TypeRef::Named;
		f->ret->name = "void";
		f->ret->single = "void";
		f->ret->loc = f->loc;
	}
	f->body = parse_block();
	generic_type_params_.resize(generic_type_params_start);
	auto td = std::make_unique<TopDecl>();
	td->kind = TopKind::Fn;
	td->fn = std::move(f);
	body.push_back(std::move(td));
	return true;
}

bool Parser::parse_mod_into(std::vector<TopDeclPtr> &body, bool is_pub, AttributeList attrs,
							bool inside_block) {
	SourceLoc loc = peek().loc;
	expect(TokenKind::KwMod, "'mod'");
	std::vector<std::string> segs;
	if (!parse_dotted_path(segs)) {
		log::report(log::Level::Error, peek().loc, "expected module name");
		return true;
	}
	for (const auto &seg : segs) {
		reject_reserved_name(loc, seg, "module name");
	}
	if (accept(TokenKind::Semicolon)) {
		if (is_pub) {
			log::error(loc, "file-level 'mod' declaration cannot be declared 'pub'")
				.code("E_PARSE_UNEXPECTED_TOKEN")
				.phase("parser")
				.emit();
		} else if (inside_block) {
			log::error(loc, "file-level 'mod' declaration is not allowed inside a block")
				.code("E_PARSE_UNEXPECTED_TOKEN")
				.phase("parser")
				.emit();
		} else {
			log::error(
				loc,
				"at most one file-level 'mod' declaration is allowed per file, and it must appear before all other declarations")
				.code("E_PARSE_UNEXPECTED_TOKEN")
				.phase("parser")
				.emit();
		}
		return true;
	}
	if (segs.size() > 1) {
		log::report(log::Level::Error, loc,
					"block-level 'mod' declaration name must be a single identifier");
	}
	auto m	   = std::make_unique<ModDecl>();
	m->name	   = segs.back();
	m->is_pub  = is_pub;
	m->loc	   = loc;
	m->attrs   = std::move(attrs);
	expect(TokenKind::LBrace, "'{'");
	bool outer_seen_non_import = seen_non_import_;
	seen_non_import_		   = false;
	while (!at(TokenKind::RBrace) && !at(TokenKind::Eof)) {
		TopKind tk = parse_top_into(m->body, true);
		if (tk == TopKind::Error && !at(TokenKind::Eof)) {
			consume();
		}
	}
	expect(TokenKind::RBrace, "'}'");
	seen_non_import_ = outer_seen_non_import;
	auto td		   = std::make_unique<TopDecl>();
	td->kind	   = TopKind::Mod;
	td->mod_decl   = std::move(m);
	body.push_back(std::move(td));
	return true;
}

bool Parser::parse_struct(Module &out, bool is_pub, AttributeList attrs) {
	return parse_struct_into(out.decls, is_pub, std::move(attrs));
}
bool Parser::parse_struct_into(std::vector<TopDeclPtr> &body, bool is_pub, AttributeList attrs) {
	auto s	  = std::make_unique<StructDecl>();
	s->is_pub = is_pub;
	s->loc	  = peek().loc;
	s->attrs  = std::move(attrs);
	expect(TokenKind::KwStruct, "'struct'");
	if (!at(TokenKind::Ident)) {
		log::report(log::Level::Error, peek().loc, "expected struct name");
		return true;
	}
	s->name = consume().text;
	reject_reserved_name(s->loc, s->name, "struct name");
	parse_generic_params(s->generic_params);
	if (accept(TokenKind::Semicolon)) {
		s->is_opaque = true;
		auto td	 = std::make_unique<TopDecl>();
		td->kind = TopKind::Struct;
		td->st	 = std::move(s);
		body.push_back(std::move(td));
		return true;
	}
	expect(TokenKind::LBrace, "'{'");
	while (!at(TokenKind::RBrace) && !at(TokenKind::Eof)) {
		const bool field_pub = accept(TokenKind::KwPub);
		if (!at(TokenKind::Ident)) {
			log::error(peek().loc, "expected struct field name, got '" + peek().text + "'")
				.code("E_PARSE_EXPECTED_TOKEN")
				.phase("parser")
				.emit();
			int brace_depth = 0;
			while (!at(TokenKind::Eof)) {
				if (at(TokenKind::LBrace)) {
					++brace_depth;
				} else if (at(TokenKind::RBrace)) {
					if (brace_depth == 0) {
						break;
					}
					--brace_depth;
				} else if (brace_depth == 0 &&
						   (at(TokenKind::Comma) || at(TokenKind::Semicolon))) {
					consume();
					break;
				}
				consume();
			}
			continue;
		}
		SourceLoc	floc = peek().loc;
		std::string nm	 = consume().text;
		reject_reserved_name(floc, nm, "field name");
		if (accept(TokenKind::Colon)) {
			s->field_names.push_back(std::move(nm));
			s->field_types.push_back(parse_type());
			s->field_is_pub.push_back(field_pub);
		} else {
			auto diag = log::error(peek().loc, "expected ':' after struct field name")
							.code("E_PARSE_EXPECTED_TOKEN")
							.phase("parser");
			SourceSpan ins_span;
			ins_span.start		  = peek().loc;
			ins_span.start.length = 0;
			ins_span.end		  = ins_span.start;
			diag.fixit(ins_span, ": ", "insert `: `");
			diag.emit();
			if (!at(TokenKind::Comma) && !at(TokenKind::RBrace) && !at(TokenKind::Eof)) {
				s->field_names.push_back(std::move(nm));
				s->field_types.push_back(parse_type());
				s->field_is_pub.push_back(field_pub);
			}
		}
		if (!accept(TokenKind::Comma) && !at(TokenKind::RBrace)) {
			expect(TokenKind::Comma, "','");
		}
	}
	expect(TokenKind::RBrace, "'}'");
	auto td	 = std::make_unique<TopDecl>();
	td->kind = TopKind::Struct;
	td->st	 = std::move(s);
	body.push_back(std::move(td));
	return true;
}

bool Parser::parse_union(Module &out, bool is_pub, AttributeList attrs) {
	return parse_union_into(out.decls, is_pub, std::move(attrs));
}

bool Parser::parse_union_into(std::vector<TopDeclPtr> &body, bool is_pub, AttributeList attrs) {
	auto u	  = std::make_unique<UnionDecl>();
	u->is_pub = is_pub;
	u->loc	  = peek().loc;
	u->attrs  = std::move(attrs);
	expect(TokenKind::KwUnion, "'union'");
	if (!at(TokenKind::Ident)) {
		log::report(log::Level::Error, peek().loc, "expected union name");
		return true;
	}
	u->name = consume().text;
	reject_reserved_name(u->loc, u->name, "union name");
	parse_generic_params(u->generic_params);
	expect(TokenKind::LBrace, "'{' ");
	while (!at(TokenKind::RBrace) && !at(TokenKind::Eof)) {
		const bool field_pub = accept(TokenKind::KwPub);
		if (!at(TokenKind::Ident)) {
			log::report(log::Level::Error, peek().loc, "expected field name");
			break;
		}
		SourceLoc	floc = peek().loc;
		std::string nm	 = consume().text;
		reject_reserved_name(floc, nm, "field name");
		if (accept(TokenKind::Colon)) {
			u->field_names.push_back(std::move(nm));
			u->field_types.push_back(parse_type());
			u->field_is_pub.push_back(field_pub);
		} else {
			auto diag = log::error(peek().loc, "expected ':' after union field name")
							.code("E_PARSE_EXPECTED_TOKEN")
							.phase("parser");
			SourceSpan ins_span;
			ins_span.start		  = peek().loc;
			ins_span.start.length = 0;
			ins_span.end		  = ins_span.start;
			diag.fixit(ins_span, ": ", "insert `: `");
			diag.emit();
			if (!at(TokenKind::Comma) && !at(TokenKind::Semicolon) && !at(TokenKind::RBrace) &&
				!at(TokenKind::Eof)) {
				u->field_names.push_back(std::move(nm));
				u->field_types.push_back(parse_type());
				u->field_is_pub.push_back(field_pub);
			}
		}
		if (!accept(TokenKind::Comma) && !at(TokenKind::RBrace)) {
			expect(TokenKind::Comma, "','");
		}
	}
	expect(TokenKind::RBrace, "'}'");
	auto td	 = std::make_unique<TopDecl>();
	td->kind = TopKind::Union;
	td->un	 = std::move(u);
	body.push_back(std::move(td));
	return true;
}

bool Parser::parse_enum(Module &out, bool is_pub, AttributeList attrs) {
	return parse_enum_into(out.decls, is_pub, std::move(attrs));
}
bool Parser::parse_enum_into(std::vector<TopDeclPtr> &body, bool is_pub, AttributeList attrs) {
	auto e	  = std::make_unique<EnumDecl>();
	e->is_pub = is_pub;
	e->loc	  = peek().loc;
	e->attrs  = std::move(attrs);
	expect(TokenKind::KwEnum, "'enum'");
	if (!at(TokenKind::Ident)) {
		log::report(log::Level::Error, peek().loc, "expected enum name");
		return true;
	}
	e->name = consume().text;
	reject_reserved_name(e->loc, e->name, "enum name");
	parse_generic_params(e->generic_params);
	if (accept(TokenKind::Colon)) {
		e->backing = parse_type();
	} else {
		auto diag = log::error(peek().loc, "expected ':' before enum underlying type")
						.code("E_PARSE_EXPECTED_TOKEN")
						.phase("parser");
		SourceSpan ins_span;
		ins_span.start		  = peek().loc;
		ins_span.start.length = 0;
		ins_span.end		  = ins_span.start;
		diag.fixit(ins_span, ": ", "insert `: `");
		diag.emit();
		if (!at(TokenKind::LBrace) && !at(TokenKind::Eof)) {
			e->backing = parse_type();
		}
	}
	if (!e->backing) {
		return true;
	}
	expect(TokenKind::LBrace, "'{'");
	while (!at(TokenKind::RBrace) && !at(TokenKind::Eof)) {
		if (!at(TokenKind::Ident)) {
			log::report(log::Level::Error, peek().loc, "expected enum member name");
			break;
		}
		EnumMember m;
		m.loc  = peek().loc;
		m.name = consume().text;
		reject_reserved_name(m.loc, m.name, "enum member name");
		if (accept(TokenKind::LParen)) {
			m.payload_kind = EnumPayloadKind::Tuple;
			while (!at(TokenKind::RParen) && !at(TokenKind::Eof)) {
				EnumVariantField f;
				f.loc = peek().loc;
				f.ty  = parse_type();
				m.payload_fields.push_back(std::move(f));
				if (!accept(TokenKind::Comma)) {
					break;
				}
			}
			expect(TokenKind::RParen, "')'");
		} else if (accept(TokenKind::LBrace)) {
			m.payload_kind = EnumPayloadKind::Struct;
			while (!at(TokenKind::RBrace) && !at(TokenKind::Eof)) {
				if (!at(TokenKind::Ident)) {
					log::report(log::Level::Error, peek().loc, "expected variant field name");
					break;
				}
				EnumVariantField f;
				f.loc  = peek().loc;
				f.name = consume().text;
				reject_reserved_name(f.loc, f.name, "variant field name");
				expect(TokenKind::Colon, "':'");
				f.ty = parse_type();
				m.payload_fields.push_back(std::move(f));
				if (!accept(TokenKind::Comma)) {
					break;
				}
			}
			expect(TokenKind::RBrace, "'}'");
		}
		if (accept(TokenKind::Assign)) {
			m.value = parse_expr();
		}
		e->members.push_back(std::move(m));
		// Enum members are comma-separated. A trailing comma before the closing
		// brace is optional.
		if (!accept(TokenKind::Comma) && !at(TokenKind::RBrace)) {
			expect(TokenKind::Comma, "','");
		}
	}
	expect(TokenKind::RBrace, "'}'");
	auto td	 = std::make_unique<TopDecl>();
	td->kind = TopKind::Enum;
	td->en	 = std::move(e);
	body.push_back(std::move(td));
	return true;
}

bool Parser::parse_global(Module &out, bool is_pub, bool is_const, AttributeList attrs,
						  bool is_extern) {
	return parse_global_into(out.decls, is_pub, is_const, std::move(attrs), is_extern);
}
bool Parser::parse_global_into(std::vector<TopDeclPtr> &body, bool is_pub, bool is_const,
							   AttributeList attrs, bool is_extern) {
	if (is_extern) {
		expect(TokenKind::KwExtern, "'extern'");
	}
	consume(); // let/const
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
		return true;
	}
	std::vector<std::pair<std::string, SourceLoc>> names;
	while (true) {
		SourceLoc	gloc = peek().loc;
		std::string name = consume().text;
		reject_reserved_name(gloc, name, "binding name");
		names.emplace_back(std::move(name), gloc);
		if (!accept(TokenKind::Comma)) {
			break;
		}
		if (!at(TokenKind::Ident)) {
			log::report(log::Level::Error, peek().loc, "expected binding name after ','");
			break;
		}
	}
	std::vector<GenericParam> generic_params;
	if (is_const) {
		parse_generic_params(generic_params);
	}
	expect(TokenKind::Colon, "':'");
	auto				  ty = parse_type();
	std::unique_ptr<Expr> init;
	const bool is_type_binding = is_const && names.size() == 1 && ty &&
								 ty->kind == TypeRef::Named && ty->single == "type";
	if (is_type_binding && is_generic_type_set_name(names.front().first)) {
		reject_reserved_name(names.front().second, names.front().first, "type binding name");
	}
	std::unique_ptr<TypeRef> type_target;
	if (is_type_binding) {
		if (is_extern) {
			log::report(log::Level::Error, names.front().second,
						"'extern' cannot be used on a compile-time type binding");
		}
		expect(TokenKind::Assign, "'='");
		type_target = parse_type();
	} else if (accept(TokenKind::Assign)) {
		if (is_extern) {
			log::report(log::Level::Error, peek().loc,
						"'extern' global declarations cannot have an initializer");
		}
		init = parse_expr();
	}
	expect(TokenKind::Semicolon, "';'");
	if (is_type_binding) {
		if (is_thread_local) {
			log::report(log::Level::Error, names.front().second,
						"'thread_local' cannot be used on a compile-time type binding");
		}
		if (is_static) {
			log::report(log::Level::Error, names.front().second,
						"'static' cannot be used on a compile-time type binding");
		}
		auto t	 = std::make_unique<TypeBindingDecl>();
		t->name	 = names.front().first;
		t->target = std::move(type_target);
		t->is_pub = is_pub;
		t->loc	 = names.front().second;
		t->attrs	 = attrs;
		t->generic_params = std::move(generic_params);
		auto td	 = std::make_unique<TopDecl>();
		td->kind = TopKind::TypeBinding;
		td->tb	 = std::move(t);
		body.push_back(std::move(td));
		return true;
	}
	for (auto &[name, loc] : names) {
		auto g		= std::make_unique<GlobalDecl>();
		g->is_pub	= is_pub;
		g->is_extern = is_extern;
		g->is_const = is_const;
		g->is_mutable = is_mutable;
		g->is_static = is_static;
		g->is_thread_local = is_thread_local;
		g->loc		= loc;
		g->attrs	= attrs;
		g->name		= name;
		if (ty) {
			g->ty = clone_typeref(*ty);
		}
		if (init) {
			g->init = clone_expr(*init);
		}
		auto td	 = std::make_unique<TopDecl>();
		td->kind = is_const ? TopKind::Const : TopKind::Var;
		td->glob = std::move(g);
		body.push_back(std::move(td));
	}
	return true;
}

bool Parser::parse_trait_into(std::vector<TopDeclPtr> &body, bool is_pub, AttributeList attrs) {
	auto tr = std::make_unique<TraitDecl>();
	tr->is_pub = is_pub;
	tr->loc = peek().loc;
	tr->attrs = std::move(attrs);
	expect(TokenKind::KwTrait, "'trait'");
	if (!at(TokenKind::Ident)) {
		log::report(log::Level::Error, peek().loc, "expected trait name");
		return false;
	}
	tr->name = consume().text;
	reject_reserved_name(tr->loc, tr->name, "trait name");
	expect(TokenKind::LBrace, "'{'");
	while (!at(TokenKind::RBrace) && !at(TokenKind::Eof)) {
		AttributeList method_attrs;
		parse_attributes(method_attrs);
		bool method_pub = false;
		if (accept(TokenKind::KwPub)) {
			method_pub = true;
		}
		if (!at(TokenKind::KwFn)) {
			log::report(log::Level::Error, peek().loc, "expected function prototype inside trait declaration");
			if (!at(TokenKind::Eof)) consume();
			continue;
		}
		SourceLoc fn_loc = peek().loc;
		consume(); // 'fn'
		if (!at(TokenKind::Ident)) {
			log::report(log::Level::Error, peek().loc, "expected method name in trait");
			continue;
		}
		std::string method_name = consume().text;
		reject_reserved_name(fn_loc, method_name, "method name");
		auto m = std::make_unique<FnDecl>();
		m->name = method_name;
		m->is_pub = is_pub || method_pub;
		m->loc = fn_loc;
		m->attrs = std::move(method_attrs);
		m->owner_path = {tr->name};

		expect(TokenKind::LParen, "'('");
		while (!at(TokenKind::RParen) && !at(TokenKind::Eof)) {
			if (accept(TokenKind::DotDotDot)) {
				m->is_variadic = true;
				break;
			}
			Param p;
			parse_attributes(p.attrs);
			SourceLoc mut_loc = peek().loc;
			if (accept(TokenKind::KwMut)) {
				log::report(log::Level::Error, mut_loc,
							"'mut' is not allowed in trait method prototypes");
			}
			if (!at(TokenKind::Ident)) {
				log::report(log::Level::Error, peek().loc, "expected parameter name");
				break;
			}
			p.loc = peek().loc;
			p.name = consume().text;
			reject_reserved_name(p.loc, p.name, "parameter name");
			expect(TokenKind::Colon, "':'");
			p.ty = parse_type();
			m->params.push_back(std::move(p));
			if (!at(TokenKind::RParen)) {
				expect(TokenKind::Comma, "',' or ')'");
			}
		}
		expect(TokenKind::RParen, "')'");
		expect(TokenKind::Arrow, "'->'");
		m->ret = parse_type();
		if (!m->ret) {
			auto v = std::make_unique<TypeRef>();
			v->kind = TypeRef::Named;
			v->name = "void";
			v->single = "void";
			v->loc = peek().loc;
			m->ret = std::move(v);
		}
		if (at(TokenKind::LBrace)) {
			log::error(peek().loc, "trait methods cannot have default implementations")
				.code("E_TRAIT_DEFAULT_IMPL")
				.phase("parser")
				.emit();
			parse_block();
		} else {
			expect(TokenKind::Semicolon, "';' after trait method prototype");
		}
		tr->methods.push_back(std::move(m));
	}
	expect(TokenKind::RBrace, "'}'");
	auto td = std::make_unique<TopDecl>();
	td->kind = TopKind::Trait;
	td->trait_decl = std::move(tr);
	body.push_back(std::move(td));
	return true;
}

bool Parser::parse_impl_into(std::vector<TopDeclPtr> &body, AttributeList attrs) {
	auto im = std::make_unique<ImplDecl>();
	im->loc = peek().loc;
	im->attrs = std::move(attrs);
	expect(TokenKind::KwImpl, "'impl'");
	parse_generic_params(im->generic_params);
	size_t impl_generic_params_start = generic_type_params_.size();
	for (const auto &param : im->generic_params) {
		if (param.kind == GenericParam::Kind::Type) {
			generic_type_params_.push_back(param.name);
		}
	}
	auto first_type = parse_type();
	if (!first_type) {
		log::report(log::Level::Error, peek().loc, "expected type or trait name after 'impl'");
		generic_type_params_.resize(impl_generic_params_start);
		return false;
	}
	if (accept(TokenKind::KwFor)) {
		im->is_inherent = false;
		if (first_type->kind != TypeRef::Named || !first_type->generic_args.empty()) {
			log::report(log::Level::Error, first_type->loc,
						"expected trait name before 'for' in impl declaration");
			generic_type_params_.resize(impl_generic_params_start);
			return false;
		}
		std::string full_trait =
			first_type->name.empty() ? first_type->single : first_type->name;
		size_t start = 0;
		while (start < full_trait.size()) {
			size_t dot = full_trait.find('.', start);
			if (dot == std::string::npos) {
				im->trait_path.push_back(full_trait.substr(start));
				break;
			}
			im->trait_path.push_back(full_trait.substr(start, dot - start));
			start = dot + 1;
		}
		im->trait_name = first_type->single;
		im->target_type = parse_type();
		if (!im->target_type) {
			log::report(log::Level::Error, peek().loc, "expected target type after 'for'");
			generic_type_params_.resize(impl_generic_params_start);
			return false;
		}
	} else {
		im->is_inherent = true;
		im->target_type = std::move(first_type);
	}
	if (im->generic_params.empty() && im->target_type &&
		im->target_type->kind == TypeRef::Named) {
		for (const auto &arg : im->target_type->generic_args) {
			if (arg.kind == GenericArg::Kind::Type && arg.type &&
				arg.type->kind == TypeRef::Named && arg.type->generic_args.empty() &&
				(arg.type->name.empty() || arg.type->name == arg.type->single) &&
				!is_prim_type_name(arg.type->single)) {
				generic_type_params_.push_back(arg.type->single);
			}
		}
	}
	expect(TokenKind::LBrace, "'{'");
	std::string type_name =
		im->target_type->single.empty() ? im->target_type->name : im->target_type->single;
	while (!at(TokenKind::RBrace) && !at(TokenKind::Eof)) {
		AttributeList item_attrs;
		parse_attributes(item_attrs);
		bool item_pub = accept(TokenKind::KwPub);
		if (at(TokenKind::KwConst)) {
			SourceLoc cloc = peek().loc;
			consume(); // 'const'
			if (!im->is_inherent) {
				log::report(log::Level::Error, cloc,
							"associated constants are only allowed in inherent 'impl' blocks");
			}
			if (!at(TokenKind::Ident)) {
				log::report(log::Level::Error, peek().loc, "expected constant name in impl block");
				if (!at(TokenKind::Eof)) consume();
				continue;
			}
			std::string cname = consume().text;
			reject_reserved_name(cloc, cname, "constant name");
			expect(TokenKind::Colon, "':'");
			auto cty = parse_type();
			expect(TokenKind::Assign, "'='");
			auto cinit = parse_expr();
			expect(TokenKind::Semicolon, "';'");
			if (im->is_inherent) {
				auto g = std::make_unique<GlobalDecl>();
				g->is_pub = item_pub;
				g->is_const = true;
				g->loc = cloc;
				g->attrs = std::move(item_attrs);
				g->name = std::move(cname);
				g->owner_path = {type_name};
				g->ty = std::move(cty);
				g->init = std::move(cinit);
				im->consts.push_back(std::move(g));
			}
			continue;
		}
		if (!at(TokenKind::KwFn)) {
			log::report(log::Level::Error, peek().loc,
						im->is_inherent
							? "expected function or constant definition inside impl block"
							: "expected function definition inside impl block");
			if (!at(TokenKind::Eof)) consume();
			continue;
		}
		SourceLoc fn_loc = peek().loc;
		consume(); // 'fn'
		if (!at(TokenKind::Ident)) {
			log::report(log::Level::Error, peek().loc, "expected method name in impl");
			continue;
		}
		std::string method_name = consume().text;
		reject_reserved_name(fn_loc, method_name, "method name");
		auto m = std::make_unique<FnDecl>();
		m->name = method_name;
		m->is_pub = item_pub;
		m->is_method = true;
		m->loc = fn_loc;
		m->attrs = std::move(item_attrs);
		if (im->is_inherent) {
			m->owner_path = {type_name};
		} else {
			m->owner_path = {im->trait_name, type_name};
		}
		parse_generic_params(m->generic_params);
		size_t fn_generic_params_start = generic_type_params_.size();
		for (const auto &param : m->generic_params) {
			if (param.kind == GenericParam::Kind::Type) {
				generic_type_params_.push_back(param.name);
			}
		}

		expect(TokenKind::LParen, "'('");
		while (!at(TokenKind::RParen) && !at(TokenKind::Eof)) {
			if (accept(TokenKind::DotDotDot)) {
				m->is_variadic = true;
				break;
			}
			Param p;
			parse_attributes(p.attrs);
			SourceLoc mut_loc = peek().loc;
			if (accept(TokenKind::KwMut)) {
				p.is_mutable = true;
			}
			if (!at(TokenKind::Ident)) {
				log::report(log::Level::Error, peek().loc, "expected parameter name");
				break;
			}
			p.loc = peek().loc;
			p.name = consume().text;
			reject_reserved_name(p.loc, p.name, "parameter name");
			expect(TokenKind::Colon, "':'");
			p.ty = parse_type();
			if (p.ty && p.ty->kind == TypeRef::Named && p.ty->single == "type") {
				p.is_comptime = true;
				if (p.is_mutable) {
					log::report(log::Level::Error, mut_loc,
								"'mut' is not allowed on comptime 'type' parameters");
					p.is_mutable = false;
				}
				bool already_declared = false;
				for (const auto &gp : m->generic_params) {
					if (gp.kind == GenericParam::Kind::Type && gp.name == p.name) {
						already_declared = true;
						break;
					}
				}
				if (!already_declared) {
					GenericParam gp;
					gp.kind = GenericParam::Kind::Type;
					gp.name = p.name;
					gp.loc = p.loc;
					m->generic_params.push_back(std::move(gp));
					generic_type_params_.push_back(p.name);
				}
			}
			m->params.push_back(std::move(p));
			if (!at(TokenKind::RParen)) {
				expect(TokenKind::Comma, "',' or ')'");
			}
		}
		expect(TokenKind::RParen, "')'");
		expect(TokenKind::Arrow, "'->'");
		m->ret = parse_type();
		if (!m->ret) {
			auto v = std::make_unique<TypeRef>();
			v->kind = TypeRef::Named;
			v->name = "void";
			v->single = "void";
			v->loc = peek().loc;
			m->ret = std::move(v);
		}
		if (!at(TokenKind::LBrace)) {
			log::report(log::Level::Error, peek().loc, "method in 'impl' block must have a body");
			if (accept(TokenKind::Semicolon)) {
				// skip
			}
		} else {
			m->body = parse_block();
		}
		generic_type_params_.resize(fn_generic_params_start);
		im->methods.push_back(std::move(m));
	}
	expect(TokenKind::RBrace, "'}'");
	generic_type_params_.resize(impl_generic_params_start);
	auto td = std::make_unique<TopDecl>();
	td->kind = TopKind::Impl;
	td->impl_decl = std::move(im);
	body.push_back(std::move(td));
	return true;
}

// statements

} // namespace stellac
