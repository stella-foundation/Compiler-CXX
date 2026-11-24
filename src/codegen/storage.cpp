#include "internal.hpp"

namespace stellac {

llvm::Value *Codegen::alloca_in_entry(llvm::Type *ty, const char *name) {
	llvm::Function	 *fn	= builder_->GetInsertBlock()->getParent();
	llvm::BasicBlock &entry = fn->getEntryBlock();
	llvm::IRBuilder<> tmp(l_.ctx());
	tmp.SetInsertPoint(&entry, entry.getFirstInsertionPt());
	auto *alloca = tmp.CreateAlloca(ty, nullptr, name);
	alloca->setAlignment(l_.module().getDataLayout().getABITypeAlign(ty));
	return alloca;
}

llvm::Value *Codegen::alloca_in_entry_typed(StType *t, llvm::Type *lt, const char *name) {
	llvm::AllocaInst *a = llvm::cast<llvm::AllocaInst>(alloca_in_entry(lt, name));
	a->setAlignment(storage_align(t, lt));
	return a;
}

llvm::Align Codegen::storage_align(StType *t, llvm::Type *lt) {
	auto &dl = l_.module().getDataLayout();
	llvm::Align align = dl.getPrefTypeAlign(lt);
	StType *u = underlying_type(t);
	StType *inner = u;
	while (inner && inner->kind == TypeKind::Array) {
		inner = underlying_type(inner->base);
	}
	if (inner && (inner->kind == TypeKind::Struct || inner->kind == TypeKind::Union) &&
		inner->explicit_align > 0) {
		align = std::max(align, llvm::Align(inner->explicit_align));
	}
	// System V AMD64 ABI 3.2.2:
	// An array uses the same alignment as its elements, except that a local or
	// global array variable of at least 16 bytes has an alignment of at least 16 bytes.
	if ((u && u->kind == TypeKind::Array) || lt->isArrayTy()) {
		llvm::Triple triple(l_.module().getTargetTriple());
		if (triple.getArch() == llvm::Triple::x86_64) {
			uint64_t total_size = dl.getTypeAllocSize(lt);
			if (total_size >= 16) {
				align = std::max(align, llvm::Align(16));
			}
		}
	}
	return align;
}

llvm::Align Codegen::pointer_align(llvm::Value *val, StType *st, llvm::Type *lt) {
	if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(val)) {
		return alloca->getAlign();
	}
	if (auto *gv = llvm::dyn_cast<llvm::GlobalVariable>(val)) {
		if (gv->getAlign().has_value()) {
			return *gv->getAlign();
		}
	}
	StType *u = underlying_type(st);
	if (u && (u->kind == TypeKind::Struct || u->kind == TypeKind::Union ||
			  u->kind == TypeKind::Enum) && u->explicit_align > 0) {
		return llvm::Align(u->explicit_align);
	}
	return l_.module().getDataLayout().getABITypeAlign(lt);
}

llvm::Align Codegen::struct_align(StType *t) {
	t = underlying_type(t);
	if (t && (t->kind == TypeKind::Struct || t->kind == TypeKind::Union ||
			  t->kind == TypeKind::Enum) && t->explicit_align > 0) {
		return llvm::Align(t->explicit_align);
	}
	return llvm::Align();
}

llvm::Value *Codegen::load_value(llvm::Value *ptr, StType *ty, const SourceLoc &loc) {
	(void)loc;
	llvm::Type	   *lt	= map_type(ty);
	llvm::LoadInst *ld	= builder_->CreateLoad(lt, ptr);
	if (ty) {
		ld->setAlignment(pointer_align(ptr, ty, lt));
	}
	return ld;
}
void Codegen::store_value(llvm::Value *ptr, llvm::Value *val, StType *ty, StType *src_st,
						  const SourceLoc &loc) {
	(void)loc;
	StType		   *dst_st = underlying_type(ty);
	llvm::Type	   *lt	   = map_type(ty);
	llvm::Value	   *cv	   = (dst_st && dst_st->kind != TypeKind::Struct &&
							  dst_st->kind != TypeKind::Union && dst_st->kind != TypeKind::Array)
								 ? coerce(*builder_, val, lt, src_st, dst_st)
								 : val;
	llvm::StoreInst *st	   = builder_->CreateStore(cv, ptr);
	if (ty) {
		st->setAlignment(pointer_align(ptr, ty, lt));
	}
}

