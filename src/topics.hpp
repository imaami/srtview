// topics.hpp -- topic-file model, parser, expander and writer.
// Standard C++, no Qt: the UI converts at its own boundary.
//
// A topic file is the hand-written source a study corpus is built
// from: a playlist of videos and a set of named regexes ("topics",
// PCRE2 syntax as consumed by QRegularExpression) that apply to
// them.  The format is a two-level dash list:
//
//   # comment (full line only: '#' is meaningful inside a regex)
//   - phone
//     - ([Aa]n )?i[Pp]hone
//     - |([Aa] )?([Ss]amsung|(1|[Oo]ne)[Pp]lus)
//
//   - userof
//     - uses? (\{phone:})
//
//   - file:///home/alice/video1.mp4
//     - file:///home/alice/subs/1.srt
//     - userof
//
// Heads ("- " at column 0) are videos when they start with file://,
// topic definitions otherwise.  Children (indented "- ") are regex
// fragments under a topic, concatenated in order; under a video they
// are the subtitle file (file://, at most one; omitted means derive
// by swapping the video's extension) or the names of the topics that
// apply -- names only, never inline regexes, so a mistyped file://
// line fails loudly as an unknown topic instead of becoming a silent
// pattern.
//
// \{name:} inside a fragment references another topic and expands to
// its fragments wrapped in (?:...); \{ in any other shape is regex
// as written (an escaped brace), and \{name:\} is the escape hatch
// for literally matching the reference syntax.  References may point
// forward; unknown names and cycles are parse errors.
//
// Every line is stripped of edge whitespace (CR included), so a
// deliberate leading or trailing space in a pattern must be written
// [ ] or \x20.  Paths are kept verbatim (no percent decoding);
// relative paths are the caller's to resolve against the file's
// directory.  write() emits a canonical form -- topics first, then
// videos -- that parses back to an equal document.
#ifndef SRTVIEW_SRC_TOPICS_HPP_
#define SRTVIEW_SRC_TOPICS_HPP_

#include <string>
#include <string_view>
#include <vector>

namespace topics {

struct topic {
	std::string              name;
	std::vector<std::string> fragments; // concatenated in order

	bool operator==(topic const &) const = default;
};

struct video {
	std::string              path;   // after file://, verbatim
	std::string              srt;    // empty: derive from path
	std::vector<std::string> topics; // names, validated to exist

	bool operator==(video const &) const = default;
};

struct doc {
	std::vector<topic> topics;
	std::vector<video> videos;

	bool operator==(doc const &) const = default;
};

// Failed parses name the first offending 1-based line; value is
// meaningful only when error is empty.
struct result {
	doc         value;
	std::string error;
	int         line = 0;
};

result parse(std::string_view text);

// The named topic, or nullptr.
topic const *find(doc const &d, std::string_view name);

// The fully expanded pattern of a topic: fragments concatenated,
// references replaced.  Requires a doc that came through parse().
std::string expand(doc const &d, topic const &t);

// Canonical text form; parses back to an equal document.
std::string write(doc const &d);

} // namespace topics

#endif // SRTVIEW_SRC_TOPICS_HPP_
