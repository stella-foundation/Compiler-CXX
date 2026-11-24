#include "log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace stellac {

namespace {

const std::string &empty_interned_path() {
	static const std::string empty;
	return empty;
}

const std::string *intern_file_path(std::string_view path) {
	if (path.empty()) {
		return &empty_interned_path();
	}
	static std::mutex										   mutex;
	static std::deque<std::string>							   storage;
	static std::unordered_map<std::string_view, const std::string *> index;
	std::lock_guard<std::mutex>								   lock(mutex);
	auto													   it = index.find(path);
	if (it != index.end()) {
		return it->second;
	}
	storage.emplace_back(path);
	const std::string *ptr = &storage.back();
	index.emplace(std::string_view(*ptr), ptr);
	return ptr;
}

} // namespace

InternedFilePath::InternedFilePath() : ptr_(&empty_interned_path()) {}
InternedFilePath::InternedFilePath(const std::string &path) : ptr_(intern_file_path(path)) {}
InternedFilePath::InternedFilePath(std::string_view path) : ptr_(intern_file_path(path)) {}
InternedFilePath::InternedFilePath(const char *path)
	: ptr_(path ? intern_file_path(path) : &empty_interned_path()) {}

InternedFilePath &InternedFilePath::operator=(const std::string &path) {
	ptr_ = intern_file_path(path);
	return *this;
}
InternedFilePath &InternedFilePath::operator=(std::string_view path) {
	ptr_ = intern_file_path(path);
	return *this;
}
InternedFilePath &InternedFilePath::operator=(const char *path) {
	ptr_ = path ? intern_file_path(path) : &empty_interned_path();
	return *this;
}

