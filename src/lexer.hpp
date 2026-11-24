#pragma once

#include "log.hpp"
#include "token.hpp"
#include <string>
#include <vector>

namespace stellac {

class Lexer {
  public:
	Lexer(std::string source, std::string fname, bool emit_diagnostics = true);

	// Tokenize entire input. Returns false on lexical error (diag emitted).
	bool tokenize(std::vector<Token> &out);

  private:
	std::string			src_;
	InternedFilePath	fname_;
	size_t				pos_			  = 0;
	int					line_			  = 1;
	int					col_			  = 1;
	bool				emit_diagnostics_ = true;
	std::vector<Token> *out_			  = nullptr;

	void				report(log::Level level, const SourceLoc &loc, const std::string &message);

	Token			   &make(TokenKind k);

	char				peek(size_t off = 0) const;
	char				adv();
	bool				eat(char c);
	void				skip_ws_comments();

	bool				lex_ident_or_kw();
	bool				lex_define_ref();
	bool				lex_builtin_module();
	bool				lex_number();
	bool				lex_string(bool is_cstring = false);
	bool				lex_char();
};

} // namespace stellac
