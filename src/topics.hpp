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

// The component topics -- referenced by another topic, so they form
// no grouping of their own: the supportive structure inside the
// exported tops.  Pointers into d, in topic order.
std::vector<topic const *> components(doc const &d);

// Adopts an on-the-spot search as an implicitly exported topic: the
// pattern becomes a single-fragment topic under a generated
// "adhocN" name, unless some topic already expands to the same
// pattern.  Patterns containing the \{name:} reference shape are
// refused: adopted text has not been through parse(), and expand()
// must never meet an unvalidated reference.  Patterns with edge
// whitespace are refused too: the file format strips it, which
// would break the write()/parse() round-trip (a deliberate edge
// space is written [ ] or \x20, per the format notes).  The stem
// names the family: "adhoc" for committed searches, "focus" for
// LLM-hypothesized threads.
bool adopt(doc &d, std::string const &pattern,
           char const *stem = "adhoc");

// The deterministic comparison key of a pattern: top-level branches
// flattened out of full-span (?i:)/(?:)/() wrappers, each branch
// case-folded when its context is case-insensitive -- an outer (?i,
// or the [Xx] two-case class idiom, which is itself a deliberate
// case-covering mark -- and tagged so folded and unfolded branches
// never collide, then sorted, deduplicated and rejoined.  Keys
// exist to COMPARE patterns ("fuzzy but accurate"): they are never
// stored, exported or hashed into an identity.  Structurally
// doubtful input is its own key, collapsing to exact comparison.
std::string normal_key(std::string_view pattern);

// Adopts only what the corpus does not already cover: branches of
// the pattern whose normal_key() a topic's expansion already has
// are dropped, and the remainder -- rebuilt from the surviving
// branches in original order, re-wrapped (?i:...) when the source
// was -- is adopted under the stem like adopt().  Returns the
// adopted text, empty when nothing novel remained or adoption was
// refused.
std::string adopt_novel(doc &d, std::string const &pattern,
                        char const *stem);

// True when name is a stem followed by one or more ASCII digits --
// the shape adopt()'s name generator mints ("term3", "focus12"),
// how a reloaded corpus's machine topics are recognized.
bool stem_name(std::string_view name, std::string_view stem);

// Extends a machine topic in place: branches of pattern whose
// normal_key() no topic's expansion already covers append to the
// named topic's single fragment as alternation, keeping its text
// and (?i:) wrap.  Refused -- returning empty -- for unknown names,
// multi-fragment or reference-carrying targets, a case context
// differing from the pattern's, patterns adopt() would refuse, and
// structural doubt on either side; empty also when nothing novel
// remained.  Otherwise returns the fragment's new text.
// extendable() exposes the refusal guards alone: a caller about to
// make an irreversible move on the strength of an extension can
// ask first, and distinguish refusal from nothing-novel.
bool extendable(doc const &d, std::string_view name,
                std::string const &pattern);
std::string extend(doc &d, std::string_view name,
                   std::string const &pattern);

// The stem-generated topic whose expansion covers the most branches
// of pattern, ties to the earliest in doc order -- the only order
// two sessions share.  Empty when no stem topic covers any branch;
// hand-written topics never resolve.
std::string cover_of(doc const &d, std::string const &pattern,
                     char const *stem);

// The tidied text of a machine-written pattern: the same branch set
// with redundant full-span wrappers flattened and repeated branches
// dropped -- (?i:(a|b|a)) becomes (?i:a|b) -- and, when the exact
// combination comes out shorter, the branches the rx subset
// re-opens re-knit into one: (?i:word|words) leaves as
// (?i:words?).  Matching semantics are preserved; structural doubt
// returns the input unchanged.  For model output only: user-typed
// patterns are never rewritten.
std::string tidy(std::string const &pattern);

// Canonical text form; parses back to an equal document.
std::string write(doc const &d);

// The gloss sidecar dialect: name -> definition text in the same
// two-level dash list as the topic file, children being prose lines
// (blank lines cannot be represented; edge whitespace strips).  The
// file lives beside the corpus as <stem>.gloss and is edited only
// outside the app, so this side only reads: tolerant parse (junk
// skipped), entries usable whether or not their name is currently
// a topic.
struct gloss_entry {
	std::string              name;
	std::vector<std::string> lines;

	bool operator==(gloss_entry const &) const = default;
};

std::vector<gloss_entry> parse_gloss(std::string_view text);

} // namespace topics

#endif // SRTVIEW_SRC_TOPICS_HPP_
