#include "lexer.hpp"
#include "intlit.hpp"
#include "log.hpp"
#include <llvm/ADT/APFloat.h>
#include <llvm/Support/Error.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>

namespace stellac {

static int source_length(size_t length) {
	return static_cast<int>(std::min(length,
		static_cast<size_t>(std::numeric_limits<int>::max())));
}

static int source_length_with_prefix(size_t length) {
	const size_t maximum = static_cast<size_t>(std::numeric_limits<int>::max());
	return length >= maximum ? std::numeric_limits<int>::max()
							  : static_cast<int>(length + 1);
}

Lexer::Lexer(std::string source, std::string fname, bool emit_diagnostics)
	: src_(std::move(source)), fname_(std::move(fname)), emit_diagnostics_(emit_diagnostics) {}

void Lexer::report(log::Level level, const SourceLoc &loc, const std::string &message) {
	if (emit_diagnostics_) {
		log::report(level, loc, message);
	}
}

char Lexer::peek(size_t off) const {
	return (pos_ + off < src_.size()) ? src_[pos_ + off] : '\0';
}
char Lexer::adv() {
	char c = (pos_ < src_.size()) ? src_[pos_++] : '\0';
	if (c == '\n') {
		++line_;
		col_ = 1;
	} else {
		++col_;
	}
	return c;
}
bool Lexer::eat(char c) {
	if (peek() == c) {
		adv();
		return true;
	}
	return false;
}

Token &Lexer::make(TokenKind k) {
	Token t;
	t.kind		 = k;
	t.loc.file	 = fname_;
	t.loc.line	 = line_;
	t.loc.column = col_;
	out_->push_back(std::move(t));
	return out_->back();
}

static bool is_ident_start(char c) {
	return std::isalpha((unsigned char)c) || c == '_';
}
static bool is_ident_cont(char c) {
	return std::isalnum((unsigned char)c) || c == '_';
}

static TokenKind kw_lookup(std::string_view s) {
	switch (s.size()) {
	case 2:
		if (s == "fn") return TokenKind::KwFn;
		if (s == "if") return TokenKind::KwIf;
		break;
	case 3:
		if (s == "mod") return TokenKind::KwMod;
		if (s == "pub") return TokenKind::KwPub;
		if (s == "let") return TokenKind::KwLet;
		if (s == "mut") return TokenKind::KwMut;
		if (s == "for") return TokenKind::KwFor;
		if (s == "asm") return TokenKind::KwAsm;
		break;
	case 4:
		if (s == "Self") return TokenKind::KwSelf;
		if (s == "else") return TokenKind::KwElse;
		if (s == "true") return TokenKind::KwTrue;
		if (s == "null") return TokenKind::KwNull;
		if (s == "enum") return TokenKind::KwEnum;
		if (s == "impl") return TokenKind::KwImpl;
		if (s == "cast") return TokenKind::KwCast;
		break;
	case 5:
		if (s == "macro") return TokenKind::KwMacro;
		if (s == "const") return TokenKind::KwConst;
		if (s == "trait") return TokenKind::KwTrait;
		if (s == "match") return TokenKind::KwMatch;
		if (s == "break") return TokenKind::KwBreak;
		if (s == "defer") return TokenKind::KwDefer;
		if (s == "yield") return TokenKind::KwYield;
		if (s == "false") return TokenKind::KwFalse;
		if (s == "union") return TokenKind::KwUnion;
		break;
	case 6:
		if (s == "import") return TokenKind::KwImport;
		if (s == "extern") return TokenKind::KwExtern;
		if (s == "static") return TokenKind::KwStatic;
		if (s == "struct") return TokenKind::KwStruct;
		if (s == "return") return TokenKind::KwReturn;
		break;
	case 7:
		if (s == "bitcast") return TokenKind::KwBitcast;
		break;
	case 8:
		if (s == "continue") return TokenKind::KwContinue;
		if (s == "volatile") return TokenKind::KwVolatile;
		break;
	case 11:
		if (s == "unreachable") return TokenKind::KwUnreachable;
		break;
	case 12:
		if (s == "thread_local") return TokenKind::KwThreadLocal;
		break;
	default:
		break;
	}
	return TokenKind::Ident;
}

bool Lexer::tokenize(std::vector<Token> &out) {
	out_ = &out;
	if (out.empty() && src_.size() >= 16) {
		out.reserve(src_.size() / 4);
	}
	while (pos_ < src_.size()) {
		skip_ws_comments();
		if (pos_ >= src_.size()) {
			break;
		}
		char	  c			 = peek();
		const int start_line = line_;
		const int start_col	 = col_;

		if (c == 'c' && peek(1) == '"') {
			if (!lex_string(true)) {
				return false;
			}
			continue;
		}
		if (is_ident_start(c)) {
			if (!lex_ident_or_kw()) {
				return false;
			}
			continue;
		}
		if (c == '@') {
			if (!lex_builtin_module()) {
				return false;
			}
			continue;
		}
		if (c == '#' && peek(1) != '[' && is_ident_start(peek(1))) {
			if (!lex_define_ref()) {
				return false;
			}
			continue;
		}
		if (std::isdigit((unsigned char)c)) {
			if (!lex_number()) {
				return false;
			}
			continue;
		}
		if (c == '"') {
			if (!lex_string()) {
				return false;
			}
			continue;
		}
		if (c == '\'') {
			if (!lex_char()) {
				return false;
			}
			continue;
		}

		// Single/multi-char punctuation
		Token &t = make(TokenKind::Unknown);
		t.loc	 = SourceLoc{fname_, start_line, start_col};
		adv();
		switch (c) {
		case '(':
			t.kind = TokenKind::LParen;
			t.text = "(";
			break;
		case ')':
			t.kind = TokenKind::RParen;
			t.text = ")";
			break;
		case '{':
			t.kind = TokenKind::LBrace;
			t.text = "{";
			break;
		case '}':
			t.kind = TokenKind::RBrace;
			t.text = "}";
			break;
		case '[':
			t.kind = TokenKind::LBracket;
			t.text = "[";
			break;
		case ']':
			t.kind = TokenKind::RBracket;
			t.text = "]";
			break;
		case ',':
			t.kind = TokenKind::Comma;
			t.text = ",";
			break;
		case '#':
			t.kind = TokenKind::Hash;
			t.text = "#";
			break;
		case ';':
			t.kind = TokenKind::Semicolon;
			t.text = ";";
			break;
		case '~':
			t.kind = TokenKind::Tilde;
			t.text = "~";
			break;
		case '^':
			if (peek() == '=') {
				adv();
				t.kind				  = TokenKind::Operator;
				t.op				  = Op::BitXor;
				t.compound_assignment = true;
				t.text				  = "^=";
			} else {
				t.kind = TokenKind::Operator;
				t.op   = Op::BitXor;
				t.text = "^";
			}
			break;
		case '%':
			if (peek() == '=') {
				adv();
				t.kind				  = TokenKind::Operator;
				t.op				  = Op::Mod;
				t.compound_assignment = true;
				t.text				  = "%=";
			} else {
				t.kind = TokenKind::Operator;
				t.op   = Op::Mod;
				t.text = "%";
			}
			break;
		case '+':
			if (peek() == '%') {
				adv();
				t.kind = TokenKind::Operator;
				t.op   = Op::AddWrap;
				if (peek() == '=') {
					adv();
					t.compound_assignment = true;
					t.text				  = "+%=";
				} else {
					t.text = "+%";
				}
			} else if (peek() == '|') {
				adv();
				t.kind = TokenKind::Operator;
				t.op   = Op::AddSat;
				if (peek() == '=') {
					adv();
					t.compound_assignment = true;
					t.text				  = "+|=";
				} else {
					t.text = "+|";
				}
			} else if (peek() == '=') {
				adv();
				t.kind				  = TokenKind::Operator;
				t.op				  = Op::Add;
				t.compound_assignment = true;
				t.text				  = "+=";
			} else {
				t.kind = TokenKind::Operator;
				t.op   = Op::Add;
				t.text = "+";
			}
			break;
		case '.':
			if (peek() == '.' && peek(1) == '.') {
				adv();
				adv();
				t.kind = TokenKind::DotDotDot;
				t.text = "...";
			} else if (peek() == '.') {
				adv();
				t.kind = TokenKind::DotDot;
				t.text = "..";
			} else {
				t.kind = TokenKind::Dot;
				t.text = ".";
			}
			break;

		case ':':
			t.kind = TokenKind::Colon;
			t.text = ":";
			break;
		case '?':
			t.kind = TokenKind::Question;
			t.text = "?";
			break;
		case '-':
			if (peek() == '%') {
				adv();
				t.kind = TokenKind::Operator;
				t.op   = Op::SubWrap;
				if (peek() == '=') {
					adv();
					t.compound_assignment = true;
					t.text				  = "-%=";
				} else {
					t.text = "-%";
				}
			} else if (peek() == '|') {
				adv();
				t.kind = TokenKind::Operator;
				t.op   = Op::SubSat;
				if (peek() == '=') {
					adv();
					t.compound_assignment = true;
					t.text				  = "-|=";
				} else {
					t.text = "-|";
				}
			} else if (peek() == '=') {
				adv();
				t.kind				  = TokenKind::Operator;
				t.op				  = Op::Sub;
				t.compound_assignment = true;
				t.text				  = "-=";
			} else if (peek() == '>') {
				adv();
				t.kind = TokenKind::Arrow;
				t.text = "->";
			} else {
				t.kind = TokenKind::Operator;
				t.op   = Op::Sub;
				t.text = "-";
			}
			break;
		case '*':
			if (peek() == '%') {
				adv();
				t.kind = TokenKind::Operator;
				t.op   = Op::MulWrap;
				if (peek() == '=') {
					adv();
					t.compound_assignment = true;
					t.text				  = "*%=";
				} else {
					t.text = "*%";
				}
			} else if (peek() == '|') {
				adv();
				t.kind = TokenKind::Operator;
				t.op   = Op::MulSat;
				if (peek() == '=') {
					adv();
					t.compound_assignment = true;
					t.text				  = "*|=";
				} else {
					t.text = "*|";
				}
			} else if (peek() == '=') {
				adv();
				t.kind				  = TokenKind::Operator;
				t.op				  = Op::Mul;
				t.compound_assignment = true;
				t.text				  = "*=";
			} else {
				t.kind = TokenKind::Operator;
				t.op   = Op::Mul;
				t.text = "*";
			}
			break;
		case '/':
			if (peek() == '=') {
				adv();
				t.kind				  = TokenKind::Operator;
				t.op				  = Op::Div;
				t.compound_assignment = true;
				t.text				  = "/=";
			} else {
				t.kind = TokenKind::Operator;
				t.op   = Op::Div;
				t.text = "/";
			}
			break;
		case '&':
			if (peek() == '&') {
				adv();
				t.kind = TokenKind::Operator;
				t.op   = Op::LogicalAnd;
				t.text = "&&";
			} else if (peek() == '=') {
				adv();
				t.kind				  = TokenKind::Operator;
				t.op				  = Op::BitAnd;
				t.compound_assignment = true;
				t.text				  = "&=";
			} else {
				t.kind = TokenKind::Operator;
				t.op   = Op::BitAnd;
				t.text = "&";
			}
			break;
		case '|':
			if (peek() == '|') {
				adv();
				t.kind = TokenKind::Operator;
				t.op   = Op::LogicalOr;
				t.text = "||";
			} else if (peek() == '=') {
				adv();
				t.kind				  = TokenKind::Operator;
				t.op				  = Op::BitOr;
				t.compound_assignment = true;
				t.text				  = "|=";
			} else {
				t.kind = TokenKind::Operator;
				t.op   = Op::BitOr;
				t.text = "|";
			}
			break;
		case '!':
			if (peek() == '=') {
				adv();
				t.kind = TokenKind::Operator;
				t.op   = Op::Ne;
				t.text = "!=";
			} else {
				t.kind = TokenKind::Bang;
				t.text = "!";
			}
			break;
		case '=':
			if (peek() == '=') {
				adv();
				t.kind = TokenKind::Operator;
				t.op   = Op::Eq;
				t.text = "==";
			} else if (peek() == '>') {
				adv();
				t.kind = TokenKind::FatArrow;
				t.text = "=>";
			} else {
				t.kind = TokenKind::Assign;
				t.text = "=";
			}
			break;
		case '<':
			if (peek() == '<') {
				adv();
				if (peek() == '=') {
					adv();
					t.kind				  = TokenKind::Operator;
					t.op				  = Op::Shl;
					t.compound_assignment = true;
					t.text				  = "<<=";
				} else {
					t.kind = TokenKind::Operator;
					t.op   = Op::Shl;
					t.text = "<<";
				}
			} else if (peek() == '=') {
				adv();
				t.kind = TokenKind::Operator;
				t.op   = Op::Le;
				t.text = "<=";
			} else {
				t.kind = TokenKind::Operator;
				t.op   = Op::Lt;
				t.text = "<";
			}
			break;
		case '>':
			if (peek() == '>') {
				adv();
				if (peek() == '=') {
					adv();
					t.kind				  = TokenKind::Operator;
					t.op				  = Op::Shr;
					t.compound_assignment = true;
					t.text				  = ">>=";
				} else {
					t.kind = TokenKind::Operator;
					t.op   = Op::Shr;
					t.text = ">>";
				}
			} else if (peek() == '=') {
				adv();
				t.kind = TokenKind::Operator;
				t.op   = Op::Ge;
				t.text = ">=";
			} else {
				t.kind = TokenKind::Operator;
				t.op   = Op::Gt;
				t.text = ">";
			}
			break;
		default:
			report(log::Level::Error, t.loc, std::string("unexpected character '") + c + "'");
			return false;
		}
		t.loc.length = source_length(t.text.empty() ? 1 : t.text.size());
	}
	// terminate
	make(TokenKind::Eof);
	return !log::has_error();
}

void Lexer::skip_ws_comments() {
	while (pos_ < src_.size()) {
		char c = peek();
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
			adv();
			continue;
		}
		if (c == '/' && peek(1) == '/') {
			while (pos_ < src_.size() && peek() != '\n') {
				adv();
			}
			continue;
		}
		break;
	}
}

