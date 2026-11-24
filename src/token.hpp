#pragma once

#include "log.hpp"
#include <cstdint>
#include <string>

namespace stellac {

// Binary operators are represented by a single TokenKind::Operator whose `op`
// field selects the base operation. Compound-assignment forms (`+=`, `*=`,
// ...) are lexed as the *same* single Operator token with the base operator
// and `compound_assignment = true`; this lets the parser desugar
// `x += y` -> `x = x + y` from one code path, without a dedicated *Eq token
// kind per operator. Plain `=` stays its own TokenKind::Assign (it has no
// binary-operator reading to pair with). Unary-only tokens (`!`, `~`) also
// stay as their own kinds since they have no binary counterpart.
enum class Op : uint8_t {
	// Arithmetic
	Add,
	Sub,
	AddWrap,
	SubWrap,
	AddSat,
	SubSat,
	Mul,
	MulWrap,
	MulSat,
	Div,
	Mod,
	// Bitwise
	BitAnd,
	BitOr,
	BitXor,
	Shl,
	Shr,
	// Comparison
	Eq,
	Ne,
	Lt,
	Le,
	Gt,
	Ge,
	// Logical
	LogicalAnd,
	LogicalOr,
};

const char *op_text(Op o);

enum class TokenKind {
	// Special
	Eof,
	Unknown,

	// Literals
	IntLit,
	FloatLit,
	StringLit, // "..." or c"..."
	CharLit,   // '...'

	// Identifiers / compile-time defines
	Ident,
	DefineRef,		  // #flag
	BuiltinModuleRef, // @of, @va, @src, @syn, and @diag built-in module roots

	// Constants
	KwTrue,
	KwFalse,
	KwNull,

	// Declaration keywords
	KwMod,
	KwImport,
	KwPub,
	KwExtern,
	KwFn,
	KwMacro,
	KwLet,
	KwMut,
	KwConst,
	KwThreadLocal,
	KwStatic,
	KwStruct,
	KwUnion,
	KwType,
	KwEnum,
	KwTrait,
	KwImpl,
	KwSelf,

	// Control flow keywords
	KwIf,
	KwElse,
	KwFor,
	KwReturn,
	KwUnreachable,
	KwBreak,
	KwContinue,
	KwDefer,
	KwYield,
	KwMatch,

	// Inline asm
	KwAsm,
	KwVolatile,

	// Cast operators
	KwCast,	   // value cast Type -- normal value/type conversion
	KwBitcast, // value bitcast Type -- bit reinterpretation (same size only)

	// Punctuation
	LParen,
	RParen,
	LBrace,
	RBrace,
	LBracket,
	RBracket,
	Comma,
	Semicolon,
	Colon,
	Question,
	Dot, // . (member access and struct literal `.field =`)
	Tilde,
	Bang,	   // unary-only operators (~, !)
	Operator,  // any binary operator; see `op` and `compound_assignment`
	Assign,	   // plain =
	Arrow,	   // ->
	FatArrow,  // =>
	DotDot,	   // ..
	DotDotDot, // ...
	Hash,	   // # (used for #[ attribute syntax)
};

struct Token {
	TokenKind	kind				= TokenKind::Eof;
	Op			op					= Op::Add; // valid when kind == Operator
	bool		compound_assignment = false;   // true for += -= *= /= %= &= |= ^= <<= >>=
	bool		is_cstring			= false;   // true for c"..." string literals
	SourceLoc	loc;
	std::string text;		  // identifier / raw text // raw text of the token
	uint64_t	int_val	 = 0; // for IntLit
	double		flt_val	 = 0; // for FloatLit
	uint32_t	char_val = 0; // for CharLit (single byte value)

	bool		is(TokenKind k) const {
		return kind == k;
	}
	bool is_op(Op o) const {
		return kind == TokenKind::Operator && op == o;
	}
	bool is_binop(Op o) const {
		return kind == TokenKind::Operator && op == o && !compound_assignment;
	}
};

const char *token_kind_name(TokenKind k);

inline bool is_keyword(TokenKind k) {
	return k >= TokenKind::KwTrue && k <= TokenKind::KwBitcast;
}

// Keywords that can begin a top-level module declaration.
inline constexpr TokenKind top_level_keywords[] = {
	TokenKind::KwMod,
	TokenKind::KwImport,
	TokenKind::KwPub,
	TokenKind::KwExtern,
	TokenKind::KwFn,
	TokenKind::KwMacro,
	TokenKind::KwLet,
	TokenKind::KwConst,
	TokenKind::KwStruct,
	TokenKind::KwUnion,
	TokenKind::KwEnum,
	TokenKind::KwTrait,
	TokenKind::KwImpl,
	TokenKind::KwIf,
};

// Keywords that can begin a statement or local declaration.
inline constexpr TokenKind statement_keywords[] = {
	TokenKind::KwLet,
	TokenKind::KwConst,
	TokenKind::KwReturn,
	TokenKind::KwIf,
	TokenKind::KwFor,
	TokenKind::KwMatch,
	TokenKind::KwDefer,
	TokenKind::KwYield,
	TokenKind::KwAsm,
	TokenKind::KwBreak,
	TokenKind::KwContinue,
	TokenKind::KwUnreachable,
};

} // namespace stellac
