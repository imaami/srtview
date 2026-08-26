// rx_test.cpp -- unit tests for the regex algebra.  Standard C++
// like the module itself: no Qt.  Match sets are verified with
// std::regex in ECMAScript grammar -- the portable-subset claim is
// only true if a second engine agrees with PCRE2 about every
// pattern knit() and braid() emit.
#include "rx.hpp"

#include <algorithm>
#include <cstdio>
#include <random>
#include <regex>
#include <string>
#include <vector>

namespace {

int g_fail = 0;

void check(bool ok, char const *what)
{
	std::printf("%s  %s\n", ok ? "OK  " : "FAIL", what);
	if (!ok)
		++g_fail;
}

bool matches(std::string const &pattern, std::string const &s)
{
	return std::regex_match(s, std::regex(pattern));
}

// The whole contract in one call: the pattern matches every word,
// refuses every probe, and unknit() recovers the exact set.
bool exact(std::vector<std::string> words,
           std::vector<std::string> const &probes)
{
	std::string const p = rx::knit(words);
	std::ranges::sort(words);
	auto const dup = std::ranges::unique(words);
	words.erase(dup.begin(), dup.end());
	for (std::string const &w : words)
		if (!matches(p, w))
			return false;
	for (std::string const &n : probes)
		if (matches(p, n))
			return false;
	auto const back = rx::unknit(p);
	return back && *back == words;
}

void testKnitShapes()
{
	check(rx::knit({"word", "words"}) == "words?",
	      "trailing s folds to ?");
	check(rx::knit({"analyse", "analyze"}) == "analy[sz]e",
	      "single-letter split folds to a class");
	check(rx::knit({"color", "colour"}) == "colou?r",
	      "dropped letter folds to ?");
	check(rx::knit({"a", "b", "c"}) == "[abc]",
	      "single letters fold to one class");
	check(rx::knit({"cat"}) == "cat", "one word stays itself");
	check(rx::knit({}).empty() && rx::knit({""}).empty(),
	      "empty input and the empty word yield empty");
	check(rx::knit({"word", "word"}) == "word",
	      "duplicates collapse");
	std::string const p = rx::knit({"ghidra", "Ghidra"});
	check(p == "[Gg]hidra", "case pair folds to a class");
}

void testKnitExact()
{
	check(exact({"word", "words"}, {"", "wordss", "wor"}),
	      "words? matches exactly");
	check(exact({"analyse", "analyze", "analyses", "analyzes"},
	            {"analye", "analysze"}),
	      "two axes of variation stay exact");
	check(exact({"p-code", "pcode", "P-code"},
	            {"pXcode", "p--code"}),
	      "metacharacters escape and stay exact");
	check(exact({"stack", "static", "step"}, {"sta", "stac"}),
	      "shared prefixes factor and stay exact");
	check(exact({"$rdi", "$rsi", "$rbp"}, {"rdi", "$r"}),
	      "leading metacharacter escapes");
	check(exact({"one", "two", "three", "thread", "threat"},
	            {"thre", "threa"}),
	      "mixed depths stay exact");
	check(exact({"^", "a", "b"}, {"c", "\\", "[", "]"}),
	      "a caret member round-trips");
}

void testKnitUtf8()
{
	// A codepoint never splits: the two-byte letters ride whole
	// inside a group, and the byte-minded verifier agrees.
	std::string const p = rx::knit({"h\xC3\xA4n", "han"});
	check(matches(p, "h\xC3\xA4n") && matches(p, "han")
	      && !matches(p, "h\xC3n") && !matches(p, "hn"),
	      "two-byte letter alternates whole");
	std::string const q = rx::knit({"sy\xC3\xB6", "sy\xC3\xB6t"});
	check(matches(q, "sy\xC3\xB6") && matches(q, "sy\xC3\xB6t")
	      && !matches(q, "sy"),
	      "optional tail after a two-byte letter");
}

void testKnitDeterminism()
{
	std::vector<std::string> words = {"word", "words", "wordy",
	                                  "sword", "analyse",
	                                  "analyze", "a", "b"};
	std::string const canon = rx::knit(words);
	std::mt19937 gen(7);
	for (int i = 0; i < 16; ++i) {
		std::ranges::shuffle(words, gen);
		if (rx::knit(words) != canon) {
			check(false, "order-blind determinism");
			return;
		}
	}
	check(true, "order-blind determinism");
}

void testUnknit()
{
	auto const s = rx::unknit("words?");
	check(s && *s == std::vector<std::string>{"word", "words"},
	      "? re-opens");
	auto const c = rx::unknit("analy[sz]e");
	check(c && *c == std::vector<std::string>{"analyse",
	                                          "analyze"},
	      "class re-opens");
	auto const g = rx::unknit("(?:foo|bar)");
	check(g && *g == std::vector<std::string>{"bar", "foo"},
	      "group re-opens sorted");
	auto const e = rx::unknit("foo\\ bar\\-baz");
	check(e && *e == std::vector<std::string>{"foo bar-baz"},
	      "escaped literals decode");
	auto const r = rx::unknit("[a-d]");
	check(r && *r == std::vector<std::string>{"a", "b", "c", "d"},
	      "class range expands");
	check(!rx::unknit("a+") && !rx::unknit("a*")
	      && !rx::unknit("a{2}") && !rx::unknit("[^a]")
	      && !rx::unknit("\\d") && !rx::unknit("(a)")
	      && !rx::unknit("(?i:a)") && !rx::unknit("a.b")
	      && !rx::unknit("^a$") && !rx::unknit("[a")
	      && !rx::unknit("(?:a"),
	      "everything beyond the subset refuses");
	auto const neg = rx::unknit("[\\^a]");
	check(neg && *neg == std::vector<std::string>{"^", "a"},
	      "an escaped leading caret is a member, not negation");
	check(!rx::unknit("\xC3|x") && !rx::unknit("\xC3?")
	      && !rx::unknit("\xC3)") && !rx::unknit("a\xC3"),
	      "a truncated codepoint refuses instead of swallowing");
	auto const cp = rx::unknit("h\xC3\xA4n");
	check(cp && *cp == std::vector<std::string>{"h\xC3\xA4n"},
	      "a whole codepoint still decodes");
	std::string blow;
	for (int i = 0; i < 7; ++i)
		blow += "(?:aa|bb)";
	check(!rx::unknit(blow), "expansion past the cap refuses");
	std::string deep;
	for (int i = 0; i < 40; ++i)
		deep += "(?:";
	deep += 'a';
	deep.append(40, ')');
	check(!rx::unknit(deep)
	      && rx::unknit("(?:(?:(?:a)))").has_value(),
	      "runaway nesting refuses, shallow decodes");
	// The convergence property: a knitted pattern re-opens, takes
	// a new spelling, and re-knits -- no one-way doors.
	auto back = rx::unknit(rx::knit({"word", "words"}));
	check(back.has_value(), "knitted pattern re-opens");
	if (back) {
		back->push_back("wordy");
		check(rx::knit(*back) == "word[sy]?",
		      "re-knit converges tighter");
	}
}

void testAlike()
{
	check(rx::alike("abc", "abc") == 1.0, "equal is 1");
	check(rx::alike("a  b", "a b") == 1.0,
	      "whitespace runs fold before comparing");
	check(rx::alike("abc", "xyz") == 0.0, "disjoint is 0");
	check(rx::alike("", "") == 1.0, "empty vs empty is 1");
	double const d = rx::alike("$Functions: Stack",
	                           "$4Functions: Stack");
	check(d > 0.9 && d < 1.0, "one garbage char barely dents");
}

void testBraidJitter()
{
	// The live cache's own family: a garbage '4' flickering into
	// one frame of a stable slide title.
	std::vector<std::string> const fam = {
		"$Functions: Stack", "$4Functions: Stack",
		"$Functions: Stack", "$Functions:  Stack"};
	rx::weave const w = rx::braid(fam);
	check(w.consensus == "$Functions: Stack",
	      "majority text wins");
	for (std::string const &v : fam)
		if (!matches(w.pattern, v)) {
			check(false, "pattern matches every sighting");
			return;
		}
	check(!matches(w.pattern, "$Functions Stack"),
	      "pattern is no catch-all");
}

void testBraidShapes()
{
	rx::weave const s = rx::braid({"analyse", "analyze"});
	check(s.consensus == "analyse" && s.pattern == "analy[sz]e",
	      "substitution braids to a class");
	rx::weave const t = rx::braid({"Function:", "Functions:"});
	check(t.consensus == "Function:"
	      && matches(t.pattern, "Function:")
	      && matches(t.pattern, "Functions:"),
	      "dropout braids optional");
	rx::weave const o = rx::braid({"one", "one", "one"});
	check(o.consensus == "one" && o.pattern == "one",
	      "unanimity braids to itself");
	rx::weave const ws = rx::braid({"a  b", "a b"});
	check(matches(ws.pattern, "a b") && matches(ws.pattern, "a  b")
	      && matches(ws.pattern, "a   b")
	      && !matches(ws.pattern, "ab"),
	      "whitespace wobble widens, presence stays required");
	rx::weave const e = rx::braid({});
	check(e.consensus.empty() && e.pattern.empty(),
	      "nothing braids to nothing");
}

void testBraidRefusal()
{
	rx::weave const w = rx::braid({"Outline: C to ASM",
	                               "cmp dword ptr [rbp-8], 0"});
	check(w.consensus == "Outline: C to ASM"
	      && w.pattern == "Outline: C to ASM",
	      "unrelated strings refuse to braid");
	// The refusal pattern still matches its witness.
	rx::weave const m = rx::braid({"a+b", "zzzzzzzz"});
	check(matches(m.pattern, m.consensus),
	      "refusal escapes its witness");
}

void testBraidDeterminism()
{
	std::vector<std::string> fam = {
		"$tFunction: C to ASM", "$Function: C to ASM",
		"$Function: C to ASM", "$Function:C to ASM"};
	rx::weave const canon = rx::braid(fam);
	std::mt19937 gen(11);
	for (int i = 0; i < 16; ++i) {
		std::ranges::shuffle(fam, gen);
		rx::weave const w = rx::braid(fam);
		if (w.consensus != canon.consensus
		    || w.pattern != canon.pattern) {
			check(false, "braid order-blind determinism");
			return;
		}
	}
	check(true, "braid order-blind determinism");
	for (std::string const &v : fam)
		if (!matches(canon.pattern, v)) {
			check(false, "braid pattern spans the family");
			return;
		}
	check(true, "braid pattern spans the family");
}

} // namespace

int main()
{
	testKnitShapes();
	testKnitExact();
	testKnitUtf8();
	testKnitDeterminism();
	testUnknit();
	testAlike();
	testBraidJitter();
	testBraidShapes();
	testBraidRefusal();
	testBraidDeterminism();
	std::printf("%s\n", g_fail ? "FAILURES" : "all ok");
	return g_fail;
}
