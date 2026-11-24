#pragma once

#include "ast.hpp"
#include "types.hpp"
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

namespace stellac {

enum class SymKind { Mod, Var, Const, Fn, Struct, Union, TypeBinding, Param, Enum, Trait };

struct Symbol {
	Module						 *owner_module = nullptr;
	SymKind					 kind = SymKind::Var;
	std::string				 simple; // last segment name
	StType					*type = nullptr;
	ModDecl					*mod_decl = nullptr;
	FnDecl					*fn	  = nullptr;
	std::vector<FnDecl *>	 overloads;
	StructDecl				*st		= nullptr;
	UnionDecl				*un		= nullptr;
	TypeBindingDecl			*tb		= nullptr;
	EnumDecl				*en		= nullptr;
	TraitDecl				*trait_decl = nullptr;
	GlobalDecl				*glob	= nullptr;
	bool					 is_pub = false;
	bool					 is_mutable = true;
	std::vector<std::string> module_path; // owning module
	std::string				 mangled;	  // mangled symbol
	SourceLoc				 loc;
	AttributeList			 attrs;
	bool					*usage				  = nullptr;
	VarDeclInfo				*local_decl			  = nullptr;
	bool					 has_const_value	  = false;
	bool					 const_value_is_float = false;
	bool const_int_high_bits_ones = false;
	bool					 const_bool_val		  = false;
	uint64_t				 const_int_val		  = 0;
	double					 const_flt_val		  = 0;
	StType					*comptime_type_value  = nullptr;
};

struct Scope {
	std::unordered_map<std::string, Symbol> names;
	Scope								   *parent = nullptr;
};

class ScopeStack {
  public:
	void enter() {
		scopes_.emplace_back();
		if (!stack_.empty()) {
			scopes_.back().parent = stack_.back();
		}
		stack_.push_back(&scopes_.back());
	}
	void leave() {
		stack_.pop_back();
	}
	Scope *top() {
		return stack_.empty() ? nullptr : stack_.back();
	}

	Symbol *lookup_current(const std::string &n) {
		if (stack_.empty()) {
			return nullptr;
		}
		auto it = stack_.back()->names.find(n);
		return it == stack_.back()->names.end() ? nullptr : &it->second;
	}

	Symbol *lookup(const std::string &n) {
		for (int i = (int)stack_.size() - 1; i >= 0; --i) {
			auto it = stack_[i]->names.find(n);
			if (it != stack_[i]->names.end()) {
				return &it->second;
			}
		}
		return nullptr;
	}
	bool define(const std::string &n, Symbol s) {
		auto *t = top();
		if (!t) {
			return false;
		}
		if (t->names.count(n)) {
			return false;
		}
		t->names.emplace(n, std::move(s));
		return true;
	}

	void collect_names(std::vector<std::string> &out) const {
		for (const Scope *s : stack_) {
			if (!s) {
				continue;
			}
			for (const auto &pair : s->names) {
				out.push_back(pair.first);
			}
		}
	}

  private:
	std::list<Scope>	 scopes_;
	std::vector<Scope *> stack_;
};

} // namespace stellac
