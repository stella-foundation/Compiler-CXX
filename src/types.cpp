#include "types.hpp"
#include "log.hpp"
#include "prim_types.hpp"

namespace stellac {

static std::unique_ptr<StType> make_prim(TypeKind k, int w, const char *nm) {
	auto t	 = std::make_unique<StType>();
	t->kind	 = k;
	t->width = w;
	t->name	 = nm;
	return t;
}

static StType *intern(std::vector<std::unique_ptr<StType>> &pool, std::unique_ptr<StType> t) {
	StType *raw = t.get();
	pool.push_back(std::move(t));
	return raw;
}

// Template helper to intern a uniquely-owned StType into a heterogeneous cache
// (int-keyed or string-keyed) and the shared pool, avoiding the repeated
// find-or-create pattern in every TypeContext::* factory method.
template <typename Cache, typename Key, typename Maker>
static StType *intern_type(Cache &cache, Key &&key, Maker &&maker,
						   std::vector<std::unique_ptr<StType>> &pool) {
	auto it = cache.find(key);
	if (it != cache.end()) {
		return it->second;
	}
	auto	t					  = maker();
	StType *raw					  = t.get();
	cache[std::forward<Key>(key)] = raw;
	pool.push_back(std::move(t));
	return raw;
}

TypeContext::TypeContext(int pointer_width) : pointer_width_(pointer_width) {
	if (pointer_width_ <= 0) {
		pointer_width_ = 64;
	}
	void_ = bool_ = usize_ = isize_ = va_list_ = type_ = poison_ = self_ = nullptr;

	// Only the fixed primitives are pre-materialized: void, bool, and the
	// target-pointer-sized usize/isize. The u<N>/i<N>/f<N> integer/float scalars
	// are an open family and are interned on demand by uint()/sint()/float_ty().
	void_	 = intern(pool_, make_prim(TypeKind::Void, 0, "void"));
	bool_	 = intern(pool_, make_prim(TypeKind::Bool, 1, "bool"));
	type_	 = intern(pool_, make_prim(TypeKind::Type, 0, "type"));
	usize_	 = intern(pool_, make_prim(TypeKind::UInt, pointer_width_, "usize"));
	isize_	 = intern(pool_, make_prim(TypeKind::SInt, pointer_width_, "isize"));
	va_list_ = intern(pool_, make_prim(TypeKind::VaList, 0, "@va.list"));
	poison_	 = intern(pool_, make_prim(TypeKind::Poison, 0, "<error>"));
	self_	 = intern(pool_, make_prim(TypeKind::SelfType, 0, "Self"));
}

StType *underlying_type(StType *t) {
	StType *slow = t;
	StType *fast = t;
	while (fast && fast->kind == TypeKind::TypeBinding) {
		slow = slow ? slow->base : nullptr;
		fast = fast->base;
		if (fast && fast->kind == TypeKind::TypeBinding) {
			fast = fast->base;
		}
		if (fast && fast->kind == TypeKind::TypeBinding && slow == fast) {
			return nullptr;
		}
	}
	while (t && t->kind == TypeKind::TypeBinding) {
		t = t->base;
	}
	return t;
}

const StType *underlying_type(const StType *t) {
	const StType *slow = t;
	const StType *fast = t;
	while (fast && fast->kind == TypeKind::TypeBinding) {
		slow = slow ? slow->base : nullptr;
		fast = fast->base;
		if (fast && fast->kind == TypeKind::TypeBinding) {
			fast = fast->base;
		}
		if (fast && fast->kind == TypeKind::TypeBinding && slow == fast) {
			return nullptr;
		}
	}
	while (t && t->kind == TypeKind::TypeBinding) {
		t = t->base;
	}
	return t;
}

size_t field_index(const StType *st, std::string_view name) {
	if (!st) {
		return SIZE_MAX;
	}
	for (size_t i = 0; i < st->field_names.size(); ++i) {
		if (st->field_names[i] == name) {
			return i;
		}
	}
	return SIZE_MAX;
}

StType *TypeContext::uint(int width) {
	if (width <= 0) {
		log::report(log::Level::Error, {}, "bad uint width");
		return usize_;
	}
	return intern_type(
		uints_, width,
		[&]() {
			auto t	 = std::make_unique<StType>();
			t->kind	 = TypeKind::UInt;
			t->width = width;
			t->name	 = "u" + std::to_string(width);
			return t;
		},
		pool_);
}

StType *TypeContext::sint(int width) {
	if (width <= 0) {
		log::report(log::Level::Error, {}, "bad sint width");
		return isize_;
	}
	return intern_type(
		sints_, width,
		[&]() {
			auto t	 = std::make_unique<StType>();
			t->kind	 = TypeKind::SInt;
			t->width = width;
			t->name	 = "i" + std::to_string(width);
			return t;
		},
		pool_);
}

StType *TypeContext::float_ty(int width) {
	return intern_type(
		floats_, width,
		[&]() {
			auto t	 = std::make_unique<StType>();
			t->kind	 = TypeKind::Float;
			t->width = width;
			t->name	 = "f" + std::to_string(width);
			return t;
		},
		pool_);
}

StType *TypeContext::pointer(StType *base, bool is_mutable, bool is_volatile) {
	base			= underlying_type(base);
	std::string key = "*";
	if (is_mutable) {
		key += "mut ";
	}
	if (is_volatile) {
		key += "volatile ";
	}
	key += base ? base->name : std::string("void");
	return intern_type(
		ptr_cache_, key,
		[&]() {
			auto p		   = std::make_unique<StType>();
			p->kind		   = TypeKind::Pointer;
			p->base		   = base;
			p->is_mutable  = is_mutable;
			p->is_volatile = is_volatile;
			p->name		   = key;
			return p;
		},
		pool_);
}

StType *TypeContext::array_of(StType *elem, size_t len) {
	elem			= underlying_type(elem);
	std::string key = (elem ? elem->name : std::string("void")) + "[" + std::to_string(len) + "]";
	return intern_type(
		arr_cache_, key,
		[&]() {
			auto a		 = std::make_unique<StType>();
			a->kind		 = TypeKind::Array;
			a->base		 = elem;
			a->array_len = len;
			a->name		 = key;
			return a;
		},
		pool_);
}

StType *TypeContext::slice(StType *elem, bool is_mutable, bool is_volatile) {
	elem			= underlying_type(elem);
	std::string key = "[]";
	if (is_mutable) {
		key += "mut ";
	}
	if (is_volatile) {
		key += "volatile ";
	}
	key += elem ? elem->name : std::string("void");
	return intern_type(
		slice_cache_, key,
		[&]() {
			auto s		   = std::make_unique<StType>();
			s->kind		   = TypeKind::Slice;
			s->base		   = elem;
			s->is_mutable  = is_mutable;
			s->is_volatile = is_volatile;
			s->name		   = key;
			return s;
		},
		pool_);
}

StType *TypeContext::function_of(std::vector<StType *> params, StType *ret, bool variadic,
								 AbiKind abi) {
	for (auto *&param : params) {
		param = underlying_type(param);
	}
	ret				= underlying_type(ret);
	std::string key = (abi == AbiKind::C ? "#[abi(\"C\")](" : "(");
	for (size_t i = 0; i < params.size(); ++i) {
		if (i) {
			key += ",";
		}
		key += params[i] ? params[i]->name : std::string("void");
	}
	if (variadic) {
		if (!params.empty()) {
			key += ",";
		}
		key += "...";
	}
	key += ")->";
	key += ret ? ret->name : std::string("void");
	return intern_type(
		fn_cache_, key,
		[&]() {
			auto f		   = std::make_unique<StType>();
			f->kind		   = TypeKind::Function;
			f->fn_params   = std::move(params);
			f->fn_ret	   = ret;
			f->fn_variadic = variadic;
			f->fn_abi	   = abi;
			f->name		   = key;
			return f;
		},
		pool_);
}

StType *TypeContext::struct_of(std::string name, std::vector<std::string> field_names,
							   std::vector<StType *> field_types, bool is_pub,
							   std::vector<bool> field_is_pub) {
	return intern_type(
		named_, name,
		[&]() {
			auto s		   = std::make_unique<StType>();
			s->kind		   = TypeKind::Struct;
			s->name		   = std::move(name);
			s->field_names = std::move(field_names);
			s->fields	   = std::move(field_types);
			s->field_is_pub = std::move(field_is_pub);
			s->is_pub	   = is_pub;
			return s;
		},
		pool_);
}

StType *TypeContext::union_of(std::string name, std::vector<std::string> field_names,
							  std::vector<StType *> field_types, bool is_pub,
							  std::vector<bool> field_is_pub) {
	std::string key = "union:" + name;
	return intern_type(
		named_, key,
		[&]() {
			auto u		   = std::make_unique<StType>();
			u->kind		   = TypeKind::Union;
			u->name		   = std::move(name);
			u->field_names = std::move(field_names);
			u->fields	   = std::move(field_types);
			u->field_is_pub = std::move(field_is_pub);
			u->is_pub	   = is_pub;
			return u;
		},
		pool_);
}

StType *TypeContext::type_binding_of(std::string name, StType *target, bool is_pub) {
	return intern_type(
		named_, name,
		[&]() {
			auto binding			 = std::make_unique<StType>();
			binding->kind			 = TypeKind::TypeBinding;
			binding->name			 = std::move(name);
			binding->base			 = target;
			binding->is_pub		 = is_pub;
			return binding;
		},
		pool_);
}

StType *TypeContext::enum_of(std::string name, StType *backing,
							 std::vector<std::string> member_names,
							 std::vector<uint64_t> member_values, bool is_pub,
							 bool is_tagged, std::vector<EnumVariantInfo> variants) {
	return intern_type(
		named_, name,
		[&]() {
			auto e			  = std::make_unique<StType>();
			e->kind			  = TypeKind::Enum;
			e->name			  = std::move(name);
			e->base			  = backing;
			e->field_names	  = std::move(member_names);
			e->enum_values	  = std::move(member_values);
			e->is_pub		  = is_pub;
			e->is_tagged_enum = is_tagged;
			e->enum_variants  = std::move(variants);
			return e;
		},
		pool_);
}

StType *TypeContext::find_named(const std::string &name) const {
	auto it = named_.find(name);
	if (it != named_.end()) {
		return it->second;
	}
	it = named_.find("union:" + name);
	return it == named_.end() ? nullptr : it->second;
}

bool TypeContext::is_int_like(StType *t) const {
	t = underlying_type(t);
	return t && (t->kind == TypeKind::UInt || t->kind == TypeKind::SInt ||
				 t->kind == TypeKind::Bool ||
				 (t->kind == TypeKind::Enum && !t->is_tagged_enum));
}
bool TypeContext::is_arith(StType *t) const {
	t = underlying_type(t);
	return t && (is_int_like(t) || t->kind == TypeKind::Float);
}
bool TypeContext::is_pointer(StType *t) const {
	return underlying_type(t) && underlying_type(t)->kind == TypeKind::Pointer;
}

} // namespace stellac