llvm::Function *Codegen::cur_fn(FnDecl &ctx) {
	// Fast path: while emitting the current function the llvm::Function* is
	// cached on cur_llvm_fn_ (set in emit_fn_body). This avoids recomputing
	// the mangled name and hashing fns_ on every If/For/Switch/Return/
	// Break/Continue statement.
	if (cur_llvm_fn_) {
		return cur_llvm_fn_;
	}
	std::string nm = fn_symbol_name(ctx);
	auto		it = fns_.find(nm);
	return it == fns_.end() ? nullptr : it->second;
}

Codegen::LocalVal *Codegen::find_local(const std::string &n) {
	for (int i = (int)local_stack_.size() - 1; i >= 0; --i) {
		auto it = local_stack_[i].find(n);
		if (it != local_stack_[i].end()) {
			return &it->second;
		}
	}
	return nullptr;
}

void Codegen::br_or_absorb(llvm::IRBuilder<> &B, llvm::BasicBlock *target) {
	auto *ib = B.GetInsertBlock();
	if (!ib || ib->hasTerminator()) {
		return; // parked after a terminator (e.g. return)
	}
	if (!absorb_dead_fallthrough(B, target, sink_block_)) {
		B.CreateBr(target);
	}
}

llvm::Value *Codegen::coerce_cond(llvm::IRBuilder<> &B, llvm::Value *c) {
	if (c->getType() != l_.llvm_int(1)) {
		c = B.CreateICmpNE(c, llvm::ConstantInt::get(c->getType(), 0));
	}
	return c;
}

llvm::Constant *Codegen::cstring_global(llvm::StringRef s, const std::string &nm) {
	// Identical string literals share one private global. This dedupes the
	// (sizeable) pool of per-call globals the previous version emitted -- two
	// calls with the same content used to produce two distinct [N x i8] arrays
	// and two distinct pointers.
	auto it = string_lits_.find(std::string(s));
	if (it != string_lits_.end()) {
		return it->second;
	}
	auto		   *arrTy = llvm::ArrayType::get(l_.llvm_int(8), s.size() + 1);
	llvm::Constant *arr	  = llvm::ConstantDataArray::getString(l_.ctx(), s, true);
	auto *gv = new llvm::GlobalVariable(l_.module(), arrTy, true, llvm::GlobalValue::PrivateLinkage,
										arr, nm);
	gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
	gv->setAlignment(llvm::Align(1));
	auto *gep = llvm::ConstantExpr::getInBoundsGetElementPtr(
		arrTy, gv,
		llvm::ArrayRef<llvm::Constant *>{llvm::ConstantInt::get(l_.llvm_int(32), 0),
										 llvm::ConstantInt::get(l_.llvm_int(32), 0)});
	string_lits_.emplace(std::string(s), gep);
	return gep;
}

llvm::Constant *Codegen::slice_string_global(llvm::StringRef s, const std::string &nm) {
	auto *sliceTy = llvm::cast<llvm::StructType>(map_type(t_.slice(t_.uint(8), false, false)));
	auto *lenConst = llvm::ConstantInt::get(l_.llvm_int(t_.pointer_width()), s.size());
	if (s.empty()) {
		return llvm::ConstantStruct::get(
			sliceTy, {llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(l_.llvm_ptr())),
					  lenConst});
	}
	auto it = slice_string_lits_.find(std::string(s));
	llvm::Constant *gep = nullptr;
	if (it != slice_string_lits_.end()) {
		gep = it->second;
	} else {
		auto		   *arrTy = llvm::ArrayType::get(l_.llvm_int(8), s.size());
		llvm::Constant *arr	  = llvm::ConstantDataArray::getString(l_.ctx(), s, false);
		auto *gv = new llvm::GlobalVariable(l_.module(), arrTy, true,
											llvm::GlobalValue::PrivateLinkage, arr, nm);
		gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
		gv->setAlignment(llvm::Align(1));
		gep = llvm::ConstantExpr::getInBoundsGetElementPtr(
			arrTy, gv,
			llvm::ArrayRef<llvm::Constant *>{llvm::ConstantInt::get(l_.llvm_int(32), 0),
											 llvm::ConstantInt::get(l_.llvm_int(32), 0)});
		slice_string_lits_.emplace(std::string(s), gep);
	}
	return llvm::ConstantStruct::get(sliceTy, {gep, lenConst});
}

} // namespace stellac
