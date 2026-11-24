#include "module_resolver.hpp"
#include "lexer.hpp"
#include "log.hpp"
#include "mangle.hpp"
#include "parser.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <sys/stat.h>

namespace stellac {

bool read_file(const std::string &path, std::string &out) {
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		return false;
	}
	std::string contents;
	char buffer[8192];
	for (;;) {
		f.read(buffer, sizeof(buffer));
		const std::streamsize count = f.gcount();
		if (count > 0) {
			contents.append(buffer, static_cast<size_t>(count));
		}
		if (f.bad()) {
			return false;
		}
		if (f.eof()) {
			break;
		}
		if (f.fail()) {
			return false;
		}
	}
	out.swap(contents);
	log::register_source(path, out);
	return true;
}

static std::string private_module_identity(const std::string &source_path,
											  const std::string &identity_base) {
	namespace fs = std::filesystem;
	fs::path source(source_path);
	fs::path base(identity_base);
	fs::path relative = source.lexically_relative(base);
	if (relative.empty() || relative.is_absolute()) {
		relative = source.filename();
	}
	const std::string key = relative.lexically_normal().generic_string();
	static constexpr char hex[] = "0123456789abcdef";
	std::string encoded;
	encoded.reserve(key.size() * 2);
	for (unsigned char byte : key) {
		encoded.push_back(hex[byte >> 4]);
		encoded.push_back(hex[byte & 0x0f]);
	}
	return "m" + encoded;
}

Module *ModuleResolver::parse_file(const std::string &fpath) {
	auto it = file_trees_.find(fpath);
	if (it != file_trees_.end()) {
		return it->second.get();
	}
	std::string src;
	if (!read_file(fpath, src)) {
		log::report(log::Level::Error, {}, "cannot read file '" + fpath + "'");
		return nullptr;
	}
	auto m	 = std::make_unique<Module>();
	m->fpath = fpath;
	Lexer			   lex(src, fpath);
	std::vector<Token> toks;
	if (!lex.tokenize(toks)) {
		return nullptr;
	}
	Parser p(std::move(toks), fpath, {});
	if (!p.parse_module(*m)) {
		return nullptr;
	}
	m->private_symbol_module_id = private_module_identity(fpath, private_symbol_base_);
	Module *raw		   = m.get();
	file_trees_[fpath] = std::move(m);
	return raw;
}

static std::string canonical_source_path(const std::string &path) {
	std::error_code ec;
	std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
	return (ec ? std::filesystem::path(path).lexically_normal() : canonical).string();
}

std::string ModuleResolver::resolve_import_path(const std::string &written,
													const Module &from) const {
	std::filesystem::path requested(written);
	std::vector<std::filesystem::path> candidates;
	if (requested.is_absolute()) {
		candidates.push_back(requested);
	} else {
		candidates.push_back(std::filesystem::path(from.fpath).parent_path() / requested);
		for (const auto &root : roots_) {
			candidates.push_back(std::filesystem::path(root) / requested);
		}
	}
	for (const auto &candidate : candidates) {
		std::error_code ec;
		if (std::filesystem::is_regular_file(candidate, ec) && !ec) {
			std::string canonical = canonical_source_path(candidate.string());
			if (canonical == canonical_source_path(from.fpath)) {
				continue;
			}
			return canonical;
		}
	}
	return {};
}

static bool has_glob_metacharacters(const std::string &path) {
	return path.find_first_of("*?") != std::string::npos;
}

static bool glob_match(std::string_view pattern, std::string_view text) {
	size_t pi = 0;
	size_t ti = 0;
	while (pi < pattern.size()) {
		if (pattern[pi] == '*' && pi + 1 < pattern.size() && pattern[pi + 1] == '*') {
			pi += 2;
			if (pi < pattern.size() && pattern[pi] == '/') {
				++pi;
				for (size_t k = ti; k <= text.size(); ++k) {
					if ((k == ti || text[k - 1] == '/') &&
						glob_match(pattern.substr(pi), text.substr(k))) {
						return true;
					}
				}
				return false;
			}
			for (size_t k = ti; k <= text.size(); ++k) {
				if (glob_match(pattern.substr(pi), text.substr(k))) {
					return true;
				}
			}
			return false;
		}
		if (pattern[pi] == '*') {
			++pi;
			for (size_t k = ti; k <= text.size(); ++k) {
				if (glob_match(pattern.substr(pi), text.substr(k))) {
					return true;
				}
				if (k < text.size() && text[k] == '/') {
					break;
				}
			}
			return false;
		}
		if (pattern[pi] == '?') {
			if (ti >= text.size() || text[ti] == '/') {
				return false;
			}
			++pi;
			++ti;
			continue;
		}
		if (ti >= text.size() || pattern[pi] != text[ti]) {
			return false;
		}
		++pi;
		++ti;
	}
	return ti == text.size();
}

