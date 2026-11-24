#pragma once

#include "ast.hpp"
#include "token.hpp"
#include <memory>
#include <string_view>
#include <vector>

namespace stellac {

class Parser {
  public:
	Parser(std::vector<Token> toks, std::string fname, std::vector<std::string> mod_path);

	// Parse a full module. Returns false if any parse error occurred (diag->false route).
	bool parse_module(Module &out);

  private:
	std::vector<Token>		 toks_;
	size_t					 idx_ = 0;
	std::string				 fname_;
	std::vector<std::string> mod_path_; // namespace path of THIS file (file-level `mod;`)
	std::vector<std::string>
				 generic_type_params_;	   // type parameters in the current generic function/impl
	bool		 seen_non_import_ = false; // imports must precede other decls
	bool		 parsing_generic_arg_ = false;
	unsigned	 generic_arg_nesting_ = 0;
	bool		 allow_struct_init_	  = true;

	const Token &peek(size_t off = 0) const;
	const Token &consume();
	bool		 at(TokenKind k) const;
	bool		 at_binop(Op o) const; // Operator with op o and !compound_assignment
	bool		 accept(TokenKind k);
	bool		 expect(TokenKind k, const char *what);

	std::unique_ptr<TypeRef> parse_type();
	void					 parse_generic_params(std::vector<GenericParam> &out);
	void					 parse_generic_args(std::vector<GenericArg> &out);
	bool					 generic_call_suffix() const;
	bool					 is_generic_type_param_name(const std::string &name) const;
	TopKind					 parse_top(Module &out);
	TopKind parse_top_into(std::vector<TopDeclPtr> &body, bool inside_block = false);
	bool	parse_top_if(std::vector<TopDeclPtr> &body);
	bool	parse_mod_into(std::vector<TopDeclPtr> &body, bool is_pub, AttributeList attrs,
						   bool inside_block = false);
	bool	parse_import(Module &out, bool is_pub, AttributeList attrs = {});
	bool	parse_import_into(std::vector<TopDeclPtr> &body, bool is_pub, AttributeList attrs = {});
	bool	parse_fn(Module &out, bool is_pub, AttributeList attrs, bool is_extern = false);
	bool	parse_fn_into(std::vector<TopDeclPtr> &body, bool is_pub, AttributeList attrs,
						  bool is_extern = false);
	bool	parse_macro_into(std::vector<TopDeclPtr> &body, bool is_pub, AttributeList attrs);
	bool	parse_struct(Module &out, bool is_pub, AttributeList attrs);
	bool	parse_struct_into(std::vector<TopDeclPtr> &body, bool is_pub, AttributeList attrs);
	bool	parse_union(Module &out, bool is_pub, AttributeList attrs);
	bool	parse_union_into(std::vector<TopDeclPtr> &body, bool is_pub, AttributeList attrs);
	bool	parse_enum(Module &out, bool is_pub, AttributeList attrs);
	bool	parse_enum_into(std::vector<TopDeclPtr> &body, bool is_pub, AttributeList attrs);
	bool	parse_trait_into(std::vector<TopDeclPtr> &body, bool is_pub, AttributeList attrs);
	bool	parse_impl_into(std::vector<TopDeclPtr> &body, AttributeList attrs);
	bool	parse_global(Module &out, bool is_pub, bool is_const, AttributeList attrs,
						 bool is_extern = false);
	bool	parse_global_into(std::vector<TopDeclPtr> &body, bool is_pub, bool is_const,
							  AttributeList attrs, bool is_extern = false);
	void	parse_attributes(AttributeList &out);
	void	parse_attribute_item(std::vector<Attribute> &out, SourceLoc &list_loc);

	StmtPtr parse_stmt(bool expect_semicolon = true);
	StmtPtr try_parse_stmt_keyword_typo(AttributeList &attrs);
	StmtPtr parse_block();
	std::unique_ptr<Expr> parse_block_expr();
	StmtPtr parse_if();
	StmtPtr parse_for();
	StmtPtr parse_match();
	MatchPattern parse_match_pattern();
	StmtPtr parse_var_or_const(bool is_const, AttributeList attrs = {},
							   bool expect_semicolon = true);
	StmtPtr parse_return(bool expect_semicolon = true);
	StmtPtr parse_defer(bool expect_semicolon = true);
	StmtPtr parse_yield(bool expect_semicolon = true);
	StmtPtr parse_asm(bool expect_semicolon = true);
	StmtPtr parse_expr_stmt_or_decl(bool expect_semicolon = true);

	std::unique_ptr<Expr> parse_expr();
	std::unique_ptr<Expr> parse_assign();
	std::unique_ptr<Expr> parse_conditional();
	std::unique_ptr<Expr> parse_logic_or();
	std::unique_ptr<Expr> parse_logic_and();
	std::unique_ptr<Expr> parse_bitor();
	std::unique_ptr<Expr> parse_bitxor();
	std::unique_ptr<Expr> parse_bitand();
	std::unique_ptr<Expr> parse_equality();
	std::unique_ptr<Expr> parse_relational();
	std::unique_ptr<Expr> parse_shift();
	std::unique_ptr<Expr> parse_additive();
	std::unique_ptr<Expr> parse_multiplicative();
	std::unique_ptr<Expr> parse_unary();
	std::unique_ptr<Expr> parse_postfix();
	std::unique_ptr<Expr> parse_primary();
	std::unique_ptr<Expr> parse_builtin_call(std::string_view module, std::string_view member,
											 SourceLoc loc);
	bool				  looks_like_compound_type_operand() const;

	// Helper to read a qualified dotted path into a vector (returns segments or empty).
	bool				  parse_dotted_path(std::vector<std::string> &out);
};

} // namespace stellac
