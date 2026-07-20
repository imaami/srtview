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
// pattern.  Topic references are an opt-in restriction: a video with
// none takes part in every topic.  They curate the exported
// artifact; interactive search is always corpus-wide.
//
// \{name:} inside a fragment references another topic and expands to
// its fragments wrapped in (?:...); \{ in any other shape is regex
// as written (an escaped brace), and \{name:\} is the escape hatch
// for literally matching the reference syntax.  References may point
// forward; unknown names and cycles are parse errors.
//
// Capture parens carry *export* semantics (export_plan()).  A topic
// referenced by another is a component: it forms no export grouping
// of its own, and the reference syntax always neutralises its
// capturedness -- \{A:} behaves as if A's body were non-capturing,
// whatever parens A's own definition uses.  A component earns a
// grouping back through acknowledgment parens in a topic that nobody
// references (a top-level topic): split such a topic's body at
// top-level |, and every branch that is exactly one capturing group
// "(...)" -- not "(?..." -- containing exactly one reference
// (arbitrary other regex allowed) acknowledges that component.  So
// "(\{A:})|(\{B:} and stuff)" acknowledges A and B, while
// "(\{A:}|\{B:})" and "\{A:}|x" acknowledge nothing.  Acknowledged
// components become named groups in the grouping's pattern, so a
// match attributes its hits to the components that fired.
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

// One export grouping: a top-level topic plus its acknowledged
// components (see the format notes above).  parts[i] is captured in
// pattern as the named group "g<i>"; a hit belongs to a component
// when its group participated in the match.
struct export_item {
	std::string              name;
	std::string              pattern;
	std::vector<std::string> parts;

	bool operator==(export_item const &) const = default;
};

// The export groupings of a document, in topic order.  Requires a
// doc that came through parse().
std::vector<export_item> export_plan(doc const &d);

// Canonical text form; parses back to an equal document.
std::string write(doc const &d);

} // namespace topics

#endif // SRTVIEW_SRC_TOPICS_HPP_
