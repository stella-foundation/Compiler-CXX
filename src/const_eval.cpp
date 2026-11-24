#include "const_eval.hpp"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Casting.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace stellac {

namespace {

struct Address {
	const llvm::Value *root	  = nullptr;
	int64_t			   offset = 0;
	uint64_t		   frame = 0;
};

struct AddressLess {
	bool operator()(const Address &lhs, const Address &rhs) const {
		if (lhs.root != rhs.root) {
			return std::less<const llvm::Value *>{}(lhs.root, rhs.root);
		}
		if (lhs.frame != rhs.frame) {
			return lhs.frame < rhs.frame;
		}
		return lhs.offset < rhs.offset;
	}
};

struct Frame {
	uint64_t frame_id = 0;
	std::unordered_map<const llvm::Value *, llvm::Constant *> values;
	std::unordered_map<const llvm::Value *, Address>		  addresses;
};

struct EvalArg {
	llvm::Constant		  *value = nullptr;
	std::optional<Address> address;
};

struct EvalState {
	ConstEvalLimits									 limits;
	uint64_t										 steps	   = 0;
	uint64_t										 allocated = 0;
	std::map<Address, llvm::Constant *, AddressLess> memory;
	uint64_t next_frame_id = 1;
};

class Evaluator {
  public:
	Evaluator(llvm::Module &module, ConstEvalLimits limits)
		: module_(module), dl_(module.getDataLayout()), limits_(limits) {}

	bool run() {
		if (limits_.steps == 0 || limits_.recursion_depth == 0 || limits_.memory_bytes == 0) {
			return false;
		}

		bool changed = false;
		// A successful replacement can expose another direct call. Repeat until
		// a complete scan makes no progress; recursive evaluation is still
		// bounded independently for every candidate.
		std::unordered_set<const llvm::CallBase *> failed_const_calls;
		for (;;) {
			bool round_changed = false;
			for (llvm::Function &fn : module_) {
				if (fn.isDeclaration()) {
					continue;
				}
				llvm::SmallVector<llvm::CallBase *, 16> calls;
				for (llvm::BasicBlock &bb : fn) {
					for (llvm::Instruction &inst : bb) {
						if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
							if (!call->getType()->isVoidTy() && !failed_const_calls.count(call)) {
								if (auto *callee = call->getCalledFunction()) {
									if (!callee->isDeclaration() || callee->isIntrinsic()) {
										calls.push_back(call);
									}
								}
							}
						}
					}
				}
				for (llvm::CallBase *call : calls) {
					if (!call->getParent()) {
						continue;
					}
					bool all_const_args = true;
					for (const llvm::Use &arg : call->args()) {
						if (!llvm::isa<llvm::Constant>(arg.get())) {
							all_const_args = false;
							break;
						}
					}
					if (!all_const_args) {
						continue;
					}
					llvm::Constant *result = nullptr;
					if (!evaluate_call(*call, result)) {
						failed_const_calls.insert(call);
						continue;
					}
					call->replaceAllUsesWith(result);
					call->eraseFromParent();
					round_changed = true;
					changed		  = true;
				}
			}
			if (!round_changed) {
				break;
			}
		}
		return changed;
	}

  private:
	llvm::Module		   &module_;
	const llvm::DataLayout &dl_;
	ConstEvalLimits			limits_;

	static bool				safe_constant(const llvm::Constant *value) {
		if (!value || llvm::isa<llvm::UndefValue>(value) || llvm::isa<llvm::PoisonValue>(value)) {
			return false;
		}
		if (auto *aggregate = llvm::dyn_cast<llvm::ConstantAggregate>(value)) {
			for (llvm::Value *operand : aggregate->operands()) {
				if (!safe_constant(llvm::cast<llvm::Constant>(operand))) {
					return false;
				}
			}
		}
		return true;
	}

	bool consume_step(EvalState &state) const {
		if (state.steps >= state.limits.steps) {
			return false;
		}
		++state.steps;
		return true;
	}

	static std::optional<int64_t> signed_offset(const llvm::APInt &offset) {
		if (offset.getBitWidth() > 64) {
			return std::nullopt;
		}
		return offset.getSExtValue();
	}

