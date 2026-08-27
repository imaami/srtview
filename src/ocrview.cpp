// ocrview.cpp -- the detached proof of the OCR pipeline: post
// frame timestamps of one video into scribe<lector>, drain notes
// as the poke lands them, print what the frames say.  Standard
// C++23 over the ocr and media modules; no Qt.  Deliberately not
// part of srtview -- this is the knob-evaluation harness (page
// layouts, upscales, tessdata flavors) and stays a standalone
// tool.
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <semaphore>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "decoder.hpp"
#include "lector.hpp"
#include "scribe.hpp"

namespace {

constexpr std::string_view kLay[] = {"any", "block", "sparse",
                                     "line"};
static_assert(std::size(kLay) == ocr::layout_count,
              "kLay mirrors ocr::layout");

struct args {
	std::vector<std::int64_t> at;
	char const  *video = nullptr;
	char const  *lang = nullptr;  // default: the reader's ladder
	char const  *tessdata = nullptr;
	ocr::options opts;
	int          scale = 2;
	bool         ok = false;
};

bool num(std::string_view s, std::int64_t &v)
{
	auto const [p, ec] = std::from_chars(s.data(),
	                                     s.data() + s.size(), v);
	return ec == std::errc() && p == s.data() + s.size()
	    && v >= 0;
}

// "a,b,c" -> values appended; false on any dud or on nothing.
bool nums(std::string_view s, std::vector<std::int64_t> &out)
{
	while (!s.empty()) {
		std::size_t const cut = s.find(',');
		std::int64_t v = 0;
		if (!num(s.substr(0, cut), v))
			return false;
		out.push_back(v);
		if (cut == std::string_view::npos)
			break;
		s.remove_prefix(cut + 1);
		if (s.empty())
			return false;         // a trailing comma promises
	}                                     // a field it never gives
	return !out.empty();
}

bool lay_of(std::string_view s, ocr::layout &l)
{
	for (std::size_t i = 0; i < ocr::layout_count; ++i) {
		if (kLay[i] != s)
			continue;
		l = ocr::layout(i);
		return true;
	}
	return false;
}

bool rect_of(std::string_view s, ocr::options &o)
{
	std::vector<std::int64_t> v;
	if (!nums(s, v) || v.size() != 4)
		return false;
	for (std::int64_t const n : v)
		if (!std::in_range<int>(n))
			return false;
	o.rx = int(v[0]);
	o.ry = int(v[1]);
	o.rw = int(v[2]);
	o.rh = int(v[3]);
	return true;
}

args parse(int argc, char **argv)
{
	args a;
	int i = 1;
	for (; i + 1 < argc && argv[i][0] == '-' && argv[i][1]; ++i) {
		std::string_view const flag = argv[i];
		char const *const val = argv[++i];
		std::int64_t v = 0;
		if (flag == "-l")
			a.lang = val;
		else if (flag == "-d")
			a.tessdata = val;
		else if (flag == "-p") {
			if (!lay_of(val, a.opts.lay))
				return a;
		} else if (flag == "-s") {
			if (!num(val, v) || v < 1
			    || v > media::gray_scale_max)
				return a;
			a.scale = int(v);
		} else if (flag == "-r") {
			if (!rect_of(val, a.opts))
				return a;
		} else
			return a;
	}
	if (i + 2 != argc)
		return a;
	a.video = argv[i];
	a.ok = nums(argv[i + 1], a.at);
	return a;
}

int usage()
{
	std::fputs("usage: ocrview [-l lang] [-d tessdata]"
	           " [-p any|block|sparse|line] [-s 1..4]"
	           " [-r x,y,w,h] video ms[,ms...]\n", stderr);
	return 2;
}

} // namespace

int main(int argc, char **argv)
{
	args const a = parse(argc, argv);
	if (!a.ok)
		return usage();

	ocr::lector back(a.lang, a.tessdata);
	if (!back) {
		std::fprintf(stderr, "ocrview: %.*s\n",
		             int(back.error().size()),
		             back.error().data());
		return 1;
	}
	std::fprintf(stderr, "ocrview: tesseract %s, lang %.*s\n",
	             ocr::tess::version(),
	             int(back.lang().size()), back.lang().data());

	std::counting_semaphore<> poked{0};
	ocr::scribe<ocr::lector> desk(back,
		[](void *ctx) noexcept {
			static_cast<std::counting_semaphore<> *>(ctx)
				->release();
		}, &poked);

	for (std::int64_t const ms : a.at) {
		ocr::request r;
		r.video = a.video;
		r.ms = ms;
		r.opts = a.opts;
		r.scale = std::uint8_t(a.scale);
		desk.post(std::move(r));
	}

	using clock = std::chrono::steady_clock;
	auto last = clock::now();
	int bad = 0;
	for (std::size_t got = 0; got < a.at.size();) {
		poked.acquire();
		for (ocr::note const &n : desk.drain()) {
			auto const now = clock::now();
			double const wall = std::chrono::duration<
				double, std::milli>(now - last).count();
			last = now;
			++got;
			if (!n.res.err.empty()) {
				std::fprintf(stderr, "%lldms: %s\n",
				             static_cast<long long>(
				                     n.r.ms),
				             n.res.err.c_str());
				++bad;
				continue;
			}
			std::printf("%lldms  wall %.0fms  conf %.1f"
			            "  %zu lines\n",
			            static_cast<long long>(n.r.ms),
			            wall, n.res.conf,
			            n.res.lines.size());
			for (ocr::span const &s : n.res.lines)
				std::printf("  [%4d,%4d %4dx%3d %5.1f]"
				            " %s\n",
				            s.x, s.y, s.w, s.h, s.conf,
				            s.text.c_str());
		}
	}

	return bad ? 1 : 0;
}
