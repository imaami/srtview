// ocr.cpp -- see ocr.hpp.  The one place in the tree that includes
// a tesseract or leptonica header; every library quirk -- call
// ordering, malloc'd text, the DPI guesser, unclamped rectangles,
// both libraries' console chatter -- is absorbed here.
#include <leptonica/allheaders.h>
#include <tesseract/baseapi.h>
#include <tesseract/resultiterator.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ocr.hpp"

namespace ocr {

namespace {

// ocr::layout -> tesseract page segmentation, same order.
constexpr tesseract::PageSegMode kPsm[] = {
	tesseract::PSM_AUTO,
	tesseract::PSM_SINGLE_BLOCK,
	tesseract::PSM_SPARSE_TEXT,
	tesseract::PSM_SINGLE_LINE,
};
static_assert(std::size(kPsm) == layout_count,
              "kPsm mirrors ocr::layout");

// SetSourceResolution when the view brings no hint: realistic for
// screen content and above the library's estimate-and-warn floor.
constexpr int kPpi = 96;

std::string_view trim(std::string_view s)
{
	constexpr std::string_view ws = " \t\r\n";
	std::size_t const a = s.find_first_not_of(ws);
	if (a == std::string_view::npos)
		return {};
	return s.substr(a, s.find_last_not_of(ws) - a + 1);
}

// Every line the recognizer saw, trimmed, with its box and
// confidence; returns the confidence sum for the caller's mean.
double take_lines(tesseract::TessBaseAPI &api,
                  std::vector<span>      &out)
{
	std::unique_ptr<tesseract::ResultIterator> const it{
		api.GetIterator()};
	if (!it)
		return 0;
	double sum = 0;
	do {
		std::unique_ptr<char[]> const raw{
			it->GetUTF8Text(tesseract::RIL_TEXTLINE)};
		std::string_view const text =
			raw ? trim(raw.get()) : std::string_view();
		if (text.empty())
			continue;
		int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
		it->BoundingBox(tesseract::RIL_TEXTLINE,
		                &x1, &y1, &x2, &y2);
		float const conf =
			it->Confidence(tesseract::RIL_TEXTLINE);
		sum += conf;
		out.push_back({std::string(text),
		               x1, y1, x2 - x1, y2 - y1, conf});
	} while (it->Next(tesseract::RIL_TEXTLINE));
	return sum;
}

} // namespace

struct tess::guts {
	tesseract::TessBaseAPI api;
	std::string            err;
};

tess::tess(char const *lang, char const *tessdata)
	: m(std::make_unique<guts>())
{
	if (!lang)
		lang = "eng";
	// Leptonica's only call in the tree: real-world frames make
	// it announce box-clip recoveries and such on stderr.
	setMsgSeverity(L_SEVERITY_NONE);
	if (m->api.Init(tessdata, lang, tesseract::OEM_LSTM_ONLY)) {
		m->err = "no model for lang=";
		m->err += lang;
		m->err += " under ";
		m->err += tessdata ? tessdata : "the default tessdata";
		return;
	}
	// Tesseract's own chatter (diacritic counts, sliver-line
	// complaints) rides its debug stream; route it to the bit
	// bucket.  After Init on purpose -- a failed Init complains
	// usefully.
	m->api.SetVariable("debug_file", "/dev/null");
}

tess::~tess() = default;
tess::tess(tess &&) noexcept = default;
tess &tess::operator=(tess &&) noexcept = default;

tess::operator bool() const
{
	return m && m->err.empty();
}

std::string_view tess::error() const
{
	return m ? std::string_view(m->err) : std::string_view();
}

char const *tess::version()
{
	return tesseract::TessBaseAPI::Version();
}

result tess::read(view const &v, options const &o)
{
	result out;
	if (!m) {
		out.err = "moved-from engine";
		return out;
	}
	if (!m->err.empty()) {
		out.err = m->err;
		return out;
	}
	if (!v.data || v.w <= 0 || v.h <= 0 || v.stride < v.w) {
		out.err = "bad image view";
		return out;
	}

	tesseract::TessBaseAPI &api = m->api;
	api.SetPageSegMode(kPsm[std::size_t(o.lay)]);
	api.SetImage(v.data, v.w, v.h, 1, v.stride);
	api.SetSourceResolution(v.ppi > 0 ? v.ppi : kPpi);
	if (o.rw > 0 && o.rh > 0) {
		// Endpoints in 64-bit: an INT_MAX corner must land in
		// the refusal below, not in signed overflow.
		int const x0 = std::max(o.rx, 0);
		int const y0 = std::max(o.ry, 0);
		int const x1 = int(std::clamp<std::int64_t>(
			std::int64_t(o.rx) + o.rw, 0, v.w));
		int const y1 = int(std::clamp<std::int64_t>(
			std::int64_t(o.ry) + o.rh, 0, v.h));
		if (x0 >= x1 || y0 >= y1) {
			api.Clear();
			out.err = "roi outside the image";
			return out;
		}
		api.SetRectangle(x0, y0, x1 - x0, y1 - y0);
	}
	if (api.Recognize(nullptr)) {
		api.Clear();
		out.err = "recognition failed";
		return out;
	}

	double const sum = take_lines(api, out.lines);
	if (!out.lines.empty())
		out.conf = float(sum / double(out.lines.size()));
	api.Clear();
	return out;
}

} // namespace ocr