static std::vector<std::string> resolve_glob_paths(const std::string &written,
														const Module &from,
														const std::vector<std::string> &roots) {
	namespace fs = std::filesystem;
	fs::path requested(written);
	std::vector<fs::path> search_roots;
	if (requested.is_absolute()) {
		search_roots.push_back(fs::path("/"));
	} else {
		search_roots.push_back(fs::path(from.fpath).parent_path());
		for (const auto &root : roots) {
			search_roots.push_back(fs::path(root));
		}
	}

	const std::string pattern = requested.generic_string();
	const size_t first_meta = pattern.find_first_of("*?");
	const size_t slash = first_meta == std::string::npos
		? std::string::npos
		: pattern.rfind('/', first_meta);
	const std::string static_prefix = first_meta == std::string::npos
		? pattern
		: pattern.substr(0, slash == std::string::npos ? 0 : slash);
	std::string relative_pattern = pattern;
	if (first_meta != std::string::npos) {
		if (requested.is_absolute() && static_prefix.empty()) {
			relative_pattern = pattern.substr(1);
		} else if (!static_prefix.empty()) {
			relative_pattern = pattern.substr(static_prefix.size() + 1);
		}
	}
	std::vector<std::string> result;
	std::set<std::string> seen;
	for (const fs::path &root : search_roots) {
		fs::path scan_root = root;
		if (!static_prefix.empty()) {
			if (requested.is_absolute()) {
				scan_root = fs::path("/") / static_prefix;
			} else {
				scan_root /= static_prefix;
			}
		}
		std::error_code ec;
		if (!fs::is_directory(scan_root, ec) || ec) {
			continue;
		}
		std::vector<std::string> matches;
		auto consider = [&](const fs::directory_entry &entry) {
			std::error_code entry_ec;
			if (!fs::is_regular_file(entry.path(), entry_ec) || entry_ec) {
				return;
			}
			std::string relative = entry.path().lexically_relative(scan_root).generic_string();
			if (!glob_match(relative_pattern, relative)) {
				return;
			}
			std::string canonical = canonical_source_path(entry.path().string());
			if (canonical == canonical_source_path(from.fpath) || !seen.insert(canonical).second) {
				return;
			}
			matches.push_back(std::move(canonical));
		};
		const bool can_match_nested = relative_pattern.find('/') != std::string::npos ||
			relative_pattern.find("**") != std::string::npos;
		if (can_match_nested) {
			fs::recursive_directory_iterator it(
				scan_root, fs::directory_options::skip_permission_denied, ec), end;
			if (ec) {
				continue;
			}
			for (; it != end; it.increment(ec)) {
				if (ec) {
					ec.clear();
					continue;
				}
				consider(*it);
			}
		} else {
			fs::directory_iterator it(
				scan_root, fs::directory_options::skip_permission_denied, ec), end;
			if (ec) {
				continue;
			}
			for (; it != end; it.increment(ec)) {
				if (ec) {
					ec.clear();
					continue;
				}
				consider(*it);
			}
		}
		std::sort(matches.begin(), matches.end());
		result.insert(result.end(), matches.begin(), matches.end());
	}
	return result;
}

Module *ModuleResolver::load_import(ImportDecl &import, const Module &from) {
	auto targets = load_import_targets(import, from);
	return targets.empty() ? nullptr : targets.front();
}

std::vector<Module *> ModuleResolver::load_import_targets(ImportDecl &import,
														const Module &from) {
	if (!import.targets.empty()) {
		return import.targets;
	}
	std::vector<std::string> paths;
	if (has_glob_metacharacters(import.written_path)) {
		paths = resolve_glob_paths(import.written_path, from, roots_);
	} else if (std::string resolved = resolve_import_path(import.written_path, from);
														!resolved.empty()) {
		paths.push_back(std::move(resolved));
	}
	import.resolved_paths = paths;
	for (const auto &path : paths) {
		Module *target = parse_file(path);
		if (!target) {
			continue;
		}
		import.targets.push_back(target);
		touched_files_.insert(path);
	}
	if (!import.targets.empty()) {
		import.target = import.targets.front();
		import.resolved_path = import.resolved_paths.front();
	}
	return import.targets;
}

// Deep-clone helpers for AST nodes. collect_leaf_decls() must produce a
// logically independent copy of the source decls -- the source tree lives in
// file_trees_ (a parsed-and-cached Module owned by the resolver) and must
// remain intact for repeated load() calls and for the depfile walk. The
// previous implementation `std::move`d unique_ptr's out of a const ref via
// const_cast, which both is undefined behavior and destructively empties the
// cached tree so that subsequent loads of the same module produce nothing.
std::unique_ptr<TypeRef> clone_typeref(const TypeRef &t);
std::unique_ptr<Expr>	 clone_expr(const Expr &e);
StmtPtr					 clone_stmt(const Stmt &s);

GenericArg clone_generic_arg(const GenericArg &a) {
	GenericArg r;
	r.kind = a.kind;
	r.loc  = a.loc;
	if (a.type) {
		r.type = clone_typeref(*a.type);
	}
	if (a.value) {
		r.value = clone_expr(*a.value);
	}
	return r;
}

GenericParam clone_generic_param(const GenericParam &p) {
	GenericParam r;
	r.kind = p.kind;
	r.name = p.name;
	r.loc  = p.loc;
	if (p.const_type) {
		r.const_type = clone_typeref(*p.const_type);
	}
	r.type_set = p.type_set;
	for (const auto &b : p.trait_bounds) {
		if (b) {
			r.trait_bounds.push_back(clone_typeref(*b));
		}
	}
	return r;
}

std::unique_ptr<TypeRef> clone_typeref(const TypeRef &t) {
	auto r	  = std::make_unique<TypeRef>();
	r->kind	  = t.kind;
	r->name	  = t.name;
	r->single = t.single;
	if (t.base) {
		r->base = clone_typeref(*t.base);
	}
	r->array_len = t.array_len;
	if (t.array_len_expr) {
		r->array_len_expr = clone_expr(*t.array_len_expr);
	}
	r->array_len_inferred = t.array_len_inferred;
	r->is_mutable		  = t.is_mutable;
	r->is_volatile		  = t.is_volatile;
	for (auto &p : t.fn_params) {
		r->fn_params.push_back(clone_typeref(*p));
	}
	if (t.fn_ret) {
		r->fn_ret = clone_typeref(*t.fn_ret);
	}
	r->fn_variadic = t.fn_variadic;
	r->fn_attrs	   = t.fn_attrs;
	for (const auto &a : t.generic_args) {
		r->generic_args.push_back(clone_generic_arg(a));
	}
	if (t.typeof_expr) {
		r->typeof_expr = clone_expr(*t.typeof_expr);
	}
	r->sema_invalid = t.sema_invalid;
	r->loc			= t.loc;
	return r;
}