	std::optional<Address> address_from_constant(const llvm::Constant *value) const {
		if (!value) {
			return std::nullopt;
		}
		llvm::GlobalValue *global = nullptr;
		llvm::APInt		   offset(dl_.getPointerSizeInBits(), 0);
		if (!llvm::IsConstantOffsetFromGlobal(const_cast<llvm::Constant *>(value), global, offset,
											  dl_)) {
			return std::nullopt;
		}
		if (!llvm::isa<llvm::GlobalVariable>(global)) {
			return std::nullopt;
		}
		auto signed_value = signed_offset(offset);
		if (!signed_value) {
			return std::nullopt;
		}
		return Address{global, *signed_value, 0};
	}

	std::optional<Address> address_of(const llvm::Value *value, const Frame &frame) const {
		if (!value) {
			return std::nullopt;
		}
		if (auto it = frame.addresses.find(value); it != frame.addresses.end()) {
			return it->second;
		}
		if (auto *constant = llvm::dyn_cast<llvm::Constant>(value)) {
			return address_from_constant(constant);
		}
		return std::nullopt;
	}

	llvm::Constant *load_from_memory(const Address &address, llvm::Type *type,
									 EvalState &state) const {
		if (address.frame == 0) {
			auto *global = llvm::dyn_cast<llvm::GlobalVariable>(address.root);
			if (!global) {
				return nullptr;
			}
			if (!global->isConstant() || !global->hasInitializer()) {
				return nullptr;
			}
			llvm::APInt offset(dl_.getPointerSizeInBits(), address.offset, true);
			auto	   *result = llvm::ConstantFoldLoadFromConst(
				const_cast<llvm::Constant *>(global->getInitializer()), type, offset, dl_);
			return safe_constant(result) ? const_cast<llvm::Constant *>(result) : nullptr;
		}

		auto it = state.memory.upper_bound(address);
		if (it == state.memory.begin()) {
			return nullptr;
		}
		--it;
		if (it->first.root != address.root || it->first.frame != address.frame) {
			return nullptr;
		}
		const llvm::Constant *best		  = it->second;
		const int64_t		  best_offset = it->first.offset;
		if (best_offset < 0 && address.offset > INT64_MAX + best_offset) {
			// Both offsets are representable, but their non-negative distance may
			// not fit in the signed offset accepted by ConstantFoldLoadFromConst.
			return nullptr;
		}
		llvm::APInt offset(dl_.getPointerSizeInBits(), address.offset - best_offset, true);
		auto	   *result =
			llvm::ConstantFoldLoadFromConst(const_cast<llvm::Constant *>(best), type, offset, dl_);
		return safe_constant(result) ? const_cast<llvm::Constant *>(result) : nullptr;
	}

	bool store_to_memory(const Address &address, llvm::Constant *value, EvalState &state) const {
		if (!value || !safe_constant(value) || !address.root || address.frame == 0) {
			return false;
		}
		// A store through a derived pointer can overlap an earlier aggregate or
		// element store at a different offset. The evaluator does not model byte
		// ranges, so discard every cached value for this allocation before
		// recording the write; otherwise a later whole-object load could observe
		// the stale aggregate instead of the field update.
		auto it = state.memory.lower_bound(Address{address.root, INT64_MIN, address.frame});
		while (it != state.memory.end() && it->first.root == address.root &&
			   it->first.frame == address.frame) {
			it = state.memory.erase(it);
		}
		state.memory[address] = value;
		return true;
	}

	llvm::Constant *operand_constant(const llvm::Value *value, const Frame &frame) const {
		if (auto *constant = llvm::dyn_cast<llvm::Constant>(value)) {
			return safe_constant(constant) ? const_cast<llvm::Constant *>(constant) : nullptr;
		}
		auto it = frame.values.find(value);
		return it == frame.values.end() || !safe_constant(it->second) ? nullptr : it->second;
	}

	std::optional<EvalArg> operand_argument(const llvm::Value *value, const Frame *frame) const {
		if (frame && value->getType()->isPointerTy()) {
			if (auto address = address_of(value, *frame)) {
				return EvalArg{nullptr, address};
			}
		}
		auto *constant =
			frame ? operand_constant(value, *frame) : llvm::dyn_cast<llvm::Constant>(value);
		if (!constant || !safe_constant(constant)) {
			return std::nullopt;
		}
		EvalArg result;
		result.value = const_cast<llvm::Constant *>(constant);
		if (value->getType()->isPointerTy()) {
			result.address = address_from_constant(constant);
		}
		return result;
	}

