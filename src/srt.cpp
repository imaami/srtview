#include "srt.hpp"

#include <QFile>

#include <cmath>
#include <cstdint>

namespace {

// ---------------------------------------------------------- scanning --

std::string_view trimView(std::string_view v)
{
	size_t a = 0, b = v.size();
	while (a < b && static_cast<unsigned char>(v[a]) <= ' ')
		++a;
	while (b > a && static_cast<unsigned char>(v[b - 1]) <= ' ')
		--b;
	return v.substr(a, b - a);
}

bool allDigits(std::string_view v)
{
	if (v.empty())
		return false;
	for (const char c : v)
		if (c < '0' || c > '9')
			return false;
	return true;
}

// One line per call.  The reference lists CRLF, LF and bare-CR
// (classic Mac) files as all occurring in the wild; handle all three.
struct LineCursor {
	const char *p;
	const char *end;

	bool next(std::string_view &line)
	{
		if (p >= end)
			return false;
		const char *e = p;
		while (e < end && *e != '\n' && *e != '\r')
			++e;
		line = {p, size_t(e - p)};
		if (e < end) {
			if (*e == '\r' && e + 1 < end && e[1] == '\n')
				e += 2;
			else
				++e;
		}
		p = e;
		return true;
	}
};

// Consumes "H+:MM:SS[,.]m{1,3}" starting at p; advances p on success.
// Reference form is zero-padded HH:MM:SS,mmm with a comma; longer
// hour fields, a period separator (a documented common deviation) and
// short millisecond fields are accepted as tolerances.
bool parseStamp(const char *&p, const char *end, double &out)
{
	const auto digits = [&](int minN, int maxN, unsigned &v) {
		int n = 0;
		v = 0;
		while (p < end && *p >= '0' && *p <= '9' && n < maxN) {
			v = v * 10 + unsigned(*p - '0');
			++p;
			++n;
		}
		return n >= minN;
	};
	unsigned h, m, s, ms;
	if (!digits(1, 9, h) || p >= end || *p != ':')
		return false;
	++p;
	if (!digits(2, 2, m) || p >= end || *p != ':')
		return false;
	++p;
	if (!digits(2, 2, s) || p >= end || (*p != ',' && *p != '.'))
		return false;
	++p;
	const char *msStart = p;
	if (!digits(1, 3, ms))
		return false;
	switch (p - msStart) {
	case 1: ms *= 100; break;
	case 2: ms *= 10;  break;
	default:           break;
	}
	out = h * 3600.0 + m * 60.0 + s + ms / 1000.0;
	return true;
}

// Timecode line: "start --> end", whitespace-tolerant.  Anything
// after the second stamp (SubRip's X1:/X2:/Y1:/Y2: positioning
// extension, or other junk) is ignored.
bool parseTimestampLine(std::string_view line, double &a, double &b)
{
	const char *p = line.data();
	const char *end = p + line.size();
	const auto ws = [&] {
		while (p < end && (*p == ' ' || *p == '\t'))
			++p;
	};
	ws();
	if (!parseStamp(p, end, a))
		return false;
	ws();
	if (end - p < 3 || p[0] != '-' || p[1] != '-' || p[2] != '>')
		return false;
	p += 3;
	ws();
	return parseStamp(p, end, b);
}

bool isTimestampLine(std::string_view line)
{
	double a, b;
	return parseTimestampLine(line, a, b);
}

// ---------------------------------------------------------- encoding --

bool validUtf8(std::string_view v)
{
	const auto *p = reinterpret_cast<const unsigned char *>(v.data());
	const auto *end = p + v.size();
	while (p < end) {
		if (*p < 0x80) {
			++p;
			continue;
		}
		int n;
		if      ((*p & 0xE0) == 0xC0 && *p >= 0xC2) n = 1;
		else if ((*p & 0xF0) == 0xE0)               n = 2;
		else if ((*p & 0xF8) == 0xF0 && *p <= 0xF4) n = 3;
		else
			return false;
		if (end - p <= n)
			return false;
		for (int i = 1; i <= n; ++i)
			if ((p[i] & 0xC0) != 0x80)
				return false;
		p += n + 1;
	}
	return true;
}

// The 0x80..0x9F range where Windows-1252 differs from Latin-1.
constexpr char16_t kCp1252C1[32] = {
	0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
	0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
	0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
	0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

QByteArray cp1252ToUtf8(const QByteArray &raw)
{
	QString s;
	s.reserve(raw.size());
	for (const char cc : raw) {
		const auto c = static_cast<unsigned char>(cc);
		s.append(QChar(c >= 0x80 && c <= 0x9F ? kCp1252C1[c - 0x80]
		                                      : char16_t(c)));
	}
	return s.toUtf8();
}

// Normalize the raw file bytes to UTF-8.  The reference lists UTF-8
// (with or without BOM) and legacy "ANSI" (Windows-1252) as the
// encodings found in practice; UTF-16 files with BOMs also exist.
QByteArray toUtf8(const QByteArray &raw)
{
	const auto u = [&](qsizetype i) {
		return static_cast<unsigned char>(raw[i]);
	};
	// Assemble UTF-16 code units bytewise: the payload starts at an
	// odd offset, so a char16_t* cast would be a misaligned pointer.
	const auto utf16 = [&](bool bigEndian) {
		QString s;
		s.reserve((raw.size() - 2) / 2);
		for (qsizetype i = 2; i + 1 < raw.size(); i += 2)
			s.append(QChar(char16_t(bigEndian
				? (u(i) << 8) | u(i + 1)
				: (u(i + 1) << 8) | u(i))));
		return s.toUtf8();
	};
	if (raw.size() >= 2 && u(0) == 0xFF && u(1) == 0xFE)          // LE
		return utf16(false);
	if (raw.size() >= 2 && u(0) == 0xFE && u(1) == 0xFF)          // BE
		return utf16(true);
	if (raw.size() >= 3 && u(0) == 0xEF && u(1) == 0xBB && u(2) == 0xBF)
		return raw.mid(3);
	if (!validUtf8({raw.constData(), size_t(raw.size())}))
		return cp1252ToUtf8(raw);
	return raw;
}

} // namespace

// ------------------------------------------------------------ parser --
// Block structure per the reference: sequence number, timecode line,
// text lines, blank-line separator.  Deviations handled:
//   - counters are optional and their values untrusted (wild files
//     misnumber freely)
//   - a timecode line met while collecting text starts a new cue
//     (missing blank separator, a documented common error)
//   - a digits-only line while collecting text is the next block's
//     counter only when a timecode line follows; otherwise it is
//     caption text (a lone "42" stays a caption)
//   - anything between blocks that is neither blank, counter nor
//     timecode is skipped (site banners, stray headers)

std::vector<Cue> parseSrtData(std::string_view utf8)
{
	std::vector<Cue> cues;
	LineCursor cur{utf8.data(), utf8.data() + utf8.size()};
	std::string_view line;
	Cue open;
	bool haveOpen = false;

	const auto close = [&] {
		if (haveOpen) {
			cues.push_back(std::move(open));
			open = {};
			haveOpen = false;
		}
	};

	while (cur.next(line)) {
		double a, b;
		if (!haveOpen) {
			if (parseTimestampLine(line, a, b)) {
				open.start = a;
				open.end = b;
				haveOpen = true;
			}
			continue;
		}
		const std::string_view t = trimView(line);
		if (t.empty()) {
			close();
			continue;
		}
		if (parseTimestampLine(line, a, b)) {
			close();                     // missing blank separator
			open.start = a;
			open.end = b;
			haveOpen = true;
			continue;
		}
		if (allDigits(t)) {
			LineCursor peek = cur;       // counter iff timecode next
			std::string_view nextLine;
			if (peek.next(nextLine) && isTimestampLine(nextLine)) {
				close();
				continue;
			}
		}
		if (!open.text.isEmpty())
			open.text += QChar(QChar::LineSeparator);
		open.text += QString::fromUtf8(t.data(), qsizetype(t.size()));
	}
	close();
	return cues;
}

std::vector<Cue> parseSrt(const QString &path, QString *err)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly)) {
		*err = f.errorString();
		return {};
	}
	const QByteArray buf = toUtf8(f.readAll());
	std::vector<Cue> cues =
		parseSrtData({buf.constData(), size_t(buf.size())});
	if (cues.empty())
		*err = QStringLiteral("no cues found (not an SRT file?)");
	return cues;
}

