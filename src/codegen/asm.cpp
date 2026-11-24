#include "internal.hpp"

namespace stellac {

void Codegen::emit_asm(Stmt &s, FnDecl &ctx, Module &m) {
	auto &B = *builder_;
	(void)ctx;
	// Build operand list: outputs + inputs. Determine value types.
	std::vector<std::string>   constraints;
	std::vector<llvm::Type *>  operandTys;
	std::vector<llvm::Value *> operands;
	struct ValueOutput {
		size_t		 index;
		llvm::Value *addr;
		bool		 is_volatile;
	};
	std::vector<ValueOutput>   value_outputs;
	std::vector<std::string>   output_constraints;
	std::vector<std::string>   input_constraints;
	std::vector<llvm::Type *>  output_operand_tys;
	std::vector<llvm::Value *> output_operands;
	std::vector<llvm::Type *>  output_element_types;
	std::vector<llvm::Type *>  input_operand_tys;
	std::vector<llvm::Value *> input_operands;
	std::vector<llvm::Type *>  input_element_types;
	llvm::Type				  *retTy = l_.llvm_void();
	std::vector<llvm::Type *>  retTys;

	for (auto &op : s.asm_outputs) {
		if (!op.operand) {
			log::report(log::Level::Error, s.loc, "inline assembly output has no operand",
						"E_CODEGEN_FAILURE", "backend");
			return;
		}
		const bool is_memory	 = is_asm_memory_constraint(op.constraint);
		const bool is_read_write = is_asm_read_write_constraint(op.constraint);
		// Memory output ("=m") takes an lvalue address directly; register
		// outputs are returned as values and stored back afterwards.
		if (is_memory) {
			output_constraints.push_back(normalize_asm_memory_output_constraint(op.constraint));
			llvm::Value *addr = emit_lvalue(*op.operand, ctx, m);
			if (!addr) {
				log::report(log::Level::Error, op.operand->loc,
							"cannot lower inline assembly memory output operand",
						"E_CODEGEN_FAILURE", "backend");
				return;
			}
			output_operand_tys.push_back(l_.llvm_ptr());
			output_operands.push_back(addr);
			output_element_types.push_back(op.operand && op.operand->ty ? map_type(op.operand->ty)
																		: nullptr);
			if (is_read_write) {
				// LLVM spells a read-write memory output as a memory output
				// plus a separate memory input using the same address.
				input_constraints.push_back("*m");
				input_operand_tys.push_back(l_.llvm_ptr());
				input_operands.push_back(addr);
				input_element_types.push_back(
					op.operand && op.operand->ty ? map_type(op.operand->ty) : nullptr);
			}
		} else {
			std::string output_constraint = op.constraint;
			if (is_read_write) {
				// LLVM represents a read-write register as an output tied to
				// an input, rather than accepting GCC's '+' shorthand here.
				output_constraint.front() = '=';
			}
			output_constraints.push_back(normalize_asm_output_constraint(output_constraint));
			StType *t = op.operand ? op.operand->ty : t_.void_ty();
			if (!t) {
				t = t_.void_ty();
			}
			llvm::Type *vt = map_type(t);
			retTys.push_back(vt);
			if (is_asm_read_write_constraint(op.constraint)) {
				// LLVM models a read-write register output as a return value
				// plus an input argument for the old value.
				llvm::Value *old_value = emit_expr(*op.operand, ctx, m);
				if (!old_value) {
					log::report(log::Level::Error, op.operand->loc,
								"cannot lower inline assembly read-write operand",
							"E_CODEGEN_FAILURE", "backend");
					return;
				}
				input_constraints.push_back(std::to_string(output_constraints.size() - 1));
				input_operand_tys.push_back(old_value->getType());
				input_operands.push_back(old_value);
				input_element_types.push_back(nullptr);
			}
			llvm::Value *addr = emit_lvalue(*op.operand, ctx, m);
			if (!addr) {
				log::report(log::Level::Error, op.operand->loc,
							"cannot lower inline assembly output operand",
							"E_CODEGEN_FAILURE", "backend");
				return;
			}
			value_outputs.push_back(
				{value_outputs.size(), addr, is_volatile_access(*op.operand)});
		}
	}
	for (auto &op : s.asm_inputs) {
		if (!op.operand) {
			log::report(log::Level::Error, s.loc, "inline assembly input has no operand",
						"E_CODEGEN_FAILURE", "backend");
			return;
		}
		const bool is_memory = is_asm_memory_constraint(op.constraint);
		input_constraints.push_back(is_memory ? normalize_asm_memory_input_constraint(op.constraint)
											  : op.constraint);
		llvm::Value *v =
			is_memory ? emit_lvalue(*op.operand, ctx, m) : emit_expr(*op.operand, ctx, m);
		if (!v) {
			log::report(log::Level::Error, op.operand->loc,
						is_memory ? "cannot lower inline assembly memory input operand"
								  : "cannot lower inline assembly input operand",
						"E_CODEGEN_FAILURE", "backend");
			return;
		}
		input_operand_tys.push_back(v->getType());
		input_operands.push_back(v);
		// Opaque pointers need the source-level pointee type on indirect
		// inline-asm operands so LLVM can verify the memory constraint.
		input_element_types.push_back(
			is_memory && op.operand && op.operand->ty ? map_type(op.operand->ty) : nullptr);
	}
	constraints.insert(constraints.end(), output_constraints.begin(), output_constraints.end());
	constraints.insert(constraints.end(), input_constraints.begin(), input_constraints.end());
	operandTys.insert(operandTys.end(), output_operand_tys.begin(), output_operand_tys.end());
	operandTys.insert(operandTys.end(), input_operand_tys.begin(), input_operand_tys.end());
	operands.insert(operands.end(), output_operands.begin(), output_operands.end());
	operands.insert(operands.end(), input_operands.begin(), input_operands.end());
	std::vector<llvm::Type *> element_types;
	element_types.reserve(output_element_types.size() + input_element_types.size());
	element_types.insert(element_types.end(), output_element_types.begin(),
						 output_element_types.end());
	element_types.insert(element_types.end(), input_element_types.begin(),
						 input_element_types.end());
	// clobbers
	for (auto &c : s.asm_clobbers) {
		if (c == "memory") {
			constraints.push_back("~{memory}");
		} else {
			constraints.push_back("~{" + c + "}");
		}
	}
	// retTy
	if (!retTys.empty()) {
		if (retTys.size() == 1) {
			retTy = retTys[0];
		} else {
			std::vector<llvm::Type *> ts;
			for (auto *t : retTys) {
				ts.push_back(t);
			}
			retTy = llvm::StructType::get(l_.ctx(), ts);
		}
	}
	auto		fnTy = llvm::FunctionType::get(retTy, operandTys, false);
	std::string cstr;
	for (size_t i = 0; i < constraints.size(); ++i) {
		if (i) {
			cstr += ",";
		}
		cstr += constraints[i];
	}
	auto *ia =
		llvm::InlineAsm::get(fnTy, normalize_asm_template(s.asm_code), cstr, s.asm_volatile, false);
	llvm::Value *res = B.CreateCall(ia, operands);
	if (auto *call = llvm::dyn_cast<llvm::CallBase>(res)) {
		for (size_t i = 0; i < element_types.size(); ++i) {
			if (element_types[i]) {
				call->addParamAttr(
					(unsigned)i,
					llvm::Attribute::get(l_.ctx(), llvm::Attribute::ElementType, element_types[i]));
			}
		}
	}
	if (value_outputs.empty()) {
		return;
	}
	if (value_outputs.size() == 1) {
		auto *store = B.CreateStore(res, value_outputs[0].addr);
		store->setVolatile(value_outputs[0].is_volatile);
	} else {
		for (size_t i = 0; i < value_outputs.size(); ++i) {
			llvm::Value *ev	   = B.CreateExtractValue(res, {(unsigned)i});
			auto		*store = B.CreateStore(ev, value_outputs[i].addr);
			store->setVolatile(value_outputs[i].is_volatile);
		}
	}
}

} // namespace stellac
