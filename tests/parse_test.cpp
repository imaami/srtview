// parse_test.cpp -- unit tests for the SRT module.  The module is
// standard C++, so this test builds without Qt: that is the proof of
// the boundary.
#include "srt.hpp"

#include <cstdio>
#include <string>
#include <string_view>

namespace {

int g_fail = 0;

void check(bool ok, const char *what)
{
	std::printf("%s  %s\n", ok ? "OK  " : "FAIL", what);
	if (!ok)
		++g_fail;
}

bool near(double a, double b) { return a > b - 1e-9 && a < b + 1e-9; }

void testStructure()
{
	using srt::parse;

	auto v = parse("1\n00:00:01,000 --> 00:00:03,500\nHello\nWorld\n\n"
	               "2\n00:01:15,250 --> 00:01:18,750\nBye\n");
	check(v.size() == 2 && near(v[0].start, 1.0) && near(v[0].end, 3.5)
	      && v[0].text == "Hello\nWorld"
	      && near(v[1].start, 75.25) && v[1].text == "Bye",
	      "canonical blocks");

	v = parse("00:00:01,000 --> 00:00:02,000\na\n\n"
	          "999\n00:00:03,000 --> 00:00:04,000\nb\n");
	check(v.size() == 2 && v[0].text == "a" && v[1].text == "b",
	      "missing / misnumbered counters");

	v = parse("1\n00:00:01,000 --> 00:00:02,000\na\n"
	          "2\n00:00:03,000 --> 00:00:04,000\nb\n");
	check(v.size() == 2 && v[0].text == "a" && near(v[1].start, 3.0)
	      && v[1].text == "b",
	      "missing blank separator recovery");

	v = parse("1\n00:00:01,000 --> 00:00:02,000\n42\n\n");
	check(v.size() == 1 && v[0].text == "42", "digits-only caption text");

	v = parse("1\n00:00:01,000 --> 00:00:02,000 X1:40 X2:600 Y1:20\nx\n");
	check(v.size() == 1 && near(v[0].end, 2.0),
	      "trailing junk on timecode line");

	v = parse("1\n00:00:01.5 --> 00:00:02.25\nx\n");
	check(v.size() == 1 && near(v[0].start, 1.5) && near(v[0].end, 2.25),
	      "period separator, short ms");

	v = parse("1\r00:00:01,000 --> 00:00:02,000\ra\rb\r\r");
	check(v.size() == 1 && v[0].text == "a\nb", "bare-CR line endings");

	v = parse("1\r\n00:00:01,000 --> 00:00:02,000\r\na\r\n\r\n");
	check(v.size() == 1 && v[0].text == "a", "CRLF line endings");

	v = parse("downloaded from example.org\n\n"
	          "1\n00:00:01,000 --> 00:00:02,000\na\n\n"
	          "stray line\n\n"
	          "2\n00:00:03,000 --> 00:00:04,000\nb\n");
	check(v.size() == 2, "junk between blocks skipped");

	check(parse("").empty() && parse("\n\n\n").empty()
	      && parse("no cues at all\n").empty(),
	      "empty / cue-free inputs");

	v = parse("1\n00:00:01,000 --> 00:00:02,000\n\n"
	          "2\n00:00:03,000 --> 00:00:04,000\nb\n");
	check(v.size() == 2 && v[0].text.empty(), "empty-text cue kept");
}

// A custom CRTP sink: proves the push API without a container.
struct counter : srt::parser<counter> {
	int    n = 0;
	double last = 0.0;

