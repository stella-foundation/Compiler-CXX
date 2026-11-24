#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace stellac {

// Levenshtein distance (with transposition) between two string views.
size_t levenshtein_distance(std::string_view s1, std::string_view s2);

// Interned file path for SourceLoc so copying tokens and AST nodes never
// allocates or copies std::string storage.
class InternedFilePath {
  public:
	InternedFilePath();
	InternedFilePath(const std::string &path);
	InternedFilePath(std::string_view path);
	InternedFilePath(const char *path);

	InternedFilePath &operator=(const std::string &path);
	InternedFilePath &operator=(std::string_view path);
	InternedFilePath &operator=(const char *path);

	bool empty() const {
		return ptr_->empty();
	}
	const char *c_str() const {
		return ptr_->c_str();
	}
	const std::string &str() const {
		return *ptr_;
	}
	operator const std::string &() const {
		return *ptr_;
	}
	operator std::string_view() const {
		return *ptr_;
	}
	bool operator==(const InternedFilePath &other) const {
		return ptr_ == other.ptr_;
	}
	bool operator!=(const InternedFilePath &other) const {
		return ptr_ != other.ptr_;
	}
	bool operator==(std::string_view other) const {
		return *ptr_ == other;
	}
	bool operator!=(std::string_view other) const {
		return *ptr_ != other;
	}

  private:
	const std::string *ptr_;
};

// Source location attached to every token and AST node. Kept in the `stella`
// namespace (not `stellac::log`) so existing unqualified uses keep working.
struct SourceLoc {
	InternedFilePath file;
	int				 line   = 0;
	int				 column = 0;
	// Number of source columns covered by this location. Zero means that the
	// diagnostic engine may infer a one-token span for legacy call sites.
	int				 length = 0;
};

struct SourceSpan {
	SourceLoc start;
	SourceLoc end;
};

namespace log {

enum class Level {
	Note,
	Error,
	Warning,
};

enum class Phase {
	Cli,
	Lexer,
	Parser,
	Resolver,
	Semantic,
	Codegen,
	Backend,
	Linker,
	Compiler,
};

enum class DiagCode {
	General,
	ErrorLimit,

	// Lexer
	LexInvalidCharacter,
	LexInvalidNumber,
	LexUnterminatedString,
	LexUnterminatedChar,

	// Parser
	ParseExpectedToken,
	ParseModuleHeader,

	// Resolver
	ModuleRead,
	ModuleNotFound,
	ModuleRoot,
	SourceNotFound,

	// Semantic
	TypeMismatch,
	UndefinedName,
	UndefinedType,
	UnknownMember,
	PrivateMember,
	InvalidAttribute,
	InvalidRange,
	InvalidControlFlow,
	InvalidAssignment,
	ImmutableAssignment,
	DuplicateDeclaration,
	LayoutMismatch,
	GenericConstraint,
	GenericTypeSet,
	TypeBindingCycle,
	InvalidCall,

	// Backend / Codegen
	CodegenFailure,
	EmitFailed,
	EmitTemp,
	BackendInit,
	TargetUnsupported,
	TargetCpu,
	TargetSysroot,

	// Linker
	LinkFailed,
	LinkCommand,
	LinkTemp,

	// CLI
	CliInvalidOption,
	CliMissingInput,
	InputRead,
	OutputPathCollision,
	DepfilePathCollision,
	DepfileWrite,
	ModuleCompile,
	RuntimeNotFound,

	// Warnings
	NumericOverflow,
	UnusedName,
	LinkFallback,
	WarningGeneral,

	// Notes
	Note,
};

const char *code_str(DiagCode code);
Phase       code_phase(DiagCode code);
const char *phase_str(Phase phase);
std::optional<DiagCode> diag_code_from_string(std::string_view str);
std::optional<Phase>    phase_from_string(std::string_view str);

enum class Format { Human, Json };
enum class Color { Auto, Always, Never };

struct Related {
	SourceSpan	span;
	std::string message;
};

struct FixIt {
	SourceSpan	span;
	std::string replacement;
	std::string description;
};

struct Attachment {
	std::string kind;
	std::string text;
};

struct MacroContext {
	SourceLoc definition;
	std::vector<SourceLoc> expansion_chain;
};

struct Diagnostic {
	Level					level = Level::Error;
	DiagCode				code = DiagCode::General;
	std::string				custom_code;
	std::string				message;
	SourceSpan				primary;
	std::vector<Related>	related;
	std::string				help;
	std::vector<FixIt>		fixits;
	std::vector<Attachment> attachments;
	Phase					phase = Phase::Compiler;
	bool					preserve_case = false;