std::unique_ptr<Expr> clone_expr(const Expr &e) {
	auto c		= std::make_unique<Expr>();
	c->kind		= e.kind;
	c->loc		= e.loc;
	c->parenthesized = e.parenthesized;
	c->binding_module = e.binding_module;
	c->macro_argument = e.macro_argument;
	c->invocation_loc = e.invocation_loc;
	c->expansion_chain = e.expansion_chain;
	c->int_val	= e.int_val;
	c->flt_val	= e.flt_val;
	c->str_val	  = e.str_val;
	c->is_cstring = e.is_cstring;
	c->char_val   = e.char_val;
	c->path		= e.path;
	c->member	= e.member;
	if (e.lhs) {
		c->lhs = clone_expr(*e.lhs);
	}
	if (e.rhs) {
		c->rhs = clone_expr(*e.rhs);
	}
	if (e.index_low) {
		c->index_low = clone_expr(*e.index_low);
	}
	c->bit_range_high = e.bit_range_high;
	c->bit_range_low = e.bit_range_low;
	c->is_bit_range = e.is_bit_range;
	if (e.condition) {
		c->condition = clone_expr(*e.condition);
	}
	if (e.then_expr) {
		c->then_expr = clone_expr(*e.then_expr);
	}
	if (e.else_expr) {
		c->else_expr = clone_expr(*e.else_expr);
	}
	for (auto &a : e.args) {
		c->args.push_back(clone_expr(*a));
	}
	c->field_names = e.field_names;
	for (const auto &a : e.generic_args) {
		c->generic_args.push_back(clone_generic_arg(a));
	}
	if (e.cast_type) {
		c->cast_type = clone_typeref(*e.cast_type);
	}
	if (e.layout_type) {
		c->layout_type = clone_typeref(*e.layout_type);
	}
	c->is_bitcast	   = e.is_bitcast;
	c->is_compound_assignment = e.is_compound_assignment;
	c->is_struct_variant	  = e.is_struct_variant;
	c->offsetof_member = e.offsetof_member;
	if (e.va_arg_ty) {
		c->va_arg_ty = clone_typeref(*e.va_arg_ty);
	}
	// va-list operands are rebound to the cloned function's locals by sema.
	c->va_list_decl			= nullptr;
	c->va_list_param		= false;
	c->ty					= e.ty;
	c->type_value			= e.type_value;
	c->resolved_fn			= e.resolved_fn;
	c->sema_invalid			= e.sema_invalid;
	c->is_lvalue			= e.is_lvalue;
	c->is_addressable		= e.is_addressable;
	c->is_const_bool		= e.is_const_bool;
	c->const_bool_val		= e.const_bool_val;
	c->has_const_value		= e.has_const_value;
	c->const_value_is_float = e.const_value_is_float;
	c->const_int_high_bits_ones = e.const_int_high_bits_ones;
	if (e.block) {
		c->block = clone_stmt(*e.block);
	}
	return c;
}

StmtPtr clone_stmt(const Stmt &s) {
	auto c	= std::make_unique<Stmt>();
	c->kind = s.kind;
	c->loc	= s.loc;
	c->binding_module = s.binding_module;
	c->macro_argument = s.macro_argument;
	c->invocation_loc = s.invocation_loc;
	c->expansion_chain = s.expansion_chain;
	for (auto &st : s.body) {
		c->body.push_back(clone_stmt(*st));
	}
	if (s.expr) {
		c->expr = clone_expr(*s.expr);
	}
	for (const auto &var : s.vars) {
		VarDeclInfo cv;
		cv.name = var.name;
		if (var.ty) {
			cv.ty = clone_typeref(*var.ty);
		}
		if (var.init) {
			cv.init = clone_expr(*var.init);
		}
		cv.loc		= var.loc;
		cv.binding_module = var.binding_module;
		cv.macro_argument = var.macro_argument;
		cv.is_const = var.is_const;
		cv.is_mutable = var.is_mutable;
		cv.is_static = var.is_static;
		cv.is_thread_local = var.is_thread_local;
		cv.resolved = var.resolved;
		cv.attrs	= var.attrs;
		cv.used		= var.used;
		c->vars.push_back(std::move(cv));
	}
	if (s.cond) {
		c->cond = clone_expr(*s.cond);
	}
	if (s.then_block) {
		c->then_block = clone_stmt(*s.then_block);
	}
	if (s.else_block) {
		c->else_block = clone_stmt(*s.else_block);
	}
	if (s.for_init) {
		c->for_init = clone_stmt(*s.for_init);
	}
	if (s.for_cond) {
		c->for_cond = clone_expr(*s.for_cond);
	}
	if (s.for_step) {
		c->for_step = clone_expr(*s.for_step);
	}
	if (s.loop_body) {
		c->loop_body = clone_stmt(*s.loop_body);
	}
	if (s.scrutinee) {
		c->scrutinee = clone_expr(*s.scrutinee);
	}
	for (auto &arm : s.arms) {
		MatchArm na;
		na.loc = arm.loc;
		for (auto &pat : arm.patterns) {
			MatchPattern np;
			np.kind				 = pat.kind;
			np.is_struct_pattern = pat.is_struct_pattern;
			np.resolved_tag		 = pat.resolved_tag;
			np.variant_index	 = pat.variant_index;
			np.loc				 = pat.loc;
			if (pat.expr) {
				np.expr = clone_expr(*pat.expr);
			}
			for (const auto &b : pat.bindings) {
				MatchBinding nb;
				nb.name		  = b.name;
				nb.field_name = b.field_name;
				nb.is_mut	  = b.is_mut;
				nb.loc		  = b.loc;
				nb.resolved	  = b.resolved;
				np.bindings.push_back(std::move(nb));
			}
			na.patterns.push_back(std::move(np));
		}
		if (arm.body) {
			na.body = clone_stmt(*arm.body);
		}
		c->arms.push_back(std::move(na));
	}
	if (s.ret_val) {
		c->ret_val = clone_expr(*s.ret_val);
	}
	if (s.defer_body) {
		c->defer_body = clone_stmt(*s.defer_body);
	}
	c->asm_code		= s.asm_code;
	c->asm_volatile = s.asm_volatile;
	for (auto &op : s.asm_outputs) {
		AsmOperand no;
		no.constraint = op.constraint;
		no.is_clobber = op.is_clobber;
		if (op.operand) {
			no.operand = clone_expr(*op.operand);
		}
		c->asm_outputs.push_back(std::move(no));
	}
	for (auto &op : s.asm_inputs) {
		AsmOperand no;
		no.constraint = op.constraint;
		no.is_clobber = op.is_clobber;
		if (op.operand) {
			no.operand = clone_expr(*op.operand);
		}
		c->asm_inputs.push_back(std::move(no));
	}
	c->asm_clobbers = s.asm_clobbers;
	c->import_path	= s.import_path;
	c->import_group = s.import_group;
	return c;
}