namespace log {

const char *code_str(DiagCode code) {
	switch (code) {
	case DiagCode::General: return "E_GENERAL";
	case DiagCode::ErrorLimit: return "E_ERROR_LIMIT";

	// Lexer
	case DiagCode::LexInvalidCharacter: return "E_LEX_INVALID_CHARACTER";
	case DiagCode::LexInvalidNumber: return "E_LEX_INVALID_NUMBER";
	case DiagCode::LexUnterminatedString: return "E_LEX_UNTERMINATED_STRING";
	case DiagCode::LexUnterminatedChar: return "E_LEX_UNTERMINATED_CHAR";

	// Parser
	case DiagCode::ParseExpectedToken: return "E_PARSE_EXPECTED_TOKEN";
	case DiagCode::ParseModuleHeader: return "E_PARSE_MODULE_HEADER";

	// Resolver
	case DiagCode::ModuleRead: return "E_MODULE_READ";
	case DiagCode::ModuleNotFound: return "E_MODULE_NOT_FOUND";
	case DiagCode::ModuleRoot: return "E_MODULE_ROOT";
	case DiagCode::SourceNotFound: return "E_SOURCE_NOT_FOUND";

	// Semantic
	case DiagCode::TypeMismatch: return "E_TYPE_MISMATCH";
	case DiagCode::UndefinedName: return "E_UNDEFINED_NAME";
	case DiagCode::UndefinedType: return "E_UNDEFINED_TYPE";
	case DiagCode::UnknownMember: return "E_UNKNOWN_MEMBER";
	case DiagCode::PrivateMember: return "E_PRIVATE_MEMBER";
	case DiagCode::InvalidAttribute: return "E_INVALID_ATTRIBUTE";
	case DiagCode::InvalidRange: return "E_INVALID_RANGE";
	case DiagCode::InvalidControlFlow: return "E_INVALID_CONTROL_FLOW";
	case DiagCode::InvalidAssignment: return "E_INVALID_ASSIGNMENT";
	case DiagCode::ImmutableAssignment: return "E_IMMUTABLE_ASSIGNMENT";
	case DiagCode::DuplicateDeclaration: return "E_DUPLICATE_DECLARATION";
	case DiagCode::LayoutMismatch: return "E_LAYOUT_MISMATCH";
	case DiagCode::GenericConstraint: return "E_GENERIC_CONSTRAINT";
	case DiagCode::GenericTypeSet: return "E_GENERIC_TYPE_SET";
	case DiagCode::TypeBindingCycle: return "E_TYPE_BINDING_CYCLE";
	case DiagCode::InvalidCall: return "E_INVALID_CALL";

	// Backend / Codegen
	case DiagCode::CodegenFailure: return "E_CODEGEN_FAILURE";
	case DiagCode::EmitFailed: return "E_EMIT_FAILED";
	case DiagCode::EmitTemp: return "E_EMIT_TEMP";
	case DiagCode::BackendInit: return "E_BACKEND_INIT";
	case DiagCode::TargetUnsupported: return "E_TARGET_UNSUPPORTED";
	case DiagCode::TargetCpu: return "E_TARGET_CPU";
	case DiagCode::TargetSysroot: return "E_TARGET_SYSROOT";

	// Linker
	case DiagCode::LinkFailed: return "E_LINK_FAILED";
	case DiagCode::LinkCommand: return "E_LINK_COMMAND";
	case DiagCode::LinkTemp: return "E_LINK_TEMP";

	// CLI
	case DiagCode::CliInvalidOption: return "E_CLI_INVALID_OPTION";
	case DiagCode::CliMissingInput: return "E_CLI_MISSING_INPUT";
	case DiagCode::InputRead: return "E_INPUT_READ";
	case DiagCode::OutputPathCollision: return "E_OUTPUT_PATH_COLLISION";
	case DiagCode::DepfilePathCollision: return "E_DEPFILE_PATH_COLLISION";
	case DiagCode::DepfileWrite: return "E_DEPFILE_WRITE";
	case DiagCode::ModuleCompile: return "E_MODULE_COMPILE";
	case DiagCode::RuntimeNotFound: return "E_RUNTIME_NOT_FOUND";

	// Warnings
	case DiagCode::NumericOverflow: return "W_NUMERIC_OVERFLOW";
	case DiagCode::UnusedName: return "W_UNUSED_NAME";
	case DiagCode::LinkFallback: return "W_LINK_FALLBACK";
	case DiagCode::WarningGeneral: return "W_GENERAL";

	// Notes
	case DiagCode::Note: return "N_NOTE";
	}
	return "E_GENERAL";
}

Phase code_phase(DiagCode code) {
	switch (code) {
	case DiagCode::LexInvalidCharacter:
	case DiagCode::LexInvalidNumber:
	case DiagCode::LexUnterminatedString:
	case DiagCode::LexUnterminatedChar:
		return Phase::Lexer;

	case DiagCode::ParseExpectedToken:
	case DiagCode::ParseModuleHeader:
		return Phase::Parser;

	case DiagCode::ModuleRead:
	case DiagCode::ModuleNotFound:
	case DiagCode::ModuleRoot:
	case DiagCode::SourceNotFound:
		return Phase::Resolver;

	case DiagCode::TypeMismatch:
	case DiagCode::UndefinedName:
	case DiagCode::UndefinedType:
	case DiagCode::UnknownMember:
	case DiagCode::PrivateMember:
	case DiagCode::InvalidAttribute:
	case DiagCode::InvalidRange:
	case DiagCode::InvalidControlFlow:
	case DiagCode::InvalidAssignment:
	case DiagCode::ImmutableAssignment:
	case DiagCode::DuplicateDeclaration:
	case DiagCode::LayoutMismatch:
	case DiagCode::GenericConstraint:
	case DiagCode::GenericTypeSet:
	case DiagCode::TypeBindingCycle:
	case DiagCode::InvalidCall:
		return Phase::Semantic;

	case DiagCode::CodegenFailure:
	case DiagCode::EmitFailed:
	case DiagCode::EmitTemp:
		return Phase::Codegen;

	case DiagCode::BackendInit:
	case DiagCode::TargetUnsupported:
	case DiagCode::TargetCpu:
	case DiagCode::TargetSysroot:
		return Phase::Backend;

	case DiagCode::LinkFailed:
	case DiagCode::LinkCommand:
	case DiagCode::LinkTemp:
		return Phase::Linker;

	case DiagCode::NumericOverflow:
		return Phase::Semantic;

	case DiagCode::UnusedName:
		return Phase::Semantic;

	case DiagCode::LinkFallback:
		return Phase::Linker;

	case DiagCode::CliInvalidOption:
	case DiagCode::CliMissingInput:
	case DiagCode::InputRead:
	case DiagCode::OutputPathCollision:
	case DiagCode::DepfilePathCollision:
	case DiagCode::DepfileWrite:
	case DiagCode::ModuleCompile:
	case DiagCode::RuntimeNotFound:
		return Phase::Cli;

	case DiagCode::General:
	case DiagCode::WarningGeneral:
	case DiagCode::ErrorLimit:
	case DiagCode::Note:
	default:
		return Phase::Compiler;
	}
}

const char *phase_str(Phase phase) {
	switch (phase) {
	case Phase::Cli: return "cli";
	case Phase::Lexer: return "lexer";
	case Phase::Parser: return "parser";
	case Phase::Resolver: return "resolver";
	case Phase::Semantic: return "semantic";
	case Phase::Codegen: return "codegen";
	case Phase::Backend: return "backend";
	case Phase::Linker: return "linker";
	case Phase::Compiler: return "compiler";
	}
	return "compiler";
}

std::optional<Phase> phase_from_string(std::string_view str) {
	if (str == "cli") return Phase::Cli;
	if (str == "lexer") return Phase::Lexer;
	if (str == "parser") return Phase::Parser;
	if (str == "resolver") return Phase::Resolver;
	if (str == "semantic") return Phase::Semantic;
	if (str == "codegen") return Phase::Codegen;
	if (str == "backend") return Phase::Backend;
	if (str == "linker") return Phase::Linker;
	if (str == "compiler") return Phase::Compiler;
	return std::nullopt;
}

std::optional<DiagCode> diag_code_from_string(std::string_view str) {
	static const std::unordered_map<std::string_view, DiagCode> map = {
		{"E_GENERAL", DiagCode::General},
		{"E_ERROR_LIMIT", DiagCode::ErrorLimit},
		{"E_LEX_INVALID_CHARACTER", DiagCode::LexInvalidCharacter},
		{"E_LEX_INVALID_NUMBER", DiagCode::LexInvalidNumber},
		{"E_LEX_UNTERMINATED_STRING", DiagCode::LexUnterminatedString},
		{"E_LEX_UNTERMINATED_CHAR", DiagCode::LexUnterminatedChar},
		{"E_PARSE_EXPECTED_TOKEN", DiagCode::ParseExpectedToken},
		{"E_PARSE_MODULE_HEADER", DiagCode::ParseModuleHeader},
		{"E_MODULE_READ", DiagCode::ModuleRead},
		{"E_MODULE_NOT_FOUND", DiagCode::ModuleNotFound},
		{"E_MODULE_ROOT", DiagCode::ModuleRoot},
		{"E_SOURCE_NOT_FOUND", DiagCode::SourceNotFound},
		{"E_TYPE_MISMATCH", DiagCode::TypeMismatch},
		{"E_UNDEFINED_NAME", DiagCode::UndefinedName},
		{"E_UNDEFINED_TYPE", DiagCode::UndefinedType},
		{"E_UNKNOWN_MEMBER", DiagCode::UnknownMember},
		{"E_PRIVATE_MEMBER", DiagCode::PrivateMember},
		{"E_INVALID_ATTRIBUTE", DiagCode::InvalidAttribute},
		{"E_INVALID_RANGE", DiagCode::InvalidRange},
		{"E_INVALID_CONTROL_FLOW", DiagCode::InvalidControlFlow},
		{"E_INVALID_ASSIGNMENT", DiagCode::InvalidAssignment},
		{"E_IMMUTABLE_ASSIGNMENT", DiagCode::ImmutableAssignment},
		{"E_DUPLICATE_DECLARATION", DiagCode::DuplicateDeclaration},
		{"E_LAYOUT_MISMATCH", DiagCode::LayoutMismatch},
		{"E_GENERIC_CONSTRAINT", DiagCode::GenericConstraint},
		{"E_GENERIC_TYPE_SET", DiagCode::GenericTypeSet},
		{"E_TYPE_BINDING_CYCLE", DiagCode::TypeBindingCycle},
		{"E_INVALID_CALL", DiagCode::InvalidCall},
		{"E_CODEGEN_FAILURE", DiagCode::CodegenFailure},
		{"E_EMIT_FAILED", DiagCode::EmitFailed},
		{"E_EMIT_TEMP", DiagCode::EmitTemp},
		{"E_BACKEND_INIT", DiagCode::BackendInit},
		{"E_TARGET_UNSUPPORTED", DiagCode::TargetUnsupported},
		{"E_TARGET_CPU", DiagCode::TargetCpu},
		{"E_TARGET_SYSROOT", DiagCode::TargetSysroot},
		{"E_LINK_FAILED", DiagCode::LinkFailed},
		{"E_LINK_COMMAND", DiagCode::LinkCommand},
		{"E_LINK_TEMP", DiagCode::LinkTemp},
		{"E_CLI_INVALID_OPTION", DiagCode::CliInvalidOption},
		{"E_CLI_MISSING_INPUT", DiagCode::CliMissingInput},
		{"E_INPUT_READ", DiagCode::InputRead},
		{"E_OUTPUT_PATH_COLLISION", DiagCode::OutputPathCollision},
		{"E_DEPFILE_PATH_COLLISION", DiagCode::DepfilePathCollision},
		{"E_DEPFILE_WRITE", DiagCode::DepfileWrite},
		{"E_MODULE_COMPILE", DiagCode::ModuleCompile},
		{"E_RUNTIME_NOT_FOUND", DiagCode::RuntimeNotFound},
		{"W_NUMERIC_OVERFLOW", DiagCode::NumericOverflow},
		{"W_UNUSED_NAME", DiagCode::UnusedName},
		{"W_LINK_FALLBACK", DiagCode::LinkFallback},
		{"W_GENERAL", DiagCode::WarningGeneral},
		{"N_NOTE", DiagCode::Note},
	};
	auto it = map.find(str);
	if (it != map.end()) {
		return it->second;
	}
	return std::nullopt;
}

std::string Diagnostic::code_name() const {
	if (!custom_code.empty()) {
		return custom_code;
	}
	return code_str(code);
}

std::string Diagnostic::phase_name() const {
	return phase_str(phase);
}

namespace {

struct SourceFile {
	std::string			content;
	std::vector<size_t> line_offsets;
};

struct FileCache {
	std::map<std::string, SourceFile> files;
	std::mutex						  mutex;
};

FileCache &source_cache() {
	static FileCache cache;
	return cache;
}

const SourceFile &load_source(const std::string &file) {
	static const SourceFile empty;
	if (file.empty()) {
		return empty;
	}
	auto					   &cache = source_cache();
	std::lock_guard<std::mutex> lock(cache.mutex);
	auto						found = cache.files.find(file);
	if (found != cache.files.end()) {
		return found->second;
	}
	SourceFile	 &source = cache.files[file];
	std::ifstream input(file, std::ios::binary);
	if (input) {
		std::stringstream contents;
		contents << input.rdbuf();
		source.content = contents.str();
		source.line_offsets.push_back(0);
		for (size_t i = 0; i < source.content.size(); ++i) {
			if (source.content[i] == '\n') {
				source.line_offsets.push_back(i + 1);
			}
		}
	}
	return source;
}

std::string line_text(const SourceFile &source, int line) {
	if (line <= 0 || static_cast<size_t>(line) > source.line_offsets.size()) {
		return {};
	}
	size_t begin = source.line_offsets[static_cast<size_t>(line - 1)];
	size_t end	 = begin;
	while (end < source.content.size() && source.content[end] != '\n') {
		++end;
	}
	if (end > begin && source.content[end - 1] == '\r') {
		--end;
	}
	return source.content.substr(begin, end - begin);
}

int digits(int value) {
	int result = 1;
	for (value /= 10; value > 0; value /= 10) {
		++result;
	}
	return result;
}

struct State {
	std::mutex					mutex;
	int							errors			 = 0;
	int							emitted_errors	 = 0;
	int							suppressed		 = 0;
	int							error_limit		 = 20;
	bool						warning_as_error = false;
	bool						summary_printed	 = false;
	std::string					current_file;
	Format						output_format = Format::Human;
	Color						output_color  = Color::Auto;
	std::map<std::string, bool> seen;
	std::vector<MacroContext> macro_contexts;
};

State &state() {
	static State value;
	return value;
}

bool use_color() {
	auto &s = state();
	if (s.output_format == Format::Json || s.output_color == Color::Never) {
		return false;
	}
	if (s.output_color == Color::Always) {
		return true;
	}
	if (std::getenv("NO_COLOR") != nullptr) {
		return false;
	}
	return isatty(fileno(stderr)) != 0;
}

const char *color_for(Level level) {
	switch (level) {
	case Level::Error:
		return "\033[31m";
	case Level::Warning:
		return "\033[33m";
	case Level::Note:
		return "\033[36m";
	}
	return "\033[31m";
}

std::string lower_first(std::string message) {
	for (char &c : message) {
		if (std::isalpha(static_cast<unsigned char>(c))) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			break;
		}
		if (!std::isspace(static_cast<unsigned char>(c))) {
			break;
		}
	}
	return message;
}

std::string normalize_quotes(std::string message) {
	bool quoted = false;
	for (char &c : message) {
		if (c == '\'') {
			c	   = '`';
			quoted = !quoted;
		}
	}
	(void)quoted;
	return message;
}

std::string json_escape(const std::string &value) {
	std::string result;
	result.reserve(value.size() + 2);
	for (unsigned char c : value) {
		switch (c) {
		case '"':
			result += "\\\"";
			break;
		case '\\':
			result += "\\\\";
			break;
		case '\n':
			result += "\\n";
			break;
		case '\r':
			result += "\\r";
			break;
		case '\t':
			result += "\\t";
			break;
		default:
			if (c < 0x20) {
				char escaped[7];
				std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
				result += escaped;
			} else {
				result.push_back(static_cast<char>(c));
			}
		}
	}
	return result;
}

void json_span(std::ostringstream &out, const SourceSpan &span) {
	out << "{\"file\":\"" << json_escape(span.start.file) << "\",\"start\":{";
	out << "\"line\":" << span.start.line << ",\"column\":" << span.start.column;
	out << "},\"end\":{";
	out << "\"line\":" << span.end.line << ",\"column\":" << span.end.column;
	out << "}}";
}

void print_human(const Diagnostic &diagnostic) {
	const bool	ansi  = use_color();
	const char *reset = "\033[0m";
	const char *dim	  = "\033[2m";
	const char *kind  = level_name(diagnostic.level);
	const char *paint = color_for(diagnostic.level);
	auto	   &s	  = state();
	std::string file  = diagnostic.primary.start.file;
	if (file.empty()) {
		file = s.current_file.empty() ? "<input>" : s.current_file;
	}

	if (ansi) {
		std::fputs("\033[1m", stderr);
	}
	if (ansi) {
		std::fputs(paint, stderr);
	}
	std::fprintf(stderr, "%s[%s]", kind, diagnostic.code_name().c_str());
	if (ansi) {
		std::fputs(reset, stderr);
	}
	std::fprintf(stderr, ": %s\n", diagnostic.message.c_str());
	if (diagnostic.primary.start.line > 0) {
		std::fprintf(stderr, " --> %s:%d:%d\n", file.c_str(), diagnostic.primary.start.line,
					 diagnostic.primary.start.column);
		const SourceFile &source = load_source(file);
		const std::string text	 = line_text(source, diagnostic.primary.start.line);
		if (!text.empty()) {
			const int width = digits(diagnostic.primary.start.line);
			std::fprintf(stderr, "%*s |\n", width, "");
			if (ansi) {
				std::fputs(dim, stderr);
			}
			std::fprintf(stderr, "%*d | %s\n", width, diagnostic.primary.start.line, text.c_str());
			if (ansi) {
				std::fputs(reset, stderr);
			}
			std::fprintf(stderr, "%*s | ", width, "");
			for (int i = 1; i < diagnostic.primary.start.column; ++i) {
				std::fputc((i - 1 < static_cast<int>(text.size()) &&
							text[static_cast<size_t>(i - 1)] == '\t')
							   ? '\t'
							   : ' ',
						   stderr);
			}
			if (ansi) {
				std::fputs(paint, stderr);
			}
			std::fputc('^', stderr);
			int length = diagnostic.primary.start.length > 0 ? diagnostic.primary.start.length : 1;
			for (int i = 1; i < length; ++i) {
				std::fputc('~', stderr);
			}
			if (ansi) {
				std::fputs(reset, stderr);
			}
			std::fputc('\n', stderr);
		}
	}
	for (const Related &related : diagnostic.related) {
		std::fprintf(stderr, " = note: %s", related.message.c_str());
		if (related.span.start.line > 0) {
			std::fprintf(stderr, " (%s:%d:%d)", related.span.start.file.c_str(),
						 related.span.start.line, related.span.start.column);
		}
		std::fputc('\n', stderr);
		if (related.span.start.line > 0) {
			std::string rel_file = related.span.start.file;
			if (rel_file.empty()) {
				rel_file = s.current_file.empty() ? "<input>" : s.current_file;
			}
			const SourceFile &source = load_source(rel_file);
			const std::string text	 = line_text(source, related.span.start.line);
			if (!text.empty()) {
				const int width = digits(related.span.start.line);
				std::fprintf(stderr, "%*s |\n", width, "");
				if (ansi) {
					std::fputs(dim, stderr);
				}
				std::fprintf(stderr, "%*d | %s\n", width, related.span.start.line, text.c_str());
				if (ansi) {
					std::fputs(reset, stderr);
				}
				std::fprintf(stderr, "%*s | ", width, "");
				for (int i = 1; i < related.span.start.column; ++i) {
					std::fputc((i - 1 < static_cast<int>(text.size()) &&
								text[static_cast<size_t>(i - 1)] == '\t')
								   ? '\t'
								   : ' ',
							   stderr);
				}
				if (ansi) {
					std::fputs(paint, stderr);
				}
				int length = related.span.start.length > 0 ? related.span.start.length : 1;
				for (int i = 0; i < length; ++i) {
					std::fputc('-', stderr);
				}
				if (ansi) {
					std::fputs(reset, stderr);
				}
				std::fputc('\n', stderr);
			}
		}
	}
	if (!diagnostic.help.empty()) {
		std::fprintf(stderr, " = help: %s\n", diagnostic.help.c_str());
	}
	for (const FixIt &fix : diagnostic.fixits) {
		if (fix.span.start.length == 0 || (fix.span.start.line == fix.span.end.line &&
										   fix.span.start.column == fix.span.end.column)) {
			std::fprintf(stderr, " = help: insert `%s`\n", fix.replacement.c_str());
		} else {
			std::fprintf(stderr, " = help: replace with `%s`\n", fix.replacement.c_str());
		}
	}
	for (const Attachment &attachment : diagnostic.attachments) {
		std::fprintf(stderr, " = note: %s: %s\n", attachment.kind.c_str(), attachment.text.c_str());
	}
}

void print_json(const Diagnostic &diagnostic) {
	std::ostringstream out;
	out << "{\"severity\":\"" << json_escape(level_name(diagnostic.level)) << "\",\"code\":\""
		<< json_escape(diagnostic.code_name()) << "\",\"message\":\"" << json_escape(diagnostic.message)
		<< "\",\"phase\":\"" << json_escape(diagnostic.phase_name()) << "\",\"primary\":";
	json_span(out, diagnostic.primary);
	out << ",\"related\":[";
	for (size_t i = 0; i < diagnostic.related.size(); ++i) {
		if (i) {
			out << ',';
		}
		out << "{\"message\":\"" << json_escape(diagnostic.related[i].message) << "\",\"span\":";
		json_span(out, diagnostic.related[i].span);
		out << '}';
	}
	out << "],\"help\":";
	if (diagnostic.help.empty()) {
		out << "null";
	} else {
		out << "\"" << json_escape(diagnostic.help) << "\"";
	}
	out << ",\"fixits\":[";
	for (size_t i = 0; i < diagnostic.fixits.size(); ++i) {
		if (i) {
			out << ',';
		}
		const FixIt &fix = diagnostic.fixits[i];
		out << "{\"span\":";
		json_span(out, fix.span);
		out << ",\"replacement\":\"" << json_escape(fix.replacement) << "\",\"description\":\""
			<< json_escape(fix.description) << "\"}";
	}
	out << "],\"attachments\":[";
	for (size_t i = 0; i < diagnostic.attachments.size(); ++i) {
		if (i) {
			out << ',';
		}
		out << "{\"kind\":\"" << json_escape(diagnostic.attachments[i].kind) << "\",\"text\":\""
			<< json_escape(diagnostic.attachments[i].text) << "\"}";
	}
	out << "]}";
	std::fputs((out.str() + "\n").c_str(), stderr);
}

} // namespace

