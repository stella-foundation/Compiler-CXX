#include "internal.hpp"

namespace stellac {

std::string Codegen::fn_symbol_name(FnDecl &f) {
	if (!f.mangled.empty()) {
		return f.mangled;
	}
	if (f.sym && !f.sym->mangled.empty()) {
		return f.sym->mangled;
	}
	return f.full_path.empty() ? mangle({f.name}) : mangle(f.full_path);
}

void Codegen::apply_fn_attrs(llvm::Function *fn, AttributeList &attrs) {
	for (auto &a : attrs.attrs) {
		if (!a.negated && a.name == "inline") {
			fn->addFnAttr(llvm::Attribute::AlwaysInline);
		} else if (a.negated && a.name == "inline") {
			fn->addFnAttr(llvm::Attribute::NoInline);
		} else if (a.name == "cold") {
			fn->addFnAttr(llvm::Attribute::Cold);
		} else if (a.name == "hot") {
			fn->addFnAttr(llvm::Attribute::Hot);
		} else if (a.name == "naked") {
			fn->addFnAttr(llvm::Attribute::Naked);
		} else if (a.negated && a.name == "return") {
			fn->addFnAttr(llvm::Attribute::NoReturn);
		} else if (a.name == "must_tail") {
			// No function-level MustTail attribute in LLVM; tail-call enforcement
			// is per-call (MustTail on the call instruction). Keeping the fn as
			// a normal function -- sema already validated placement.
		} else if (a.name == "section") {
			if (!a.args.empty()) {
				fn->setSection(a.args[0]);
			}
		} else if (a.name == "used") {
			llvm::appendToUsed(l_.module(), {fn});
		} else if (a.negated && a.name == "sanitize") {
			if (!a.args.empty()) {
				const std::string &s = a.args[0];
				if (s == "address") {
					fn->removeFnAttr(llvm::Attribute::SanitizeAddress);
				} else if (s == "memory") {
					fn->removeFnAttr(llvm::Attribute::SanitizeMemory);
				} else if (s == "thread") {
					fn->removeFnAttr(llvm::Attribute::SanitizeThread);
				}
			}
		} else if (a.name == "link_name" || a.name == "abi") {
			// handled by mangling and calling convention in declare_fn().
		}
	}
}

void Codegen::apply_struct_attrs(llvm::StructType *st, StructDecl &s) {
	// LLVM StructType carries no per-type alignment in IR; #[align(N)] on a
	// struct is honored at every storage site (alloca/global) that holds an
	// instance -- see struct_align() / alloca_in_entry() / emit_global().
	// The struct body itself uses natural (non-packed) layout from DataLayout.
	(void)st;
	(void)s;
}

void Codegen::apply_global_attrs(llvm::GlobalVariable *gv, AttributeList &attrs) {
	for (auto &a : attrs.attrs) {
		if (a.name == "section") {
			if (!a.args.empty()) {
				gv->setSection(a.args[0]);
			}
		} else if (a.name == "align") {
			if (!a.args.empty()) {
				ParsedInt pi = parse_int_literal(a.args[0]);
				if (pi.ok && pi.value > 0) {
					gv->setAlignment(llvm::Align(pi.value));
				}
			}
		} else if (a.name == "used") {
			llvm::appendToUsed(l_.module(), {gv});
		}
	}
}

} // namespace stellac
