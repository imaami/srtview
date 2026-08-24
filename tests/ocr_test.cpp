// ocr_test.cpp -- the tesseract containment vessel over synthetic
// pixels, and the scribe's mailbox over a gated fake backend.
// Standard C++; no Qt, no FFmpeg, no fixture files -- the
// recognition image is rendered from an embedded 5x7 bitmap font.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <semaphore>
#include <string>
#include <string_view>
#include <vector>

#include "ocr.hpp"
#include "scribe.hpp"

namespace {

int g_fail = 0;

void check(bool ok, char const *what)
{
	std::printf("%s  %s\n", ok ? "OK  " : "FAIL", what);
	if (!ok)
		++g_fail;
}

void require(bool ok, char const *what)
{
	check(ok, what);
	if (!ok) {
		std::printf("FAILED (precondition)\n");
		std::exit(1);
	}
}

// A white gray8 canvas letters get stamped onto.
struct canvas {
	std::vector<std::uint8_t> px;
	int w = 0;
	int h = 0;

	canvas(int cw, int ch)
		: px(std::size_t(cw) * std::size_t(ch), 255),
		  w(cw), h(ch) {}

	ocr::view view() const { return {px.data(), w, h, w, 0}; }
};

// 5x7 glyphs for the letters the tests spell with; bit 4 is the
// left column.  Characters outside the set stay white (spaces).
constexpr char kOrder[] = "HELOWRD";
constexpr std::uint8_t kFont[7][7] = {
	{0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11},  // H
	{0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f},  // E
	{0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f},  // L
	{0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e},  // O
	{0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a},  // W
	{0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11},  // R
	{0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e},  // D
};

void fill(canvas &c, int x0, int y0, int cell)
{
	for (int y = 0; y < cell; ++y)
		for (int x = 0; x < cell; ++x)
			c.px[std::size_t(y0 + y) * std::size_t(c.w)
			     + std::size_t(x0 + x)] = 0;
}

void stamp(canvas &c, char ch, int ox, int oy, int cell)
{
	std::size_t const at = std::string_view(kOrder).find(ch);
	if (at == std::string_view::npos)
		return;
	for (int r = 0; r < 7; ++r)
		for (int col = 0; col < 5; ++col)
			if (kFont[at][r] >> (4 - col) & 1)
				fill(c, ox + col * cell,
				     oy + r * cell, cell);
}

canvas render(std::string_view text, int cell)
{
	int const margin = 4 * cell;
	canvas c(2 * margin + cell * (6 * int(text.size()) - 1),
	         2 * margin + 7 * cell);
	for (std::size_t i = 0; i < text.size(); ++i)
		stamp(c, text[i], margin + 6 * cell * int(i),
		      margin, cell);
	return c;
}

std::string upper_join(ocr::result const &res)
{
	std::string all;
	for (ocr::span const &s : res.lines) {
		all += s.text;
		all += ' ';
	}
	for (char &ch : all)
		if (ch >= 'a' && ch <= 'z')
			ch = char(ch - 32);
	return all;
}

void test_lifecycle()
{
	check(ocr::tess::version() && *ocr::tess::version(),
	      "version string present");

	ocr::tess bad("eng", "/nonexistent/tessdata");
	check(!bad, "bogus tessdata leaves the engine down");
	check(!bad.error().empty(), "and says why");
	ocr::result const r = bad.read(canvas(64, 64).view(), {});
	check(!r.err.empty() && r.lines.empty(),
	      "read on a down engine reports, not crashes");
}

void test_reads(ocr::tess &eng)
{
	canvas const blank(64, 64);
	ocr::result r = eng.read(blank.view(), {});
	check(r.err.empty(), "blank image: no error");
	check(r.lines.empty() && r.conf == 0, "blank image: no lines");

	r = eng.read({}, {});
	check(!r.err.empty(), "null view refused");

	ocr::options roi;
	roi.rx = 1000;
	roi.ry = 1000;
	roi.rw = 50;
	roi.rh = 50;
	r = eng.read(blank.view(), roi);
	check(!r.err.empty(), "roi outside the image refused");

	roi = {};
	roi.rx = -8;
	roi.ry = -8;
	roi.rw = 80;
	roi.rh = 80;
	r = eng.read(blank.view(), roi);
	check(r.err.empty(), "overhanging roi clamped, not fatal");

	roi = {};
	roi.rx = std::numeric_limits<int>::max() - 10;
	roi.rw = 100;
	roi.rh = 10;
	r = eng.read(blank.view(), roi);
	check(!r.err.empty(), "int-max corner refused, not overflowed");

	roi.rx = -2'000'000'000;
	r = eng.read(blank.view(), roi);
	check(!r.err.empty(), "far-left roi refused, not wrapped");
}

void test_recognition(ocr::tess &eng)
{
	canvas const c = render("HELLO WORLD", 12);
	ocr::options o;
	o.lay = ocr::layout::line;
	ocr::result const r = eng.read(c.view(), o);
	check(r.err.empty(), "pixel font: read completes");
	std::string const all = upper_join(r);
	check(all.find("HELLO") != std::string::npos
	      && all.find("WORLD") != std::string::npos,
	      "pixel font: HELLO WORLD comes back");
	bool boxed = !r.lines.empty();
	for (ocr::span const &s : r.lines)
		boxed = boxed && s.w > 0 && s.h > 0
		     && s.x >= 0 && s.x + s.w <= c.w
		     && s.y >= 0 && s.y + s.h <= c.h;
	check(boxed, "spans carry sane boxes");
	check(r.conf > 0, "confidence populated");
}

// The mailbox's backend stand-in: performs by echoing the request
// into one recognizable line.  entered is released as a job starts;
// when gated, each job then waits for one gate release -- the tests
// freeze the worker mid-job to build deterministic queue states.
struct fake {
	std::counting_semaphore<> entered{0};
	std::counting_semaphore<> gate{0};
	std::mutex                mtx;
	std::vector<std::string>  log;   // perform order, by video
	bool                      gated = false;

