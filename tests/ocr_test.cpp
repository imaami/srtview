// ocr_test.cpp -- the tesseract containment vessel over synthetic
// pixels.  Standard C++; no Qt, no FFmpeg, no fixture files -- the
// recognition image is rendered from an embedded 5x7 bitmap font.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "ocr.hpp"

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

} // namespace

int main()
{
	test_lifecycle();

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
