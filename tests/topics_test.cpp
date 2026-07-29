// topics_test.cpp -- unit tests for the topic-file module.  Standard
// C++ like the module itself: builds without Qt, which is the proof
// of the boundary.
#include "topics.hpp"

#include <cstdio>
#include <string>

namespace {

int g_fail = 0;

void check(bool ok, char const *what)
{
	std::printf("%s  %s\n", ok ? "OK  " : "FAIL", what);
	if (!ok)
		++g_fail;
}

char const *const kSketch =
	"# the draft sketch, colon typo fixed\n"
	"- phone\n"
	"  - ([Aa]n )?i[Pp]hone|([Aa] )?([Ss]amsung|(1|[Oo]ne)[Pp]lus)\n"
	"\n"
	"- userof\n"
	"  - uses? (\\{phone:})\n"
	"\n"
	"- file:///home/alice/video1.mp4\n"
	"   - userof\n"
	"\n"
	"- file:///home/alice/video2.mp4\n"
	"  - file:///home/alice/subs/2.srt\n"
	"  - userof\n";

void testStructure()
{
	auto const r = topics::parse(kSketch);
	check(r.error.empty(), "sketch parses");
	auto const &d = r.value;
	check(d.topics.size() == 2 && d.videos.size() == 2,
	      "two topics, two videos");
	check(d.topics[0].name == "phone"
	      && d.topics[0].fragments.size() == 1
	      && d.topics[1].name == "userof",
	      "topic names and fragments");
	check(d.videos[0].path == "/home/alice/video1.mp4"
	      && d.videos[0].srt.empty()
	      && d.videos[0].topics == std::vector<std::string>{"userof"},
	      "video without srt override");
	check(d.videos[1].srt == "/home/alice/subs/2.srt",
	      "explicit srt override");

	auto const rt = topics::parse(topics::write(d));
	check(rt.error.empty() && rt.value == d,
	      "canonical form round-trips");
}

void testExpansion()
{
	auto const r = topics::parse("- a\n"
	                             "  - x+\n"
	                             "  - |y\n"
	                             "\n"
	                             "- b\n"
	                             "  - pre(\\{a:})post\n");
	check(r.error.empty(), "composed topics parse");
	auto const *a = topics::find(r.value, "a");
	auto const *b = topics::find(r.value, "b");
	check(a && topics::expand(r.value, *a) == "x+|y",
	      "fragments concatenate in order");
	check(b && topics::expand(r.value, *b) == "pre((?:x+|y))post",
	      "references expand wrapped in a group");
	check(topics::find(r.value, "c") == nullptr, "find misses politely");

	auto const esc = topics::parse("- a\n"
	                               "  - z\n"
	                               "- b\n"
	                               "  - a\\{2} \\{a:\\} \\{a:}\n");
	check(esc.error.empty(), "escaped braces parse");
	auto const *eb = topics::find(esc.value, "b");
	check(eb && topics::expand(esc.value, *eb)
	      == "a\\{2} \\{a:\\} (?:z)",
	      "only the exact reference shape expands");
}

void testForwardAndNesting()
{
	auto const r = topics::parse("- file:///v.mp4\n"
	                             "  - late\n"
	                             "\n"
	                             "- late\n"
	                             "  - inner(\\{later:})\n"
	                             "\n"
	                             "- later\n"
	                             "  - z\n");
	check(r.error.empty(), "forward references resolve");
	auto const *t = topics::find(r.value, "late");
	check(t && topics::expand(r.value, *t) == "inner((?:z))",
	      "nested expansion");
}

void testErrors()
{
	auto r = topics::parse("- v\n  - x\n\n"
	                       "- file:///a.mp4\n"
	                       "  - file///bad/subs.srt\n");
	check(!r.error.empty() && r.line == 5,
	      "srt typo fails loudly as an unknown topic");

	r = topics::parse("- a\n  - \\{missing:}\n");
	check(!r.error.empty() && r.line == 2,
	      "unknown fragment reference");

	r = topics::parse("- a\n  - \\{b:}\n- b\n  - \\{a:}\n");
	check(!r.error.empty(), "reference cycle");

	r = topics::parse("- a\n  - x\n- a\n  - y\n");
	check(!r.error.empty() && r.line == 3, "duplicate topic");

	r = topics::parse("- file:///v.mp4\n- file:///v.mp4\n");
	check(!r.error.empty() && r.line == 2, "duplicate video");

	r = topics::parse("- file:///v.mp4\n"
	                  "  - file:///a.srt\n  - file:///b.srt\n");
	check(!r.error.empty() && r.line == 3, "second subtitle file");

	r = topics::parse("- a\n");
	check(!r.error.empty() && r.line == 1, "topic without pattern");

	r = topics::parse("  - orphan\n");
	check(!r.error.empty() && r.line == 1, "child outside any block");

	r = topics::parse("- not a file so a topic\n  - x\n");
	check(!r.error.empty() && r.line == 1,
	      "whitespace in a topic name");

	r = topics::parse("plain text\n");
	check(!r.error.empty() && r.line == 1, "non-item line");

	r = topics::parse("- \n");
	check(!r.error.empty() && r.line == 1, "empty item");
}

void testExportPlan()
{
	auto const r = topics::parse(
		"- A\n  - alpha\n"
		"- B\n  - beta\n"
		"- C\n  - (\\{A:})|(\\{B:} and stuff)\n"
		"- D\n  - (\\{A:}|\\{B:})\n"
		"- lone\n  - solo\n");
	check(r.error.empty(), "plan corpus parses");
	auto const plan = topics::export_plan(r.value);
	check(plan.size() == 3, "components form no groupings");
	check(plan[0].name == "C" && plan[0].parts.size() == 2
	      && plan[0].parts[0] == "A" && plan[0].parts[1] == "B",
	      "whole-branch capture parens acknowledge components");
	check(plan[0].pattern
	      == "(?<g0>(?:alpha))|(?<g1>(?:beta) and stuff)",
	      "acknowledged parens become named groups");
	check(plan[1].name == "D" && plan[1].parts.empty()
	      && plan[1].pattern == "((?:alpha)|(?:beta))",
	      "a shared group acknowledges nothing");
	check(plan[2].name == "lone" && plan[2].parts.empty()
	      && plan[2].pattern == "solo",
	      "independent topics stay plain groupings");

	auto const edge = topics::parse(
		"- A\n  - a+\n"
		"- C\n  - \\{A:}|x\n"
		"- E\n  - ([\\|(]\\{A:})|(\\(\\{A:})\n");
	check(edge.error.empty(), "edge corpus parses");
	auto const ep = topics::export_plan(edge.value);
	check(ep.size() == 2, "referenced A is pruned");
	check(ep[0].parts.empty() && ep[0].pattern == "(?:a+)|x",
	      "bare references acknowledge nothing");
	check(ep[1].parts.size() == 2,
	      "classes and escapes do not hide real groups");
}

void testLexical()
{
	auto const r = topics::parse("\xEF\xBB\xBF"
	                             "# comment\r\n"
	                             "\r\n"
	                             "- a\r\n"
	                             "  - x \r\n"
	                             "\t- [ ]y\r\n");
	check(r.error.empty(), "BOM, CRLF, comments, blank lines");
	auto const *a = topics::find(r.value, "a");
	check(a && topics::expand(r.value, *a) == "x[ ]y",
	      "edge whitespace stripped, [ ] survives");

	auto const bare = topics::parse("- a\r  - x\r");
	check(bare.error.empty()
	      && topics::find(bare.value, "a") != nullptr,
	      "bare-CR line endings");
}

void testComponents()
{
	auto const r = topics::parse(kSketch);
	auto const c = topics::components(r.value);
	check(c.size() == 1 && c[0]->name == "phone",
	      "the referenced topic is the sole component");
	auto d = r.value;
	check(topics::adopt(d, "solo")
	      && topics::components(d).size() == 1,
	      "adopted topics are tops, never components");
}

void testAdopt()
{
	auto r = topics::parse(kSketch);
	auto &d = r.value;
	auto const before = d.topics.size();

	check(topics::adopt(d, "(?i:kestrel)")
	      && d.topics.size() == before + 1
	      && d.topics.back().name == "adhoc1"
	      && d.topics.back().fragments
	         == std::vector<std::string>{"(?i:kestrel)"},
	      "a fresh pattern adopts as adhoc1");
	check(!topics::adopt(d, "(?i:kestrel)")
	      && d.topics.size() == before + 1,
	      "the same pattern deduplicates");
	check(topics::adopt(d, "[Bb]udget")
	      && d.topics.back().name == "adhoc2",
	      "the next adoption numbers upward");
	check(!topics::adopt(d,
	      "uses? ((?:([Aa]n )?i[Pp]hone|([Aa] )?([Ss]amsung|"
	      "(1|[Oo]ne)[Pp]lus)))"),
	      "a pattern equal to a topic's expansion deduplicates");
	check(!topics::adopt(d, ""), "the empty pattern is refused");
	check(!topics::adopt(d, " foo") && !topics::adopt(d, "foo\t")
	      && !topics::adopt(d, "foo "),
	      "edge whitespace is refused: it cannot round-trip");
	check(!topics::adopt(d, "foo\nbar")
	      && !topics::adopt(d, "foo\rbar"),
	      "interior line breaks are refused for the same reason");
	check(!topics::adopt(d, "x\\{phone:}y"),
	      "reference syntax is refused unvalidated");
	check(topics::adopt(d, "brace \\{2} escape"),
	      "an escaped brace without the reference shape is fine");
	check(topics::adopt(d, "(?i:thread)", "focus")
	      && d.topics.back().name == "focus1"
	      && topics::adopt(d, "(?i:threads)", "focus")
	      && d.topics.back().name == "focus2",
	      "stems name their own families");

	auto const plan = topics::export_plan(d);
	bool found = false;
	for (auto const &e : plan)
		found = found || (e.name == "adhoc1"
		                  && e.pattern == "(?i:kestrel)"
		                  && e.parts.empty());
	check(found, "adopted topics are top-level export groupings");

	auto const back = topics::parse(topics::write(d));
	check(back.error.empty() && back.value == d,
	      "a document with adoptions round-trips through write");
}

void testNormalKey()
{
	using topics::normal_key;
	check(normal_key("(?i:b|a|a)") == normal_key("(?i:a|b)"),
	      "branch order and repeats collapse");
	check(normal_key("(?i)(a|b)") == normal_key("(?i:a|b)") &&
	      normal_key("(?i:(a|b))") == normal_key("(?i:a|b)"),
	      "both live wrapper shapes normalize alike");
	check(normal_key("(?:foo)|(bar)") == normal_key("foo|bar"),
	      "redundant group wrappers peel");
	check(normal_key("[Qq]uorum") == normal_key("(?i:quorum)"),
	      "the two-case idiom meets (?i:) in the middle");
	check(normal_key("Foo") != normal_key("(?i:foo)") &&
	      normal_key("Foo") != normal_key("foo"),
	      "case-sensitive text keeps its case identity");
	std::string const k = normal_key("(?i:b|a)");
	check(normal_key(k) == k, "keys are fixpoints");
	check(normal_key("a||b") == "a||b" &&
	      normal_key("(a|b") == "(a|b",
	      "structural doubt collapses to exact text");
}

void testAdoptNovel()
{
	auto r = topics::parse("- quorum\n"
	                       "  - [Qq]uorum\n"
	                       "\n"
	                       "- backups\n"
	                       "  - [Bb]ack ?ups?\n");
	check(r.error.empty(), "novelty sketch parses");
	topics::doc d = r.value;

	check(topics::adopt_novel(d, "(?i:quorum)", "focus").empty()
	      && d.topics.size() == 2,
	      "a fully covered regex adopts nothing");

	std::string const kept =
		topics::adopt_novel(d, "(?i:quorum|etsy-?d|et ?cd)",
		                    "focus");
	check(kept == "(?i:etsy-?d|et ?cd)" && d.topics.size() == 3 &&
	      d.topics[2].name == "focus1" &&
	      d.topics[2].fragments == std::vector<std::string>{kept},
	      "covered branches subtract; the novel remainder adopts");

	check(topics::adopt_novel(d, "(?i:(et ?cd)|zeta|zeta)",
	                          "focus") == "(?i:zeta)",
	      "wrapping, repeats and fresh coverage all collapse");

	auto r2 = topics::parse("- shout\n  - Foo\n");
	check(r2.error.empty(), "case-sensitive sketch parses");
	check(topics::adopt_novel(r2.value, "(?i:foo|bar)", "focus")
	      == "(?i:foo|bar)",
	      "a case-sensitive topic subtracts no (?i:) branch");

	check(topics::adopt(d, "(?i:bar|foo)") &&
	      !topics::adopt(d, "(?i:foo|bar)"),
	      "adopt() refuses key-level duplicates");

	auto const back = topics::parse(topics::write(d));
	check(back.error.empty() && back.value == d,
	      "novel adoptions round-trip through write");
}

void testTidy()
{
	using topics::tidy;
	check(tidy("(?i:(a|b|a))") == "(?i:a|b)",
	      "wrappers flatten and repeats drop");
	check(tidy("(?i)(x|(?:y)|x|x)") == "(?i:x|y)",
	      "the prefix shape and inner wraps tidy to one form");
	check(tidy("[Qq]uorum") == "[Qq]uorum" &&
	      tidy("(?i:froz|freez)") == "(?i:froz|freez)",
	      "already-clean patterns pass through byte-identical");
	check(tidy("(?i:a|b") == "(?i:a|b" && tidy("a||b") == "a||b",
	      "structural doubt passes through unchanged");
	check(tidy("") == "", "emptiness stays empty");
	check(tidy("[Ee]tsy-?d|[Ee]tsy-?[Dd]")
	      == "[Ee]tsy-?d|[Ee]tsy-?[Dd]",
	      "case-sensitive context keeps case-distinct twins");
	check(tidy("(?i:[Ee]tsy-?d|[Ee]tsy-?[Dd])") == "(?i:[Ee]tsy-?d)",
	      "the same twins collapse under an outer (?i:)");
}

void testClassCanon()
{
	using topics::normal_key;
	check(normal_key("[cab]") == normal_key("[abc]") &&
	      normal_key("[abc]") == normal_key("[a-c]"),
	      "member order and equivalent ranges canonicalize");
	check(normal_key("a[ -]?b") == normal_key("a[- ]?b"),
	      "edge-dash literal position stops mattering");
	check(normal_key("[^ba]") == normal_key("[^ab]") &&
	      normal_key("[^ab]") != normal_key("[ab]"),
	      "negation canonicalizes and never collides with plain");
	check(normal_key("(?i:[AB]c)") == normal_key("(?i:[ba]C)"),
	      "class members fold under a case-insensitive branch");
	check(normal_key("[a-cx]") == normal_key("[xcab]"),
	      "ranges and singles merge into one member set");
	check(normal_key("[a\\]]") == normal_key("[\\]a]"),
	      "escaped members join the set like any other");
	check(normal_key("[\\da]") == normal_key("[a\\d]") &&
	      normal_key("[\\d]") != normal_key("[\\D]"),
	      "class-type escapes are opaque members, case intact");
	check(normal_key("[z-a]") == "[z-a]" &&
	      normal_key("[z-a]") != normal_key("[a-z]") &&
	      normal_key("[\\x41]") != normal_key("[A]"),
	      "inverted ranges and numeric escapes bail to verbatim");
	check(normal_key("[Qq]uorum") == normal_key("(?i:quorum)"),
	      "the idiom fold still outranks canonicalization");
	check(normal_key("i:foo") != normal_key("(?i:FOO)"),
	      "a literal i: branch cannot forge a folded key");
	check(normal_key("x[[:alpha:]]y") == "x[[:alpha:]]y",
	      "POSIX classes stay verbatim, never member debris");
}

} // namespace

int main()
{
	testStructure();
	testExpansion();
	testForwardAndNesting();
	testExportPlan();
	testErrors();
	testLexical();
	testComponents();
	testAdopt();
	testNormalKey();
	testAdoptNovel();
	testTidy();
	testClassCanon();
	std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED",
	            g_fail, g_fail == 1 ? "" : "s");
	return g_fail ? 1 : 0;
}
