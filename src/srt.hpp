// srt.hpp -- SRT cue model and parser.  Standard C++, no Qt: the UI
// converts at its own boundary.
//
// SubRip has no official specification; the format is defined by
// common practice.  The rules implemented here follow the de-facto
// references:
//   - Subtitle Edit format reference
//     https://subtitleedit.github.io/subtitleedit/reference/subrip.html
//   - Library of Congress format description fdd000569
//   - Wikipedia "SubRip" / Matroska subtitle notes
//
// The parser is a CRTP push parser: srt::parser<Derived> walks the
// document and emits derived().on_cue(start, end, text) per cue --
// statically dispatched, container-agnostic.  srt::parse() is the
// vector-collecting convenience built on it.
#ifndef SRTVIEW_SRC_SRT_HPP_
#define SRTVIEW_SRC_SRT_HPP_

#include "crtp.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace srt {

struct cue {
	double      start = 0.0;
	double      end   = 0.0;
	std::string text;              // UTF-8, '\n' line breaks
};

// Normalize raw file bytes to UTF-8.  Handles UTF-8 with or without
// BOM, UTF-16 LE/BE by BOM, and falls back to Windows-1252 (the
// documented legacy "ANSI" encoding) when the bytes are not valid
// UTF-8.
std::string to_utf8(std::string_view raw);

// Strict UTF-8 validity (rejects overlongs, surrogates, > U+10FFFF).
// Table-driven DFA, branch-free scan.
bool valid_utf8(std::string_view v);

// Escape one cue text for rich-text display, letting the SRT
// inline-tag subset back through: <i> <b> <u> and <font> with
// color/face/size attributes, per the reference; {\...} ASS override
// blocks, which carry positioning irrelevant to a transcript view,
// are dropped.  UTF-8 in, UTF-8 out.
std::string cue_html(std::string_view text);

// ------------------------------------------------- scanning detail --

namespace detail {

// Byte classification: one table, small bitmasks.
enum : std::uint8_t {
	b_digit = 1 << 0,              // 0-9
	b_space = 1 << 1,              // ' ' '\t'
	b_eol   = 1 << 2,              // '\r' '\n'
	b_trim  = 1 << 3,              // anything <= 0x20
};

inline constexpr auto cls = [] {
	std::array<std::uint8_t, 256> t{};
	for (int c = '0'; c <= '9'; ++c)
		t[c] |= b_digit;
	t[' '] |= b_space;
	t['\t'] |= b_space;
	t['\r'] |= b_eol;
	t['\n'] |= b_eol;
	for (int c = 0; c <= 0x20; ++c)
		t[c] |= b_trim;
	return t;
}();

constexpr bool in(char c, std::uint8_t mask)
{
	return (cls[static_cast<unsigned char>(c)] & mask) != 0;
}

inline std::string_view trim(std::string_view v)
{
	while (!v.empty() && in(v.front(), b_trim))
		v.remove_prefix(1);
	while (!v.empty() && in(v.back(), b_trim))
		v.remove_suffix(1);
	return v;
}

inline bool all_digits(std::string_view v)
{
	return !v.empty()
	    && std::ranges::all_of(v, [](char c) { return in(c, b_digit); });
}

// One line per call; CRLF, LF and bare-CR (classic Mac) files all
// occur in the wild per the reference.  Copyable: a copy is a peek.
struct line_cursor {
	std::string_view rest;

	bool next(std::string_view &line)
	{
		if (rest.empty())
			return false;
		const std::size_t n = rest.find_first_of("\r\n");
		line = rest.substr(0, n);
		rest.remove_prefix(std::min(n, rest.size()));
		drop_break();
		return true;
	}

private:
	void drop_break()
	{
		if (rest.starts_with("\r\n")) {
			rest.remove_prefix(2);
			return;
		}
		if (!rest.empty())
			rest.remove_prefix(1);
	}
};

// Byte scanner with an explicit receiver: every call site shows what
// advances.
struct scanner {
	const char *p;
	const char *end;

	explicit scanner(std::string_view v)
		: p(v.data()), end(v.data() + v.size()) {}

	void spaces()
	{
		while (p < end && in(*p, b_space))
			++p;
	}

	bool lit(std::string_view s)
	{
		if (!std::string_view(p, std::size_t(end - p)).starts_with(s))
			return false;
		p += s.size();
		return true;
	}

