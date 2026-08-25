// ocr.hpp -- text recognition over borrowed pixels: the tesseract
// containment vessel.  Standard C++23, no Qt, no FFmpeg; tesseract
// and leptonica types never escape ocr.cpp, which feeds the library
// raw gray8 buffers and never touches a Pix.  LSTM-only on purpose:
// no adaptive classifier and no cross-image state, so a result is a
// pure function of (pixels, options, lang, tessdata) -- the identity
// a future cache can key on.  Blocking by design: read() runs on
// whatever worker thread owns the instance; one tess per thread,
// never shared.
#ifndef SRTVIEW_SRC_OCR_HPP_
#define SRTVIEW_SRC_OCR_HPP_

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ocr {

// Borrowed gray8 pixels, consumed entirely within read().
struct view {
	std::uint8_t const *data = nullptr;
	int w = 0;
	int h = 0;
	int stride = 0;
	int ppi = 0;         // resolution hint; 0 = assume screen 96
};

// Page shapes worth distinguishing here, mapped to tesseract
// segmentation modes in ocr.cpp: full auto segmentation, one text
// block (slides), scattered fragments (screencast UI), one line.
enum struct layout : std::uint8_t { any, block, sparse, line };
inline constexpr std::size_t layout_count = 4;

// The enum is public and arrives from callers; everything that
// indexes a layout-sized table checks this first.
inline constexpr bool valid_layout(layout l)
{
	return std::to_underlying(l) < layout_count;
}

// Bumped whenever any result-shaping behavior changes -- the
// upscale filter, the resolution hint, the trimming, the box
// conversion, the segmentation map.  Cached results carry it, so
// a pipeline change re-earns its slots exactly like a library or
// model upgrade does.
inline constexpr unsigned pipeline_version = 1;

struct options {
	int    rx = 0;       // region of interest in view pixels;
	int    ry = 0;       // zero size = the whole image
	int    rw = 0;
	int    rh = 0;
	layout lay = layout::any;

	friend bool operator==(options const &,
	                       options const &) = default;
};

// One recognized text line.
struct span {
	std::string text;    // UTF-8, edge-trimmed
	int   x = 0;         // box in the pixels read() saw
	int   y = 0;
	int   w = 0;
	int   h = 0;
	float conf = 0;      // tesseract line confidence, 0..100
};

struct result {
	std::vector<span> lines;     // reading order
	std::string       err;       // empty on success
	float             conf = 0;  // mean over lines, 0 when none
};

using ticket = std::uint64_t;    // 0 marks a refused post -- and,
                                 // on a note, a planned reading

// The unit of work: which video, which moment, how to look.  Pure
// data -- the mailbox that queues it lives in scribe.hpp.  The
// defaulted equality is work identity: coalescing in the mailbox,
// the cache key in the archive.  The ROI rides in source frame
// pixels; the backend maps it through its own upscale.
struct request {
	std::string  video;      // path; posters resolve ids
	std::string  id;         // discovery identity, poster-resolved;
	                         // the cache key downstream, empty =
	                         // uncacheable
	std::int64_t ms = 0;     // frame time, decoder semantics
	options      opts;
	std::uint8_t scale = 2;  // decode upscale, backend clamps

	friend bool operator==(request const &,
	                       request const &) = default;
};

// One finished reading, echoing what was asked.
struct note {
	request r;
	result  res;
	ticket  t = 0;
};

// What a reading pipeline asks of whoever actually reads frames --
// the lector in the app, the archive decorating any of them, a
// fake in the tests.
template <class B>
concept backend = requires(B &b, request const &r) {
	{ b.perform(r) } -> std::same_as<result>;
};

class tess
{
public:
	// nullptr tessdata means the system default.  A named lang
	// loads that language only; nullptr walks the default ladder
	// -- fin first, the language this corpus reads best under,
	// its English included, then eng, the model every install
	// has -- and lang() names whichever loaded.  A failed load
	// leaves the instance false with error() set, and read()
	// reports it per call.
	explicit tess(char const *lang = nullptr,
	              char const *tessdata = nullptr);
	~tess();

	tess(tess &&) noexcept;
	tess &operator=(tess &&) noexcept;

	explicit operator bool() const;
	std::string_view error() const;

	// The directory Init resolved the models from, and the
	// language it actually loaded; both empty while the engine
	// is down.  Callers derive model identity from them.
	std::string datapath() const;
	std::string_view lang() const;

	result read(view const &v, options const &o);

	static char const *version();

private:
	struct guts;
	std::unique_ptr<guts> m;
};

} // namespace ocr

#endif // SRTVIEW_SRC_OCR_HPP_