	ocr::result perform(ocr::request const &r)
	{
		entered.release();
		if (gated)
			gate.acquire();
		{
			std::lock_guard const lk(mtx);
			log.push_back(r.video);
		}
		ocr::result res;
		res.lines.push_back({r.video + "@"
		                     + std::to_string(r.ms),
		                     0, 0, 1, 1, 100.0f});
		res.conf = 100;
		return res;
	}
};

void poke_release(void *ctx)
{
	static_cast<std::counting_semaphore<> *>(ctx)->release();
}

ocr::request req(char const *video, std::int64_t ms = 0)
{
	ocr::request r;
	r.video = video;
	r.ms = ms;
	return r;
}

void test_mailbox_smoke()
{
	fake f;
	std::counting_semaphore<> poked{0};
	ocr::scribe<fake> s(f, poke_release, &poked);
	ocr::request const r = req("V", 1234);
	ocr::ticket const t = s.post(r);
	check(t != 0, "tickets start past zero");
	poked.acquire();
	std::vector<ocr::note> notes = s.drain();
	check(notes.size() == 1, "one note per post");
	check(notes[0].t == t && notes[0].r == r,
	      "note echoes ticket and request");
	check(notes[0].res.lines.size() == 1
	      && notes[0].res.lines[0].text == "V@1234",
	      "result carried through");
	check(s.drain().empty(), "drain empties the box");
}

void test_mailbox_order()
{
	fake f;
	f.gated = true;
	std::counting_semaphore<> poked{0};
	std::vector<ocr::note> notes;
	{
		ocr::scribe<fake> s(f, poke_release, &poked);
		s.post(req("A"));
		f.entered.acquire();          // A live, frozen
		s.post(req("E"));             // rest: E
		s.post(req("C"));             // rest: E C
		s.post(req("D"), true);       // rush: D
		s.post(req("C"), true);       // twin hoists C to rush
		f.gate.release(4);            // open all four jobs
		for (int i = 0; i < 4; ++i)   // one poke per job
			poked.acquire();
		notes = s.drain();
	}
	check(f.log == std::vector<std::string>({"A", "D", "C", "E"}),
	      "rush lane first, hoisted twin before the rest");
	check(notes.size() == 5, "five notes for five tickets");
	std::vector<ocr::ticket> ts;
	for (ocr::note const &n : notes)
		ts.push_back(n.t);
	std::ranges::sort(ts);
	check(std::ranges::adjacent_find(ts) == ts.end(),
	      "every ticket its own note");
	auto const of = [&notes](char const *v) {
		return std::size_t(std::ranges::count_if(notes,
			[v](ocr::note const &n) {
				return n.r.video == v;
			}));
	};
	check(of("C") == 2 && of("A") == 1 && of("D") == 1
	      && of("E") == 1, "the coalesced job notified twice");
}

void test_mailbox_cancel()
{
	fake f;
	f.gated = true;
	std::counting_semaphore<> poked{0};
	std::vector<ocr::note> notes;
	{
		ocr::scribe<fake> s(f, poke_release, &poked);
		ocr::ticket const ta = s.post(req("A"));
		f.entered.acquire();              // A live, frozen
		ocr::ticket const tb = s.post(req("B"));
		s.post(req("C"));
		check(s.cancel(tb), "queued ticket cancels");
		check(!s.cancel(tb), "second cancel finds nothing");
		check(!s.cancel(9999), "unknown ticket refused");
		check(s.cancel(ta), "running ticket cancels");
		f.gate.release(2);                // A (to nobody) + C
		poked.acquire();                  // only C pokes
		notes = s.drain();
	}
	check(f.log == std::vector<std::string>({"A", "C"}),
	      "dropped job never performs, running one finishes");
	check(notes.size() == 1 && notes[0].r.video == "C",
	      "cancelled work delivers nothing");
}

void test_mailbox_late_cancel()
{
	fake f;
	std::counting_semaphore<> poked{0};
	ocr::scribe<fake> s(f, poke_release, &poked);
	ocr::ticket const t = s.post(req("V", 1));
	poked.acquire();                  // completed, not drained
	check(s.cancel(t), "cancel scrubs a completed note");
	check(s.drain().empty(), "the scrubbed note never surfaces");
	check(!s.cancel(t), "and the ticket is truly gone");
}

void test_mailbox_teardown()
{
	fake f;
	f.gated = true;
	std::counting_semaphore<> poked{0};
	std::vector<ocr::note> notes;
	{
		ocr::scribe<fake> s(f, poke_release, &poked);
		s.post(req("A"));
		f.entered.acquire();          // A live, frozen
		s.post(req("B"));
		s.post(req("C"));
		s.stop();                     // wind-down beats the gate
		check(s.post(req("Z")) == 0, "post after stop refused");
		f.gate.release();             // A finishes and delivers
		poked.acquire();
		notes = s.drain();
	}                                     // dtor joins promptly
	check(f.log == std::vector<std::string>({"A"}),
	      "stop drops the backlog after the live job");
	check(notes.size() == 1 && notes[0].r.video == "A",
	      "the live job still delivered");
}

} // namespace

int main()
{
	test_lifecycle();
	test_mailbox_smoke();
	test_mailbox_order();
	test_mailbox_cancel();
	test_mailbox_late_cancel();
	test_mailbox_teardown();

	ocr::tess eng;
	require(bool(eng), "system tessdata provides eng");
	test_reads(eng);
	test_recognition(eng);

	if (g_fail)
		std::printf("%d FAILED\n", g_fail);
	else
		std::printf("all passed\n");
	return g_fail ? 1 : 0;
}
