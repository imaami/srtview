// rx.hpp -- mechanical regex algebra over finite string sets: no
// semantics, no heuristics about meaning, just lossless compression
// of string lists into minimal patterns.  Standard C++23, no Qt.
// knit() turns a set of literal words into the shortest pattern of
// the portable subset -- literals, [classes], ?, (?:groups) -- that
// matches exactly those words (word|words -> words?, analyse|analyze
// -> analy[sz]e); unknit() inverts it, decoding a pattern of that
// same subset back into its finite match set, so a knitted pattern
// re-opens for the next spelling and the compression stays
// convergent instead of one-way.  braid() merges near-identical
// variants of one string (OCR jitter) into a consensus plus a
// pattern tolerating the observed substitutions, dropouts and
// whitespace wobble; alike() is the whitespace-normalized
// similarity both braid and its callers gate on.
#ifndef SRTVIEW_SRC_RX_HPP_
#define SRTVIEW_SRC_RX_HPP_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rx {

// A pattern matching exactly the given literal strings and nothing
// else, in the portable subset valid under PCRE2 and ECMAScript
// alike.  Safe to splice as one branch of an alternation.  Empty
// input and a lone empty word both yield an empty pattern.
std::string knit(std::vector<std::string> words);

// The finite match set of a pattern knit() could have produced --
// including plain escaped literals from any escaper.  Empty when
// the pattern reaches beyond the subset or the set would exceed
// the expansion caps; the caller then treats it as opaque.
std::optional<std::vector<std::string>> unknit(std::string_view pattern);

// Whitespace-normalized similarity in [0, 1]: twice the longest
// common subsequence over the summed lengths, runs of whitespace
// squeezed to one space first.  1 is equality up to whitespace.
// Strings past the internal alignment cap compare by equality
// alone -- the quadratic table is never bought for them.
double alike(std::string_view a, std::string_view b);

// braid()'s result: the majority text and a pattern matching every
// braided variant.
struct weave {
	std::string consensus;
	std::string pattern;
};

// Merge near-identical variants of one string.  The most frequent
// variant (ties to the lexical minimum) seeds the alignment; every
// variant must clear the similarity floor against it or the braid
// refuses, returning the seed alone with its escaped pattern --
// braiding unrelated strings is not this function's job.
weave braid(std::vector<std::string> const &variants);

} // namespace rx

#endif // SRTVIEW_SRC_RX_HPP_