static std::unique_ptr<FnDecl> clone_fn(const FnDecl &src) {
	auto f		  = std::make_unique<FnDecl>();
	f->owner_path = src.owner_path;
	f->name		  = src.name;
	f->full_path  = src.full_path;
	for (auto &p : src.params) {
		Param np;
		np.name = p.name;
		if (p.ty) {
			np.ty = clone_typeref(*p.ty);
		}
		np.loc		   = p.loc;
		np.resolved	   = p.resolved;
		np.attrs	   = p.attrs; // per-parameter attributes (e.g. #[!alias])
		np.used		   = p.used;
		np.is_comptime = p.is_comptime;
		np.is_mutable  = p.is_mutable;
		f->params.push_back(std::move(np));
	}
	if (src.ret) {
		f->ret = clone_typeref(*src.ret);
	}
	if (src.body) {
		f->body = clone_stmt(*src.body);
	}
	f->is_pub	   = src.is_pub;
	f->is_method   = src.is_method;
	f->is_variadic = src.is_variadic;
	f->is_macro = src.is_macro;
	f->macro_returns_value = src.macro_returns_value;
	f->is_extern   = src.is_extern;
	f->abi		   = src.abi;
	for (const auto &p : src.generic_params) {
		f->generic_params.push_back(clone_generic_param(p));
	}
	f->impl_generic_param_count = src.impl_generic_param_count;
	f->loc		   = src.loc;
	f->attrs.attrs = src.attrs.attrs;
	if (!src.attrs.attrs.empty() && !f->attrs.attrs.empty()) {
		f->attrs.loc = src.attrs.loc;
	}
	f->fn_sig	   = src.fn_sig;
	f->sym		   = src.sym;
	f->mangled	   = src.mangled;
	f->used		   = src.used;
	f->decl_module = src.decl_module;
	return f;
}

