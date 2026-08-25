// archive.hpp -- the OCR out-tray that remembers: a caching
// decorator satisfying the same one-call backend concept it wraps,
// so scribe<archive<lector>> reads a frame once, ever.  Standard
// C++23 over std::filesystem; no Qt, no tesseract, no FFmpeg.
// Results key on (discovery id, ms, layout, scale) -- the knobs
// that change the pixels the recognizer saw -- and the header
// stamp (tool version plus label) is enforced on read, so a slot
// written by another recognizer configuration reads as a miss and
// is re-earned.  Runs on the scribe's worker thread.  Stores go
// through a tmp-then-rename (the vault executor's pattern): a
// crash can never leave a truncated slot that passes validation,
// and a corrupt or oversized one reads as a miss and is rewritten.
// Empty results cache too: a textless frame must not re-OCR every
// session.  Requests without an id and requests with an ROI bypass
// the cache untouched; errors are never stored.
#ifndef SRTVIEW_SRC_ARCHIVE_HPP_
#define SRTVIEW_SRC_ARCHIVE_HPP_

#include <charconv>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>

#include "decoder.hpp"
#include "ocr.hpp"
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
	// The label provider is called once, lazily, on the thread
	// that performs -- it may do IO (the app hashes the model
	// file there), which is exactly why it must not run at
	// construction on the owner's thread.  perform() runs on one
	// thread by contract (the scribe's worker), so the lazy
	// build needs no lock.
	archive(std::string root, std::string (*label)(void *),
	        void *ctx, B &inner)
		: m_b(inner), m_root(std::move(root)),
		  m_label(label), m_ctx(ctx) {}

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
	// no identity, an ROI (a later phase's concern, if ever), or
	// a layout the tables cannot index.
	std::filesystem::path place(request const &r) const
	{
		if (r.id.empty() || (r.opts.rw > 0 && r.opts.rh > 0)
		    || !valid_layout(r.opts.lay))
			return {};
		constexpr char lay[] = "absl";
		static_assert(sizeof lay - 1 == layout_count,
		              "lay mirrors ocr::layout");
		int const scale = media::gray_scale(int(r.scale));
		return m_root / r.id
		     / (std::to_string(r.ms) + '.'
		        + lay[std::size_t(r.opts.lay)]
		        + char('0' + scale) + ".txt");
	}

	// The stamp, built on first use from the provided label.
	std::string const &stamp()
	{
		if (m_stamp.empty()) {
			m_stamp = "tesseract ";
			m_stamp += tess::version();
			m_stamp += ' ';
			m_stamp += m_label ? m_label(m_ctx)
			                   : std::string("unlabeled");
			m_stamp += " pipe ";
			m_stamp += std::to_string(pipeline_version);
		}
		return m_stamp;
	}

	// Strict on purpose: a header from any other recognizer
	// configuration, any malformed line, a torn tail, an
	// implausibly large slot, or a span no recognizer could have
	// emitted -- the store is world-editable state -- all fail
	// into a miss.  The mean confidence is derived, never stored.
	std::optional<result> load(std::filesystem::path const &p)
	{
		constexpr std::size_t kSlotCap = std::size_t{1} << 20;
		std::string all;
		if (!slurp(p, all, kSlotCap))
			return {};
		std::string_view rest(all);
		std::string_view line;
		if (!detail::next_line(rest, line) || line != stamp())
			return {};
		result out;
		double sum = 0;
		while (detail::next_line(rest, line)) {
			span s;
			if (!(detail::eat(line, s.x)
			      && detail::eat(line, s.y)
			      && detail::eat(line, s.w)
			      && detail::eat(line, s.h)
			      && detail::eat(line, s.conf))
			    || line.empty())
				return {};
			if (s.x < 0 || s.y < 0 || s.w <= 0 || s.h <= 0
			    || !(s.conf >= 0.0f && s.conf <= 100.0f))
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
	           result const           &res)
	{
		std::string text = stamp();
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
		// Whole slot or no slot: build beside, rename over.  A
		// crash mid-write leaves only a tmp nothing ever reads.
		// The tmp name carries the writer's pid: two instances
		// share the cache (the srtjump sibling shares players),
		// and a shared tmp could interleave into a mixed slot
		// that validates.  Within one process the scribe's
		// single worker serializes stores.
		std::error_code ec;
		std::filesystem::create_directories(p.parent_path(), ec);
		std::filesystem::path const tmp =
			p.parent_path() / (p.filename().string() + ".tmp."
			                   + std::to_string(getpid()));
		std::ofstream f(tmp, std::ios::binary
		                     | std::ios::trunc);
		f.write(text.data(), std::streamsize(text.size()));
		// close() flushes the buffered tail; only a stream still
		// clean after it has proven every byte reached the
		// filesystem.  The destructor's flush would fail past
		// the rename, and a torn tmp renamed over the slot is
		// the very hole the tmp exists to close.
		f.close();
		if (f.fail()) {
			std::filesystem::remove(tmp, ec);
			return;
		}
		std::filesystem::rename(tmp, p, ec);
	}

	B                    &m_b;
	std::filesystem::path m_root;
	std::string         (*m_label)(void *);
	void                 *m_ctx;
	std::string           m_stamp;   // lazy; empty = not yet built
};

} // namespace ocr

#endif // SRTVIEW_SRC_ARCHIVE_HPP_
