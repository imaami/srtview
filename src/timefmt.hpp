// timefmt.hpp -- cue time as text: "m:ss" or "h:mm:ss", optionally
// ".mmm".  Standard C++, no Qt: the same text names a cue to the
// model, in an artifact and on screen (timefmtq.hpp wraps it for
// the Qt layer).
#ifndef SRTVIEW_SRC_TIMEFMT_HPP_
#define SRTVIEW_SRC_TIMEFMT_HPP_

#include <cmath>
#include <cstdio>
#include <string>

// A cue time is finite, not negative and below a million hours --
// a parsed timestamp cannot exceed that, and the milliseconds of
// anything under it fit a long long, where a long is 32 bits on
// ILP32 targets and on Windows and overflows past 596 hours;
// anything else is the start of the file.
inline std::string fmt_time(double t, bool with_ms)
{
	constexpr double cap = 1e6 * 3600.0;
	long long ms = t > 0.0 && t < cap ? std::llround(t * 1000.0) : 0;
	long long const h = ms / 3600000;
	ms %= 3600000;
	long long const m = ms / 60000;
	ms %= 60000;
	long long const s = ms / 1000;
	ms %= 1000;
	char buf[48];
	int n = h > 0
		? std::snprintf(buf, sizeof buf, "%lld:%02lld:%02lld", h, m, s)
		: std::snprintf(buf, sizeof buf, "%lld:%02lld", m, s);
	if (with_ms)
		n += std::snprintf(buf + n, sizeof buf - std::size_t(n),
		                   ".%03lld", ms);
	return {buf, std::size_t(n)};
}

#endif // SRTVIEW_SRC_TIMEFMT_HPP_