	bool allocate(const llvm::AllocaInst &alloca, Frame &frame, EvalState &state) const {
		uint64_t size = dl_.getTypeAllocSize(alloca.getAllocatedType());
		if (state.allocated > state.limits.memory_bytes ||
			size > state.limits.memory_bytes - state.allocated) {
			return false;
		}
		state.allocated += size;
		Address address{&alloca, 0, frame.frame_id};
		frame.addresses[&alloca] = address;
		// Ordinary local allocas are uninitialized until a store executes. Do
		// not guess zero here: a load before the first write must make constant
		// evaluation fail rather than specialize the call with a fabricated value.
		return true;
	}

	bool evaluate_gep(const llvm::GetElementPtrInst &gep, Frame &frame, EvalState &) const {
		auto base = address_of(gep.getPointerOperand(), frame);
		if (!base) {
			return false;
		}
		llvm::SmallVector<llvm::Value *, 8>	   indices;
		llvm::SmallVector<llvm::Constant *, 8> owned_indices;
		for (llvm::Value *index : gep.indices()) {
			auto *constant = operand_constant(index, frame);
			auto *integer  = llvm::dyn_cast_or_null<llvm::ConstantInt>(constant);
			if (!integer) {
				return false;
			}
			owned_indices.push_back(
				llvm::ConstantInt::get(integer->getType(), integer->getValue()));
			indices.push_back(owned_indices.back());
		}
		int64_t		offset_value = dl_.getIndexedOffsetInType(gep.getSourceElementType(), indices);
		llvm::APInt offset(dl_.getPointerSizeInBits(), static_cast<uint64_t>(offset_value), true);
		auto		signed_value = signed_offset(offset);
		if (!signed_value || ((*signed_value > 0) && base->offset > INT64_MAX - *signed_value) ||
			(*signed_value < 0 && base->offset < INT64_MIN - *signed_value)) {
			return false;
		}
		frame.addresses[&gep] =
			Address{base->root, base->offset + *signed_value, base->frame};
		return true;
	}

	bool evaluate_intrinsic(const llvm::CallBase &call, llvm::ArrayRef<llvm::Constant *> args,
							llvm::Constant *&result) const {
		const llvm::Function *callee = call.getCalledFunction();
		if (!callee || !callee->isIntrinsic()) {
			return false;
		}
		llvm::StringRef name = callee->getName();
		if (name.starts_with("llvm.lifetime.") || name == "llvm.assume" ||
			name.starts_with("llvm.dbg.")) {
			if (name == "llvm.assume" && args.size() == 1) {
				auto *condition = llvm::dyn_cast<llvm::ConstantInt>(args[0]);
				result			= nullptr;
				return condition && !condition->isZero();
			}
			result = nullptr;
			return true;
		}
		if (name.starts_with("llvm.abs.") && args.size() >= 1) {
			auto *value = llvm::dyn_cast<llvm::ConstantInt>(args[0]);
			if (!value) {
				return false;
			}
			if (args.size() > 1 && llvm::isa<llvm::ConstantInt>(args[1]) &&
				llvm::cast<llvm::ConstantInt>(args[1])->isOne() && value->isMinSignedValue()) {
				return false;
			}
			result = llvm::ConstantInt::get(value->getType(), value->getValue().isNegative()
																  ? -value->getValue()
																  : value->getValue());
			return true;
		}
		if ((name.starts_with("llvm.umin.") || name.starts_with("llvm.umax.") ||
			 name.starts_with("llvm.smin.") || name.starts_with("llvm.smax.")) &&
			args.size() == 2) {
			auto *lhs = llvm::dyn_cast<llvm::ConstantInt>(args[0]);
			auto *rhs = llvm::dyn_cast<llvm::ConstantInt>(args[1]);
			if (!lhs || !rhs) {
				return false;
			}
			bool signed_compare = name.starts_with("llvm.s");
			bool choose_lhs		= name.contains("min")
									  ? (signed_compare ? lhs->getValue().slt(rhs->getValue())
														: lhs->getValue().ult(rhs->getValue()))
									  : (signed_compare ? lhs->getValue().sgt(rhs->getValue())
														: lhs->getValue().ugt(rhs->getValue()));
			result				= choose_lhs ? lhs : rhs;
			return true;
		}
		// LLVM knows how to fold additional pure intrinsics using target/APFloat
		// semantics. Do not accept a non-folded call as a guessed result.
		result = llvm::ConstantFoldInstOperands(&call, args, dl_);
		return safe_constant(result);
	}