static TopDeclPtr clone_topdecl(const TopDecl &td) {
	auto r	= std::make_unique<TopDecl>();
	r->kind = td.kind;
	if (td.fn) {
		r->fn = clone_fn(*td.fn);
	} else if (td.st) {
		auto s		   = std::make_unique<StructDecl>();
		s->name		   = td.st->name;
		s->field_names = td.st->field_names;
		s->field_is_pub = td.st->field_is_pub;
		for (auto &ft : td.st->field_types) {
			s->field_types.push_back(ft ? clone_typeref(*ft) : nullptr);
		}
		s->is_pub	   = td.st->is_pub;
		s->is_opaque   = td.st->is_opaque;
		s->loc		   = td.st->loc;
		s->attrs.attrs = td.st->attrs.attrs;
		if (!td.st->attrs.attrs.empty()) {
			s->attrs.loc = td.st->attrs.loc;
		}
		s->resolved = td.st->resolved;
		for (const auto &p : td.st->generic_params) {
			s->generic_params.push_back(clone_generic_param(p));
		}
		r->st = std::move(s);
	} else if (td.mod_decl) {
		auto m		   = std::make_unique<ModDecl>();
		m->name		   = td.mod_decl->name;
		m->owner_path  = td.mod_decl->owner_path;
		m->is_pub	   = td.mod_decl->is_pub;
		m->loc		   = td.mod_decl->loc;
		m->attrs.attrs = td.mod_decl->attrs.attrs;
		if (!td.mod_decl->attrs.attrs.empty()) {
			m->attrs.loc = td.mod_decl->attrs.loc;
		}
		for (const auto &child : td.mod_decl->body) {
			if (child) {
				m->body.push_back(clone_topdecl(*child));
			}
		}
		r->mod_decl = std::move(m);
	} else if (td.un) {
		auto u		   = std::make_unique<UnionDecl>();
		u->name		   = td.un->name;
		u->field_names = td.un->field_names;
		u->field_is_pub = td.un->field_is_pub;
		for (auto &ft : td.un->field_types) {
			u->field_types.push_back(ft ? clone_typeref(*ft) : nullptr);
		}
		u->is_pub	   = td.un->is_pub;
		u->loc		   = td.un->loc;
		u->attrs.attrs = td.un->attrs.attrs;
		if (!td.un->attrs.attrs.empty()) {
			u->attrs.loc = td.un->attrs.loc;
		}
		u->resolved = td.un->resolved;
		for (const auto &p : td.un->generic_params) {
			u->generic_params.push_back(clone_generic_param(p));
		}
		r->un = std::move(u);
	} else if (td.tb) {
		auto t	= std::make_unique<TypeBindingDecl>();
		t->name = td.tb->name;
		if (td.tb->target) {
			t->target = clone_typeref(*td.tb->target);
		}
		t->is_pub		 = td.tb->is_pub;
		t->loc			 = td.tb->loc;
		t->attrs.attrs	 = td.tb->attrs.attrs;
		for (const auto &p : td.tb->generic_params) {
			t->generic_params.push_back(clone_generic_param(p));
		}
		if (!td.tb->attrs.attrs.empty()) {
			t->attrs.loc = td.tb->attrs.loc;
		}
		r->tb = std::move(t);
	} else if (td.glob) {
		auto g	= std::make_unique<GlobalDecl>();
		g->name = td.glob->name;
		g->owner_path = td.glob->owner_path;
		if (td.glob->ty) {
			g->ty = clone_typeref(*td.glob->ty);
		}
		if (td.glob->init) {
			g->init = clone_expr(*td.glob->init);
		}
		g->is_const	   = td.glob->is_const;
		g->is_mutable  = td.glob->is_mutable;
		g->is_static   = td.glob->is_static;
		g->is_thread_local = td.glob->is_thread_local;
		g->is_pub	   = td.glob->is_pub;
		g->is_extern   = td.glob->is_extern;
		g->loc		   = td.glob->loc;
		g->attrs.attrs = td.glob->attrs.attrs;
		if (!td.glob->attrs.attrs.empty()) {
			g->attrs.loc = td.glob->attrs.loc;
		}
		g->used = td.glob->used;
		r->glob = std::move(g);
	} else if (td.imp) {
		auto i		= std::make_unique<ImportDecl>();
		i->path		= td.imp->path;
		i->group	= td.imp->group;
		i->written_path = td.imp->written_path;
		i->resolved_path = td.imp->resolved_path;
		i->target = td.imp->target;
		i->targets = td.imp->targets;
		i->resolved_paths = td.imp->resolved_paths;
		i->is_pub = td.imp->is_pub;
		i->loc		= td.imp->loc;
		i->attrs	= td.imp->attrs;
		i->used		= td.imp->used;
		i->resolved = td.imp->resolved;
		r->imp		= std::move(i);
	} else if (td.en) {
		auto e	= std::make_unique<EnumDecl>();
		e->name = td.en->name;
		if (td.en->backing) {
			e->backing = clone_typeref(*td.en->backing);
		}
		for (auto &m : td.en->members) {
			EnumMember nm;
			nm.name			= m.name;
			nm.payload_kind = m.payload_kind;
			for (const auto &pf : m.payload_fields) {
				EnumVariantField npf;
				npf.name = pf.name;
				npf.loc  = pf.loc;
				if (pf.ty) {
					npf.ty = clone_typeref(*pf.ty);
				}
				nm.payload_fields.push_back(std::move(npf));
			}
			if (m.value) {
				nm.value = clone_expr(*m.value);
			}
			nm.resolved = m.resolved;
			nm.loc		= m.loc;
			e->members.push_back(std::move(nm));
		}
		e->is_pub	   = td.en->is_pub;
		e->loc		   = td.en->loc;
		e->attrs.attrs = td.en->attrs.attrs;
		if (!td.en->attrs.attrs.empty()) {
			e->attrs.loc = td.en->attrs.loc;
		}
		e->resolved = td.en->resolved;
		for (const auto &p : td.en->generic_params) {
			e->generic_params.push_back(clone_generic_param(p));
		}
		r->en = std::move(e);
	} else if (td.conditional) {
		auto cd = std::make_unique<ConditionalDecl>();
		cd->loc = td.conditional->loc;
		for (const auto &arm : td.conditional->arms) {
			ConditionalArm ca;
			ca.loc = arm.loc;
			if (arm.cond) {
				ca.cond = clone_expr(*arm.cond);
			}
			for (const auto &child : arm.body) {
				if (child) {
					ca.body.push_back(clone_topdecl(*child));
				}
			}
			cd->arms.push_back(std::move(ca));
		}
		r->conditional = std::move(cd);
	} else if (td.trait_decl) {
		auto tr = std::make_unique<TraitDecl>();
		tr->name = td.trait_decl->name;
		tr->is_pub = td.trait_decl->is_pub;
		tr->loc = td.trait_decl->loc;
		tr->attrs.attrs = td.trait_decl->attrs.attrs;
		if (!td.trait_decl->attrs.attrs.empty()) {
			tr->attrs.loc = td.trait_decl->attrs.loc;
		}
		tr->decl_module = td.trait_decl->decl_module;
		tr->mangled = td.trait_decl->mangled;
		for (const auto &m : td.trait_decl->methods) {
			if (m) {
				tr->methods.push_back(clone_fn(*m));
			}
		}
		r->trait_decl = std::move(tr);
	} else if (td.impl_decl) {
		auto im = std::make_unique<ImplDecl>();
		im->is_inherent = td.impl_decl->is_inherent;
		im->trait_name = td.impl_decl->trait_name;
		im->trait_path = td.impl_decl->trait_path;
		im->owner_path = td.impl_decl->owner_path;
		for (const auto &p : td.impl_decl->generic_params) {
			im->generic_params.push_back(clone_generic_param(p));
		}
		if (td.impl_decl->target_type) {
			im->target_type = clone_typeref(*td.impl_decl->target_type);
		}
		im->loc = td.impl_decl->loc;
		im->attrs.attrs = td.impl_decl->attrs.attrs;
		if (!td.impl_decl->attrs.attrs.empty()) {
			im->attrs.loc = td.impl_decl->attrs.loc;
		}
		im->decl_module = td.impl_decl->decl_module;
		im->resolved_trait = td.impl_decl->resolved_trait;
		im->resolved_target_type = td.impl_decl->resolved_target_type;
		for (const auto &c : td.impl_decl->consts) {
			if (!c) {
				continue;
			}
			auto g = std::make_unique<GlobalDecl>();
			g->name = c->name;
			g->owner_path = c->owner_path;
			if (c->ty) {
				g->ty = clone_typeref(*c->ty);
			}
			if (c->init) {
				g->init = clone_expr(*c->init);
			}
			g->is_const = c->is_const;
			g->is_mutable = c->is_mutable;
			g->is_static = c->is_static;
			g->is_thread_local = c->is_thread_local;
			g->is_pub = c->is_pub;
			g->loc = c->loc;
			g->attrs.attrs = c->attrs.attrs;
			if (!c->attrs.attrs.empty()) {
				g->attrs.loc = c->attrs.loc;
			}
			g->used = c->used;
			im->consts.push_back(std::move(g));
		}
		for (const auto &m : td.impl_decl->methods) {
			if (m) {
				im->methods.push_back(clone_fn(*m));
			}
		}
		r->impl_decl = std::move(im);
	}
	return r;
}

void ModuleResolver::collect_leaf_decls(const std::vector<TopDeclPtr>  &decls,
										const std::vector<std::string> &prefix,
										const std::vector<std::string> &qpath,
										std::vector<TopDeclPtr>		   &out) {
	bool match = (prefix == qpath);
	for (const auto &td : decls) {
		if (!td) {
			continue;
		}
		if (td->kind == TopKind::Mod && td->mod_decl && !td->mod_decl->body.empty()) {
			std::vector<std::string> cq = prefix;
			cq.push_back(td->mod_decl->name);
			if (cq == qpath) {
				for (const auto &child : td->mod_decl->body) {
					if (!child) {
						continue;
					}
					if (child->kind != TopKind::Mod) {
						out.push_back(clone_topdecl(*child));
					}
				}
			} else {
				collect_leaf_decls(td->mod_decl->body, cq, qpath, out);
			}
		} else if (match) {
			out.push_back(clone_topdecl(*td));
		}
	}
}

