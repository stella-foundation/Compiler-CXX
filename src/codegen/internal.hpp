#pragma once

#include "codegen.hpp"
#include "intlit.hpp"
#include "llvm/ADT/APInt.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "log.hpp"
#include "mangle.hpp"
#include <cctype>
#include <filesystem>
#include <limits>
#include <set>
#include <string_view>

namespace stellac {

namespace {

static inline bool contains_layout_intrinsic(const Expr &e) {
	if (e.kind == ExprKind::Sizeof || e.kind == ExprKind::Alignof || e.kind == ExprKind::Bitsof ||
		e.kind == ExprKind::Offsetof) {
		return true;
	}
	if ((e.lhs && contains_layout_intrinsic(*e.lhs)) ||
		(e.rhs && contains_layout_intrinsic(*e.rhs)) ||
		(e.condition && contains_layout_intrinsic(*e.condition)) ||
		(e.then_expr && contains_layout_intrinsic(*e.then_expr)) ||
		(e.else_expr && contains_layout_intrinsic(*e.else_expr))) {
		return true;
	}
	for (const auto &arg : e.args) {
		if (arg && contains_layout_intrinsic(*arg)) {
			return true;
		}
	}
	return false;
}

static inline bool fits_target_usize(uint64_t value, int pointer_width) {
	return pointer_width <= 0 || pointer_width >= 64 ||
		value <= ((uint64_t{1} << pointer_width) - 1);
}

static inline bool bitsof_type(StType *type, int pointer_width, const llvm::DataLayout &dl,
								   llvm::Type *llvm_type, uint64_t &bits) {
	type = underlying_type(type);
	if (!type) {
		return false;
	}
	switch (type->kind) {
	case TypeKind::Bool:
		bits = 1;
		return true;
	case TypeKind::UInt:
	case TypeKind::SInt:
	case TypeKind::Float:
		bits = type->width > 0 ? static_cast<uint64_t>(type->width)
								: static_cast<uint64_t>(pointer_width);
		return pointer_width > 0;
	case TypeKind::Pointer:
	case TypeKind::Function:
		bits = static_cast<uint64_t>(pointer_width);
		return pointer_width > 0;
	case TypeKind::Enum:
		return bitsof_type(type->base, pointer_width, dl, llvm_type, bits);
	default: {
		if (!llvm_type) {
			return false;
		}
		const uint64_t size = dl.getTypeAllocSize(llvm_type);
		if (size > std::numeric_limits<uint64_t>::max() / 8) {
			return false;
		}
		bits = size * 8;
		return true;
	}
	}
}

static inline bool flatten_member_path(const Expr &e, std::vector<std::string> &out) {
	if (e.kind == ExprKind::Ident) {
		out = e.path;
		return !out.empty();
	}
	if (e.kind == ExprKind::Member && e.lhs && flatten_member_path(*e.lhs, out)) {
		out.push_back(e.member);
		return true;
	}
	return false;
}

static inline StType *layout_operand_type(const Expr *operand) {
	if (!operand) {
		return nullptr;
	}
	// A type-valued expression carries the queried type separately from its
	// expression type (`type`). Ordinary expressions use their expression type.
	if (operand->type_value && operand->ty && underlying_type(operand->ty)->kind == TypeKind::Type) {
		return operand->type_value;
	}
	return operand->ty;
}

static inline bool is_fixed_register_name(std::string_view name) {
	static constexpr std::string_view regs[] = {
		"al",	"cl",	"dl",	"bl",	"ah",	"ch",	"dh",	"bh",	"ax",	"cx",	"dx",
		"bx",	"sp",	"bp",	"si",	"di",	"eax",	"ecx",	"edx",	"ebx",	"esp",	"ebp",
		"esi",	"edi",	"rax",	"rcx",	"rdx",	"rbx",	"rsp",	"rbp",	"rsi",	"rdi",	"r8",
		"r9",	"r10",	"r11",	"r12",	"r13",	"r14",	"r15",	"r8b",	"r9b",	"r10b", "r11b",
		"r12b", "r13b", "r14b", "r15b", "r8w",	"r9w",	"r10w", "r11w", "r12w", "r13w", "r14w",
		"r15w", "r8d",	"r9d",	"r10d", "r11d", "r12d", "r13d", "r14d", "r15d",
	};
	for (std::string_view reg : regs) {
		if (name == reg) {
			return true;
		}
	}
	return false;
}

static inline std::string normalize_asm_output_constraint(const std::string &constraint) {
	if (constraint.size() > 1 && constraint[0] == '=' && constraint[1] != '{' &&
		is_fixed_register_name(std::string_view(constraint).substr(1))) {
		return "={" + constraint.substr(1) + "}";
	}
	return constraint;
}

static inline bool is_asm_memory_constraint(std::string_view constraint) {
	size_t i = 0;
	if (i < constraint.size() && (constraint[i] == '=' || constraint[i] == '+')) {
		++i;
	}
	if (i < constraint.size() && constraint[i] == '*') {
		++i;
	}
	return i < constraint.size() && constraint[i] == 'm';
}

static inline bool is_asm_read_write_constraint(std::string_view constraint) {
	return !constraint.empty() && constraint.front() == '+';
}

static inline std::string normalize_asm_memory_output_constraint(const std::string &constraint) {
	size_t i = 0;
	if (i < constraint.size() && (constraint[i] == '=' || constraint[i] == '+')) {
		++i;
	}
	if (i < constraint.size() && constraint[i] == '*') {
		++i;
	}
	return "=*" + constraint.substr(i);
}

static inline std::string normalize_asm_memory_input_constraint(const std::string &constraint) {
	size_t i = 0;
	if (i < constraint.size() && constraint[i] == '*') {
		return constraint;
	}
	return "*" + constraint;
}

static inline std::string normalize_asm_template(const std::string &asm_code) {
	std::string normalized;
	normalized.reserve(asm_code.size());
	for (size_t i = 0; i < asm_code.size(); ++i) {
		if (asm_code[i] == '%' && i + 1 < asm_code.size() &&
			std::isdigit(static_cast<unsigned char>(asm_code[i + 1]))) {
			normalized.push_back('$');
			++i;
			while (i < asm_code.size() && std::isdigit(static_cast<unsigned char>(asm_code[i]))) {
				normalized.push_back(asm_code[i]);
				++i;
			}
			--i;
		} else {
			normalized.push_back(asm_code[i]);
		}
	}
	return normalized;
}

static inline const SourceLoc *top_decl_loc(const TopDecl &td) {
	switch (td.kind) {
	case TopKind::Fn:
		return td.fn ? &td.fn->loc : nullptr;
	case TopKind::Mod:
		return td.mod_decl ? &td.mod_decl->loc : nullptr;
	case TopKind::Struct:
		return td.st ? &td.st->loc : nullptr;
	case TopKind::Union:
		return td.un ? &td.un->loc : nullptr;
	case TopKind::TypeBinding:
		return td.tb ? &td.tb->loc : nullptr;
	case TopKind::Var:
	case TopKind::Const:
		return td.glob ? &td.glob->loc : nullptr;
	case TopKind::Import:
		return td.imp ? &td.imp->loc : nullptr;
	case TopKind::Enum:
		return td.en ? &td.en->loc : nullptr;
	case TopKind::Conditional:
		return td.conditional ? &td.conditional->loc : nullptr;
	case TopKind::Trait:
		return td.trait_decl ? &td.trait_decl->loc : nullptr;
	case TopKind::Impl:
		return td.impl_decl ? &td.impl_decl->loc : nullptr;
	default:
		return nullptr;
	}
}

static inline bool same_source_file(const std::string &lhs, const std::string &rhs) {
	if (lhs == rhs) {
		return true;
	}
	std::error_code		  lhs_ec;
	std::error_code		  rhs_ec;
	std::filesystem::path left	= std::filesystem::weakly_canonical(lhs, lhs_ec);
	std::filesystem::path right = std::filesystem::weakly_canonical(rhs, rhs_ec);
	if (lhs_ec) {
		left = std::filesystem::path(lhs).lexically_normal();
	}
	if (rhs_ec) {
		right = std::filesystem::path(rhs).lexically_normal();
	}
	return left == right;
}

static inline bool belongs_to_translation_unit(const Module &root, const TopDecl &td) {
	const SourceLoc *loc = top_decl_loc(td);
	return loc && !root.fpath.empty() && same_source_file(root.fpath, loc->file);
}

static inline bool is_signed_integer(StType *t) {
	t = underlying_type(t);
	if (!t) {
		return false;
	}
	if (t->kind == TypeKind::SInt) {
		return true;
	}
	return t->kind == TypeKind::Enum && is_signed_integer(t->base);
}

static inline bool is_integer_type(StType *t) {
	t = underlying_type(t);
	return t &&
		   (t->kind == TypeKind::Bool || t->kind == TypeKind::UInt || t->kind == TypeKind::SInt ||
			t->kind == TypeKind::Enum);
}

// After emitting a body that may end in a control-transfer statement
// (return/break/continue), the IRBuilder is parked on a fresh synthetic
// "dead" block (ret.dead / after.break / after.continue) that no
// predecessor branches into. Such a block is unreachable; if a caller
// would otherwise append a fallback `br X` to it, that branch is
// spurious. Returns true and erases the dead block when this is the case.
// Caller must reposition the builder (typically onto the merge/end BB)
// before issuing any further code, since the block the builder was on is
// gone after this call returns true.
inline bool absorb_dead_fallthrough(llvm::IRBuilder<> &B, llvm::BasicBlock *next_bb,
									llvm::BasicBlock *&sink) {
	llvm::BasicBlock *cur = B.GetInsertBlock();
	if (!cur || cur->hasNPredecessorsOrMore(1) || !cur->empty()) {
		return false;
	}
	B.SetInsertPoint(next_bb);
	if (cur == sink) {
		sink = nullptr; // we're erasing the shared dead-pool block
	}
	cur->eraseFromParent();
	return true;
}

// Coerce an LLVM value to a target scalar type, truncating/zero- or
// sign-extending integers, extending/truncating floats, and converting
// between int/float/pointer as needed. src_st/dst_st carry signedness so
// widening picks zext vs sext and int<->float picks signed vs unsigned.
inline llvm::Value *coerce(llvm::IRBuilder<> &B, llvm::Value *v, llvm::Type *dst, StType *src_st,
						   StType *dst_st) {
	llvm::Type *src = v->getType();
	if (src == dst) {
		return v;
	}
	StType *src_type = underlying_type(src_st);
	StType *dst_type = underlying_type(dst_st);
	if (dst_type && dst_type->kind == TypeKind::Slice && src_type &&
		src_type->kind == TypeKind::Pointer && src_type->base &&
		src_type->base->kind == TypeKind::Array && dst->isStructTy()) {
		auto		*slice_st = llvm::cast<llvm::StructType>(dst);
		llvm::Value *len_val =
			llvm::ConstantInt::get(slice_st->getElementType(1), src_type->base->array_len);
		llvm::Value *res = llvm::UndefValue::get(slice_st);
		res				 = B.CreateInsertValue(res, v, 0);
		res				 = B.CreateInsertValue(res, len_val, 1);
		return res;
	}
	if (dst_type && dst_type->kind == TypeKind::Bool) {
		if (src->isIntegerTy()) {
			return B.CreateICmpNE(v, llvm::ConstantInt::get(src, 0));
		}
		if (src->isFloatingPointTy()) {
			// An unordered comparison makes NaN truthy, matching nonzero
			// conversion semantics.
			return B.CreateFCmpUNE(v, llvm::ConstantFP::get(src, 0.0));
		}
		if (src->isPointerTy()) {
			return B.CreateICmpNE(v, llvm::ConstantPointerNull::get(
				llvm::cast<llvm::PointerType>(src)));
		}
	}
	if (src->isIntegerTy() && dst->isIntegerTy()) {
		unsigned sb = src->getIntegerBitWidth();
		unsigned db = dst->getIntegerBitWidth();
		if (sb == db) {
			return v;
		}
		if (sb < db) {
			bool z = !is_signed_integer(src_type);
			return z ? (llvm::Value *)B.CreateZExt(v, dst) : (llvm::Value *)B.CreateSExt(v, dst);
		}
		return B.CreateTrunc(v, dst);
	}
	if (src->isFloatingPointTy() && dst->isFloatingPointTy()) {
		unsigned sb = src->getFPMantissaWidth();
		unsigned db = dst->getFPMantissaWidth();
		if (sb == db) {
			return v;
		}
		if (sb < db) {
			return B.CreateFPExt(v, dst);
		}
		return B.CreateFPTrunc(v, dst);
	}
	if (src->isIntegerTy() && dst->isFloatingPointTy()) {
		return is_signed_integer(src_type) ? B.CreateSIToFP(v, dst) : B.CreateUIToFP(v, dst);
	}
	if (src->isFloatingPointTy() && dst->isIntegerTy()) {
		return is_signed_integer(dst_type) ? B.CreateFPToSI(v, dst) : B.CreateFPToUI(v, dst);
	}
	if (src->isPointerTy() && dst->isIntegerTy()) {
		return B.CreatePtrToInt(v, dst);
	}
	if (src->isIntegerTy() && dst->isPointerTy()) {
		return B.CreateIntToPtr(v, dst);
	}
	if (src->isPointerTy() && dst->isPointerTy()) {
		return B.CreateBitCast(v, dst);
	}
	return v;
}

} // namespace

static inline bool is_volatile_access(const Expr &e) {
	auto pointer_is_volatile = [](const Expr *expr) {
		if (!expr || !expr->ty) {
			return false;
		}
		StType *ty = underlying_type(expr->ty);
		return ty && (ty->kind == TypeKind::Pointer || ty->kind == TypeKind::Slice) &&
			   ty->is_volatile;
	};

	switch (e.kind) {
	case ExprKind::Deref:
		return pointer_is_volatile(e.lhs.get()) || (e.lhs && is_volatile_access(*e.lhs));
	case ExprKind::Index:
	case ExprKind::Member:
		return pointer_is_volatile(e.lhs.get()) || (e.lhs && is_volatile_access(*e.lhs));
	default:
		return false;
	}
}

} // namespace stellac