const char *level_name(Level level) {
	switch (level) {
	case Level::Error:
		return "error";
	case Level::Warning:
		return "warning";
	case Level::Note:
		return "note";
	}
	return "error";
}

void reset() {
	auto					   &s = state();
	std::lock_guard<std::mutex> lock(s.mutex);
	s.errors		  = 0;
	s.emitted_errors  = 0;
	s.suppressed	  = 0;
	s.error_limit	  = 20;
	s.summary_printed = false;
	s.current_file.clear();
	s.seen.clear();
	s.output_format = Format::Human;
	s.output_color	= Color::Auto;
	s.macro_contexts.clear();
}

void configure(Format output_format, Color output_color, int error_limit) {
	auto					   &s = state();
	std::lock_guard<std::mutex> lock(s.mutex);
	s.output_format = output_format;
	s.output_color	= output_color;
	s.error_limit	= std::max(0, error_limit);
}

void finish() {
	auto					   &s = state();
	std::lock_guard<std::mutex> lock(s.mutex);
	if (s.summary_printed || s.suppressed == 0) {
		return;
	}
	s.summary_printed = true;
	if (s.output_format == Format::Json) {
		std::fprintf(stderr,
					 "{\"severity\":\"error\",\"code\":\"E_ERROR_LIMIT\",\"message\":\"%d "
					 "additional diagnostics "
					 "suppressed\",\"phase\":\"compiler\",\"primary\":null,\"related\":[],\"help\":"
					 "null,\"fixits\":[],\"attachments\":[]}\n",
					 s.suppressed);
	} else {
		std::fprintf(stderr, "error[E_ERROR_LIMIT]: %d additional diagnostics suppressed\n",
					 s.suppressed);
	}
}

