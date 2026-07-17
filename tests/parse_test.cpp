// parse_test.cpp -- unit tests for the SRT parser and markup scanner.
#include "srt.hpp"

#include <QTemporaryFile>

#include <cstdio>
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

QString ls(const char *a, const char *b)
{
	return QString::fromUtf8(a) + QChar(QChar::LineSeparator)
	     + QString::fromUtf8(b);
}

std::vector<Cue> parse(std::string_view s) { return parseSrtData(s); }

void testStructure()
{
	// canonical two blocks
	auto v = parse("1\n00:00:01,000 --> 00:00:03,500\nHello\nWorld\n\n"
	               "2\n00:01:15,250 --> 00:01:18,750\nBye\n");
	check(v.size() == 2 && near(v[0].start, 1.0) && near(v[0].end, 3.5)
	      && v[0].text == ls("Hello", "World")
	      && near(v[1].start, 75.25) && v[1].text == QStringLiteral("Bye"),
	      "canonical blocks");

	// counters optional, values untrusted
	v = parse("00:00:01,000 --> 00:00:02,000\na\n\n"
	          "999\n00:00:03,000 --> 00:00:04,000\nb\n");
	check(v.size() == 2 && v[0].text == QStringLiteral("a")
	      && v[1].text == QStringLiteral("b"),
	      "missing / misnumbered counters");

	// missing blank separator: documented common error
	v = parse("1\n00:00:01,000 --> 00:00:02,000\na\n"
	          "2\n00:00:03,000 --> 00:00:04,000\nb\n");
	check(v.size() == 2 && v[0].text == QStringLiteral("a")
	      && near(v[1].start, 3.0) && v[1].text == QStringLiteral("b"),
	      "missing blank separator recovery");

	// lone digits caption stays text
	v = parse("1\n00:00:01,000 --> 00:00:02,000\n42\n\n");
	check(v.size() == 1 && v[0].text == QStringLiteral("42"),
	      "digits-only caption text");

	// timecode positioning extension / trailing junk ignored
	v = parse("1\n00:00:01,000 --> 00:00:02,000 X1:40 X2:600 Y1:20\nx\n");
	check(v.size() == 1 && near(v[0].end, 2.0),
	      "trailing junk on timecode line");

	// period separator and short milliseconds
	v = parse("1\n00:00:01.5 --> 00:00:02.25\nx\n");
	check(v.size() == 1 && near(v[0].start, 1.5) && near(v[0].end, 2.25),
	      "period separator, short ms");

	// bare-CR (classic Mac) and CRLF line endings
	v = parse("1\r00:00:01,000 --> 00:00:02,000\ra\rb\r\r");
	check(v.size() == 1 && v[0].text == ls("a", "b"),
	      "bare-CR line endings");
	v = parse("1\r\n00:00:01,000 --> 00:00:02,000\r\na\r\n\r\n");
	check(v.size() == 1 && v[0].text == QStringLiteral("a"),
	      "CRLF line endings");

	// junk between blocks skipped
	v = parse("downloaded from example.org\n\n"
	          "1\n00:00:01,000 --> 00:00:02,000\na\n\n"
	          "stray line\n\n"
	          "2\n00:00:03,000 --> 00:00:04,000\nb\n");
	check(v.size() == 2, "junk between blocks skipped");

	// pathological inputs
	check(parse("").empty() && parse("\n\n\n").empty()
	      && parse("no cues at all\n").empty(),
	      "empty / cue-free inputs");

	// empty-text cue is kept
	v = parse("1\n00:00:01,000 --> 00:00:02,000\n\n"
	          "2\n00:00:03,000 --> 00:00:04,000\nb\n");
	check(v.size() == 2 && v[0].text.isEmpty(), "empty-text cue kept");
}

void testEncodings()
{
	const auto viaFile = [](const QByteArray &bytes) {
		QTemporaryFile f;
		f.open();
		f.write(bytes);
		f.flush();
		QString err;
		return parseSrt(f.fileName(), &err);
	};
	const QByteArray body =
		"1\n00:00:01,000 --> 00:00:02,000\ncaf\xc3\xa9\n";

	check(viaFile(body).size() == 1
	      && viaFile(body)[0].text == QString::fromUtf8("caf\xc3\xa9"),
	      "plain UTF-8");
	check(viaFile("\xef\xbb\xbf" + body).size() == 1, "UTF-8 BOM");

	// UTF-16 LE and BE with BOM
	const QString uni = QString::fromUtf8(body);
	QByteArray le("\xff\xfe", 2), be("\xfe\xff", 2);
	for (const QChar ch : uni) {
		const char16_t u = ch.unicode();
		le += char(u & 0xff); le += char(u >> 8);
		be += char(u >> 8);   be += char(u & 0xff);
	}
	check(viaFile(le).size() == 1
	      && viaFile(le)[0].text == QString::fromUtf8("caf\xc3\xa9"),
	      "UTF-16LE BOM");
	check(viaFile(be).size() == 1
	      && viaFile(be)[0].text == QString::fromUtf8("caf\xc3\xa9"),
	      "UTF-16BE BOM");

	// Windows-1252 fallback: e9=e-acute, 97=em dash, 93/94=curly quotes
	QByteArray ansi = "1\n00:00:01,000 --> 00:00:02,000\n"
	                  "caf\xe9 \x97 \x93q\x94\n";
	const auto v = viaFile(ansi);
	check(v.size() == 1
	      && v[0].text == QString::fromUtf8(
	             "caf\xc3\xa9 \xe2\x80\x94 \xe2\x80\x9cq\xe2\x80\x9d"),
	      "Windows-1252 fallback");
}

void testMarkup()
{
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
		// own -- tags are vetted independently, not balanced, and
		// QTextDocument ignores unmatched closers.
		{ "<font onclick=\"evil()\">x</font>",
		  "&lt;font onclick=&quot;evil()&quot;&gt;x</font>" },
		{ "<font color=\"#fff\" onclick=x>y</font>",
		  "&lt;font color=&quot;#fff&quot; onclick=x&gt;y</font>" },
		{ "a{\\pos(1,2)}b", "ab" },
		{ "unterminated {\\x rest", "unterminated {\\x rest" },
	};
	for (const auto &c : cases) {
		const QString got = cueHtml(QString::fromUtf8(c.in));
		const bool ok = got == QString::fromUtf8(c.want);
		check(ok, c.in);
		if (!ok)
			std::printf("      got: %s\n     want: %s\n",
			            qPrintable(got), c.want);
	}
	check(cueHtml(ls("a", "b")) == QStringLiteral("a<br>b"),
	      "line separator -> <br>");
}

} // namespace

int main()
{
	testStructure();
	testEncodings();
	testMarkup();
	std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED",
	            g_fail, g_fail == 1 ? "" : "s");
	return g_fail ? 1 : 0;
}