	void on_cue(double, double b, std::string &&) { ++n; last = b; }
};

void testSink()
{
	counter c;
	c.parse("1\n00:00:01,000 --> 00:00:02,000\na\n\n"
	        "2\n00:00:03,000 --> 00:00:04,500\nb\n");
	check(c.n == 2 && near(c.last, 4.5), "CRTP sink");
}

void testEncodings()
{
	using srt::to_utf8;
	using srt::valid_utf8;
	const std::string body = "1\n00:00:01,000 --> 00:00:02,000\n"
	                         "caf\xc3\xa9\n";

	check(to_utf8(body) == body, "plain UTF-8 passthrough");
	check(to_utf8("\xef\xbb\xbf" + body) == body, "UTF-8 BOM stripped");

	std::string le("\xff\xfe", 2), be("\xfe\xff", 2);
	// "café" line in UTF-16: build from the UTF-8 body's code points
	const char16_t units[] = {'1', '\n', '0', '0', ':', '0', '0', ':',
		'0', '1', ',', '0', '0', '0', ' ', '-', '-', '>', ' ', '0',
		'0', ':', '0', '0', ':', '0', '2', ',', '0', '0', '0', '\n',
		'c', 'a', 'f', 0x00E9, '\n'};
	for (const char16_t u : units) {
		le += char(u & 0xff); le += char(u >> 8);
		be += char(u >> 8);   be += char(u & 0xff);
	}
	check(to_utf8(le) == body, "UTF-16LE BOM");
	check(to_utf8(be) == body, "UTF-16BE BOM");

	// e9 = e-acute, 97 = em dash, 93/94 = curly quotes
	check(to_utf8("caf\xe9 \x97 \x93q\x94")
	          == "caf\xc3\xa9 \xe2\x80\x94 \xe2\x80\x9cq\xe2\x80\x9d",
	      "Windows-1252 fallback");

	// validator strictness
	check(valid_utf8("plain ascii") && valid_utf8("caf\xc3\xa9")
	      && valid_utf8("\xe2\x82\xac")            // U+20AC
	      && valid_utf8("\xf0\x9d\x84\x9e"),       // U+1D11E, 4-byte
	      "valid sequences accepted");
	check(valid_utf8("\xdf\xbf") && valid_utf8("\xe0\xa0\x80")
	      && valid_utf8("\xef\xbf\xbf") && valid_utf8("\xf0\x90\x80\x80")
	      && valid_utf8("\xf4\x8f\xbf\xbf"),
	      "boundary code points accepted");
	check(!valid_utf8("\xc0\x80")                  // overlong NUL
	      && !valid_utf8("\xe0\x80\x80")           // overlong 3-byte
	      && !valid_utf8("\xf0\x80\x80\x80")       // overlong 4-byte
	      && !valid_utf8("\xed\xa0\x80")           // surrogate
	      && !valid_utf8("\xf4\x90\x80\x80")       // > U+10FFFF
	      && !valid_utf8("\xc3")                   // truncated
	      && !valid_utf8("\x80")                   // bare continuation
	      && !valid_utf8("\xff"),
	      "invalid sequences rejected");
}

void testMarkup()
{
	using srt::cue_html;
	struct { const char *in, *want; } cases[] = {
		{ "{\\an8}Neque <i>porro</i> <b>quisquam</b> "
		  "<font color=\"#e05050\">est</font>, 3 < 5 && x",
		  "Neque <i>porro</i> <b>quisquam</b> "
		  "<font color=\"#e05050\">est</font>, 3 &lt; 5 &amp;&amp; x" },
		{ "<I>caps</I> <u>u</u> <script>evil</script>",
		  "<I>caps</I> <u>u</u> &lt;script&gt;evil&lt;/script&gt;" },
		{ "<font color=red>r</font>", "<font color=\"red\">r</font>" },
		{ "<font face=\"Courier New\" size=18 color='#ff0000'>x</font>",
		  "<font face=\"Courier New\" size=\"18\" color=\"#ff0000\">"
		  "x</font>" },
		// Off-whitelist opening tags are fully escaped (quotes
		// included); the stray </font> passes the whitelist on its
		// own -- tags are vetted independently, not balanced.
		{ "<font onclick=\"evil()\">x</font>",
		  "&lt;font onclick=&quot;evil()&quot;&gt;x</font>" },
		{ "<font color=\"#fff\" onclick=x>y</font>",
		  "&lt;font color=&quot;#fff&quot; onclick=x&gt;y</font>" },
		{ "a{\\pos(1,2)}b", "ab" },
		{ "unterminated {\\x rest", "unterminated {\\x rest" },
		{ "a\nb", "a<br>b" },
		{ "caf\xc3\xa9 <i>x</i>", "caf\xc3\xa9 <i>x</i>" },
	};
	for (const auto &c : cases) {
		const std::string got = cue_html(c.in);
		check(got == c.want, c.in);
		if (got != c.want)
			std::printf("      got: %s\n     want: %s\n", got.c_str(),
			            c.want);
	}
}

} // namespace

int main()
{
	testStructure();
	testSink();
	testEncodings();
	testMarkup();
	std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED",
	            g_fail, g_fail == 1 ? "" : "s");
	return g_fail ? 1 : 0;
}