Format format() {
	return state().output_format;
}
Color color() {
	return state().output_color;
}

void set_file(std::string file) {
	auto					   &s = state();
	std::lock_guard<std::mutex> lock(s.mutex);
	s.current_file = std::move(file);
}

void set_warning_as_error(bool enabled) {
	auto					   &s = state();
	std::lock_guard<std::mutex> lock(s.mutex);
	s.warning_as_error = enabled;
}

bool warning_as_error() {
	return state().warning_as_error;
}

const std::string &current_file() {
	return state().current_file;
}

void push_macro_context(const SourceLoc &definition,
						const std::vector<SourceLoc> &expansion_chain) {
	auto &s = state();
	std::lock_guard<std::mutex> lock(s.mutex);
	s.macro_contexts.push_back({definition, expansion_chain});
}

void pop_macro_context() {
	auto &s = state();
	std::lock_guard<std::mutex> lock(s.mutex);
	if (!s.macro_contexts.empty()) {
		s.macro_contexts.pop_back();
	}
}

void emit(Diagnostic diagnostic) {
	auto					   &s = state();
	std::lock_guard<std::mutex> lock(s.mutex);
	const Level					original_level = diagnostic.level;
	if (diagnostic.level == Level::Warning && s.warning_as_error) {
		diagnostic.level = Level::Error;
	}
	if (diagnostic.custom_code.empty()) {
		if (diagnostic.code == DiagCode::General) {
			if (original_level == Level::Warning) {
				diagnostic.code = DiagCode::WarningGeneral;
			} else if (original_level == Level::Note) {
				diagnostic.code = DiagCode::Note;
			}
		}
		if (diagnostic.phase == Phase::Compiler && diagnostic.code != DiagCode::General) {
			diagnostic.phase = code_phase(diagnostic.code);
		}
	}
	if (!diagnostic.preserve_case) {
		diagnostic.message = lower_first(std::move(diagnostic.message));
	}
	diagnostic.message		  = normalize_quotes(std::move(diagnostic.message));
	const size_t detail_break = diagnostic.message.find('\n');
	if (detail_break != std::string::npos) {
		Attachment detail;
		detail.kind = "compiler detail";
		detail.text = diagnostic.message.substr(detail_break + 1);
		diagnostic.message.resize(detail_break);
		diagnostic.attachments.push_back(std::move(detail));
	}
	if (diagnostic.primary.start.file.empty()) {
		diagnostic.primary.start.file = s.current_file;
	}
	if (diagnostic.primary.start.file.empty()) {
		diagnostic.primary.start.file = "<input>";
	}
	if (diagnostic.primary.start.line > 0 && diagnostic.primary.start.length <= 0) {
		diagnostic.primary = span_for(diagnostic.primary.start, diagnostic.primary.start.file);
	} else if (diagnostic.primary.end.column <= diagnostic.primary.start.column) {
		diagnostic.primary.end = diagnostic.primary.start;
	}
	if (!s.macro_contexts.empty()) {
		const MacroContext &context = s.macro_contexts.back();
		if (context.definition.line > 0) {
			Related related;
			related.span = span_for(
				context.definition,
				context.definition.file.empty() ? s.current_file : context.definition.file.str());
			related.message = "macro definition";
			diagnostic.related.push_back(std::move(related));
		}
		for (const SourceLoc &location : context.expansion_chain) {
			if (location.line <= 0) {
				continue;
			}
			Related related;
			related.span =
				span_for(location, location.file.empty() ? s.current_file : location.file.str());
			related.message = "macro expansion";
			diagnostic.related.push_back(std::move(related));
		}
	}

	const SourceSpan  &span = diagnostic.primary;
	std::ostringstream key;
	key << level_name(diagnostic.level) << '|' << diagnostic.code_name() << '|'
		<< span.start.file.str() << '|' << span.start.line << '|' << span.start.column << '|'
		<< diagnostic.message;
	if (s.seen[key.str()]) {
		return;
	}
	s.seen[key.str()] = true;
	if (diagnostic.level == Level::Error) {
		++s.errors;
		if (s.error_limit > 0 && s.emitted_errors >= s.error_limit) {
			++s.suppressed;
			return;
		}
		++s.emitted_errors;
	}
	if (s.output_format == Format::Json) {
		print_json(diagnostic);
	} else {
		print_human(diagnostic);
	}
}

