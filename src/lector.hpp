// lector.hpp -- the corpus-facing OCR backend: asked for (video,
// frame time), it reads the on-screen text aloud.  Standard C++23
// over media::decoder and ocr::tess; no Qt.  One decode context,
// reopened on video switch (the grabber's own pattern), blocking
// on whatever worker thread owns the instance -- made to sit
// behind scribe<lector>.  Span boxes come back in source frame
// pixels whatever the upscale: the frame-addressed identity
// downstream consumers will key on.
#ifndef SRTVIEW_SRC_LECTOR_HPP_
#define SRTVIEW_SRC_LECTOR_HPP_

#include <string>
#include <string_view>

#include "decoder.hpp"
#include "ocr.hpp"
#include "scribe.hpp"

namespace ocr {

class lector
{
public:
	// Defaults inherit the reader's language ladder (ocr.cpp).
	explicit lector(char const *lang = nullptr,
	                char const *tessdata = nullptr);

	explicit operator bool() const { return bool(m_tess); }
	std::string_view error() const { return m_tess.error(); }
	std::string datapath() const { return m_tess.datapath(); }
	std::string_view lang() const { return m_tess.lang(); }

	result perform(request const &r);

private:
	tess           m_tess;
	media::decoder m_dec;
};

} // namespace ocr

#endif // SRTVIEW_SRC_LECTOR_HPP_
