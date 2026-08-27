// loom_test.cpp -- unit tests for the temporal weave.  Standard
// C++ like the module itself: no Qt, no tesseract -- spans are
// hand-built fixtures modeled on the jitter a live corpus showed:
// stable slide lines with flickering garbage characters, dropped
// letters, whitespace wobble, single-pixel box tremble, and the
// occasional frame read as noise.
#include "loom.hpp"

#include <cstdio>
#include <limits>
#include <map>
#include <regex>
#include <string>
#include <vector>

namespace {

int g_fail = 0;

void check(bool ok, char const *what)
{
	std::printf("%s  %s\n", ok ? "OK  " : "FAIL", what);
	if (!ok)
		++g_fail;
}

ocr::span sp(std::string text, int x, int y, int w, int h)
{
	ocr::span s;
	s.text = std::move(text);
	s.x = x;
	s.y = y;
	s.w = w;
	s.h = h;
	s.conf = 90.0f;
	return s;
}

using moments = std::map<double, std::vector<ocr::span>>;

void testStableSlide()
{
	// One slide title over five moments, box trembling by a pixel,
	// one moment reading a garbage '4' into it.
	moments m;
	m[10.0] = {sp("$Functions: Stack", 100, 50, 400, 30)};
	m[12.0] = {sp("$Functions: Stack", 101, 50, 399, 30)};
	m[14.0] = {sp("$4Functions: Stack", 100, 51, 401, 31)};
	m[16.0] = {sp("$Functions: Stack", 100, 50, 400, 30)};
	m[18.0] = {sp("$Functions:  Stack", 99, 50, 400, 30)};
	auto const r = ocr::weave(m);
	check(r.size() == 1, "one slide weaves to one region");
	if (r.size() != 1)
		return;
	check(r[0].t0 == 10.0 && r[0].t1 == 18.0
	      && r[0].sightings == 5,
	      "the region spans every sighting");
	check(r[0].consensus == "$Functions: Stack",
	      "the majority text wins the consensus");
	check(r[0].jitter <= 2, "pixel tremble reads as tiny jitter");
	std::regex const re(r[0].pattern);
	bool all = true;
	for (auto const &[at, spans] : m)
		for (ocr::span const &s : spans)
			all = all && std::regex_match(s.text, re);
	check(all, "the pattern matches every sighting");
}

void testTwoLines()
{
	// Two stable lines per frame stay two regions, never one.
	moments m;
	for (double at : {5.0, 7.0, 9.0}) {
		m[at] = {sp("Outline: C to ASM", 80, 40, 300, 24),
		         sp("Class Admin", 80, 90, 200, 24)};
	}
	auto const r = ocr::weave(m);
	check(r.size() == 2, "two lines weave to two regions");
	check(r.size() == 2 && r[0].y < r[1].y,
	      "regions order by position");
	check(r.size() == 2 && r[0].consensus == "Outline: C to ASM"
	      && r[1].consensus == "Class Admin",
	      "each region keeps its own text");
}

void testSlideChange()
{
	// The same box showing different text is a new region: slides
	// succeed each other in place.
	moments m;
	m[10.0] = {sp("Outline: C to ASM", 80, 40, 300, 24)};
	m[12.0] = {sp("Outline: C to ASM", 80, 40, 300, 24)};
	m[14.0] = {sp("Ghidra Exercise Tips", 80, 40, 300, 24)};
	m[16.0] = {sp("Ghidra Exercise Tips", 80, 40, 300, 24)};
	auto const r = ocr::weave(m);
	check(r.size() == 2, "changed text starts a new region");
	check(r.size() == 2 && r[0].t1 < r[1].t0,
	      "the regions do not overlap in time");
}

void testScrolling()
{
	// Scrolling content moves its box past the slack: no merging.
	moments m;
	m[1.0] = {sp("mov eax, dword ptr [rbp]", 60, 300, 350, 20)};
	m[2.0] = {sp("mov eax, dword ptr [rbp]", 60, 200, 350, 20)};
	m[3.0] = {sp("mov eax, dword ptr [rbp]", 60, 100, 350, 20)};
	auto const r = ocr::weave(m);
	check(r.size() == 3, "scrolling text never links");
}

void testDropout()
{
	// One unreadable frame does not end the slide; two do.
	moments m;
	m[10.0] = {sp("Class Admin", 80, 90, 200, 24)};
	m[12.0] = {};                            // noise frame
	m[14.0] = {sp("Class Admin", 80, 90, 200, 24)};
	m[16.0] = {};
	m[18.0] = {};
	m[20.0] = {sp("Class Admin", 80, 90, 200, 24)};
	auto const r = ocr::weave(m);
	check(r.size() == 2, "one gap bridges, two seal");
	check(r.size() == 2 && r[0].t1 == 14.0 && r[0].sightings == 2
	      && r[1].t0 == 20.0 && r[1].sightings == 1,
	      "the bridged region spans the gap it survived");
}

void testGarbageFrame()
{
	// A frame misread wholesale becomes its own singleton region
	// while the real chain rides the dropout allowance across it.
	moments m;
	m[10.0] = {sp("Function: C to ASM", 80, 40, 300, 24)};
	m[12.0] = {sp("cS ee eae", 82, 44, 290, 22)};
	m[14.0] = {sp("Function: C to ASM", 80, 40, 300, 24)};
	auto const r = ocr::weave(m);
	check(r.size() == 2, "a garbage read stays a singleton");
	bool bridged = false, noise = false;
	for (ocr::region const &g : r) {
		if (g.sightings == 2 && g.consensus == "Function: C to ASM")
			bridged = true;
		if (g.sightings == 1 && g.consensus == "cS ee eae")
			noise = true;
	}
	check(bridged && noise,
	      "the real chain bridges over the noise");
}

void testTimeGap()
{
	// The same box, the same text, an unsampled hour apart -- a
	// replay's ends before the plan fills the middle: two regions,
	// so no window between them inherits unobserved evidence.
	moments m;
	m[10.0] = {sp("Title Slide", 80, 40, 300, 24)};
	m[3600.0] = {sp("Title Slide", 80, 40, 300, 24)};
	auto const r = ocr::weave(m);
	check(r.size() == 2 && r[0].t1 == 10.0 && r[1].t0 == 3600.0,
	      "an unsampled gap parts the chain");
}

void testExtremes()
{
	// The archive validates signs, not magnitudes: a corrupt slot
	// can carry any positive coordinate, and sealing it must not
	// overflow on the way to a midpoint.
	int const big = std::numeric_limits<int>::max();
	moments m;
	m[1.0] = {sp("zzz", big, big, big, big)};
	m[3.0] = {sp("zzz", big, big, big, big)};
	auto const r = ocr::weave(m);
	check(r.size() == 1 && r[0].x == big && r[0].h == big
	      && r[0].jitter == 0,
	      "INT_MAX boxes seal without overflow");
}

void testEmpty()
{
	check(ocr::weave({}).empty(), "no moments weave to nothing");
	moments m;
	m[1.0] = {};
	check(ocr::weave(m).empty(),
	      "textless moments weave to nothing");
}

void testDeterminism()
{
	moments m;
	m[10.0] = {sp("$Function: C to ASM", 100, 50, 400, 30),
	           sp("Compiled with gcc -O0", 100, 100, 380, 26)};
	m[12.0] = {sp("$tFunction: C to ASM", 101, 50, 399, 30),
	           sp("Compiled with gcc -O0", 100, 101, 380, 26)};
	m[14.0] = {sp("$Function: C to ASM", 100, 50, 400, 30),
	           sp("Compiled with gcc  -O0", 100, 100, 381, 26)};
	auto const a = ocr::weave(m);
	auto const b = ocr::weave(m);
	bool same = a.size() == b.size() && a.size() == 2;
	for (std::size_t i = 0; same && i < a.size(); ++i)
		same = a[i].consensus == b[i].consensus
		    && a[i].pattern == b[i].pattern
		    && a[i].t0 == b[i].t0 && a[i].t1 == b[i].t1
		    && a[i].x == b[i].x && a[i].jitter == b[i].jitter;
	check(same, "the weave is a pure function of its moments");
	if (a.size() != 2)
		return;
	std::regex const re(a[0].pattern);
	check(std::regex_match("$Function: C to ASM", re)
	      && std::regex_match("$tFunction: C to ASM", re)
	      && !std::regex_match("$Function: C to AS", re),
	      "the jitter family's pattern spans it exactly");
}

} // namespace

int main()
{
	testStableSlide();
	testTwoLines();
	testSlideChange();
	testScrolling();
	testDropout();
	testGarbageFrame();
	testTimeGap();
	testExtremes();
	testEmpty();
	testDeterminism();
	std::printf("%s\n", g_fail ? "FAILURES" : "all ok");
	return g_fail;
}
