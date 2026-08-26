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
{
	if (lang)
		m_lang.emplace(lang);
	if (tessdata)
		m_data.emplace(tessdata);
}

// The lazy model load: one calling thread by contract -- the
// performing worker, or the harness's main -- so no lock and no
// atomics guard it; wave() from elsewhere touches only the bail
// flag and never this.
tess &lector::engine()
{
	if (!m_tess)
		m_tess.emplace(m_lang ? m_lang->c_str() : nullptr,
		               m_data ? m_data->c_str() : nullptr);
	return *m_tess;
}

result lector::perform(request const &r)
{
	result out;
	if (m_bail.load()) {
		out.err = "waved off";
		return out;
	}
	tess &t = engine();
	if (!t) {
		out.err.assign(t.error());
		return out;
	}
	if (m_dec.path() != r.video && !m_dec.open(r.video)) {
		out.err = "cannot open video: " + r.video;
		return out;
	}
	int const scale = media::gray_scale(int(r.scale));
	media::gray g;
	if (!m_dec.gray_at(r.ms, scale, g)) {
		out.err = "cannot decode frame at "
		        + std::to_string(r.ms) + "ms";
		return out;
	}

	// Between the decode and the read: a shutdown that arrived
	// during the seek walks away before paying for recognition.
	if (m_bail.load()) {
		out.err = "waved off";
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
	out = t.read({g.px.data(), g.width, g.height, g.width,
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