void report(Level level, const SourceLoc &loc, const std::string &msg, DiagCode code,
			const std::string &help,
			const std::vector<Attachment> &attachments, bool preserve_case) {
	Diagnostic diagnostic;
	diagnostic.level	   = level;
	diagnostic.code		   = code;
	diagnostic.phase	   = code_phase(code);
	diagnostic.message	   = msg;
	diagnostic.help		   = help;
	diagnostic.attachments = attachments;
	diagnostic.preserve_case = preserve_case;
	diagnostic.primary	   = span_for(loc, loc.file.empty() ? current_file() : loc.file.str());
	emit(std::move(diagnostic));
}

void report(Level level, const SourceLoc &loc, const std::string &msg, const std::string &code,
			const std::string &phase, const std::string &help,
			const std::vector<Attachment> &attachments, bool preserve_case) {
	Diagnostic diagnostic;
	diagnostic.level	   = level;
	if (auto parsed_code = diag_code_from_string(code)) {
		diagnostic.code = *parsed_code;
		diagnostic.phase = code_phase(*parsed_code);
	} else if (!code.empty()) {
		diagnostic.code = DiagCode::General;
		diagnostic.custom_code = code;
	}
	if (!phase.empty()) {
		if (auto parsed_phase = phase_from_string(phase)) {
			diagnostic.phase = *parsed_phase;
		}
	}
	diagnostic.message	   = msg;
	diagnostic.help		   = help;
	diagnostic.attachments = attachments;
	diagnostic.preserve_case = preserve_case;
	diagnostic.primary	   = span_for(loc, loc.file.empty() ? current_file() : loc.file.str());
	emit(std::move(diagnostic));
}