	bool one_of(std::string_view set)
	{
		if (p >= end || set.find(*p) == std::string_view::npos)
			return false;
		++p;
		return true;
	}

	// min..max digits; n reports how many were consumed
	bool digits(int min, int max, unsigned &v, int &n)
	{
		v = 0;
		n = 0;
		while (p < end && in(*p, b_digit) && n < max) {
			v = v * 10 + unsigned(*p - '0');
			++p;
			++n;
		}
		return n >= min;
	}
};

struct stamp_pair {
	double a = 0.0;
	double b = 0.0;
};

// "H+:MM:SS[,.]m{1,3}".  Reference form is zero-padded HH:MM:SS,mmm
// with a comma; longer hour fields, a period separator (a documented
// common deviation) and short millisecond fields are tolerated.
inline constexpr unsigned ms_scale[4]{0, 100, 10, 1};

inline bool stamp(scanner &sc, double &out)
{
	unsigned h, m, s, ms;
	int n;
	if (!sc.digits(1, 9, h, n) || !sc.lit(":"))
		return false;
	if (!sc.digits(2, 2, m, n) || !sc.lit(":"))
		return false;
	if (!sc.digits(2, 2, s, n) || !sc.one_of(",."))
		return false;
	if (!sc.digits(1, 3, ms, n))
		return false;
	out = h * 3600.0 + m * 60.0 + s + ms * ms_scale[n] / 1000.0;
	return true;
}

// Timecode line: "start --> end"; anything after the second stamp
// (SubRip's X1:/X2:/Y1:/Y2: positioning extension, or junk) ignored.
inline bool timecode_line(std::string_view line, stamp_pair &ts)
{
	scanner sc(line);
	sc.spaces();
	if (!stamp(sc, ts.a))
		return false;
	sc.spaces();
	if (!sc.lit("-->"))
		return false;
	sc.spaces();
	return stamp(sc, ts.b);
}

} // namespace detail

// ------------------------------------------------------------ parser --
// Block structure per the reference: sequence number, timecode line,
// text lines, blank-line separator.  Deviations handled:
//   - counters are optional and their values untrusted
//   - a timecode line anywhere starts a new cue (missing blank
//     separator is a documented common error)
//   - a digits-only line while collecting text is the next block's
//     counter only when a timecode line follows; otherwise it is
//     caption text (a lone "42" stays a caption)
//   - anything between blocks that is neither blank, counter nor
//     timecode is skipped (site banners, stray headers)

template <typename Derived>
class parser : public crtp<Derived, parser>
{
public:
	void parse(std::string_view utf8)
	{
		detail::line_cursor lc{utf8};
		for (std::string_view line; lc.next(line);)
			step(line, lc);
		flush();
	}

private:
	void step(std::string_view line, const detail::line_cursor &ahead)
	{
		detail::stamp_pair ts;
		if (detail::timecode_line(line, ts)) {
			flush();
			begin(ts);
			return;
		}
		if (!open_)
			return;                       // junk between blocks
		const std::string_view t = detail::trim(line);
		if (t.empty()) {
			flush();
			return;
		}
		if (counter_ahead(t, ahead)) {
			flush();
			return;
		}
		append(t);
	}

	// The peek is a copy of the cursor: reading it consumes nothing.
	static bool counter_ahead(std::string_view t, detail::line_cursor peek)
	{
		std::string_view next;
		detail::stamp_pair ts;
		return detail::all_digits(t) && peek.next(next)
		    && detail::timecode_line(next, ts);
	}

	void begin(detail::stamp_pair ts)
	{
		start_ = ts.a;
		end_ = ts.b;
		open_ = true;
	}

	void append(std::string_view t)
	{
		if (!text_.empty())
			text_ += '\n';
		text_ += t;
	}

	void flush()
	{
		if (!open_)
			return;
		this->impl().on_cue(start_, end_, std::move(text_));
		text_.clear();
		open_ = false;
	}

	double      start_ = 0.0;
	double      end_ = 0.0;
	std::string text_;
	bool        open_ = false;
};

// Vector-collecting convenience over the push parser.
std::vector<cue> parse(std::string_view utf8);

} // namespace srt

#endif // SRTVIEW_SRC_SRT_HPP_