static void scan_namespace_paths(const std::vector<Token> &toks, std::vector<std::string> &file_mod,
								 std::vector<std::vector<std::string>> &nested) {
	file_mod.clear();
	size_t i = 0;
	size_t n = toks.size();

	auto   skip_attrs = [&]() {
		while (i + 1 < n && toks[i].kind == TokenKind::Hash &&
			   toks[i + 1].kind == TokenKind::LBracket) {
			i += 2; // consume `#[`
			int depth = 1;
			for (; i < n && depth > 0; ++i) {
				if (toks[i].kind == TokenKind::LBracket) {
					++depth;
				} else if (toks[i].kind == TokenKind::RBracket) {
					--depth;
				}
			}
		}
	};

	skip_attrs();
	if (i >= n || toks[i].kind != TokenKind::KwStruct) {
		return;
	}
	++i; // consume `struct`
	std::vector<std::string> segs;
	while (i < n && toks[i].kind == TokenKind::Ident) {
		segs.push_back(toks[i].text);
		++i;
		if (i < n && toks[i].kind == TokenKind::Dot) {
			++i;
			continue;
		}
		break;
	}
	file_mod = segs;
	if (i >= n) {
		return;
	}
	if (toks[i].kind == TokenKind::Semicolon) {
		++i; // consume `;`
	} else if (toks[i].kind == TokenKind::LBrace) {
		++i; // consume the file-level module body's opening brace
	} else {
		return;
	}

	// Brace-depth scan of the rest of the file. Track a per-depth prefix
	// (the enclosing struct namespace's dotted path); a `struct <ident...> {` at depth d
	// opens depth d+1 with prefix[d] + ident-segs.
	struct Frame {
		std::vector<std::string> prefix;
		bool					 is_mod;
	};
	std::vector<Frame> stack;
	stack.push_back({file_mod, false}); // depth-0 prefix is the file namespace

	bool					 saw_struct = false;
	std::vector<std::string> pending;

	auto					 reset_pending = [&]() {
		saw_struct = false;
		pending.clear();
	};
	auto namespace_body = [&](size_t open) {
		int depth = 1;
		for (size_t j = open + 1; j < n && depth > 0; ++j) {
			if (toks[j].kind == TokenKind::LBrace) {
				++depth;
				continue;
			}
			if (toks[j].kind == TokenKind::RBrace) {
				--depth;
				continue;
			}
			if (depth == 1) {
				switch (toks[j].kind) {
				case TokenKind::KwPub:
				case TokenKind::KwFn:
				case TokenKind::KwMacro:
				case TokenKind::KwExtern:
				case TokenKind::KwLet:
				case TokenKind::KwConst:
				case TokenKind::KwThreadLocal:
				case TokenKind::KwStatic:
				case TokenKind::KwEnum:
				case TokenKind::KwImport:
				case TokenKind::KwStruct:
				case TokenKind::KwIf:
					return true;
				default:
					break;
				}
			}
		}
		return false;
	};

	for (; i < n; ++i) {
		TokenKind k = toks[i].kind;
		switch (k) {
		case TokenKind::KwStruct:
			saw_struct = true;
			pending.clear();
			break;
		case TokenKind::Ident:
			if (saw_struct) {
				pending.push_back(toks[i].text);
			} else {
				reset_pending();
			}
			break;
		case TokenKind::Dot:
			// dotted path inside a struct namespace name -- keep collecting.
			break;
		case TokenKind::LBrace: {
			if (saw_struct && !pending.empty() && namespace_body(i)) {
				std::vector<std::string> newp = stack.back().prefix;
				for (auto &s : pending) {
					newp.push_back(s);
				}
				nested.push_back(newp);
				stack.push_back({std::move(newp), true});
			} else {
				stack.push_back({stack.back().prefix, false});
			}
			reset_pending();
			break;
		}
		case TokenKind::RBrace:
			if (stack.size() > 1) {
				stack.pop_back();
			}
			reset_pending();
			break;
		case TokenKind::Semicolon:
			reset_pending();
			break;
		case TokenKind::Hash:
			// Attribute span that may appear before any nested decl. Skip
			// `#[ ... ]` so any stray namespace-like token inside its args is ignored.
			if (i + 1 < n && toks[i + 1].kind == TokenKind::LBracket) {
				i += 2;
				int depth = 1;
				for (; i < n && depth > 0; ++i) {
					if (toks[i].kind == TokenKind::LBracket) {
						++depth;
					} else if (toks[i].kind == TokenKind::RBracket) {
						--depth;
					}
				}
				--i; // compensate for loop's ++i
			} else {
				reset_pending();
			}
			break;
		case TokenKind::Eof:
			return;
		default:
			reset_pending();
			break;
		}
	}
}

void ModuleResolver::build_index() {
	if (index_built_) {
		return;
	}
	index_built_ = true;
	// Walk every import root for .sl files and lex each one (full parsing of
	// individual files happens lazily in load() via parse_file). The token
	// stream is scanned for the file-level `struct;` header and nested
	// namespace-capable `struct { }` blocks; those keys seed mod_index_.
	namespace fs = std::filesystem;
	for (const auto &root : roots_) {
		std::error_code ec;
		if (!fs::exists(root, ec) || ec) {
			continue;
		}
		fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied,
											ec),
			end;
		if (ec) {
			continue;
		}
		while (it != end) {
			const auto &p = it->path();
			if (p.extension() == ".sl") {
				std::error_code		  path_ec;
				std::filesystem::path canonical = fs::weakly_canonical(p, path_ec);
				std::string			  fpath		= path_ec ? p.string() : canonical.string();
				std::string			  src;
				if (read_file(fpath, src)) {
					std::vector<Token> toks;
					// Indexing is discovery only: malformed files are diagnosed when
					// the requested module is actually loaded, not for unrelated files.
					Lexer			   lex(src, fpath, false);
					if (lex.tokenize(toks)) {
						std::vector<std::string>			  file_mod;
						std::vector<std::vector<std::string>> nested;
						scan_namespace_paths(toks, file_mod, nested);
						if (!file_mod.empty()) {
							auto register_key = [&](const std::vector<std::string> &segs) {
								std::string key = join_path(segs);
								auto	   &v	= mod_index_[key];
								for (auto &f : v) {
									if (f == fpath) {
										return;
									}
								}
								v.push_back(fpath);
							};
							register_key(file_mod);
							for (auto &np : nested) {
								register_key(np);
							}
						}
					}
				}
			}
			it.increment(ec);
			if (ec) {
				break;
			}
		}
	}
}

