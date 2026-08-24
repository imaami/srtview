// archive.hpp -- the OCR out-tray that remembers: a caching
// decorator satisfying the same one-call backend concept it wraps,
// so scribe<archive<lector>> reads a frame once, ever.  Standard
// C++23 over std::filesystem; no Qt, no tesseract, no FFmpeg.
// Results key on (discovery id, ms, layout, scale) -- the knobs
// that change the pixels the recognizer saw; the tool version and
// label ride in the file header as a record, never a key.  Runs on
// the scribe's worker thread.  A torn or corrupt slot reads as a
// miss and is rewritten -- self-healing, so plain writes suffice.
// Empty results cache too: a textless frame must not re-OCR every
// session.  Requests without an id and requests with an ROI bypass
// the cache untouched; errors are never stored.
#ifndef SRTVIEW_SRC_ARCHIVE_HPP_
#define SRTVIEW_SRC_ARCHIVE_HPP_

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "decoder.hpp"
#include "ocr.hpp"
#include "scribe.hpp"
#include "slurp.hpp"

namespace ocr {

namespace detail {

// One numeric field off the front, its trailing space eaten.
template <class T>
inline bool eat(std::string_view &s, T &v)
{
	auto const [p, ec] = std::from_chars(s.data(),
	                                     s.data() + s.size(), v);
	if (ec != std::errc() || p == s.data() + s.size() || *p != ' ')
		return false;
	s.remove_prefix(std::size_t(p - s.data()) + 1);
	return true;
}

// One '\n'-terminated line off the front; a tail without the
// terminator is a torn write, left behind as the caller's cue.
inline bool next_line(std::string_view &rest, std::string_view &line)
{
	std::size_t const nl = rest.find('\n');
	if (nl == std::string_view::npos)
		return false;
	line = rest.substr(0, nl);
	rest.remove_prefix(nl + 1);
	return true;
}

// Shortest exact float form: to_chars round-trips through
// from_chars bit-for-bit, and neither consults the locale.
inline void put(std::string &out, float v)
{
	char buf[32];
	auto const [p, ec] = std::to_chars(buf, buf + sizeof buf, v);
	out.append(buf, ec == std::errc() ? p : buf);
}

} // namespace detail

template <class B> requires backend<B>
class archive
{
public:
	archive(std::string root, std::string label, B &inner)
		: m_b(inner), m_root(std::move(root)),
		  m_label(std::move(label)) {}

	result perform(request const &r)
	{
		std::filesystem::path const p = place(r);
		if (!p.empty()) {
			if (std::optional<result> hit = load(p))
				return std::move(*hit);
		}
		result res = m_b.perform(r);
		if (!p.empty() && res.err.empty())
			store(p, res);
		return res;
	}

private:
	// The cache slot, or empty when the request is uncacheable:
	// no identity, or an ROI (a later phase's concern, if ever).
	std::filesystem::path place(request const &r) const
	{
		if (r.id.empty() || (r.opts.rw > 0 && r.opts.rh > 0))
			return {};
		constexpr char lay[] = "absl";
		static_assert(sizeof lay - 1 == layout_count,
		              "lay mirrors ocr::layout");
		int const scale = std::clamp(int(r.scale), 1,
		                             media::gray_scale_max);
		return m_root / r.id
		     / (std::to_string(r.ms) + '.'
		        + lay[std::size_t(r.opts.lay)]
		        + char('0' + scale) + ".txt");
	}

	// Strict on purpose: any malformed line, or a torn tail,
	// fails the whole slot into a miss.  The mean confidence is
	// derived, never stored.
	std::optional<result> load(std::filesystem::path const &p) const
	{
		std::string all;
		if (!slurp(p, all))
			return {};
		std::string_view rest(all);
		std::string_view line;
		if (!detail::next_line(rest, line)
		    || !line.starts_with("tesseract "))
			return {};
		result out;
		double sum = 0;
		while (detail::next_line(rest, line)) {
			span s;
			if (!(detail::eat(line, s.x)
			      && detail::eat(line, s.y)
			      && detail::eat(line, s.w)
			      && detail::eat(line, s.h)
			      && detail::eat(line, s.conf)))
				return {};
			s.text.assign(line);
			sum += s.conf;
			out.lines.push_back(std::move(s));
		}
		if (!rest.empty())
			return {};
		if (!out.lines.empty())
			out.conf = float(sum / double(out.lines.size()));
		return out;
	}

	void store(std::filesystem::path const &p,
	           result const           &res) const
	{
		std::string text = "tesseract ";
		text += tess::version();
		text += ' ';
		text += m_label;
		text += '\n';
		for (span const &s : res.lines) {
			text += std::to_string(s.x);
			text += ' ';
			text += std::to_string(s.y);
			text += ' ';
			text += std::to_string(s.w);
			text += ' ';
			text += std::to_string(s.h);
			text += ' ';
			detail::put(text, s.conf);
			text += ' ';
			text += s.text;
			text += '\n';
		}
		std::error_code ec;
		std::filesystem::create_directories(p.parent_path(), ec);
		std::ofstream f(p, std::ios::binary | std::ios::trunc);
		f.write(text.data(), std::streamsize(text.size()));
	}

	B                    &m_b;
	std::filesystem::path m_root;
	std::string           m_label;
};

} // namespace ocr

#endif // SRTVIEW_SRC_ARCHIVE_HPP_
