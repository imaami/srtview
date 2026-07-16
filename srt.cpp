#include "srt.hpp"

#include <QFile>

#include <cmath>
#include <cstring>
#include <string_view>

namespace {

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

// Consumes "H+:MM:SS[,.]m{1,3}" starting at p; advances p on success.
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

// Normalize the raw file bytes to UTF-8 (handles UTF-16 BOM files).
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
	return raw;
}

} // namespace

std::vector<Cue> parseSrt(const QString &path, QString *err)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly)) {
		*err = f.errorString();
		return {};
	}
	const QByteArray buf = toUtf8(f.readAll());

	// Split into line views over the buffer; no per-line allocation.
	std::vector<std::string_view> lines;
	lines.reserve(size_t(buf.size()) / 24 + 8);
	const char *p = buf.constData();
	const char *end = p + buf.size();
	while (p < end) {
		const char *nl = static_cast<const char *>(
			std::memchr(p, '\n', size_t(end - p)));
		const char *e = nl ? nl : end;
		if (e > p && e[-1] == '\r')
			--e;
		lines.emplace_back(p, size_t(e - p));
		p = nl ? nl + 1 : end;
	}

	std::vector<Cue> cues;
	for (size_t i = 0; i < lines.size(); ++i) {
		Cue c;
		if (!parseTimestampLine(lines[i], c.start, c.end))
			continue;
		size_t j = i + 1;
		for (; j < lines.size(); ++j) {
			const std::string_view t = trimView(lines[j]);
			if (t.empty())
				break;
			if (isTimestampLine(lines[j]))
				break;                          // unseparated next cue
			if (allDigits(t)                    // cue index line just
			    && j + 1 < lines.size()         // before a timestamp
			    && isTimestampLine(lines[j + 1]))
				break;
			if (!c.text.isEmpty())
				c.text += QChar(QChar::LineSeparator);
			c.text += QString::fromUtf8(t.data(), qsizetype(t.size()));
		}
		cues.push_back(std::move(c));
		i = j - 1;
	}
	if (cues.empty())
		*err = QStringLiteral("no cues found (not an SRT file?)");
	return cues;
}

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
			if (close > 0 && close - i <= 64) {
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
				                            Qt::CaseInsensitive)) {
					QStringView rest = inner.mid(4).trimmed();
					if (rest.startsWith(QLatin1StringView("color="),
					                    Qt::CaseInsensitive)) {
						QStringView v = rest.mid(6).trimmed();
						if (!v.isEmpty() && (v.front() == QLatin1Char('"')
						     || v.front() == QLatin1Char('\'')))
							v = v.mid(1);
						if (!v.isEmpty() && (v.back() == QLatin1Char('"')
						     || v.back() == QLatin1Char('\'')))
							v.chop(1);
						bool ok = !v.isEmpty();
						for (const QChar vc : v)
							ok = ok && (vc == QLatin1Char('#')
							            || vc.isLetterOrNumber());
						if (ok) {
							out += QLatin1StringView("<font color=\"");
							out += v;
							out += QLatin1StringView("\">");
							i = close;
							emitted = true;
						}
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
