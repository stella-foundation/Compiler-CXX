#include "ctx.hpp"
#include "const_eval.hpp"
#include "log.hpp"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/InlineCost.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/AArch64TargetParser.h"
#include "llvm/TargetParser/ARMTargetParser.h"
#include "llvm/TargetParser/CSKYTargetParser.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/LoongArchTargetParser.h"
#include "llvm/TargetParser/PPCTargetParser.h"
#include "llvm/TargetParser/RISCVTargetParser.h"
#include "llvm/TargetParser/TargetParser.h"
#include "llvm/TargetParser/X86TargetParser.h"
#include "llvm/Transforms/IPO/Attributor.h"
#include "llvm/Transforms/IPO/GlobalDCE.h"
#include "llvm/Transforms/IPO/MergeFunctions.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/ADCE.h"
#include "llvm/Transforms/Scalar/SCCP.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Utils/LowerMemIntrinsics.h"
#include "llvm/Transforms/Vectorize/LoopVectorize.h"

#include <algorithm>
#include <unordered_set>
#include <utility>
#include <vector>

namespace stellac {

namespace {

// Stella's default startup is freestanding. LLVM's target lowering normally
// turns memory intrinsics into calls to the platform C ABI (for example,
// `llvm.memset` becomes `memset`), which would make otherwise freestanding
// objects depend on libc. Expand these intrinsics while they are still LLVM
// IR so instruction selection emits the operation inline instead.
class LowerFreestandingMemoryIntrinsicsPass
	: public llvm::PassInfoMixin<LowerFreestandingMemoryIntrinsicsPass> {
  public:
	llvm::PreservedAnalyses run(llvm::Function &function, llvm::FunctionAnalysisManager &analyses) {
		llvm::SmallVector<llvm::MemIntrinsic *, 8> intrinsics;
		for (auto &block : function) {
			for (auto &instruction : block) {
				if (auto *intrinsic = llvm::dyn_cast<llvm::MemIntrinsic>(&instruction)) {
					intrinsics.push_back(intrinsic);
				}
			}
		}

		if (intrinsics.empty()) {
			return llvm::PreservedAnalyses::all();
		}

		auto &tti = analyses.getResult<llvm::TargetIRAnalysis>(function);
		const uint64_t max_inline = tti.getMaxMemIntrinsicInlineSizeThreshold();
		for (llvm::MemIntrinsic *intrinsic : intrinsics) {
			if (const auto *len = llvm::dyn_cast<llvm::ConstantInt>(intrinsic->getLength())) {
				if (len->getZExtValue() <= max_inline) {
					continue;
				}
			}
			if (auto *memset = llvm::dyn_cast<llvm::MemSetInst>(intrinsic)) {
				llvm::expandMemSetAsLoop(memset);
				memset->eraseFromParent();
			} else if (auto *memcpy = llvm::dyn_cast<llvm::MemCpyInst>(intrinsic)) {
				llvm::expandMemCpyAsLoop(memcpy, tti);
				memcpy->eraseFromParent();
			} else if (auto *memmove = llvm::dyn_cast<llvm::MemMoveInst>(intrinsic)) {
				if (llvm::expandMemMoveAsLoop(memmove, tti)) {
					memmove->eraseFromParent();
				}
			}
		}
		return llvm::PreservedAnalyses::none();
	}
};

bool cpu_in_list(llvm::ArrayRef<llvm::StringRef> names, llvm::StringRef cpu) {
	return std::find(names.begin(), names.end(), cpu) != names.end();
}

bool valid_cpu_name(const llvm::Triple &triple, llvm::StringRef cpu, bool tune) {
	if (cpu == "generic") {
		return true;
	}
	llvm::SmallVector<llvm::StringRef, 64> names;
	switch (triple.getArch()) {
	case llvm::Triple::x86:
		llvm::X86::fillValidCPUArchList(names, false);
		break;
	case llvm::Triple::x86_64:
		if (tune) {
			llvm::X86::fillValidTuneCPUList(names, true);
		} else {
			llvm::X86::fillValidCPUArchList(names, true);
		}
		break;
	case llvm::Triple::aarch64:
	case llvm::Triple::aarch64_be:
	case llvm::Triple::aarch64_32:
		llvm::AArch64::fillValidCPUArchList(names);
		break;
	case llvm::Triple::arm:
	case llvm::Triple::armeb:
	case llvm::Triple::thumb:
	case llvm::Triple::thumbeb:
		llvm::ARM::fillValidCPUArchList(names);
		break;
	case llvm::Triple::riscv32:
	case llvm::Triple::riscv32be:
		if (tune) {
			llvm::RISCV::fillValidTuneCPUArchList(names, false);
		} else {
			llvm::RISCV::fillValidCPUArchList(names, false);
		}
		break;
	case llvm::Triple::riscv64:
	case llvm::Triple::riscv64be:
		if (tune) {
			llvm::RISCV::fillValidTuneCPUArchList(names, true);
		} else {
			llvm::RISCV::fillValidCPUArchList(names, true);
		}
		break;
	case llvm::Triple::ppc:
	case llvm::Triple::ppcle:
	case llvm::Triple::ppc64:
	case llvm::Triple::ppc64le:
		if (tune) {
			llvm::PPC::fillValidTuneCPUList(names);
		} else {
			llvm::PPC::fillValidCPUList(names);
		}
		break;
	case llvm::Triple::loongarch32:
	case llvm::Triple::loongarch64:
		if (!tune) {
			llvm::LoongArch::fillValidCPUList(names);
		} else {
			return llvm::LoongArch::isValidCPUName(cpu);
		}
		break;
	case llvm::Triple::csky:
		llvm::CSKY::fillValidCPUArchList(names);
		break;
	default:
		// Targets without a public CPU table remain LLVM-defined.
		return true;
	}
	return cpu_in_list(names, cpu);
}

std::string format_host_cpu_features() {
	llvm::StringMap<bool, llvm::MallocAllocator> host_features = llvm::sys::getHostCPUFeatures();
	if (host_features.empty()) {
		return "";
	}
	std::vector<std::pair<llvm::StringRef, bool>> sorted;
	sorted.reserve(host_features.size());
	for (const auto &entry : host_features) {
		sorted.emplace_back(entry.first(), entry.second);
	}
	std::sort(sorted.begin(), sorted.end(),
			  [](const auto &a, const auto &b) { return a.first < b.first; });
	std::string result;
	for (size_t i = 0; i < sorted.size(); ++i) {
		if (i > 0) {
			result += ',';
		}
		result += (sorted[i].second ? '+' : '-');
		result.append(sorted[i].first.data(), sorted[i].first.size());
	}
	return result;
}

bool resolve_cpu(const TargetInfo &tgt, const llvm::Triple &triple, std::string &cpu,
				 std::string &features, std::string &error) {
	if (tgt.cpu_specific.empty() || tgt.cpu_tune.empty()) {
		error = "cpu options must not be empty";
		return false;
	}
	if (tgt.cpu_tune != "generic" && tgt.cpu_specific != "generic" &&
		tgt.cpu_tune != tgt.cpu_specific &&
		!(tgt.cpu_tune == "native" && tgt.cpu_specific == "native")) {
		error = "--tar.cpu and --tar.tune cannot select different CPUs; LLVM supports one "
				"target CPU here";
		return false;
	}
	cpu = tgt.cpu_specific != "generic" ? tgt.cpu_specific : tgt.cpu_tune;
	features.clear();
	if (cpu == "native") {
		llvm::Triple host(llvm::sys::getDefaultTargetTriple());
		if (triple.getArch() != host.getArch()) {
			error = "cpu `native` is only valid when the target architecture matches the host "
					"architecture";
			return false;
		}
		cpu = llvm::sys::getHostCPUName().str();
		if (cpu.empty()) {
			cpu = "generic";
		}
		features = format_host_cpu_features();
	}
	bool from_tune = tgt.cpu_specific == "generic" && tgt.cpu_tune != "generic";
	if (!valid_cpu_name(triple, cpu, from_tune)) {
		error = "cpu `" + cpu + "` is not supported for target `" + triple.str() + "`";
		return false;
	}
	return true;
}

} // namespace

std::optional<llvm::CodeModel::Model> code_model_from_string(const std::string &s) {
	if (s == "tiny") {
		return llvm::CodeModel::Tiny;
	}
	if (s == "small") {
		return llvm::CodeModel::Small;
	}
	if (s == "kernel") {
		return llvm::CodeModel::Kernel;
	}
	if (s == "medium") {
		return llvm::CodeModel::Medium;
	}
	if (s == "large") {
		return llvm::CodeModel::Large;
	}
	return std::nullopt;
}

std::optional<OptLevel> opt_level_from_string(std::string_view s) {
	if (s == "none") {
		return OptLevel::None;
	}
	if (s == "low") {
		return OptLevel::Low;
	}
	if (s == "medium") {
		return OptLevel::Medium;
	}
	if (s == "high") {
		return OptLevel::High;
	}
	if (s == "size") {
		return OptLevel::Size;
	}
	return std::nullopt;
}

const char *opt_level_name(OptLevel level) {
	switch (level) {
	case OptLevel::None:
		return "none";
	case OptLevel::Low:
		return "low";
	case OptLevel::Medium:
		return "medium";
	case OptLevel::High:
		return "high";
	case OptLevel::Size:
		return "size";
	}
	return "none";
}

unsigned opt_level_lto_value(OptLevel level) {
	switch (level) {
	case OptLevel::Low:
		return 1;
	case OptLevel::Medium:
	case OptLevel::Size:
		return 2;
	case OptLevel::High:
		return 3;
	case OptLevel::None:
		return 0;
	}
	return 0;
}

std::optional<LtoMode> lto_mode_from_string(const std::string &s) {
	if (s == "none") {
		return LtoMode::None;
	}
	if (s == "full") {
		return LtoMode::Full;
	}
	if (s == "thin") {
		return LtoMode::Thin;
	}
	return std::nullopt;
}

std::optional<TargetInfo::RelocationModel> relocation_model_from_string(const std::string &s) {
	if (s == "static") {
		return TargetInfo::RelocationModel::Static;
	}
	if (s == "pic") {
		return TargetInfo::RelocationModel::PIC;
	}
	return std::nullopt;
}

bool LLVMCtx::init(const TargetInfo &tgt, OptLevel opt_level) {
	llvm::InitializeAllTargetInfos();
	llvm::InitializeAllTargets();
	llvm::InitializeAllTargetMCs();
	llvm::InitializeAllAsmParsers();
	llvm::InitializeAllAsmPrinters();

	std::string			err;
	llvm::Triple		triple(llvm::Triple::normalize(tgt.triple));
	const llvm::Target *target = llvm::TargetRegistry::lookupTarget(triple, err);
	if (!target) {
		log::report(log::Level::Error, {}, "no target for triple '" + tgt.triple + "': " + err);
		return false;
	}

	llvm::TargetOptions opt;
	opt.ExceptionModel	 = llvm::ExceptionHandling::None;
	opt.FunctionSections = tgt.function_sections;
	opt.DataSections	 = tgt.function_sections;
	auto rm = std::optional<llvm::Reloc::Model>(tgt.relocation == TargetInfo::RelocationModel::PIC
													? llvm::Reloc::PIC_
													: llvm::Reloc::Static);

	std::string cpu;
	std::string features;
	std::string cpu_error;
	if (!resolve_cpu(tgt, triple, cpu, features, cpu_error)) {
		log::report(log::Level::Error, {}, cpu_error, "E_TARGET_CPU", "backend");
		return false;
	}

	std::optional<llvm::CodeModel::Model> cm = code_model_from_string(tgt.code_model);

	// The CLI exposes CPU names, not LLVM's target-feature mini-language, so
	// features is empty except when resolved from host CPU features for `native`.
	llvm::CodeGenOptLevel				  codegen_opt_level;
	switch (opt_level) {
	case OptLevel::None:
		codegen_opt_level = llvm::CodeGenOptLevel::None;
		break;
	case OptLevel::Low:
		codegen_opt_level = llvm::CodeGenOptLevel::Less;
		break;
	case OptLevel::Medium:
	case OptLevel::Size:
		codegen_opt_level = llvm::CodeGenOptLevel::Default;
		break;
	case OptLevel::High:
		codegen_opt_level = llvm::CodeGenOptLevel::Aggressive;
		break;
	}

	tm_.reset(target->createTargetMachine(triple, cpu, features, opt, rm, cm, codegen_opt_level));
	if (!tm_) {
		log::report(log::Level::Error, {}, "failed to create TargetMachine");
		return false;
	}

	module_ = std::make_unique<llvm::Module>(tgt.triple, ctx_);
	module_->setTargetTriple(triple);
	module_->setDataLayout(tm_->createDataLayout());
	ok_ = true;
	return true;
}

llvm::Type *LLVMCtx::llvm_void() {
	return llvm::Type::getVoidTy(ctx_);
}
llvm::Type *LLVMCtx::llvm_int(unsigned width) {
	return llvm::IntegerType::get(ctx_, width);
}
llvm::Type *LLVMCtx::llvm_bool() {
	return llvm::Type::getInt1Ty(ctx_);
}
llvm::Type *LLVMCtx::llvm_float(int width) {
	switch (width) {
	case 16:
		return llvm::Type::getHalfTy(ctx_);
	case 32:
		return llvm::Type::getFloatTy(ctx_);
	default:
		return llvm::Type::getDoubleTy(ctx_);
	}
}
llvm::Type *LLVMCtx::llvm_ptr() {
	return llvm::PointerType::get(ctx_, 0);
}

bool LLVMCtx::finalize_to_file(const std::string &path, bool emit_ir, OptLevel opt_level,
							   LtoMode lto, bool internalize_linkonce) {
	if (!ok_ || !module_) {
		return false;
	}
	std::error_code		 ec;
	llvm::raw_fd_ostream out(path, ec, llvm::sys::fs::OF_None);
	if (ec) {
		log::report(log::Level::Error, {}, "cannot open output '" + path + "': " + ec.message());
		return false;
	}
	// Stella is freestanding by default. Tell every LLVM optimization and codegen
	// pass that no platform library is available, including libc and libm.
	// Avoid Attribute::NoBuiltin on functions because it blocks interprocedural
	// optimizations such as IPSCCP argument specialization.
	llvm::TargetLibraryInfoImpl no_platform_libraries(llvm::Triple(module_->getTargetTriple()));
	no_platform_libraries.disableAllFunctions();

	std::unordered_set<const llvm::GlobalValue *> used_globals;
	if (internalize_linkonce && opt_level != OptLevel::None) {
		for (const char *used_name : {"llvm.used", "llvm.compiler.used"}) {
			if (const auto *gv = module_->getGlobalVariable(used_name)) {
				if (const auto *init = llvm::dyn_cast_or_null<llvm::ConstantArray>(
						gv->getInitializer())) {
					for (const llvm::Value *op : init->operands()) {
						if (const auto *stripped = llvm::dyn_cast<llvm::GlobalValue>(
								op->stripPointerCasts())) {
							used_globals.insert(stripped);
						}
					}
				}
			}
		}
	}

	const llvm::StringRef target_cpu	  = tm_->getTargetCPU();
	const llvm::StringRef target_features = tm_->getTargetFeatureString();
	for (llvm::Function &function : *module_) {
		if (!function.isIntrinsic()) {
			if (!target_cpu.empty() && target_cpu != "generic" &&
				!function.hasFnAttribute("target-cpu")) {
				function.addFnAttr("target-cpu", target_cpu);
			}
			if (!target_features.empty() && !function.hasFnAttribute("target-features")) {
				function.addFnAttr("target-features", target_features);
			}
		}
		if (opt_level == OptLevel::Size && !function.isDeclaration()) {
			function.addFnAttr(llvm::Attribute::MinSize);
		}
		if (internalize_linkonce && opt_level != OptLevel::None && !function.isDeclaration() &&
			llvm::GlobalValue::isLinkOnceODRLinkage(function.getLinkage()) &&
			used_globals.count(&function) == 0) {
			function.setLinkage(llvm::GlobalValue::InternalLinkage);
		}
		if (!function.isDeclaration() && function.hasLocalLinkage()) {
			function.setDSOLocal(true);
		}
	}
	for (llvm::GlobalVariable &gv : module_->globals()) {
		if (!gv.isDeclaration() && gv.hasLocalLinkage()) {
			gv.setDSOLocal(true);
		}
	}

	auto lower_freestanding_mem_intrinsics = [&](llvm::FunctionAnalysisManager &fam) {
		llvm::SmallVector<llvm::Function *, 4> mem_fns;
		for (llvm::Function &function : *module_) {
			if (function.isDeclaration()) {
				continue;
			}
			for (auto &block : function) {
				bool found = false;
				for (auto &instruction : block) {
					if (llvm::isa<llvm::MemIntrinsic>(&instruction)) {
						mem_fns.push_back(&function);
						found = true;
						break;
					}
				}
				if (found) {
					break;
				}
			}
		}
		if (mem_fns.empty()) {
			return;
		}
		LowerFreestandingMemoryIntrinsicsPass pass;
		llvm::FunctionPassManager			  vec_fpm;
		const bool							  vectorize =
			opt_level != OptLevel::None && opt_level != OptLevel::Size;
		if (vectorize) {
			vec_fpm.addPass(llvm::LoopVectorizePass());
			vec_fpm.addPass(llvm::InstCombinePass());
			vec_fpm.addPass(llvm::SimplifyCFGPass());
		}
		for (llvm::Function *function : mem_fns) {
			auto preserved = pass.run(*function, fam);
			if (!preserved.areAllPreserved() && vectorize) {
				fam.invalidate(*function, preserved);
				vec_fpm.run(*function, fam);
			}
		}
	};

	if (opt_level != OptLevel::None) {
		llvm::LoopAnalysisManager	  LAM;
		llvm::FunctionAnalysisManager FAM;
		llvm::CGSCCAnalysisManager	  CGAM;
		llvm::ModuleAnalysisManager	  MAM;

		llvm::PassBuilder			  PB(tm_.get());
		PB.registerModuleAnalyses(MAM);
		PB.registerCGSCCAnalyses(CGAM);
		FAM.registerPass(
			[no_platform_libraries] { return llvm::TargetLibraryAnalysis(no_platform_libraries); });
		PB.registerFunctionAnalyses(FAM);
		PB.registerLoopAnalyses(LAM);
		PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

		llvm::OptimizationLevel O;
		switch (opt_level) {
		case OptLevel::Low:
			O = llvm::OptimizationLevel::O1;
			break;
		case OptLevel::Medium:
			O = llvm::OptimizationLevel::O2;
			break;
		case OptLevel::High:
			O = llvm::OptimizationLevel::O3;
			break;
		case OptLevel::Size:
			// LLVM 23 no longer exposes Os/Oz as OptimizationLevel values.
			// MinSize attributes above make size-aware passes use their
			// size-oriented behavior in the O2 pipeline.
			O = llvm::OptimizationLevel::O2;
			break;
		case OptLevel::None:
			return false;
		}

		llvm::ModulePassManager MPM;
		MPM.addPass(llvm::AttributorLightPass());
		MPM.addPass(PB.buildPerModuleDefaultPipeline(O));
		if (opt_level == OptLevel::Size) {
			MPM.addPass(llvm::MergeFunctionsPass());
		}
		// Run ordinary optimization before the evaluator so scalar replacement,
		// CFG simplification, and inlining expose constants hidden in the IR.
		MPM.run(*module_, MAM);

		ConstEvalLimits limits;
		switch (opt_level) {
		case OptLevel::Low:
			limits = {8192, 16, 64 * 1024};
			break;
		case OptLevel::Medium:
			limits = {65536, 32, 256 * 1024};
			break;
		case OptLevel::Size:
			limits = {4096, 16, 64 * 1024};
			break;
		case OptLevel::High:
			limits = {1000000, 128, 1024 * 1024};
			break;
		case OptLevel::None:
			break;
		}
		const bool eval_changed = run_constant_evaluator(*module_, limits);

		if (eval_changed) {
			llvm::ModulePassManager cleanup;
			// Clean up constants and dead branches exposed by the evaluator.
			cleanup.addPass(llvm::createModuleToFunctionPassAdaptor(llvm::SCCPPass()));
			cleanup.addPass(llvm::createModuleToFunctionPassAdaptor(llvm::InstCombinePass()));
			cleanup.addPass(llvm::createModuleToFunctionPassAdaptor(llvm::SimplifyCFGPass()));
			cleanup.addPass(llvm::createModuleToFunctionPassAdaptor(llvm::ADCEPass()));
			cleanup.addPass(llvm::GlobalDCEPass());
			if (opt_level == OptLevel::Size) {
				cleanup.addPass(llvm::MergeFunctionsPass());
			}
			cleanup.run(*module_, MAM);
		}

		// Keep freestanding output independent of platform C memory routines. This
		// must run after the optimizer, which is where source-level byte loops are
		// commonly recognized as LLVM memory intrinsics.
		lower_freestanding_mem_intrinsics(FAM);
	} else {
		bool has_mem = false;
		for (llvm::Function &function : *module_) {
			if (function.isDeclaration()) {
				continue;
			}
			for (auto &block : function) {
				for (auto &instruction : block) {
					if (llvm::isa<llvm::MemIntrinsic>(&instruction)) {
						has_mem = true;
						break;
					}
				}
				if (has_mem) {
					break;
				}
			}
			if (has_mem) {
				break;
			}
		}
		if (has_mem) {
			llvm::FunctionAnalysisManager FAM;
			llvm::PassBuilder			  PB(tm_.get());
			FAM.registerPass([no_platform_libraries] {
				return llvm::TargetLibraryAnalysis(no_platform_libraries);
			});
			PB.registerFunctionAnalyses(FAM);
			lower_freestanding_mem_intrinsics(FAM);
		}
	}
	if (emit_ir) {
		module_->print(out, nullptr);
		out.flush();
		out.close();
		return true;
	}
	// LTO: hand lld LLVM bitcode instead of a native object so it can run the
	// link-time pipeline (--opt.lto=full / --opt.lto=thin). --emit.ir already writes
	// textual IR and is unaffected.
	if (lto != LtoMode::None) {
		llvm::WriteBitcodeToFile(*module_, out);
		out.flush();
		return true;
	}
	llvm::legacy::PassManager pm;
	pm.add(new llvm::TargetLibraryInfoWrapperPass(no_platform_libraries));
	llvm::CodeGenFileType ft = llvm::CodeGenFileType::ObjectFile;
	if (tm_->addPassesToEmitFile(pm, out, nullptr, ft)) {
		log::report(log::Level::Error, {}, "target cannot emit object file");
		return false;
	}
	pm.run(*module_);
	out.flush();
	return true;
}

} // namespace stellac
