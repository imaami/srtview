// loom.hpp -- the temporal weave over OCR moments: near-identical
// spans sighted at the same place across consecutive readings link
// into regions -- three-dimensional bounding boxes whose third axis
// is time.  Standard C++23, no Qt, no tesseract; spans come in as
// ocr.hpp declares them, regions come out with an rx::braid()
// consensus and a pattern matching every sighting.  A slide's title
// that fifty moments read with flickering garbage becomes one
// region carrying its majority text; scrolling content moves its
// box and correctly never merges.  Pure function of its input --
// same moments, same regions, byte for byte.
#ifndef SRTVIEW_SRC_LOOM_HPP_
#define SRTVIEW_SRC_LOOM_HPP_

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "ocr.hpp"

namespace ocr {

// A stretch of moments where one box read the same-ish text.  The
// box fields are the midpoint of each field's observed spread;
// jitter is the widest spread, a sensor-noise figure.  consensus
// is the weight-majority text; pattern matches every sighted
// variant (rx::braid()'s contract).
struct region {
	double      t0 = 0;         // seconds, first sighting
	double      t1 = 0;         // seconds, last sighting
	std::size_t sightings = 0;  // moments it was seen in
	std::string consensus;
	std::string pattern;
	int         x = 0;
	int         y = 0;
	int         w = 0;
	int         h = 0;
	int         jitter = 0;
};

// One video's moments -- seconds mapped to that reading's spans --
// woven into regions ordered by (t0, y, x).
std::vector<region> weave(
	std::map<double, std::vector<span>> const &moments);

} // namespace ocr

#endif // SRTVIEW_SRC_LOOM_HPP_
