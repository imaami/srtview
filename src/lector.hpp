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

#include <atomic>
#include <optional>
#include <string>
#include <string_view>

#include "decoder.hpp"
#include "ocr.hpp"

namespace ocr {

class lector
{
public:
	// Defaults inherit the reader's language ladder (ocr.cpp).
	// Construction is cheap on purpose: the model loads lazily,
	// on whichever single thread first performs or asks about it
	// -- megabytes of traineddata must never bill the owning (UI)
	// thread's clock.  The accessors below force that load and
	// share the performer's thread; wave() alone may come from
	// anywhere.
	explicit lector(char const *lang = nullptr,
	                char const *tessdata = nullptr);

	explicit operator bool() { return bool(engine()); }
	std::string_view error() { return engine().error(); }
	std::string datapath() { return engine().datapath(); }
	std::string_view lang() { return engine().lang(); }

	result perform(request const &r);

	// The shutdown wave-off, callable from any thread: the next
	// checkpoint inside perform() abandons the read with an
	// error, which is never cached, so a later session finishes
	// the thought.  There is no way back -- the lector dies with
	// the stack that waved.
	void wave() { m_bail.store(true); }

private:
	tess &engine();

	std::optional<std::string> m_lang;   // ctor args, kept for
	std::optional<std::string> m_data;   // the deferred build
	std::optional<tess>        m_tess;
	media::decoder             m_dec;
	std::atomic_bool           m_bail{false};
};

} // namespace ocr

#endif // SRTVIEW_SRC_LECTOR_HPP_