// ---------------------------------------------------------- markup --

namespace {

// Sanitized " key=\"value\"" sequence for a <font ...> tag, or empty
// if anything is off-whitelist or malformed (caller then escapes the
// whole tag).  Accepted attributes per the reference: color, face,
// size.
QString fontAttrs(QStringView s)
{
	QString out;
	qsizetype i = 0;
	const auto ws = [&] {
		while (i < s.size() && s[i].isSpace())
			++i;
	};
	while (true) {
		ws();
		if (i >= s.size())
			break;
		const qsizetype k0 = i;
		while (i < s.size() && s[i].isLetter())
			++i;
		const QStringView key = s.mid(k0, i - k0);
		ws();
		if (i >= s.size() || s[i] != QLatin1Char('='))
			return {};
		++i;
		ws();
		QStringView val;
		if (i < s.size() && (s[i] == QLatin1Char('"')
		                     || s[i] == QLatin1Char('\''))) {
			const QChar q = s[i];
			++i;
			const qsizetype v0 = i;
			while (i < s.size() && s[i] != q)
				++i;
			if (i >= s.size())
				return {};
			val = s.mid(v0, i - v0);
			++i;
		} else {
			const qsizetype v0 = i;
			while (i < s.size() && !s[i].isSpace())
				++i;
			val = s.mid(v0, i - v0);
		}
		if (val.isEmpty())
			return {};
		QLatin1StringView name;
		bool ok = true;
		if (key.compare(QLatin1StringView("color"),
		                Qt::CaseInsensitive) == 0) {
			name = QLatin1StringView("color");
			for (const QChar c : val)
				ok = ok && (c == QLatin1Char('#') || c.isLetterOrNumber());
		} else if (key.compare(QLatin1StringView("face"),
		                       Qt::CaseInsensitive) == 0) {
			name = QLatin1StringView("face");
			for (const QChar c : val)
				ok = ok && (c.isLetterOrNumber() || c == QLatin1Char(' ')
				            || c == QLatin1Char('-')
				            || c == QLatin1Char('_'));
		} else if (key.compare(QLatin1StringView("size"),
		                       Qt::CaseInsensitive) == 0) {
			name = QLatin1StringView("size");
			for (const QChar c : val)
				ok = ok && c.isDigit();
		} else {
			return {};
		}
		if (!ok)
			return {};
		out += QLatin1Char(' ');
		out += name;
		out += QLatin1StringView("=\"");
		out += val;
		out += QLatin1Char('"');
	}
	return out;
}

} // namespace