	bool evaluate_call(const llvm::CallBase &call, llvm::Constant *&result,
					   const Frame *frame = nullptr, EvalState *state = nullptr,
					   unsigned depth = 1) const {
		if (call.isInlineAsm() || call.getFunctionType()->isVarArg()) {
			return false;
		}
		const llvm::Function *callee = call.getCalledFunction();
		if (!callee) {
			return false;
		}
		if (!callee->isIntrinsic() && call.getCallingConv() != llvm::CallingConv::C &&
			call.getCallingConv() != llvm::CallingConv::Fast) {
			return false;
		}
		std::vector<EvalArg> args;
		args.reserve(call.arg_size());
		for (const llvm::Use &operand : call.args()) {
			auto argument = operand_argument(operand.get(), frame);
			if (!argument) {
				return false;
			}
			args.push_back(*argument);
		}
		if (callee->isIntrinsic()) {
			std::vector<llvm::Constant *> constants;
			constants.reserve(args.size());
			for (const EvalArg &arg : args) {
				if (!arg.value) {
					return false;
				}
				constants.push_back(arg.value);
			}
			return evaluate_intrinsic(call, constants, result) &&
				   (result == nullptr || safe_constant(result));
		}
		if (callee->isDeclaration() || callee->getFunctionType()->isVarArg() || callee->empty()) {
			return false;
		}
		if (state) {
			return execute(*callee, args, *state, depth, result);
		}
		EvalState fresh_state;
		fresh_state.limits = limits_;
		return execute(*callee, args, fresh_state, 1, result);
	}