void register_source(const std::string &path, std::string content) {
	if (path.empty()) {
		return;
	}
	auto						&cache = source_cache();
	std::lock_guard<std::mutex> lock(cache.mutex);
	SourceFile				   &source = cache.files[path];
	source.content					   = std::move(content);
	source.line_offsets.clear();
	source.line_offsets.push_back(0);
	for (size_t i = 0; i < source.content.size(); ++i) {
		if (source.content[i] == '\n') {
			source.line_offsets.push_back(i + 1);
		}
	}
}

SourceSpan span_for(const SourceLoc &location, const std::string &file) {
	SourceSpan span;
	span.start		= location;
	if (span.start.file.empty()) {
		span.start.file = file;
	}
	int length		= location.length;
	if (length <= 0 && location.line > 0) {
		const std::string text = line_text(load_source(span.start.file), location.line);
		if (location.column > 0 && static_cast<size_t>(location.column - 1) < text.size()) {
			size_t at = static_cast<size_t>(location.column - 1);
			if (std::isalnum(static_cast<unsigned char>(text[at])) || text[at] == '_') {
				size_t end = at;
				while (end < text.size() &&
					   (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_')) {
					++end;
				}
				length = static_cast<int>(end - at);
			}
		}
	}
	if (length <= 0) {
		length = 1;
	}
	span.start.length = length;
	span.end		  = span.start;
	span.end.column	  = span.start.column > 0 ? span.start.column + length : 0;
	span.end.length	  = 0;
	return span;
}

DiagnosticBuilder::DiagnosticBuilder(Level level, SourceLoc loc, std::string msg) {
	diag_.level	  = level;
	diag_.message = std::move(msg);
	diag_.primary = span_for(loc, loc.file.empty() ? current_file() : loc.file.str());
}

DiagnosticBuilder::~DiagnosticBuilder() {
	if (!emitted_) {
		emit();
	}
}

DiagnosticBuilder::DiagnosticBuilder(DiagnosticBuilder &&other) noexcept
	: diag_(std::move(other.diag_)), emitted_(other.emitted_) {
	other.emitted_ = true;
}

DiagnosticBuilder &DiagnosticBuilder::operator=(DiagnosticBuilder &&other) noexcept {
	if (this != &other) {
		if (!emitted_) {
			emit();
		}
		diag_		   = std::move(other.diag_);
		emitted_	   = other.emitted_;
		other.emitted_ = true;
	}
	return *this;
}

DiagnosticBuilder &DiagnosticBuilder::code(DiagCode c) & {
	diag_.code = c;
	diag_.custom_code.clear();
	diag_.phase = code_phase(c);
	return *this;
}
DiagnosticBuilder &&DiagnosticBuilder::code(DiagCode c) && {
	code(c);
	return std::move(*this);
}

DiagnosticBuilder &DiagnosticBuilder::code(std::string c) & {
	if (auto parsed = diag_code_from_string(c)) {
		diag_.code = *parsed;
		diag_.custom_code.clear();
		diag_.phase = code_phase(*parsed);
	} else {
		diag_.code = DiagCode::General;
		diag_.custom_code = std::move(c);
	}
	return *this;
}
DiagnosticBuilder &&DiagnosticBuilder::code(std::string c) && {
	code(std::move(c));
	return std::move(*this);
}

DiagnosticBuilder &DiagnosticBuilder::phase(Phase p) & {
	diag_.phase = p;
	return *this;
}
DiagnosticBuilder &&DiagnosticBuilder::phase(Phase p) && {
	phase(p);
	return std::move(*this);
}

DiagnosticBuilder &DiagnosticBuilder::phase(std::string p) & {
	if (auto parsed = phase_from_string(p)) {
		diag_.phase = *parsed;
	} else {
		diag_.phase = Phase::Compiler;
	}
	return *this;
}
DiagnosticBuilder &&DiagnosticBuilder::phase(std::string p) && {
	phase(std::move(p));
	return std::move(*this);
}

DiagnosticBuilder &DiagnosticBuilder::span(SourceSpan sp) & {
	diag_.primary = std::move(sp);
	return *this;
}
DiagnosticBuilder &&DiagnosticBuilder::span(SourceSpan sp) && {
	diag_.primary = std::move(sp);
	return std::move(*this);
}

DiagnosticBuilder &DiagnosticBuilder::length(int len) & {
	if (len > 0) {
		diag_.primary.start.length = len;
		diag_.primary.end		   = diag_.primary.start;
		diag_.primary.end.column   = diag_.primary.start.column > 0 ? diag_.primary.start.column + len : 0;
		diag_.primary.end.length   = 0;
	}
	return *this;
}
DiagnosticBuilder &&DiagnosticBuilder::length(int len) && {
	length(len);
	return std::move(*this);
}

DiagnosticBuilder &DiagnosticBuilder::note(const SourceLoc &loc, std::string msg) & {
	Related r;
	r.span	  = span_for(loc, loc.file.empty() ? current_file() : loc.file.str());
	r.message = std::move(msg);
	diag_.related.push_back(std::move(r));
	return *this;
}
DiagnosticBuilder &&DiagnosticBuilder::note(const SourceLoc &loc, std::string msg) && {
	note(loc, std::move(msg));
	return std::move(*this);
}

DiagnosticBuilder &DiagnosticBuilder::note(const SourceSpan &sp, std::string msg) & {
	Related r;
	r.span	  = sp;
	r.message = std::move(msg);
	diag_.related.push_back(std::move(r));
	return *this;
}
DiagnosticBuilder &&DiagnosticBuilder::note(const SourceSpan &sp, std::string msg) && {
	note(sp, std::move(msg));
	return std::move(*this);
}

DiagnosticBuilder &DiagnosticBuilder::help(std::string h) & {
	diag_.help = std::move(h);
	return *this;
}
DiagnosticBuilder &&DiagnosticBuilder::help(std::string h) && {
	diag_.help = std::move(h);
	return std::move(*this);
}

DiagnosticBuilder &DiagnosticBuilder::fixit(const SourceSpan &sp, std::string replacement, std::string desc) & {
	FixIt f;
	f.span		  = sp;
	f.replacement = std::move(replacement);
	f.description = std::move(desc);
	diag_.fixits.push_back(std::move(f));
	return *this;
}
DiagnosticBuilder &&DiagnosticBuilder::fixit(const SourceSpan &sp, std::string replacement, std::string desc) && {
	fixit(sp, std::move(replacement), std::move(desc));
	return std::move(*this);
}

DiagnosticBuilder &DiagnosticBuilder::fixit(const SourceLoc &loc, std::string replacement, std::string desc) & {
	return fixit(span_for(loc, loc.file.empty() ? current_file() : loc.file.str()), std::move(replacement),
				 std::move(desc));
}
DiagnosticBuilder &&DiagnosticBuilder::fixit(const SourceLoc &loc, std::string replacement, std::string desc) && {
	fixit(loc, std::move(replacement), std::move(desc));
	return std::move(*this);
}

DiagnosticBuilder &DiagnosticBuilder::attachment(std::string kind, std::string text) & {
	Attachment a;
	a.kind = std::move(kind);
	a.text = std::move(text);
	diag_.attachments.push_back(std::move(a));
	return *this;
}
DiagnosticBuilder &&DiagnosticBuilder::attachment(std::string kind, std::string text) && {
	attachment(std::move(kind), std::move(text));
	return std::move(*this);
}

DiagnosticBuilder &DiagnosticBuilder::preserve_case(bool p) & {
	diag_.preserve_case = p;
	return *this;
}

DiagnosticBuilder &&DiagnosticBuilder::preserve_case(bool p) && {
	diag_.preserve_case = p;
	return std::move(*this);
}

void DiagnosticBuilder::emit() {
	if (!emitted_) {
		emitted_ = true;
		log::emit(std::move(diag_));
	}
}

DiagnosticBuilder error(const SourceLoc &loc, std::string msg) {
	return DiagnosticBuilder(Level::Error, loc, std::move(msg));
}

DiagnosticBuilder warning(const SourceLoc &loc, std::string msg) {
	return DiagnosticBuilder(Level::Warning, loc, std::move(msg));
}

DiagnosticBuilder note(const SourceLoc &loc, std::string msg) {
	return DiagnosticBuilder(Level::Note, loc, std::move(msg));
}

bool has_error() {
	return state().errors > 0;
}
int error_count() {
	return state().errors;
}
int suppressed_count() {
	return state().suppressed;
}

} // namespace log

size_t levenshtein_distance(std::string_view s1, std::string_view s2) {
	const size_t m = s1.size();
	const size_t n = s2.size();
	if (m == 0) return n;
	if (n == 0) return m;

	std::vector<std::vector<size_t>> d(m + 1, std::vector<size_t>(n + 1));
	for (size_t i = 0; i <= m; ++i) d[i][0] = i;
	for (size_t j = 0; j <= n; ++j) d[0][j] = j;

	for (size_t i = 1; i <= m; ++i) {
		for (size_t j = 1; j <= n; ++j) {
			size_t cost = (s1[i - 1] == s2[j - 1]) ? 0 : 1;
			d[i][j] = std::min({
				d[i - 1][j] + 1,
				d[i][j - 1] + 1,
				d[i - 1][j - 1] + cost
			});
			if (i > 1 && j > 1 && s1[i - 1] == s2[j - 2] && s1[i - 2] == s2[j - 1]) {
				d[i][j] = std::min(d[i][j], d[i - 2][j - 2] + 1);
			}
		}
	}
	return d[m][n];
}

} // namespace stellac