Module *ModuleResolver::load(const std::vector<std::string> &mod_path) {
	(void)mod_path;
	return nullptr;
	/*
	build_index();
	std::string key = join_path(mod_path);
	auto		fit = merged_.find(key);
	if (fit != merged_.end()) {
		return fit->second;
	}

	auto it = mod_index_.find(key);
	if (it == mod_index_.end() || it->second.empty()) {
		return nullptr;
	}

	auto m		= std::make_unique<Module>();
	m->mod_path = mod_path;
	// Import roots are ordered: earlier roots are the user's explicit roots
	// and later roots are fallbacks.  Once a root provides a module, later
	// roots must not contribute another copy of that module.  Files from the
	// selected root are still merged, so module fragments within one root
	// continue to work.
	std::vector<std::filesystem::path> canonical_roots;
	canonical_roots.reserve(roots_.size());
	for (const auto &root : roots_) {
		std::error_code root_ec;
		canonical_roots.push_back(std::filesystem::weakly_canonical(root, root_ec));
		if (root_ec) {
			canonical_roots.back() = std::filesystem::path(root).lexically_normal();
		}
	}
	auto first_root_for = [&](const std::string &fpath) {
		std::error_code		  path_ec;
		std::filesystem::path source = std::filesystem::weakly_canonical(fpath, path_ec);
		if (path_ec) {
			source = std::filesystem::path(fpath).lexically_normal();
		}
		for (size_t i = 0; i < canonical_roots.size(); ++i) {
			std::filesystem::path relative = source.lexically_relative(canonical_roots[i]);
			if (relative.empty() || relative == "." || relative == "..") {
				continue;
			}
			auto component = relative.begin();
			if (component == relative.end() || *component == "..") {
				continue;
			}
			return i;
		}
		return canonical_roots.size();
	};
	size_t selected_root = canonical_roots.size();
	for (const auto &fpath : it->second) {
		selected_root = std::min(selected_root, first_root_for(fpath));
	}
	for (auto &fpath : it->second) {
		if (first_root_for(fpath) != selected_root) {
			continue;
		}
		Module *tree = parse_file(fpath);
		if (!tree) {
			continue;
		}
		std::vector<TopDeclPtr> collected;
		// Seed prefix with the file's own module path so file-level decls
		// match when qpath == tree->mod_path.
		collect_leaf_decls(tree->decls, tree->mod_path, mod_path, collected);
		for (auto &td : collected) {
			m->decls.push_back(std::move(td));
		}
		touched_files_.insert(fpath);
		merged_files_[key].push_back(fpath);
	}
	Module *raw = m.get();
	owned_.push_back(std::move(m));
	merged_[key] = raw;
	return raw;
	*/
}

Module *ModuleResolver::find_loaded(const std::vector<std::string> &mod_path) const {
	for (const auto &entry : file_trees_) {
		if (entry.second && entry.second->mod_path == mod_path) {
			return entry.second.get();
		}
	}
	return nullptr;
}

bool ModuleResolver::same_source_file(const Module &a, const Module &b) const {
	return !a.fpath.empty() && a.fpath == b.fpath;
/*

	auto files_for = [&](const Module &module) {
		std::set<std::string> files;
		auto it = merged_files_.find(join_path(module.mod_path));
		if (it != merged_files_.end()) {
			files.insert(it->second.begin(), it->second.end());
		}
		if (!module.fpath.empty()) {
			files.insert(module.fpath);
		}
		return files;
	};

	const auto a_files = files_for(a);
	const auto b_files = files_for(b);
	for (const auto &file : a_files) {
		if (b_files.count(file) != 0) {
			return true;
		}
	}
	return false; */
}

std::vector<Module *> ModuleResolver::load_descendants(const std::vector<std::string> &mod_path) {
	(void)mod_path;
	return {};
	/*
	build_index();
	std::vector<Module *> result;
	const std::string	  prefix = join_path(mod_path);

	// mod_index_ contains concrete declarations, so a strict dotted-prefix
	// match finds all modules belonging to an imported namespace.
	for (const auto &entry : mod_index_) {
		const std::string &key = entry.first;
		if (key.size() <= prefix.size() || key.compare(0, prefix.size(), prefix) != 0 ||
			key[prefix.size()] != '.') {
			continue;
		}
		Module *m = load(demangle(key));
		if (m) {
			result.push_back(m);
		}
	}
	return result; */
}

std::vector<Module *>
ModuleResolver::load_same_file_descendants(const std::vector<std::string> &mod_path) {
	(void)mod_path;
	return {};
	/*
	build_index();
	std::vector<Module *> result;
	std::set<std::string> source_files;
	const std::string	  parent = join_path(mod_path);
	auto				  files	 = merged_files_.find(parent);
	if (files == merged_files_.end()) {
		return result;
	}
	for (const auto &fpath : files->second) {
		source_files.insert(fpath);
	}
	for (const auto &entry : mod_index_) {
		const std::string &key = entry.first;
		if (key.size() <= parent.size() || key.compare(0, parent.size(), parent) != 0 ||
			key[parent.size()] != '.') {
			continue;
		}
		bool same_file = false;
		for (const auto &fpath : entry.second) {
			if (source_files.count(fpath) != 0) {
				same_file = true;
				break;
			}
		}
		if (same_file) {
			if (Module *child = load(demangle(key))) {
				result.push_back(child);
			}
		}
	}
	return result; */
}

std::vector<Module *> ModuleResolver::all() const {
	std::vector<Module *> r;
	for (const auto &entry : file_trees_) {
		if (entry.second) {
			r.push_back(entry.second.get());
		}
	}
	return r;
}