bool Lexer::lex_ident_or_kw() {
	const int	 start_line = line_;
	const int	 start_col	= col_;
	const size_t start_pos	= pos_;
	while (pos_ < src_.size() && is_ident_cont(src_[pos_])) {
		++pos_;
	}
	const size_t	 len = pos_ - start_pos;
	col_ += static_cast<int>(len);
	std::string_view sv(src_.data() + start_pos, len);
	Token			&t = make(kw_lookup(sv));
	t.loc			   = SourceLoc{fname_, start_line, start_col, source_length(len)};
	t.text.assign(sv.data(), sv.size());
	return true;
}

bool Lexer::lex_builtin_module() {
	SourceLoc start{fname_, line_, col_};
	adv(); // consume '@'
	if (!is_ident_start(peek())) {
		report(log::Level::Error, start, "bare '@' must be followed by an identifier");
		return false;
	}
	std::string s;
	while (pos_ < src_.size() && is_ident_cont(peek())) {
		s.push_back(adv());
	}
	if (s != "of" && s != "va" && s != "src" && s != "syn" && s != "diag") {
		report(log::Level::Error, start, "unknown compiler builtin '@" + s + "'");
		return false;
	}
	Token &t	 = make(TokenKind::BuiltinModuleRef);
	t.loc		 = start;
	t.text		 = s;
	t.loc.length = source_length_with_prefix(s.size());
	return true;
}

