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

} // namespace

int main()
{
	testStructure();
	testExpansion();
	testForwardAndNesting();
	testErrors();
	testLexical();
	std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED",
	            g_fail, g_fail == 1 ? "" : "s");
	return g_fail ? 1 : 0;
}
