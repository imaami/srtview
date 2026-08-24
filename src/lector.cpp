// lector.cpp -- see lector.hpp.
#include <algorithm>
#include <cstdint>
#include <string>

#include "lector.hpp"

namespace ocr {

namespace {

// The resolution story told to tesseract: screen content, scaled.
constexpr int kBasePpi = 96;

} // namespace

lector::lector(char const *lang, char const *tessdata)
	: m_tess(lang, tessdata)
{
}

result lector::perform(request const &r)
{
	result out;
	if (!m_tess) {
		out.err.assign(m_tess.error());
		return out;
	}
	if (m_dec.path() != r.video && !m_dec.open(r.video)) {
		out.err = "cannot open video: " + r.video;
		return out;
	}
	int const scale = std::clamp(int(r.scale), 1,
	                             media::gray_scale_max);
	media::gray g;
	if (!m_dec.gray_at(r.ms, scale, g)) {
		out.err = "cannot decode frame at "
		        + std::to_string(r.ms) + "ms";
		return out;
	}

	// The ROI arrives in frame pixels and tess sees view pixels;
	// the boxes make the return trip, edges rounded outward.
	// The clamp against the source frame runs in 64-bit before
	// the upscale, so no coordinate a caller can utter overflows
	// the multiplication.
	options o = r.opts;
	if (o.rw > 0 && o.rh > 0) {
		std::int64_t const fw = g.width / scale;
		std::int64_t const fh = g.height / scale;
		std::int64_t const x0 =
			std::clamp<std::int64_t>(o.rx, 0, fw);
		std::int64_t const y0 =
			std::clamp<std::int64_t>(o.ry, 0, fh);
		std::int64_t const x1 = std::clamp<std::int64_t>(
			std::int64_t(o.rx) + o.rw, x0, fw);
		std::int64_t const y1 = std::clamp<std::int64_t>(
			std::int64_t(o.ry) + o.rh, y0, fh);
		if (x0 >= x1 || y0 >= y1) {
			out.err = "roi outside the image";
			return out;
		}
		o.rx = int(x0 * scale);
		o.ry = int(y0 * scale);
		o.rw = int((x1 - x0) * scale);
		o.rh = int((y1 - y0) * scale);
	}
	out = m_tess.read({g.px.data(), g.width, g.height, g.width,
	                   kBasePpi * scale}, o);
	for (span &s : out.lines) {
		int const x1 = (s.x + s.w + scale - 1) / scale;
		int const y1 = (s.y + s.h + scale - 1) / scale;
		s.x /= scale;
		s.y /= scale;
		s.w = x1 - s.x;
		s.h = y1 - s.y;
	}

	return out;
}

} // namespace ocr