bool Lexer::lex_define_ref() {
	SourceLoc start{fname_, line_, col_};
	adv(); // consume '#'
	std::string s;
	while (pos_ < src_.size() && is_ident_cont(peek())) {
		s.push_back(adv());
	}
	Token &t	 = make(TokenKind::DefineRef);
	t.loc		 = start;
	t.text		 = s;
	t.loc.length = source_length_with_prefix(s.size());
	return true;
}

static int hexd(char c) {
	return std::isdigit((unsigned char)c) ? c - '0' : (std::tolower((unsigned char)c) - 'a' + 10);
}

bool Lexer::lex_number() {
	SourceLoc	start{fname_, line_, col_};
	std::string s;
	s.reserve(32);

	bool is_hex = false;
	bool is_bin = false;
	if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
		is_hex = true;
		s.push_back(adv());
		s.push_back(adv());
	} else if (peek() == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
		is_bin = true;
		s.push_back(adv());
		s.push_back(adv());
	}

	bool is_float		  = false;
	bool has_digits		  = false;
	bool has_float_suffix = false;

	auto invalid_separator = [&]() {
		start.length = source_length(std::max<size_t>(1, s.size()));
		report(log::Level::Error, start, "numeric separator '_' must appear between valid digits");
		return false;
	};
	auto is_digit = [&](char c) {
		if (is_hex) {
			return std::isxdigit((unsigned char)c) != 0;
		}
		if (is_bin) {
			return c == '0' || c == '1';
		}
		return std::isdigit((unsigned char)c) != 0;
	};
	auto decimal_digit	= [](char c) { return std::isdigit((unsigned char)c) != 0; };
	auto consume_digits = [&](bool &component_has_digits, auto valid_digit) {
		component_has_digits = false;
		while (pos_ < src_.size()) {
			char c = peek();
			if (valid_digit(c)) {
				s.push_back(adv());
				component_has_digits = true;
				continue;
			}
			if (c == '_') {
				if (!component_has_digits || !valid_digit(peek(1))) {
					return invalid_separator();
				}
				s.push_back(adv());
				continue;
			}
			break;
		}
		return true;
	};

	if (!consume_digits(has_digits, is_digit)) {
		return false;
	}
	if (!has_digits) {
		if (is_hex) {
			start.length = source_length(s.size());
			report(log::Level::Error, start, "missing digits after '0x'");
		} else if (is_bin) {
			start.length = source_length(s.size());
			report(log::Level::Error, start, "missing digits after '0b'");
		} else {
			start.length = 1;
			report(log::Level::Error, start, "invalid number literal");
		}
		return false;
	}

	if (!is_bin && peek() == '.') {
		if (is_digit(peek(1))) {
			is_float = true;
			s.push_back(adv());
			bool fraction_has_digits = false;
			if (!consume_digits(fraction_has_digits, is_digit)) {
				return false;
			}
		} else if (peek(1) == '_') {
			return invalid_separator();
		}
	}

	if (!is_bin && ((is_hex && (peek() == 'p' || peek() == 'P')) ||
					(!is_hex && (peek() == 'e' || peek() == 'E')))) {
		is_float = true;
		s.push_back(adv());
		if (peek() == '+' || peek() == '-') {
			s.push_back(adv());
		}
		bool exponent_has_digits = false;
		if (!consume_digits(exponent_has_digits, decimal_digit)) {
			return false;
		}
		if (!exponent_has_digits) {
			start.length = source_length(std::max<size_t>(1, s.size()));
			report(log::Level::Error, start, "malformed exponent in floating-point literal");
			return false;
		}
	}

	bool has_lossy_suffix = false;
	if (is_float && peek() == '~') {
		has_lossy_suffix = true;
		s.push_back(adv());
	}
	if (!is_bin && (peek() == 'f' || peek() == 'F')) {
		is_float		 = true;
		has_float_suffix = true;
		s.push_back(adv());
	}
	if (is_float && !has_lossy_suffix && peek() == '~') {
		has_lossy_suffix = true;
		s.push_back(adv());
	}
	if (is_float && peek() == '~') {
		start.length = source_length(s.size() + 1);
		report(log::Level::Error, start, "duplicate '~' suffix on floating-point literal");
		return false;
	}
	if (peek() == '_' || (peek() == '.' && peek(1) == '_')) {
		return invalid_separator();
	}

	Token &t	 = make(is_float ? TokenKind::FloatLit : TokenKind::IntLit);
	t.loc		 = start;
	t.text		 = s;
	t.loc.length = source_length(s.size());
	if (is_float) {
		std::string normalized = normalize_numeric_literal(s);
		if (has_lossy_suffix) {
			size_t pos = normalized.find('~');
			if (pos != std::string::npos) {
				normalized.erase(pos, 1);
			}
		}
		if (has_float_suffix) {
			size_t pos = normalized.find_first_of("fF");
			if (pos != std::string::npos) {
				normalized.erase(pos, 1);
			}
		}
		llvm::APFloat parsed(llvm::APFloat::IEEEdouble());
		auto status = parsed.convertFromString(normalized, llvm::APFloat::rmNearestTiesToEven);
		if (!status) {
			llvm::consumeError(status.takeError());
			start.length = source_length(s.size());
			report(log::Level::Error, start, "malformed floating-point literal '" + s + "'");
			t.flt_val = 0.0;
		} else {
			const auto conversion_status = status.get();
			if (conversion_status & llvm::APFloat::opOverflow) {
				report(log::Level::Error, start,
					   "floating-point literal '" + s + "' overflows f64");
			} else if (conversion_status & llvm::APFloat::opUnderflow) {
				report(log::Level::Error, start,
					   "floating-point literal '" + s + "' underflows f64");
			}
			t.flt_val = parsed.convertToDouble();
		}
	} else {
		ParsedInt pi = parse_int_literal(s);
		t.int_val	 = pi.ok ? pi.value : 0;
		if (pi.overflow) {
			report(log::Level::Error, start,
				   std::string("integer literal '") + s + "' exceeds the maximum supported value");
		}
	}
	return true;
}