	bool evaluate_instruction(llvm::Instruction &instruction, Frame &frame, EvalState &state,
							  unsigned depth) const {
		if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(&instruction)) {
			return allocate(*alloca, frame, state);
		}
		if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&instruction)) {
			return evaluate_gep(*gep, frame, state);
		}
		if (auto *bitcast = llvm::dyn_cast<llvm::BitCastInst>(&instruction)) {
			if (bitcast->getType()->isPointerTy()) {
				auto address = address_of(bitcast->getOperand(0), frame);
				if (!address) {
					return false;
				}
				frame.addresses[&instruction] = *address;
				return true;
			}
		}
		if (auto *addrspace = llvm::dyn_cast<llvm::AddrSpaceCastInst>(&instruction)) {
			auto address = address_of(addrspace->getOperand(0), frame);
			if (!address) {
				return false;
			}
			frame.addresses[&instruction] = *address;
			return true;
		}
		if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
			if (load->isVolatile() || load->isAtomic()) {
				return false;
			}
			auto address = address_of(load->getPointerOperand(), frame);
			if (!address) {
				return false;
			}
			auto *value = load_from_memory(*address, load->getType(), state);
			if (!value) {
				return false;
			}
			frame.values[&instruction] = value;
			return true;
		}
		if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
			if (store->isVolatile() || store->isAtomic()) {
				return false;
			}
			auto  address = address_of(store->getPointerOperand(), frame);
			auto *value	  = operand_constant(store->getValueOperand(), frame);
			return address && value && store_to_memory(*address, value, state);
		}
		if (auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
			if (call->getType()->isVoidTy()) {
				llvm::Constant *ignored = nullptr;
				return evaluate_call(*call, ignored, &frame, &state, depth + 1) &&
					   call->getCalledFunction() && call->getCalledFunction()->isIntrinsic();
			}
			llvm::Constant *value = nullptr;
			if (!evaluate_call(*call, value, &frame, &state, depth + 1) || !value) {
				return false;
			}
			frame.values[&instruction] = value;
			return true;
		}
		if (instruction.isTerminator()) {
			return true;
		}

		std::vector<llvm::Constant *> operands;
		operands.reserve(instruction.getNumOperands());
		for (llvm::Use &operand : instruction.operands()) {
			auto *value = operand_constant(operand.get(), frame);
			if (!value) {
				return false;
			}
			operands.push_back(value);
		}
		auto *result = llvm::ConstantFoldInstOperands(&instruction, operands, dl_);
		if (!safe_constant(result)) {
			return false;
		}
		frame.values[&instruction] = result;
		return true;
	}

	bool execute(const llvm::Function &function, llvm::ArrayRef<EvalArg> args, EvalState &state,
				 unsigned depth, llvm::Constant *&result) const {
		if (depth > state.limits.recursion_depth || args.size() != function.arg_size()) {
			return false;
		}

		Frame frame;
		if (state.next_frame_id == 0) {
			return false;
		}
		frame.frame_id = state.next_frame_id++;
		size_t arg_index = 0;
		for (const llvm::Argument &argument : function.args()) {
			const EvalArg &argument_value = args[arg_index++];
			if (argument.getType()->isPointerTy()) {
				auto address = argument_value.address;
				if (!address) {
					return false;
				}
				frame.addresses[&argument] = *address;
			} else {
				if (!argument_value.value) {
					return false;
				}
				frame.values[&argument] = argument_value.value;
			}
		}

		const llvm::BasicBlock *current		= &function.getEntryBlock();
		const llvm::BasicBlock *predecessor = nullptr;
		while (current) {
			if (!consume_step(state)) {
				return false;
			}
			for (const llvm::Instruction &instruction : *current) {
				if (auto *phi = llvm::dyn_cast<llvm::PHINode>(&instruction)) {
					const llvm::Value *incoming = nullptr;
					if (!predecessor) {
						incoming = phi->getIncomingValue(0);
					} else {
						int index = phi->getBasicBlockIndex(predecessor);
						if (index < 0) {
							return false;
						}
						incoming = phi->getIncomingValue(static_cast<unsigned>(index));
					}
					auto *value = operand_constant(incoming, frame);
					if (!value) {
						return false;
					}
					frame.values[&instruction] = value;
				}
			}

			bool			  transferred = false;
			llvm::BasicBlock *next_block  = nullptr;
			for (llvm::Instruction &instruction : *const_cast<llvm::BasicBlock *>(current)) {
				if (!consume_step(state)) {
					return false;
				}
				if (llvm::isa<llvm::PHINode>(&instruction)) {
					continue;
				}
				if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(&instruction)) {
					result = ret->getReturnValue() ? operand_constant(ret->getReturnValue(), frame)
												   : nullptr;
					return !ret->getReturnValue() || result != nullptr;
				}
				if (auto *branch = llvm::dyn_cast<llvm::UncondBrInst>(&instruction)) {
					next_block = branch->getSuccessor(0);
					predecessor = current;
					current		= next_block;
					transferred = true;
					break;
				}
				if (auto *branch = llvm::dyn_cast<llvm::CondBrInst>(&instruction)) {
					auto *condition = llvm::dyn_cast<llvm::ConstantInt>(
						operand_constant(branch->getCondition(), frame));
					if (!condition) {
						return false;
					}
					next_block = branch->getSuccessor(condition->isZero() ? 1 : 0);
					predecessor = current;
					current		= next_block;
					transferred = true;
					break;
				}
				if (auto *switch_inst = llvm::dyn_cast<llvm::SwitchInst>(&instruction)) {
					auto *condition = llvm::dyn_cast<llvm::ConstantInt>(
						operand_constant(switch_inst->getCondition(), frame));
					if (!condition) {
						return false;
					}
					next_block = switch_inst->getDefaultDest();
					for (auto case_it : switch_inst->cases()) {
						if (case_it.getCaseValue()->getValue() == condition->getValue()) {
							next_block = case_it.getCaseSuccessor();
							break;
						}
					}
					predecessor = current;
					current		= next_block;
					transferred = true;
					break;
				}
				if (llvm::isa<llvm::UnreachableInst>(&instruction) ||
					llvm::isa<llvm::ResumeInst>(&instruction)) {
					return false;
				}
				if (!evaluate_instruction(instruction, frame, state, depth)) {
					return false;
				}
			}
			if (!transferred) {
				return false;
			}
		}
		return false;
	}
};

} // namespace

bool run_constant_evaluator(llvm::Module &module, ConstEvalLimits limits) {
	return Evaluator(module, limits).run();
}

} // namespace stellac
