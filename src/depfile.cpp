#include "depfile.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <set>
#include <unistd.h>

namespace stellac {

std::string make_escape(const std::string &tok) {
	std::string out;
	out.reserve(tok.size());
	for (char c : tok) {
		switch (c) {
		case '\\':
			out += "\\\\";
			break;
		case ' ':
			out += "\\ ";
			break;
		case '\t':
			out += "\\\t";
			break;
		case '$':
			out += "$$";
			break;
		case '#':
			out += "\\#";
			break;
		case '%':
			out += "\\%";
			break;
		case '*':
		case '?':
		case '[':
			out += '\\';
			out += c;
			break;
		case ':':
			out += "\\:";
			break;
		default:
			out += c;
			break;
		}
	}
	return out;
}

std::string canonicalize_dep_path(const std::string &p) {
	std::error_code ec;
	// weakly_canonical handles both existing and partially-existing paths:
	// existing prefix resolved against the real fs; nonexistent tail folded
	// lexically.
	auto			can = std::filesystem::weakly_canonical(p, ec);
	if (ec) {
		// Fall back to a lexical-only normalization if fs resolution fails.
		can = std::filesystem::path(p).lexically_normal();
	}
	return can.string();
}

std::string render_depfile(const std::vector<std::string> &targets,
						   const std::vector<std::string> &prereqs) {
	std::vector<std::string> canonical_targets;
	std::set<std::string>	 seen_targets;
	for (const auto &target : targets) {
		std::string canonical = canonicalize_dep_path(target);
		if (seen_targets.insert(canonical).second) {
			canonical_targets.push_back(std::move(canonical));
		}
	}
	// Canonicalize + dedup + sort prereqs.
	std::set<std::string> seen;
	for (const auto &p : prereqs) {
		seen.insert(canonicalize_dep_path(p));
	}

	std::string out;
	// Targets (escaped, joined by space).
	for (size_t i = 0; i < canonical_targets.size(); ++i) {
		if (i) {
			out += ' ';
		}
		out += make_escape(canonical_targets[i]);
	}
	out += ':';
	// Prereqs (escaped, sorted via std::set ordering).
	for (const auto &p : seen) {
		out += ' ';
		out += make_escape(p);
	}
	out += '\n';
	return out;
}

bool write_depfile(const std::string &path, const std::vector<std::string> &targets,
				   const std::vector<std::string> &prereqs) {
	static std::atomic<unsigned long long> sequence{0};
	std::filesystem::path				   destination(path);
	std::filesystem::path temporary;
	int fd = -1;
	for (;;) {
		temporary = destination;
		temporary += ".tmp." + std::to_string(static_cast<unsigned long long>(getpid())) + "." +
			std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
		fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
		if (fd >= 0 || errno != EEXIST) {
			break;
		}
	}
	if (fd < 0) {
		return false;
	}
	const std::string contents = render_depfile(targets, prereqs);
	size_t offset = 0;
	while (offset < contents.size()) {
		auto written = ::write(fd, contents.data() + offset, contents.size() - offset);
		if (written < 0 && errno == EINTR) {
			continue;
		}
		if (written <= 0) {
			close(fd);
			std::error_code ignored;
			std::filesystem::remove(temporary, ignored);
			return false;
		}
		offset += static_cast<size_t>(written);
	}
	if (close(fd) != 0) {
		std::error_code ignored;
		std::filesystem::remove(temporary, ignored);
		return false;
	}

	// Rename within the destination directory so successful updates are
	// atomic on platforms whose filesystem rename replaces an existing file.
	std::error_code ec;
	std::filesystem::rename(temporary, destination, ec);
	if (!ec) {
		return true;
	}

	// Windows does not replace an existing destination with rename(). Preserve
	// the no-partial-file guarantee, accepting a brief replacement window on
	// that platform.
	std::error_code remove_ec;
	if (std::filesystem::remove(destination, remove_ec) && !remove_ec) {
		ec.clear();
		std::filesystem::rename(temporary, destination, ec);
		if (!ec) {
			return true;
		}
	}
	std::error_code ignored;
	std::filesystem::remove(temporary, ignored);
	return false;
}

} // namespace stellac
