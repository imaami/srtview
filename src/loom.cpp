// loom.cpp -- see loom.hpp.  Chains grow greedily in reading
// order: each incoming span joins the first open chain whose
// anchor -- the box and text of the chain's first sighting -- it
// still resembles, so drift is bounded against the origin, not
// smeared one hop at a time until anything links to anything.
// The tolerances are sensor figures for observed OCR jitter, like
// the confidence floor: a box wobbles by single pixels between
// readings of a static slide, garbage characters dent similarity
// by a few percent, and an occasional unreadable frame drops a
// sighting without ending the slide.
#include <algorithm>
#include <numeric>
#include <tuple>
#include <utility>

#include "loom.hpp"
#include "rx.hpp"

namespace ocr {

namespace {

// A sighting must stay this alike to the chain's first text; well
// above rx::braid()'s own floor, so a woven chain has no practical
// way to refuse braiding (and an impractical one still seals with
// its majority text).
constexpr double kAlikeFloor = 0.9;

// Box agreement vs the anchor: within 8 px or a tenth of the
// anchor's span, whichever is looser -- x and w measured against
// the width, y and h against the height.
constexpr int kSlackPx  = 8;
constexpr int kSlackDiv = 10;

// Moments a chain may go unsighted and still continue: one, the
// occasional frame OCR reads as noise.
constexpr std::size_t kDropout = 1;

// A chain may bridge sampled dropouts, never unsampled time: two
// matching sightings an hour apart -- a manifest replay's ends
// before the reading plan fills the middle -- must not become one
// region whose whole stretch then enters every window in between
// as evidence nobody observed.  Past this gap the slide is met
// again as a new region, which its windows still receive.
constexpr double kMaxGapSeconds = 30.0;

struct chain {
	std::vector<std::string> texts;
	double      t0, t1;
	int         ax, ay, aw, ah;              // anchor box
	int         lox, hix, loy, hiy;          // observed spreads
	int         low, hiw, loh, hih;
	std::size_t idle;                        // moments unsighted
	bool        grew;                        // this moment

	chain(span const &s, double at)
		: t0(at), t1(at), ax(s.x), ay(s.y), aw(s.w), ah(s.h),
		  lox(s.x), hix(s.x), loy(s.y), hiy(s.y),
		  low(s.w), hiw(s.w), loh(s.h), hih(s.h),
		  idle(0), grew(true)
	{
		texts.push_back(s.text);
	}

	void grow(span const &s, double at)
	{
		texts.push_back(s.text);
		t1 = at;
		lox = std::min(lox, s.x); hix = std::max(hix, s.x);
		loy = std::min(loy, s.y); hiy = std::max(hiy, s.y);
		low = std::min(low, s.w); hiw = std::max(hiw, s.w);
		loh = std::min(loh, s.h); hih = std::max(hih, s.h);
		idle = 0;
		grew = true;
	}
};

bool near(int a, int b, int slack)
{
	return a - b <= slack && b - a <= slack;
}

bool fits(chain const &c, span const &s)
{
	int const sx = std::max(kSlackPx, c.aw / kSlackDiv);
	int const sy = std::max(kSlackPx, c.ah / kSlackDiv);
	return near(s.x, c.ax, sx) && near(s.w, c.aw, sx)
	    && near(s.y, c.ay, sy) && near(s.h, c.ah, sy);
}

region seal(chain &&c)
{
	region r;
	r.t0 = c.t0;
	r.t1 = c.t1;
	r.sightings = c.texts.size();
	// std::midpoint, not (lo + hi) / 2: the archive admits any
	// positive coordinate, and a corrupt slot at INT_MAX must not
	// buy signed overflow here.
	r.x = std::midpoint(c.lox, c.hix);
	r.y = std::midpoint(c.loy, c.hiy);
	r.w = std::midpoint(c.low, c.hiw);
	r.h = std::midpoint(c.loh, c.hih);
	r.jitter = std::max({c.hix - c.lox, c.hiy - c.loy,
	                     c.hiw - c.low, c.hih - c.loh});
	rx::weave w = rx::braid(c.texts);
	r.consensus = std::move(w.consensus);
	r.pattern = std::move(w.pattern);
	return r;
}

} // namespace

std::vector<region> weave(
	std::map<double, std::vector<span>> const &moments)
{
	std::vector<chain> open;
	std::vector<region> out;
	for (auto const &[at, spans] : moments) {
		for (chain &c : open)
			c.grew = false;
		for (span const &s : spans) {
			auto const takes = [&s, at](chain const &c) {
				return !c.grew
				    && at - c.t1 <= kMaxGapSeconds
				    && fits(c, s)
				    && rx::alike(c.texts.front(), s.text)
				       >= kAlikeFloor;
			};
			auto const it = std::ranges::find_if(open, takes);
			if (it != open.end())
				it->grow(s, at);
			else
				open.emplace_back(s, at);
		}
		// A chain unsighted past the dropout allowance seals; a
		// fresh one counts as sighted by construction.
		for (std::size_t i = open.size(); i-- > 0;) {
			chain &c = open[i];
			if (c.grew || ++c.idle <= kDropout)
				continue;
			out.push_back(seal(std::move(c)));
			open.erase(open.begin() + std::ptrdiff_t(i));
		}
	}
	for (chain &c : open)
		out.push_back(seal(std::move(c)));
	std::ranges::sort(out, {}, [](region const &r) {
		return std::tie(r.t0, r.y, r.x, r.consensus);
	});
	return out;
}

} // namespace ocr