static bool lex_escape(char c, uint32_t &out) {
	switch (c) {
	case 'n':
		out = '\n';
		return true;
	case 't':
		out = '\t';
		return true;
	case 'r':
		out = '\r';
		return true;
	case '\\':
		out = '\\';
		return true;
	case '\'':
		out = '\'';
		return true;
	case '"':
		out = '"';
		return true;
	case '0':
		out = '\0';
		return true;
	case 'a':
		out = '\a';
		return true;
	case 'b':
		out = '\b';
		return true;
	case 'f':
		out = '\f';
		return true;
	case 'v':
		out = '\v';
		return true;
	case 'x':
		return false; // hex handled by caller
	default:
		return false;
	}
}

bool Lexer::lex_string(bool is_cstring) {
	const size_t start_pos = pos_;
	SourceLoc start{fname_, line_, col_};
	if (is_cstring) {
		adv(); // c
	}
	adv(); // "
	std::string out;
	while (pos_ < src_.size() && peek() != '"') {
		char c = adv();
		if (c == '\\') {
			if (pos_ >= src_.size()) {
				break;
			}
			char e = adv();
			if (e == 'x') {
				std::string h;
				for (int i = 0; i < 2 && pos_ < src_.size() && std::isxdigit((unsigned char)peek());
					 ++i) {
					 h.push_back(adv());
				}
				if (h.size() != 2) {
					report(log::Level::Error, start, "hex escape requires exactly two digits");
				}
				uint32_t v = 0;
				for (char x : h) {
					v = (v << 4) | hexd(x);
				}
				out.push_back((char)(unsigned char)v);
			} else {
				uint32_t v;
				if (!lex_escape(e, v)) {
					report(log::Level::Warning, start, "unknown escape");
					v = (unsigned char)e;
				}
				out.push_back((char)v);
			}
		} else {
			out.push_back(c);
		}
	}
	if (peek() == '"') {
		adv();
	} else {
		start.length = 1;
		report(log::Level::Error, start, "unterminated string literal");
	}
	Token &t	 = make(TokenKind::StringLit);
	t.is_cstring = is_cstring;
	t.loc		 = start;
	t.text		 = out;
	t.loc.length = source_length(pos_ - start_pos);
	return true;
}

