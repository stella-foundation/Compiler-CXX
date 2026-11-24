#pragma once

#include "ast.hpp"
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace stellac {

// Read an entire file into `out`. Returns false (and leaves `out` untouched)
// on open or read failure. Single file-reading helper shared by main + resolver.
bool read_file(const std::string &path, std::string &out);

class ModuleResolver {
  public:
	void add_import_root(std::string dir) {
		roots_.push_back(std::move(dir));
	}

	// Load a source file named by an import. A file-level `struct <qualified>;`
	// header gives it an explicit namespace; without one, its declarations are
	// part of the flat global namespace.
	Module					*load(const std::vector<std::string> &mod_path);
	Module					*load_import(ImportDecl &import, const Module &from);
	std::vector<Module *> load_import_targets(ImportDecl &import, const Module &from);

	// Return a module already loaded by import resolution, without loading it.
	Module					*find_loaded(const std::vector<std::string> &mod_path) const;

	// Namespace modules materialized from the same source file share lexical
	// visibility, even when the logical module was loaded from imported files.
	bool					 same_source_file(const Module &a, const Module &b) const;

	// Load every concrete module below a namespace path.
	std::vector<Module *>	 load_descendants(const std::vector<std::string> &mod_path);

	// Load nested modules declared in the same source files as a module.
	std::vector<Module *>	 load_same_file_descendants(const std::vector<std::string> &mod_path);

	// All merged modules loaded so far, including the root.
	std::vector<Module *>	 all() const;

	// Every `.sl` source file that contributed to a loaded module (root +
	// transitively merged). Used for depfile prereqs.
	std::vector<std::string> all_files() const;

	// Every source file contributing to a loaded logical module, for link inputs.
	std::vector<std::string> all_module_files() const;

	// Set the root (entry) module manually. The supplied Module is the parsed
	// entry file. Nested namespace-capable structs inside the entry file are
	// also registered as root modules so codegen emits their bodies.
	void					 set_root(std::unique_ptr<Module> m);
	Module					*root() const {
		return root_;
	}

	// Modules belonging to the current translation-unit root set: the entry
	// file plus any nested namespace structs inside it. Codegen emits bodies
	// only for these.
	std::vector<Module *> root_modules() const;

  private:
	std::vector<std::string>						roots_;
	std::vector<std::unique_ptr<Module>>			owned_;
	// merged modules keyed by dotted qualified name (join_path(mod_path)).
	std::map<std::string, Module *>					merged_;
	std::map<std::string, std::vector<std::string>> merged_files_;
	Module										   *root_ = nullptr;
	std::vector<std::string>						root_keys_; // dotted names

	// file-scan index
	// Per-file parse cache (file path -> parsed Module). Built lazily by
	// build_index(); reused when merging.
	std::map<std::string, std::unique_ptr<Module>>	file_trees_;
	std::string private_symbol_base_;
	// qualified dotted name -> files that contain a namespace-capable struct.
	std::map<std::string, std::vector<std::string>> mod_index_;
	bool											index_built_ = false;
	// every file that has been parsed or merged (for depfile + dedup).
	std::set<std::string>							touched_files_;

	void											build_index();
	// Append leaf declarations from the subtree whose qualified path
	// matches `qpath` into `out`. Deep-clones the leaves so the cached source
	// tree (file_trees_) stays intact for repeated load() calls.
	void	collect_leaf_decls(const std::vector<TopDeclPtr>  &decls,
							   const std::vector<std::string> &prefix,
							   const std::vector<std::string> &qpath, std::vector<TopDeclPtr> &out);
	// Parse a single file, cache its tree, return pointer (null on failure).
	Module *parse_file(const std::string &fpath);
	std::string resolve_import_path(const std::string &written, const Module &from) const;
};

} // namespace stellac
