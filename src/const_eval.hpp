#pragma once

#include "llvm/IR/Module.h"
#include <cstdint>

namespace stellac {

struct ConstEvalLimits {
	uint64_t steps			 = 0;
	unsigned recursion_depth = 0;
	uint64_t memory_bytes	 = 0;
};

// Evaluate safe direct calls whose arguments are LLVM constants.  This is an
// optimization only: a failed evaluation is deliberately indistinguishable
// from an ineligible call to the caller.
bool run_constant_evaluator(llvm::Module &module, ConstEvalLimits limits);

} // namespace stellac