QString cueHtml(const QString &text)
{
	QString out;
	out.reserve(text.size() + text.size() / 8 + 8);
	const qsizetype n = text.size();
	const QStringView tv(text);
	const auto is = [](QStringView a, QLatin1StringView b) {
		return a.compare(b, Qt::CaseInsensitive) == 0;
	};
	for (qsizetype i = 0; i < n; ++i) {
		const QChar c = text[i];
		if (c == QChar(QChar::LineSeparator)) {
			out += QLatin1StringView("<br>");
		} else if (c == QLatin1Char('{') && i + 1 < n
		           && text[i + 1] == QLatin1Char('\\')) {
			const qsizetype close = text.indexOf(QLatin1Char('}'), i);
			if (close >= 0) {
				i = close;                  // drop {\...} ASS override
				continue;
			}
			out += c;                       // unterminated: literal
		} else if (c == QLatin1Char('<')) {
			const qsizetype close = text.indexOf(QLatin1Char('>'), i + 1);
			bool emitted = false;
			if (close > 0 && close - i <= 96) {
				const QStringView inner =
					tv.mid(i + 1, close - i - 1).trimmed();
				if (is(inner, QLatin1StringView("i"))
				    || is(inner, QLatin1StringView("b"))
				    || is(inner, QLatin1StringView("u"))
				    || is(inner, QLatin1StringView("/i"))
				    || is(inner, QLatin1StringView("/b"))
				    || is(inner, QLatin1StringView("/u"))
				    || is(inner, QLatin1StringView("/font"))) {
					out += QLatin1Char('<');
					out += inner;
					out += QLatin1Char('>');
					i = close;
					emitted = true;
				} else if (inner.startsWith(QLatin1StringView("font"),
				                            Qt::CaseInsensitive)
				           && inner.size() > 4 && inner[4].isSpace()) {
					const QString attrs = fontAttrs(inner.mid(5));
					if (!attrs.isEmpty()) {
						out += QLatin1StringView("<font");
						out += attrs;
						out += QLatin1Char('>');
						i = close;
						emitted = true;
					}
				}
			}
			if (!emitted)
				out += QLatin1StringView("&lt;");
		} else if (c == QLatin1Char('&')) {
			out += QLatin1StringView("&amp;");
		} else if (c == QLatin1Char('>')) {
			out += QLatin1StringView("&gt;");
		} else if (c == QLatin1Char('"')) {
			out += QLatin1StringView("&quot;");
		} else {
			out += c;
		}
	}
	return out;
}

QString fmtTime(double t, bool withMs)
{
	int ms = int(std::lround(t * 1000.0));
	int h = ms / 3600000; ms %= 3600000;
	int m = ms / 60000;   ms %= 60000;
	int s = ms / 1000;    ms %= 1000;
	QString out = (h > 0)
		? QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0'))
		                            .arg(s, 2, 10, QLatin1Char('0'))
		: QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
	if (withMs)
		out += QStringLiteral(".%1").arg(ms, 3, 10, QLatin1Char('0'));
	return out;
}
