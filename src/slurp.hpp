// slurp.hpp -- the one whole-file read of the standard C++ layer.
// Standard C++23, no Qt.  A read is bounded: a file past the cap
// is refused before any allocation and again on the bytes actually
// read, a read that fails midway is a failure, and out changes
// only on success, so a missing file tells apart from an empty one.
#ifndef SRTVIEW_SRC_SLURP_HPP_
#define SRTVIEW_SRC_SLURP_HPP_

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

inline bool slurp(std::filesystem::path const &path, std::string &out,
                  std::size_t cap
                  = std::numeric_limits<std::size_t>::max())
{
	std::error_code ec;
	auto const n = std::filesystem::file_size(path, ec);
	if (ec || n > cap)
		return false;
	std::ifstream f(path, std::ios::binary);
	if (!f)
		return false;
	std::string bytes(std::istreambuf_iterator<char>(f), {});
	if (f.bad() || bytes.size() > cap)
		return false;
	out = std::move(bytes);
	return true;
}

// By value for callers that treat missing and empty alike.
inline std::string slurp(std::filesystem::path const &path,
                         std::size_t cap
                         = std::numeric_limits<std::size_t>::max())
{
	std::string out;
	slurp(path, out, cap);
	return out;
}

#endif // SRTVIEW_SRC_SLURP_HPP_
