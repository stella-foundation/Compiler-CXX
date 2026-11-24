#include "internal.hpp"

namespace stellac {

llvm::Type *Codegen::map_type(StType *t) {
	if (!t) {
		return l_.llvm_void();
	}
	t		= underlying_type(t);
	auto it = type_cache_.find(t);
	if (it != type_cache_.end()) {
		return it->second;
	}
	llvm::Type *r = nullptr;
	switch (t->kind) {
	case TypeKind::Void:
		r = l_.llvm_void();
		break;
	case TypeKind::Bool:
		r = l_.llvm_bool();
		break;
	case TypeKind::UInt:
	case TypeKind::SInt:
		r = l_.llvm_int(t->width > 0 ? t->width : t_.pointer_width());
		break;
	case TypeKind::Float:
		r = l_.llvm_float(t->width);
		break;
	case TypeKind::Pointer:
		r = l_.llvm_ptr();
		break;
	case TypeKind::Array:
		r = llvm::ArrayType::get(map_type(t->base), t->array_len);
		break;
	case TypeKind::Slice:
		r = llvm::StructType::get(l_.ctx(), {l_.llvm_ptr(), l_.llvm_int(t_.pointer_width())});
		break;
	case TypeKind::Struct:
		r = map_named_struct(t);
		break;
	case TypeKind::Union:
		r = map_named_union(t);
		break;
	case TypeKind::Enum:
		r = t->is_tagged_enum ? map_tagged_enum(t) : map_type(t->base);
		break;
	case TypeKind::Function:
		r = l_.llvm_ptr();
		break;
	case TypeKind::VaList:
		r = va_list_llvm_type();
		break;
	default:
		r = l_.llvm_void();
		break;
	}
	type_cache_[t] = r;
	return r;
}

bool Codegen::target_supports_va_lists() const {
	llvm::Triple triple(l_.module().getTargetTriple());
	return triple.getOS() == llvm::Triple::Linux &&
		(triple.getArch() == llvm::Triple::x86_64 ||
		 triple.getArch() == llvm::Triple::aarch64 ||
		 triple.getArch() == llvm::Triple::riscv64);
}

llvm::StructType *Codegen::va_list_llvm_type() {
	llvm::Triple triple(l_.module().getTargetTriple());
	const char *name = "stella.va_list.unsupported";
	switch (triple.getArch()) {
	case llvm::Triple::x86_64: name = "stella.va_list.sysv_x86_64"; break;
	case llvm::Triple::aarch64: name = "stella.va_list.aapcs64"; break;
	case llvm::Triple::riscv64: name = "stella.va_list.riscv64"; break;
	default: break;
	}
	if (auto *existing = llvm::StructType::getTypeByName(l_.ctx(), name)) {
		return existing;
	}
	auto *ty = llvm::StructType::create(l_.ctx(), name);
	if (target_supports_va_lists()) {
		switch (triple.getArch()) {
		case llvm::Triple::x86_64:
			// gp_offset, fp_offset, overflow_arg_area, reg_save_area.
			ty->setBody({l_.llvm_int(32), l_.llvm_int(32), l_.llvm_ptr(), l_.llvm_ptr()}, false);
			break;
		case llvm::Triple::aarch64:
			// __stack, __gr_top, __vr_top, __gr_offs, __vr_offs.
			ty->setBody({l_.llvm_ptr(), l_.llvm_ptr(), l_.llvm_ptr(),
						 l_.llvm_int(32), l_.llvm_int(32)}, false);
			break;
		case llvm::Triple::riscv64:
			// A pointer into the contiguous register-save and stack argument area.
			ty->setBody({l_.llvm_ptr()}, false);
			break;
		default: break;
		}
	}
	return ty;
}

llvm::Value *Codegen::va_list_operand(Expr &operand, FnDecl &ctx, Module &m) {
	StType *type = underlying_type(operand.ty);
	if (type && type->kind == TypeKind::Pointer) {
		return emit_expr(operand, ctx, m);
	}
	if (operand.va_list_decl) {
		auto it = va_list_storage_.find(operand.va_list_decl);
		if (it != va_list_storage_.end()) {
			return it->second;
		}
	}
	return emit_lvalue(operand, ctx, m);
}

// Lower scalar va_arg explicitly. In particular, LLVM's x86 FastISel
// lowering can lose a non-alloca cursor across its generated control flow.
// Using ordinary IR also makes borrowed cursors work at every opt level.
llvm::Value *Codegen::emit_va_arg(llvm::Value *cursor, StType *type) {
	auto &B = *builder_;
	auto *value_type = map_type(type);
	auto &dl = l_.module().getDataLayout();
	const uint64_t size = dl.getTypeAllocSize(value_type);
	const uint64_t alignment = dl.getABITypeAlign(value_type).value();
	const bool floating = underlying_type(type)->kind == TypeKind::Float;
	llvm::Triple triple(l_.module().getTargetTriple());
	auto *layout = va_list_llvm_type();
	auto field = [&](unsigned index) { return B.CreateStructGEP(layout, cursor, index); };
	auto align_pointer = [&](llvm::Value *pointer, uint64_t align) -> llvm::Value * {
		if (align <= 8) { return pointer; }
		auto *integer = B.CreatePtrToInt(pointer, l_.llvm_int(64));
		integer = B.CreateAnd(B.CreateAdd(integer, B.getInt64(align - 1)),
							 B.getInt64(~(align - 1)));
		return B.CreateIntToPtr(integer, l_.llvm_ptr());
	};
	auto stack_argument = [&](unsigned index) -> llvm::Value * {
		auto *slot = field(index);
		auto *pointer = align_pointer(B.CreateLoad(l_.llvm_ptr(), slot), alignment);
		B.CreateStore(B.CreateGEP(B.getInt8Ty(), pointer, B.getInt64((size + 7) & ~7ULL)), slot);
		return pointer;
	};
	if (triple.getArch() == llvm::Triple::riscv64) {
		return B.CreateAlignedLoad(value_type, stack_argument(0), llvm::Align(1));
	}

	auto *function = B.GetInsertBlock()->getParent();
	auto *register_block = llvm::BasicBlock::Create(l_.ctx(), "va.register", function);
	auto *stack_block = llvm::BasicBlock::Create(l_.ctx(), "va.stack", function);
	auto *merge_block = llvm::BasicBlock::Create(l_.ctx(), "va.merge", function);
	llvm::Value *offset_slot;
	llvm::Value *offset;
	llvm::Value *next_offset;
	unsigned top_index;
	unsigned stack_index;
	if (triple.getArch() == llvm::Triple::aarch64) {
		offset_slot = field(floating ? 4 : 3);
		offset = B.CreateLoad(B.getInt32Ty(), offset_slot);
		if (!floating && alignment > 8) {
			offset = B.CreateAnd(B.CreateAdd(offset, B.getInt32(15)), B.getInt32(~15U));
		}
		next_offset = B.CreateAdd(offset, B.getInt32(floating ? 16 : ((size + 7) & ~7ULL)));
		B.CreateStore(next_offset, offset_slot);
		B.CreateCondBr(B.CreateICmpSLE(next_offset, B.getInt32(0)), register_block, stack_block);
		top_index = floating ? 2 : 1;
		stack_index = 0;
	} else {
		offset_slot = field(floating ? 1 : 0);
		offset = B.CreateLoad(B.getInt32Ty(), offset_slot);
		const unsigned step = floating ? 16 : ((size + 7) & ~7ULL);
		next_offset = B.CreateAdd(offset, B.getInt32(step));
		B.CreateCondBr(B.CreateICmpULE(offset, B.getInt32((floating ? 176 : 48) - step)),
					   register_block, stack_block);
		top_index = 3;
		stack_index = 2;
	}
	B.SetInsertPoint(register_block);
	auto *top = B.CreateLoad(l_.llvm_ptr(), field(top_index));
	auto *wide_offset = triple.getArch() == llvm::Triple::aarch64
		? B.CreateSExt(offset, B.getInt64Ty()) : B.CreateZExt(offset, B.getInt64Ty());
	auto *register_pointer = B.CreateGEP(B.getInt8Ty(), top, wide_offset);
	if (triple.getArch() == llvm::Triple::x86_64) { B.CreateStore(next_offset, offset_slot); }
	B.CreateBr(merge_block);
	B.SetInsertPoint(stack_block);
	auto *stack_pointer = stack_argument(stack_index);
	B.CreateBr(merge_block);
	B.SetInsertPoint(merge_block);
	auto *pointer = B.CreatePHI(l_.llvm_ptr(), 2, "va.argument");
	pointer->addIncoming(register_pointer, register_block);
	pointer->addIncoming(stack_pointer, stack_block);
	return B.CreateAlignedLoad(value_type, pointer, llvm::Align(1));
}

llvm::Type *Codegen::map_abi_type(StType *t) {
	StType *u = underlying_type(t);
	if (!u || u->kind != TypeKind::Union) {
		return map_type(t);
	}
	llvm::Type *storage = map_type(u);
	if (!storage->isStructTy() || u->fields.empty()) {
		return storage;
	}
	auto	   &dl		 = l_.module().getDataLayout();
	uint64_t	max_size = 0;
	llvm::Align max_align(1);
	StType	   *carrier		 = nullptr;
	uint64_t	carrier_size = 0;
	for (auto *f : u->fields) {
		llvm::Type *ft = map_type(f);
		uint64_t	sz = dl.getTypeAllocSize(ft);
		llvm::Align al = dl.getABITypeAlign(ft);
		if (sz > max_size) {
			max_size = sz;
		}
		if (al > max_align || (al == max_align && sz > carrier_size)) {
			max_align	 = al;
			carrier		 = underlying_type(f);
			carrier_size = sz;
		}
	}
	if (carrier && carrier->kind == TypeKind::Array && carrier->base && carrier_size == max_size) {
		StType *elem = underlying_type(carrier->base);
		if (elem && elem->kind == TypeKind::Float) {
			return llvm::VectorType::get(map_type(elem), carrier->array_len, false);
		}
	}
	return storage;
}

llvm::FunctionType *Codegen::map_fn_type(StType *t) {
	t = underlying_type(t);
	if (!t || t->kind != TypeKind::Function) {
		// Treat null/void as a nullary void-returning fn.
		return llvm::FunctionType::get(l_.llvm_void(), false);
	}
	std::vector<StType *> st_params;
	st_params.reserve(t->fn_params.size());
	for (auto *p : t->fn_params) {
		if (p && p->kind == TypeKind::Type) {
			continue;
		}
		st_params.push_back(p);
	}
	if (t->fn_abi == AbiKind::C) {
		return compute_c_abi_signature(st_params, t->fn_ret, t->fn_variadic).fn_type;
	}
	std::vector<llvm::Type *> params;
	params.reserve(st_params.size());
	for (auto *p : st_params) {
		params.push_back(map_type(p));
	}
	return llvm::FunctionType::get(map_type(t->fn_ret), params, t->fn_variadic);
}

llvm::Value *Codegen::to_abi_value(llvm::Value *v, llvm::Type *dst) {
	if (!v || v->getType() == dst) {
		return v;
	}
	if (auto *vec = llvm::dyn_cast<llvm::VectorType>(dst)) {
		llvm::Value *src = v;
		if (auto *st = llvm::dyn_cast<llvm::StructType>(src->getType())) {
			if (st->getNumElements() == 1) {
				src = builder_->CreateExtractValue(src, {0}, "abi.carrier");
			}
		}
		if (auto *arr = llvm::dyn_cast<llvm::ArrayType>(src->getType())) {
			if (arr->getNumElements() == vec->getElementCount().getFixedValue() &&
				arr->getElementType() == vec->getElementType()) {
				llvm::Value *out = llvm::UndefValue::get(dst);
				for (unsigned i = 0; i < arr->getNumElements(); ++i) {
					out = builder_->CreateInsertElement(
						out, builder_->CreateExtractValue(src, {i}, "abi.elem"), i, "abi.vec");
				}
				return out;
			}
		}
	}
	return builder_->CreateBitCast(v, dst);
}

llvm::Value *Codegen::from_abi_value(llvm::Value *v, llvm::Type *dst) {
	if (!v || v->getType() == dst) {
		return v;
	}
	auto *vec = llvm::dyn_cast<llvm::VectorType>(v->getType());
	auto *st  = llvm::dyn_cast<llvm::StructType>(dst);
	if (vec && st && st->getNumElements() == 1) {
		auto *arr = llvm::dyn_cast<llvm::ArrayType>(st->getElementType(0));
		if (arr && arr->getNumElements() == vec->getElementCount().getFixedValue() &&
			arr->getElementType() == vec->getElementType()) {
			llvm::Value *array = llvm::UndefValue::get(arr);
			for (unsigned i = 0; i < arr->getNumElements(); ++i) {
				auto *index = llvm::ConstantInt::get(l_.llvm_int(32), i);
				array = builder_->CreateInsertValue(
					array, builder_->CreateExtractElement(v, index, "abi.elem"), {i}, "abi.array");
			}
			llvm::Value *aggregate = llvm::UndefValue::get(st);
			return builder_->CreateInsertValue(aggregate, array, {0}, "abi.union");
		}
	}
	log::report(log::Level::Error, {}, "cannot convert ABI value to its Stella storage type",
				"E_CODEGEN_FAILURE", "backend");
	return llvm::UndefValue::get(dst);
}

llvm::Type *Codegen::map_named_struct(StType *t) {
	if (!t || t->kind != TypeKind::Struct) {
		return l_.llvm_void();
	}
	auto it = structs_.find(t->name);
	if (it != structs_.end()) {
		return it->second;
	}
	auto *st		  = llvm::StructType::create(l_.ctx(), t->name);
	structs_[t->name] = st;
	if (t->is_opaque) {
		return st;
	}
	std::vector<llvm::Type *> fields;
	fields.reserve(t->fields.size());
	for (auto *f : t->fields) {
		fields.push_back(map_type(f));
	}
	if (t->repr == ReprKind::Stella && fields.size() > 1) {
		const auto &dl = l_.module().getDataLayout();
		struct FieldOrder {
			size_t	 index;
			uint64_t align;
			uint64_t size;
		};
		std::vector<FieldOrder> order;
		order.reserve(fields.size());
		for (size_t i = 0; i < fields.size(); ++i) {
			order.push_back({i, dl.getABITypeAlign(fields[i]).value(),
							 dl.getTypeAllocSize(fields[i]).getFixedValue()});
		}
		std::stable_sort(order.begin(), order.end(), [](const FieldOrder &a, const FieldOrder &b) {
			if (a.align != b.align) {
				return a.align > b.align;
			}
			if (a.size != b.size) {
				return a.size > b.size;
			}
			return a.index < b.index;
		});
		std::vector<StType *>	 new_fields;
		std::vector<std::string> new_names;
		std::vector<bool>		 new_pub;
		std::vector<llvm::Type *> new_llvm_fields;
		new_fields.reserve(fields.size());
		new_names.reserve(fields.size());
		new_pub.reserve(fields.size());
		new_llvm_fields.reserve(fields.size());
		for (const auto &entry : order) {
			new_fields.push_back(t->fields[entry.index]);
			if (entry.index < t->field_names.size()) {
				new_names.push_back(std::move(t->field_names[entry.index]));
			}
			if (entry.index < t->field_is_pub.size()) {
				new_pub.push_back(t->field_is_pub[entry.index]);
			}
			new_llvm_fields.push_back(fields[entry.index]);
		}
		t->fields		= std::move(new_fields);
		t->field_names	= std::move(new_names);
		if (!new_pub.empty()) {
			t->field_is_pub = std::move(new_pub);
		}
		fields = std::move(new_llvm_fields);
	}
	st->setBody(fields);
	if (t->layout_size || t->layout_align) {
		const auto &data_layout = l_.module().getDataLayout();
		const uint64_t actual_size = data_layout.getTypeAllocSize(st).getFixedValue();
		const uint64_t actual_align = data_layout.getABITypeAlign(st).value();
		if (t->layout_size && *t->layout_size != actual_size) {
			log::report(log::Level::Error, t->layout_loc,
						"layout guarantee for struct '" + t->name + "' failed: expected size " +
							std::to_string(*t->layout_size) + ", target ABI size is " +
							std::to_string(actual_size), "E_LAYOUT_MISMATCH", "sema");
		}
		if (t->layout_align && *t->layout_align != actual_align) {
			log::report(log::Level::Error, t->layout_loc,
						"layout guarantee for struct '" + t->name + "' failed: expected alignment " +
							std::to_string(*t->layout_align) + ", target ABI alignment is " +
							std::to_string(actual_align), "E_LAYOUT_MISMATCH", "sema");
		}
	}
	return st;
}

llvm::Type *Codegen::map_named_union(StType *t) {
	if (!t || t->kind != TypeKind::Union) {
		return l_.llvm_void();
	}
	std::string key = "union:" + t->name;
	auto		it	= structs_.find(key);
	if (it != structs_.end()) {
		return it->second;
	}
	auto *ut	  = llvm::StructType::create(l_.ctx(), "U_" + t->name);
	structs_[key] = ut;
	if (t->fields.empty()) {
		ut->setBody({llvm::Type::getInt8Ty(l_.ctx())});
		return ut;
	}
	auto	   &dl		 = l_.module().getDataLayout();
	uint64_t	max_size = 0;
	llvm::Align max_align(1);
	StType	   *carrier_st	 = nullptr;
	llvm::Type *carrier_lt	 = nullptr;
	uint64_t	carrier_size = 0;
	for (auto *f : t->fields) {
		llvm::Type *ft	  = map_type(f);
		uint64_t	size  = dl.getTypeAllocSize(ft);
		llvm::Align align = dl.getABITypeAlign(ft);
		if (size > max_size) {
			max_size = size;
		}
		if (align > max_align || (align == max_align && size > carrier_size)) {
			max_align	 = align;
			carrier_size = size;
			carrier_st	 = f;
			carrier_lt	 = ft;
		}
	}
	const uint64_t alignment = max_align.value();
	uint64_t	  union_size = max_size;
	const uint64_t remainder = max_size % alignment;
	if (remainder != 0) {
		const uint64_t padding = alignment - remainder;
		if (max_size > std::numeric_limits<uint64_t>::max() - padding) {
			log::report(log::Level::Error, {},
						"layout of union '" + t->name + "' exceeds the maximum representable size");
			ut->setBody({llvm::Type::getInt8Ty(l_.ctx())});
			return ut;
		}
		union_size += padding;
	}
	const int pointer_width = t_.pointer_width();
	if (pointer_width > 0 && pointer_width < 64 &&
		union_size > ((uint64_t{1} << pointer_width) - 1)) {
		log::report(log::Level::Error, {},
					"layout of union '" + t->name + "' does not fit the target usize type");
		ut->setBody({llvm::Type::getInt8Ty(l_.ctx())});
		return ut;
	}
	uint64_t				  pad = union_size - carrier_size;
	std::vector<llvm::Type *> body{carrier_lt};
	if (pad) {
		body.push_back(llvm::ArrayType::get(l_.llvm_int(8), pad));
	}
	ut->setBody(body, false);
	(void)carrier_st;
	return ut;
}

llvm::Type *Codegen::map_tagged_enum(StType *t) {
	return get_tagged_enum_layout(t).outer_type;
}

const Codegen::TaggedEnumLayout &Codegen::get_tagged_enum_layout(StType *t) {
	auto cached = tagged_enum_layouts_.find(t);
	if (cached != tagged_enum_layouts_.end()) {
		return cached->second;
	}
	TaggedEnumLayout &layout = tagged_enum_layouts_[t];
	auto			 *st	 = llvm::StructType::create(l_.ctx(), "E_" + t->name);
	layout.outer_type		 = st;
	layout.tag_type			 = map_type(t->base);

	const auto &dl				  = l_.module().getDataLayout();
	uint64_t	max_payload_size  = 0;
	uint64_t	max_payload_align = 1;
	llvm::Type *carrier_lt		  = nullptr;
	uint64_t	carrier_size	  = 0;

	layout.variant_types.resize(t->enum_variants.size(), nullptr);
	layout.variant_field_map.resize(t->enum_variants.size());

	for (size_t vi = 0; vi < t->enum_variants.size(); ++vi) {
		const auto &variant = t->enum_variants[vi];
		if (variant.field_types.empty()) {
			continue;
		}
		std::vector<llvm::Type *> ftypes;
		ftypes.reserve(variant.field_types.size());
		uint64_t variant_explicit_align = 1;
		for (StType *ft : variant.field_types) {
			llvm::Type *lft = map_type(ft);
			ftypes.push_back(lft);
			StType *uft = underlying_type(ft);
			if (uft && uft->explicit_align > variant_explicit_align) {
				variant_explicit_align = uft->explicit_align;
			}
		}

		layout.variant_field_map[vi].resize(ftypes.size());
		std::vector<llvm::Type *> ordered_ftypes(ftypes.size());
		if (t->repr == ReprKind::Stella && ftypes.size() > 1) {
			struct FOrder {
				size_t	 idx;
				uint64_t align;
				uint64_t size;
			};
			std::vector<FOrder> order;
			order.reserve(ftypes.size());
			for (size_t fi = 0; fi < ftypes.size(); ++fi) {
				StType	*uft	= underlying_type(variant.field_types[fi]);
				uint64_t falign = dl.getABITypeAlign(ftypes[fi]).value();
				if (uft && uft->explicit_align > falign) falign = uft->explicit_align;
				order.push_back(
					{fi, falign, dl.getTypeAllocSize(ftypes[fi]).getFixedValue()});
			}
			std::stable_sort(order.begin(), order.end(), [](const FOrder &a, const FOrder &b) {
				if (a.align != b.align) return a.align > b.align;
				if (a.size != b.size) return a.size > b.size;
				return a.idx < b.idx;
			});
			for (size_t li = 0; li < order.size(); ++li) {
				ordered_ftypes[li]					   = ftypes[order[li].idx];
				layout.variant_field_map[vi][order[li].idx] = static_cast<unsigned>(li);
			}
		} else {
			for (size_t fi = 0; fi < ftypes.size(); ++fi) {
				ordered_ftypes[fi]				= ftypes[fi];
				layout.variant_field_map[vi][fi] = static_cast<unsigned>(fi);
			}
		}

		llvm::StructType *vst = llvm::StructType::get(l_.ctx(), ordered_ftypes, false);
		layout.variant_types[vi] = vst;
		uint64_t vsize			 = dl.getTypeAllocSize(vst).getFixedValue();
		uint64_t valign			 = std::max<uint64_t>(dl.getABITypeAlign(vst).value(),
													  variant_explicit_align);
		if (vsize > max_payload_size) {
			max_payload_size = vsize;
		}
		if (valign > max_payload_align || (valign == max_payload_align && vsize > carrier_size)) {
			max_payload_align = valign;
			carrier_size	  = vsize;
			carrier_lt		  = vst;
		}
	}

	if (max_payload_size == 0) {
		layout.has_payload		 = false;
		layout.tag_field_idx	 = 0;
		layout.payload_field_idx = 0;
		st->setBody({layout.tag_type}, false);
	} else {
		layout.has_payload		 = true;
		uint64_t union_size		 = max_payload_size;
		uint64_t rem			 = max_payload_size % max_payload_align;
		if (rem != 0) {
			union_size += (max_payload_align - rem);
		}
		uint64_t				  pad = (union_size > carrier_size) ? (union_size - carrier_size) : 0;
		std::vector<llvm::Type *> union_body{carrier_lt};
		if (pad > 0) {
			union_body.push_back(llvm::ArrayType::get(l_.llvm_int(8), pad));
		}
		llvm::Type *payload_lt = llvm::StructType::get(l_.ctx(), union_body, false);

		uint64_t tag_align = dl.getABITypeAlign(layout.tag_type).value();
		uint64_t tag_size  = dl.getTypeAllocSize(layout.tag_type).getFixedValue();
		bool	 payload_first = false;
		if (t->repr == ReprKind::Stella) {
			if (max_payload_align > tag_align ||
				(max_payload_align == tag_align && union_size > tag_size)) {
				payload_first = true;
			}
		}
		if (payload_first) {
			layout.payload_field_idx = 0;
			layout.tag_field_idx	 = 1;
			st->setBody({payload_lt, layout.tag_type}, false);
		} else {
			layout.tag_field_idx	 = 0;
			layout.payload_field_idx = 1;
			st->setBody({layout.tag_type, payload_lt}, false);
		}
	}

	uint64_t abi_align = dl.getABITypeAlign(st).value();
	if (max_payload_align > abi_align) {
		t->explicit_align = std::max(t->explicit_align, max_payload_align);
	}

	if (t->layout_size || t->layout_align) {
		const uint64_t actual_size	= dl.getTypeAllocSize(st).getFixedValue();
		const uint64_t actual_align = std::max<uint64_t>(dl.getABITypeAlign(st).value(),
														   t->explicit_align);
		if (t->layout_size && *t->layout_size != actual_size) {
			log::report(log::Level::Error, t->layout_loc,
						"layout guarantee for enum '" + t->name + "' failed: expected size " +
							std::to_string(*t->layout_size) + ", target ABI size is " +
							std::to_string(actual_size),
						"E_LAYOUT_MISMATCH", "sema");
		}
		if (t->layout_align && *t->layout_align != actual_align) {
			log::report(log::Level::Error, t->layout_loc,
						"layout guarantee for enum '" + t->name + "' failed: expected alignment " +
							std::to_string(*t->layout_align) + ", target ABI alignment is " +
							std::to_string(actual_align),
						"E_LAYOUT_MISMATCH", "sema");
		}
	}
	return layout;
}

namespace {

enum class SysVClass { None, Integer, Sse, Memory };

SysVClass merge_sysv_class(SysVClass a, SysVClass b) {
	if (a == b) return a;
	if (a == SysVClass::None) return b;
	if (b == SysVClass::None) return a;
	if (a == SysVClass::Memory || b == SysVClass::Memory) return SysVClass::Memory;
	if (a == SysVClass::Integer || b == SysVClass::Integer) return SysVClass::Integer;
	return SysVClass::Sse;
}

void classify_sysv_fields(llvm::Type *ty, const llvm::DataLayout &dl, uint64_t offset,
						  SysVClass eb[2], bool &only_f32_eb0, bool &only_f32_eb1) {
	if (ty->isVoidTy()) return;
	if (auto *st = llvm::dyn_cast<llvm::StructType>(ty)) {
		const llvm::StructLayout *layout = dl.getStructLayout(st);
		for (unsigned i = 0; i < st->getNumElements(); ++i) {
			classify_sysv_fields(st->getElementType(i), dl, offset + layout->getElementOffset(i),
								 eb, only_f32_eb0, only_f32_eb1);
		}
		return;
	}
	if (auto *arr = llvm::dyn_cast<llvm::ArrayType>(ty)) {
		uint64_t elem_size = dl.getTypeAllocSize(arr->getElementType()).getFixedValue();
		for (uint64_t i = 0; i < arr->getNumElements(); ++i) {
			classify_sysv_fields(arr->getElementType(), dl, offset + i * elem_size, eb,
								 only_f32_eb0, only_f32_eb1);
		}
		return;
	}
	uint64_t sz = dl.getTypeAllocSize(ty).getFixedValue();
	uint64_t al = dl.getABITypeAlign(ty).value();
	if (al > 0 && (offset % al) != 0) {
		eb[0] = SysVClass::Memory;
		eb[1] = SysVClass::Memory;
		return;
	}
	SysVClass c = SysVClass::Integer;
	bool	  is_f32 = false;
	if (ty->isFloatTy() || ty->isDoubleTy() || ty->isHalfTy()) {
		c	   = SysVClass::Sse;
		is_f32 = ty->isFloatTy();
	}
	uint64_t first_eb = offset / 8;
	uint64_t last_eb  = (offset + (sz > 0 ? sz - 1 : 0)) / 8;
	for (uint64_t e = first_eb; e <= last_eb && e < 2; ++e) {
		eb[e] = merge_sysv_class(eb[e], c);
		if (!is_f32) {
			if (e == 0) only_f32_eb0 = false;
			if (e == 1) only_f32_eb1 = false;
		}
	}
}

llvm::Type *sysv_eb_type(llvm::LLVMContext &ctx, SysVClass c, uint64_t bytes, bool only_f32) {
	if (c == SysVClass::Sse) {
		if (only_f32 && bytes <= 4) return llvm::Type::getFloatTy(ctx);
		if (only_f32 && bytes <= 8) return llvm::FixedVectorType::get(llvm::Type::getFloatTy(ctx), 2);
		return llvm::Type::getDoubleTy(ctx);
	}
	if (bytes <= 1) return llvm::Type::getInt8Ty(ctx);
	if (bytes <= 2) return llvm::Type::getInt16Ty(ctx);
	if (bytes <= 4) return llvm::Type::getInt32Ty(ctx);
	return llvm::Type::getInt64Ty(ctx);
}

bool collect_hfa_element(llvm::Type *ty, const llvm::DataLayout &dl, llvm::Type *&elem_ty,
						 unsigned &count) {
	if (ty->isFloatTy() || ty->isDoubleTy()) {
		if (!elem_ty) {
			elem_ty = ty;
		} else if (elem_ty != ty) {
			return false;
		}
		++count;
		return count <= 4;
	}
	if (auto *st = llvm::dyn_cast<llvm::StructType>(ty)) {
		const llvm::StructLayout *layout = dl.getStructLayout(st);
		uint64_t				  expected_off = 0;
		for (unsigned i = 0; i < st->getNumElements(); ++i) {
			if (layout->getElementOffset(i) != expected_off) return false;
			if (!collect_hfa_element(st->getElementType(i), dl, elem_ty, count)) return false;
			expected_off += dl.getTypeAllocSize(st->getElementType(i)).getFixedValue();
		}
		return expected_off == layout->getSizeInBytes();
	}
	if (auto *arr = llvm::dyn_cast<llvm::ArrayType>(ty)) {
		for (uint64_t i = 0; i < arr->getNumElements(); ++i) {
			if (!collect_hfa_element(arr->getElementType(), dl, elem_ty, count)) return false;
		}
		return true;
	}
	return false;
}

} // namespace

Codegen::CAbiArgInfo Codegen::classify_c_abi_type(StType *t, bool is_return) {
	t = underlying_type(t);
	CAbiArgInfo info;
	if (!t || t->kind == TypeKind::Void) {
		info.kind		  = CAbiArgInfo::Kind::Direct;
		info.storage_type = l_.llvm_void();
		info.coerced_type = l_.llvm_void();
		return info;
	}
	llvm::Type *lt = map_type(t);
	info.storage_type = lt;
	info.coerced_type = lt;
	if (t->kind != TypeKind::Struct && t->kind != TypeKind::Union && t->kind != TypeKind::Array &&
		!(t->kind == TypeKind::Enum && t->is_tagged_enum)) {
		return info;
	}
	const auto &dl	  = l_.module().getDataLayout();
	uint64_t	size  = dl.getTypeAllocSize(lt).getFixedValue();
	uint64_t	align = dl.getABITypeAlign(lt).value();
	info.align		  = align;
	if (size == 0) {
		info.kind = CAbiArgInfo::Kind::Ignore;
		return info;
	}
	llvm::Triple triple(l_.module().getTargetTriple());
	if (triple.getArch() == llvm::Triple::x86_64) {
		if (triple.isOSWindows()) {
			if (size == 1 || size == 2 || size == 4 || size == 8) {
				info.kind		  = CAbiArgInfo::Kind::Direct;
				info.coerced_type = l_.llvm_int(static_cast<unsigned>(size * 8));
			} else {
				info.kind = is_return ? CAbiArgInfo::Kind::SRet : CAbiArgInfo::Kind::Indirect;
			}
			return info;
		}
		// SysV x86-64
		if (size > 16) {
			info.kind  = is_return ? CAbiArgInfo::Kind::SRet : CAbiArgInfo::Kind::ByVal;
			info.align = std::max<uint64_t>(align, 8);
			return info;
		}
		SysVClass eb[2]		   = {SysVClass::None, SysVClass::None};
		bool	  only_f32_eb0 = true;
		bool	  only_f32_eb1 = true;
		if (t->kind == TypeKind::Enum && t->is_tagged_enum) {
			const auto &te_layout = get_tagged_enum_layout(t);
			const auto *outer_sl  = dl.getStructLayout(te_layout.outer_type);
			classify_sysv_fields(te_layout.tag_type, dl,
								 outer_sl->getElementOffset(te_layout.tag_field_idx), eb,
								 only_f32_eb0, only_f32_eb1);
			if (te_layout.has_payload) {
				uint64_t poff = outer_sl->getElementOffset(te_layout.payload_field_idx);
				for (auto *vt : te_layout.variant_types) {
					if (vt) {
						classify_sysv_fields(vt, dl, poff, eb, only_f32_eb0, only_f32_eb1);
					}
				}
			}
		} else {
			classify_sysv_fields(lt, dl, 0, eb, only_f32_eb0, only_f32_eb1);
		}
		if (eb[0] == SysVClass::Memory || eb[1] == SysVClass::Memory) {
			info.kind  = is_return ? CAbiArgInfo::Kind::SRet : CAbiArgInfo::Kind::ByVal;
			info.align = std::max<uint64_t>(align, 8);
			return info;
		}
		llvm::Type *t0 = sysv_eb_type(l_.ctx(), eb[0], std::min<uint64_t>(size, 8), only_f32_eb0);
		if (size <= 8 || eb[1] == SysVClass::None) {
			info.kind		  = CAbiArgInfo::Kind::Direct;
			info.coerced_type = t0;
			return info;
		}
		llvm::Type *t1 = sysv_eb_type(l_.ctx(), eb[1], size - 8, only_f32_eb1);
		info.kind		  = CAbiArgInfo::Kind::Direct;
		info.coerced_type = llvm::StructType::get(l_.ctx(), {t0, t1});
		return info;
	}
	if (triple.getArch() == llvm::Triple::aarch64) {
		llvm::Type *hfa_elem  = nullptr;
		unsigned	hfa_count = 0;
		if (collect_hfa_element(lt, dl, hfa_elem, hfa_count) && hfa_elem && hfa_count > 0) {
			info.kind		  = CAbiArgInfo::Kind::Direct;
			info.coerced_type = llvm::ArrayType::get(hfa_elem, hfa_count);
			return info;
		}
		if (size <= 16) {
			info.kind = CAbiArgInfo::Kind::Direct;
			if (size <= 8) {
				info.coerced_type = l_.llvm_int(static_cast<unsigned>(((size + 7) / 8) * 64));
				if (size == 1 || size == 2 || size == 4) {
					info.coerced_type = l_.llvm_int(static_cast<unsigned>(size * 8));
				}
			} else {
				info.coerced_type = llvm::ArrayType::get(l_.llvm_int(64), 2);
			}
			return info;
		}
		info.kind = is_return ? CAbiArgInfo::Kind::SRet : CAbiArgInfo::Kind::Indirect;
		return info;
	}
	if (triple.getArch() == llvm::Triple::riscv64) {
		if (size <= 16) {
			info.kind = CAbiArgInfo::Kind::Direct;
			if (size <= 8) {
				info.coerced_type = l_.llvm_int(static_cast<unsigned>(((size + 7) / 8) * 64));
			} else {
				info.coerced_type = llvm::ArrayType::get(l_.llvm_int(64), 2);
			}
			return info;
		}
		info.kind = is_return ? CAbiArgInfo::Kind::SRet : CAbiArgInfo::Kind::Indirect;
		return info;
	}
	return info;
}

Codegen::CAbiSignature Codegen::compute_c_abi_signature(const std::vector<StType *> &param_types,
														  StType *ret_type, bool is_variadic) {
	CAbiSignature sig;
	sig.ret = classify_c_abi_type(ret_type, true);
	std::vector<llvm::Type *> llvm_params;
	if (sig.ret.kind == CAbiArgInfo::Kind::SRet) {
		llvm_params.push_back(l_.llvm_ptr());
	}
	for (StType *pt : param_types) {
		CAbiArgInfo pi = classify_c_abi_type(pt, false);
		sig.params.push_back(pi);
		if (pi.kind == CAbiArgInfo::Kind::Ignore) {
			continue;
		}
		if (pi.kind == CAbiArgInfo::Kind::ByVal || pi.kind == CAbiArgInfo::Kind::Indirect) {
			llvm_params.push_back(l_.llvm_ptr());
		} else {
			llvm_params.push_back(pi.coerced_type);
		}
	}
	llvm::Type *llvm_ret = sig.ret.kind == CAbiArgInfo::Kind::SRet ||
								   sig.ret.kind == CAbiArgInfo::Kind::Ignore
							   ? l_.llvm_void()
							   : sig.ret.coerced_type;
	sig.fn_type = llvm::FunctionType::get(llvm_ret, llvm_params, is_variadic);
	return sig;
}

void Codegen::apply_c_abi_attrs(llvm::Function *fn, const CAbiSignature &sig) {
	unsigned arg_idx = 0;
	if (sig.ret.kind == CAbiArgInfo::Kind::SRet) {
		fn->addParamAttr(arg_idx,
						 llvm::Attribute::getWithStructRetType(l_.ctx(), sig.ret.storage_type));
		fn->addParamAttr(arg_idx, llvm::Attribute::NoAlias);
		if (sig.ret.align > 0) {
			fn->addParamAttr(arg_idx,
							 llvm::Attribute::getWithAlignment(l_.ctx(), llvm::Align(sig.ret.align)));
		}
		++arg_idx;
	}
	for (const auto &pi : sig.params) {
		if (pi.kind == CAbiArgInfo::Kind::Ignore) {
			continue;
		}
		if (pi.kind == CAbiArgInfo::Kind::ByVal) {
			fn->addParamAttr(arg_idx,
							 llvm::Attribute::getWithByValType(l_.ctx(), pi.storage_type));
			if (pi.align > 0) {
				fn->addParamAttr(arg_idx,
								 llvm::Attribute::getWithAlignment(l_.ctx(), llvm::Align(pi.align)));
			}
		}
		++arg_idx;
	}
}

void Codegen::apply_c_abi_call_attrs(llvm::CallInst *call, const CAbiSignature &sig) {
	unsigned arg_idx = 0;
	if (sig.ret.kind == CAbiArgInfo::Kind::SRet) {
		call->addParamAttr(arg_idx,
						   llvm::Attribute::getWithStructRetType(l_.ctx(), sig.ret.storage_type));
		call->addParamAttr(arg_idx, llvm::Attribute::NoAlias);
		if (sig.ret.align > 0) {
			call->addParamAttr(
				arg_idx, llvm::Attribute::getWithAlignment(l_.ctx(), llvm::Align(sig.ret.align)));
		}
		++arg_idx;
	}
	for (const auto &pi : sig.params) {
		if (pi.kind == CAbiArgInfo::Kind::Ignore) {
			continue;
		}
		if (pi.kind == CAbiArgInfo::Kind::ByVal) {
			call->addParamAttr(arg_idx,
							   llvm::Attribute::getWithByValType(l_.ctx(), pi.storage_type));
			if (pi.align > 0) {
				call->addParamAttr(
					arg_idx, llvm::Attribute::getWithAlignment(l_.ctx(), llvm::Align(pi.align)));
			}
		}
		++arg_idx;
	}
}

llvm::Value *Codegen::coerce_abi_via_memory(llvm::IRBuilder<> &B, llvm::Value *v,
											 llvm::Type *dst) {
	if (!v || v->getType() == dst) {
		return v;
	}
	const auto &dl		= l_.module().getDataLayout();
	uint64_t	src_sz	= dl.getTypeAllocSize(v->getType()).getFixedValue();
	uint64_t	dst_sz	= dl.getTypeAllocSize(dst).getFixedValue();
	llvm::Type *buf_ty	= src_sz >= dst_sz ? v->getType() : dst;
	llvm::Align align	= std::max(dl.getABITypeAlign(v->getType()), dl.getABITypeAlign(dst));
	llvm::AllocaInst *tmp = llvm::cast<llvm::AllocaInst>(alloca_in_entry(buf_ty, "abi.coerce"));
	tmp->setAlignment(align);
	B.CreateAlignedStore(v, tmp, align);
	return B.CreateAlignedLoad(dst, tmp, align);
}

} // namespace stellac