	std::string code_name() const;
	std::string phase_name() const;
};

// Start a fresh compilation-scoped stream and configure its output. All
// compiler phases submit diagnostics through the same engine.
void		reset();
void		configure(Format format, Color color, int error_limit = 20);
void		finish();
Format		format();
Color		color();

// Pre-register source content for a file path (such as stdin or memory buffers).
void		register_source(const std::string &path, std::string content);

// Compute a SourceSpan for a SourceLoc, with token/word length inference if length is 0.
SourceSpan	span_for(const SourceLoc &loc, const std::string &fallback_file = {});

const char *level_name(Level level);
void		emit(Diagnostic diagnostic);

// Fluent Diagnostic Builder
class DiagnosticBuilder {
  public:
	DiagnosticBuilder(Level level, SourceLoc loc, std::string msg);
	~DiagnosticBuilder();

	DiagnosticBuilder(DiagnosticBuilder &&other) noexcept;
	DiagnosticBuilder &operator=(DiagnosticBuilder &&other) noexcept;
	DiagnosticBuilder(const DiagnosticBuilder &)			= delete;
	DiagnosticBuilder &operator=(const DiagnosticBuilder &) = delete;

	DiagnosticBuilder &code(DiagCode c) &;
	DiagnosticBuilder &&code(DiagCode c) &&;
	DiagnosticBuilder &code(std::string c) &;
	DiagnosticBuilder &&code(std::string c) &&;
	DiagnosticBuilder &phase(Phase p) &;
	DiagnosticBuilder &&phase(Phase p) &&;
	DiagnosticBuilder &phase(std::string p) &;
	DiagnosticBuilder &&phase(std::string p) &&;
	DiagnosticBuilder &span(SourceSpan sp) &;
	DiagnosticBuilder &&span(SourceSpan sp) &&;
	DiagnosticBuilder &length(int len) &;
	DiagnosticBuilder &&length(int len) &&;
	DiagnosticBuilder &note(const SourceLoc &loc, std::string msg) &;
	DiagnosticBuilder &&note(const SourceLoc &loc, std::string msg) &&;
	DiagnosticBuilder &note(const SourceSpan &sp, std::string msg) &;
	DiagnosticBuilder &&note(const SourceSpan &sp, std::string msg) &&;
	DiagnosticBuilder &help(std::string h) &;
	DiagnosticBuilder &&help(std::string h) &&;
	DiagnosticBuilder &fixit(const SourceSpan &sp, std::string replacement, std::string desc = {}) &;
	DiagnosticBuilder &&fixit(const SourceSpan &sp, std::string replacement, std::string desc = {}) &&;
	DiagnosticBuilder &fixit(const SourceLoc &loc, std::string replacement, std::string desc = {}) &;
	DiagnosticBuilder &&fixit(const SourceLoc &loc, std::string replacement, std::string desc = {}) &&;
	DiagnosticBuilder &attachment(std::string kind, std::string text) &;
	DiagnosticBuilder &&attachment(std::string kind, std::string text) &&;
	DiagnosticBuilder &preserve_case(bool p = true) &;
	DiagnosticBuilder &&preserve_case(bool p = true) &&;

	void emit();

  private:
	Diagnostic diag_;
	bool	   emitted_ = false;
};

DiagnosticBuilder error(const SourceLoc &loc, std::string msg);
DiagnosticBuilder warning(const SourceLoc &loc, std::string msg);
DiagnosticBuilder note(const SourceLoc &loc, std::string msg);

// Add a fully structured diagnostic. `phase` is serialized for consumers and
// may be one of cli, lexer, parser, resolver, semantic, codegen, backend, or
// linker.
void report(Level level, const SourceLoc &loc, const std::string &msg, DiagCode code,
			const std::string &help = {},
			const std::vector<Attachment> &attachments = {}, bool preserve_case = false);
void report(Level level, const SourceLoc &loc, const std::string &msg, const std::string &code = {},
			const std::string &phase = {}, const std::string &help = {},
			const std::vector<Attachment> &attachments = {}, bool preserve_case = false);

// Set the current source file, used as a fallback for diagnostics whose
// SourceLoc carries no filename (e.g. compiler-internal errors emitted with
// an empty location).
void set_file(std::string f);
const std::string &current_file();

// Attach macro definition and expansion-chain notes to diagnostics emitted
// while semantic analysis is checking a generated AST node.
void push_macro_context(const SourceLoc &definition,
						const std::vector<SourceLoc> &expansion_chain);
void pop_macro_context();

void			   set_warning_as_error(bool enabled);
bool			   warning_as_error();

// Cumulative error tally across the whole compilation.
bool			   has_error();
int				   error_count();
int				   suppressed_count();

} // namespace log
} // namespace stellac
