#pragma once

#include "types.hpp"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Target/TargetMachine.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace stellac {

struct TargetInfo {
	std::string triple; // e.g. x86_64-unknown-linux-gnu
	std::string cpu_specific = "native";
	std::string cpu_tune	 = "generic";
	std::string code_model	 = "small";
	bool			 function_sections = false;
	enum class RelocationModel { Static, PIC };
	RelocationModel relocation = RelocationModel::Static;
	// Derived flags
	std::string		arch; // x86_64 / arm64 / x86
	std::string		os;	  // linux / none / windows
	int				bits = 64;
};

std::optional<TargetInfo::RelocationModel> relocation_model_from_string(const std::string &s);

// Parse a code-model name into the LLVM enum. Returns nullopt for an
// unrecognized name. Single source of truth for both --tar.code-model validation
// (main.cpp) and TargetMachine construction (LLVMCtx::init).
std::optional<llvm::CodeModel::Model>	   code_model_from_string(const std::string &s);

enum class OptLevel {
	None,	// O0
	Low,	// O1
	Medium, // O2
	High,	// O3
	Size	// size-oriented O2 pipeline with LLVM's minsize function attribute
};

std::optional<OptLevel> opt_level_from_string(std::string_view s);
const char			   *opt_level_name(OptLevel level);

// lld's --lto-O option accepts only numeric optimization levels. Size
// optimization therefore uses O2 at link time; the compiler marks functions
// minsize so LLVM's size-aware passes can adjust their decisions.
unsigned				opt_level_lto_value(OptLevel level);

// Link-Time Optimization mode. Full LTO is the default; --opt.lto=thin selects
// ThinLTO. Both are driven by lld at link time, which requires the compiler
// to emit LLVM bitcode (rather than a native object) into the intermediate.
enum class LtoMode { None, Full, Thin };
std::optional<LtoMode> lto_mode_from_string(const std::string &s);

class LLVMCtx {
  public:
	bool			   init(const TargetInfo &tgt, OptLevel opt_level = OptLevel::None);
	bool			   finalize_to_file(const std::string &path, bool emit_ir, OptLevel opt_level,
										LtoMode lto = LtoMode::None,
										bool internalize_linkonce = false);

	llvm::LLVMContext &ctx() {
		return ctx_;
	}
	llvm::Module &module() {
		return *module_;
	}
	bool ok() const {
		return ok_;
	}

	llvm::Type *llvm_void();
	llvm::Type *llvm_int(unsigned width);
	llvm::Type *llvm_bool();
	llvm::Type *llvm_float(int width);
	llvm::Type *llvm_ptr();

  private:
	llvm::LLVMContext					 ctx_;
	std::unique_ptr<llvm::Module>		 module_;
	std::unique_ptr<llvm::TargetMachine> tm_;
	bool								 ok_ = false;
};

} // namespace stellac
