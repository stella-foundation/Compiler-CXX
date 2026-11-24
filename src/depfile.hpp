#pragma once

#include <string>
#include <vector>

namespace stellac {

// Escape a single token for make depfile syntax:
//   backslash -> "\\", space -> "\ ", "$" -> "$$",
//   "#" -> "\#", ":" -> "\:"
std::string make_escape(const std::string &tok);

// Canonicalize a filesystem path (absolute, normalized). Non-existent
// suffixes are folded lexically; existing prefix resolved via weakly_canonical.
std::string canonicalize_dep_path(const std::string &p);

// Build the full depfile body. targets and prereqs are escaped &
// canonicalized inside this call. Returns one line:
//   "t1 t2: p1 p2 p3 ...\n"
std::string render_depfile(const std::vector<std::string> &targets,
						   const std::vector<std::string> &prereqs);

// Write a depfile to `path` containing the given targets/prereqs.
// Returns true on success. On failure, sets nothing (caller reports).
bool		write_depfile(const std::string &path, const std::vector<std::string> &targets,
						  const std::vector<std::string> &prereqs);

} // namespace stellac