bool Lexer::lex_char() {
	const size_t start_pos = pos_;
	SourceLoc start{fname_, line_, col_};
	adv(); // '
	uint32_t v = 0;
	bool has_value = false;
	if (peek() == '\\') {
		adv();
		char e = adv();
		has_value = true;
		if (e == 'x') {
			std::string h;
			for (int i = 0; i < 2 && pos_ < src_.size() && std::isxdigit((unsigned char)peek());
				 ++i) {
				 h.push_back(adv());
			}
			if (h.size() != 2) {
				report(log::Level::Error, start, "hex escape requires exactly two digits");
			}
			for (char x : h) {
				v = (v << 4) | hexd(x);
			}
		} else {
			if (!lex_escape(e, v)) {
				report(log::Level::Warning, start, "unknown escape");
				v = (unsigned char)e;
			}
		}
	} else if (peek() != '\0') {
		v = (unsigned char)adv();
		has_value = true;
	}
	if (!has_value) {
		report(log::Level::Error, start, "empty char literal");
	}
	if (peek() == '\'') {
		adv();
	} else {
		start.length = 1;
		report(log::Level::Error, start, "unterminated char literal");
	}
	Token &t	 = make(TokenKind::CharLit);
	t.loc		 = start;
	t.char_val	 = v & 0xFF;
	t.loc.length = source_length(pos_ - start_pos);
	return true;
}

} // namespace stellac
