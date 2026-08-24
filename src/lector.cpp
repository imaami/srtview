// lector.cpp -- see lector.hpp.
#include <algorithm>
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
	options o = r.opts;
	o.rx *= scale;
	o.ry *= scale;
	o.rw *= scale;
	o.rh *= scale;
	out = m_tess.read({g.px.data(), g.width, g.height, g.width,
	                   kBasePpi * scale}, o);
	for (span &s : out.lines) {
		s.x /= scale;
		s.y /= scale;
		s.w = (s.w + scale - 1) / scale;
		s.h = (s.h + scale - 1) / scale;
	}

	return out;
}

} // namespace ocr