std::vector<std::string> ModuleResolver::all_files() const {
	std::vector<std::string> r(touched_files_.begin(), touched_files_.end());
	return r;
}

std::vector<std::string> ModuleResolver::all_module_files() const {
	return all_files();
}

std::vector<Module *> ModuleResolver::root_modules() const {
	std::vector<Module *> r;
	if (root_) {
		r.push_back(root_);
	}
	return r;
}

// Walk the namespace-capable struct tree recursively and record the full
// dotted qualified path of every nested struct inside the entry file. The file namespace
// itself is added separately (the entry file's mod_path) -- only nested
// blocks get walked here.
static void collect_nested_mod_paths(const std::vector<TopDeclPtr>	&decls,
									 const std::vector<std::string> &prefix,
									 std::vector<std::string>		&out) {
	for (auto &td : decls) {
		if (!td || td->kind != TopKind::Mod || !td->mod_decl || td->mod_decl->body.empty()) {
			continue;
		}
		std::vector<std::string> q = prefix;
		q.push_back(td->mod_decl->name);
		out.push_back(join_path(q));
		collect_nested_mod_paths(td->mod_decl->body, q, out);
	}
}

void ModuleResolver::set_root(std::unique_ptr<Module> m) {
	if (!m) {
		return;
	}
	std::error_code		  path_ec;
	std::filesystem::path canonical	  = std::filesystem::weakly_canonical(m->fpath, path_ec);
	std::string			  entry_fpath = path_ec ? m->fpath : canonical.string();
	m->fpath						  = entry_fpath;
	// The root is a canonical source file just like every imported file. Do
	// not merge namespace fragments or materialize cloned module trees.
	owned_.clear();
	merged_.clear();
	merged_files_.clear();
	root_keys_.clear();
	file_trees_.clear();
	touched_files_.clear();
	private_symbol_base_ = std::filesystem::path(entry_fpath).parent_path().string();
	m->private_symbol_module_id = private_module_identity(entry_fpath, private_symbol_base_);
	root_ = m.get();
	touched_files_.insert(entry_fpath);
	file_trees_[entry_fpath] = std::move(m);
	return;

	// Legacy namespace-merging implementation retained below for reference. The
	// active resolver above keeps each source file as its own module; a missing
	// header therefore remains the flat global namespace.
	if (m->mod_path.empty()) {
		log::report(log::Level::Error, SourceLoc{},
					"entry file must begin with a file-level 'struct <name>;' declaration");
		return;
	}

	merged_.clear();
	merged_files_.clear();
	owned_.clear();
	root_keys_.clear();
	std::string file_key = join_path(m->mod_path);
	root_keys_.push_back(file_key);
	collect_nested_mod_paths(m->decls, m->mod_path, root_keys_);

	// Cache the entry tree as a file-tree, but materialize the root modules
	// directly from this file. Other files declaring the same logical module
	// are fragments of that module and must be visible while compiling the
	// entry fragment on its own (for example, snprintf.sl referring to
	// vsnprintf.sl).
	build_index();
	std::vector<std::string> root_fragment_files;
	if (auto index_it = mod_index_.find(file_key); index_it != mod_index_.end()) {
		std::vector<std::filesystem::path> canonical_roots;
		canonical_roots.reserve(roots_.size());
		for (const auto &root : roots_) {
			std::error_code root_ec;
			canonical_roots.push_back(std::filesystem::weakly_canonical(root, root_ec));
			if (root_ec) {
				canonical_roots.back() = std::filesystem::path(root).lexically_normal();
			}
		}
		auto first_root_for = [&](const std::string &fpath) {
			std::error_code path_ec;
			std::filesystem::path source = std::filesystem::weakly_canonical(fpath, path_ec);
			if (path_ec) {
				source = std::filesystem::path(fpath).lexically_normal();
			}
			for (size_t i = 0; i < canonical_roots.size(); ++i) {
				std::filesystem::path relative = source.lexically_relative(canonical_roots[i]);
				if (relative.empty() || relative == "." || relative == "..") {
					continue;
				}
				auto component = relative.begin();
				if (component == relative.end() || *component == "..") {
					continue;
				}
				return i;
			}
			return canonical_roots.size();
		};
		size_t selected_root = first_root_for(entry_fpath);
		if (selected_root == canonical_roots.size()) {
			for (const auto &fpath : index_it->second) {
				selected_root = std::min(selected_root, first_root_for(fpath));
			}
		}
		for (const auto &fpath : index_it->second) {
			if (first_root_for(fpath) != selected_root || fpath == entry_fpath) {
				continue;
			}
			root_fragment_files.push_back(fpath);
		}
	}
	for (const auto &fpath : root_fragment_files) {
		Module *tree = parse_file(fpath);
		if (!tree) {
			continue;
		}
		std::vector<TopDeclPtr> collected;
		collect_leaf_decls(tree->decls, tree->mod_path, m->mod_path, collected);
		for (auto &td : collected) {
			m->decls.push_back(std::move(td));
		}
		touched_files_.insert(fpath);
	}
	file_trees_[entry_fpath] = std::move(m);

	touched_files_.insert(entry_fpath);

	// Materialize the root module(s) so root() and root_modules() are valid
	// before sema phase 0 runs. The first key is the file module -- treat it
	// as `root()` so callers see the entry module. Imported modules are still
	// discovered from explicit import roots by load().
	root_ = nullptr;
	for (auto &k : root_keys_) {
		std::vector<std::string> segs = split_path(k);
		auto					 rm	  = std::make_unique<Module>();
		rm->mod_path				  = segs;
		rm->fpath					  = entry_fpath;
		Module *entry_tree			  = file_trees_.at(entry_fpath).get();
		collect_leaf_decls(entry_tree->decls, entry_tree->mod_path, segs, rm->decls);
		Module *raw = rm.get();
		owned_.push_back(std::move(rm));
		merged_[k] = raw;
		merged_files_[k].push_back(entry_fpath);
		if (k == file_key) {
			for (const auto &fpath : root_fragment_files) {
				merged_files_[k].push_back(fpath);
			}
		}
		if (!root_) {
			root_ = raw;
		}
	}
}

} // namespace stellac
