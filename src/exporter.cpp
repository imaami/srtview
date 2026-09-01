// exporter.cpp -- see exporter.hpp.
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QTextDocumentFragment>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "exporter.hpp"

#include "grabber.hpp"
#include "srt.hpp"
#include "timefmtq.hpp"

namespace exporter {

namespace {

// Cue text as the *rendered* one-line form (tags consumed, like the
// reading view), so export hits agree with interactive hits and the
// digests read clean.
QString oneLine(std::string const &text)
{
	std::string const html = srt::cue_html(text);
	QString s = QTextDocumentFragment::fromHtml(
		QString::fromUtf8(html.data(), qsizetype(html.size())))
		.toPlainText();
	s.replace(QChar::LineSeparator, QLatin1Char(' '));
	s.replace(QLatin1Char('\n'), QLatin1Char(' '));
	return s;
}

// Frame names must survive Markdown links and shells.
QString safeStem(QString const &path)
{
	QString s = QFileInfo(path).completeBaseName();
	for (QChar &c : s)
		if (!c.isLetterOrNumber() && c != QLatin1Char('.')
		    && c != QLatin1Char('-'))
			c = QLatin1Char('_');
	return s;
}

void writeMd(QString const &path, QString const &md)
{
	QFile f(path);
	if (f.open(QIODevice::WriteOnly | QIODevice::Truncate
	           | QIODevice::Text))
		f.write(md.toUtf8());
}

// The identity a frame name leans on: the whole content id --
// a truncation would reintroduce the collision it exists to kill
// -- or a path hash when no identity could be resolved.
QString sourceTag(source const &v)
{
	if (!v.id.isEmpty())
		return v.id;
	return QString::fromLatin1(QCryptographicHash::hash(
		v.video.toUtf8(),
		QCryptographicHash::Blake2b_256).toHex().left(16));
}

QString frameName(source const &v, qint64 ms,
                  QSet<QString> const &dup)
{
	QString const s = safeStem(v.video);
	// Stem twins salt with the source identity: two videos'
	// frames must never claim one name, or the content collapse
	// silently cross-links their digests.  Twinhood is judged
	// case-folded -- the export may land on a case-insensitive
	// filesystem (the WSL2 /mnt/c reality), where Foo and foo
	// are one file.
	QString const tag = dup.contains(s.toCaseFolded())
		? QStringLiteral("-") + sourceTag(v)
		: QString();
	return s + tag + QLatin1Char('-') + QString::number(ms)
	     + QStringLiteral(".png");
}

QString mdImg(QString const &name)
{
	return QStringLiteral("![](frames/") + name
	     + QStringLiteral(") ");
}

// The timestamp heading and matched-cue blockquote of one hit.
QString snippet(transcript const &tx, std::size_t i)
{
	qsizetype const at = qsizetype(i);
	QString s = QStringLiteral("\n### ")
	          + fmtTime(tx.cues[i].start, true)
	          + QStringLiteral("\n\n");
	if (i > 0)
		s += QStringLiteral("> ") + tx.lines[at - 1]
		   + QLatin1Char('\n');
	s += QStringLiteral("> **") + tx.lines[at]
	   + QStringLiteral("**\n");
	if (i + 1 < tx.cues.size())
		s += QStringLiteral("> ") + tx.lines[at + 1]
		   + QLatin1Char('\n');
	return s;
}

// Everything one grouping's pass writes into: its own digest plus
// the digests of its acknowledged components, which persist across
// groupings (a component may be acknowledged by several).
struct sink {
	topics::doc const         &corpus;
	topics::export_item const &item;
	Grabber                   &grab;
	stats                     &st;
	transcripts               &texts;
	QHash<QString, QString>   &partMd;
	QSet<QString>             &partHead; // "<part>\n<video>" emitted
	QSet<QString> const       &dup;     // colliding video stems
	QString const             &outDir;
	QString                    tdir;     // the grouping's directory
	QString                    md;       // the grouping's digest
	QHash<QByteArray, QString> byHash{}; // content → canonical name
	QHash<QString, QString>    canon{};  // requested → canonical
	QHash<QString, QByteArray> hashOf{}; // cache path → content hash
};

// Grouping frames are real copies out of the grab cache -- collapsed
// by content, so accidentally identical frames (a static screen
// straddling several picks) become one file every digest references.
QString frameLink(sink &k, source const &v, qint64 ms)
{
	QString const requested = frameName(v, ms, k.dup);
	QString name = requested;
	QString const src = k.grab.framePath(v.id, ms);
	QByteArray hash = k.hashOf.value(src);
	if (hash.isEmpty()) {
		QFile f(src);
		if (f.open(QIODevice::ReadOnly))
			hash = QCryptographicHash::hash(f.readAll(),
				QCryptographicHash::Blake2b_256);
		k.hashOf.insert(src, hash);
	}
	if (!hash.isEmpty()) {
		QString const seen = k.byHash.value(hash);
		if (seen.isEmpty())
			k.byHash.insert(hash, name);
		else
			name = seen;
	}
	k.canon.insert(requested, name);
	QString const dst = k.tdir + QStringLiteral("/frames/") + name;
	if (!QFile::exists(dst))
		QFile::copy(src, dst);
	return mdImg(name);
}

// Component frames are relative symlinks into the grouping's copies:
// one PNG on disk, however many digests reference it.
QString partLink(sink &k, QString const &part, source const &v,
                 qint64 ms)
{
	// Resolve through the grouping's content collapse: the copy the
	// grouping actually holds is the one worth linking.
	QString const requested = frameName(v, ms, k.dup);
	QString const name = k.canon.value(requested, requested);
	QString const dir = k.outDir + QLatin1Char('/') + part
	                  + QStringLiteral("/frames");
	QDir().mkpath(dir);
	QString const lnk = dir + QLatin1Char('/') + name;
	if (!QFileInfo(lnk).isSymLink() && !QFile::exists(lnk))
		QFile::link(QStringLiteral("../../")
		            + QString::fromStdString(k.item.name)
		            + QStringLiteral("/frames/") + name, lnk);
	return mdImg(name);
}

void partHit(sink &k, QString const &part, transcript const &tx,
             std::size_t i, source const &v)
{
	QString &md = k.partMd[part];
	if (md.isEmpty()) {
		topics::topic const *t =
			topics::find(k.corpus, part.toStdString());
		md = QStringLiteral("# ") + part
		   + QStringLiteral("\n\nComponent; frames link into `")
		   + QString::fromStdString(k.item.name)
		   + QStringLiteral("`.\n\nPattern: `")
		   + QString::fromStdString(topics::expand(k.corpus, *t))
		   + QStringLiteral("`\n");
	}
	QString const headKey = part + QLatin1Char('\n') + v.video;
	if (!k.partHead.contains(headKey)) {
		k.partHead.insert(headKey);
		md += QStringLiteral("\n## ")
		    + QFileInfo(v.video).fileName()
		    + QLatin1Char('\n');
	}
	md += snippet(tx, i);
	qint64 const ms = qint64(tx.cues[i].start * 1000.0 + 0.5);
	qint64 prev = -1, next = -1;
	if (!k.grab.picksFor(v.video, v.id, ms, prev, next)) {
		md += QStringLiteral("\n*(frames pending)*\n");
		return;                      // queued by the grouping pass
	}
	md += QLatin1Char('\n');
	for (qint64 const f : {prev, ms, next})
		if (f >= 0)
			md += partLink(k, part, v, f);
	md += QLatin1Char('\n');
}

void exportHit(sink &k, transcript const &tx, std::size_t i,
               source const &v)
{
	++k.st.hits;
	k.md += snippet(tx, i);
	qint64 const ms = qint64(tx.cues[i].start * 1000.0 + 0.5);
	qint64 prev = -1, next = -1;
	if (!k.grab.picksFor(v.video, v.id, ms, prev, next)) {
		k.grab.enqueue(v.video, v.id, tx.cues[i].start);
		++k.st.queued;
		k.md += QStringLiteral("\n*(frames pending)*\n");
		return;
	}
	k.md += QLatin1Char('\n');
	for (qint64 const f : {prev, ms, next})
		if (f >= 0)
			k.md += frameLink(k, v, f);
	k.md += QLatin1Char('\n');
	++k.st.framed;
}

// Which acknowledged components fired anywhere in the cue?  One
// match reports only the alternation branch that won at its own
// position, so participation is the union over every match in the
// line.
void attribute(sink &k, QRegularExpressionMatchIterator it,
               transcript const &tx, std::size_t i, source const &v)
{
	QSet<qsizetype> fired;
	while (it.hasNext()) {
		QRegularExpressionMatch const m = it.next();
		for (std::size_t g = 0; g < k.item.parts.size(); ++g)
			if (m.capturedStart(QStringLiteral("g")
			                    + QString::number(g)) >= 0)
				fired.insert(qsizetype(g));
	}
	QSet<QString> seen;
	for (std::size_t g = 0; g < k.item.parts.size(); ++g) {
		if (!fired.contains(qsizetype(g)))
			continue;
		QString const part =
			QString::fromStdString(k.item.parts[g]);
		if (seen.contains(part))
			continue;
		seen.insert(part);
		partHit(k, part, tx, i, v);
	}
}

void exportVideo(sink &k, QRegularExpression const &re,
                 source const &v)
{
	transcript const &tx = load(k.texts, v.srt);
	bool head = false;
	for (std::size_t i = 0; i < tx.cues.size(); ++i) {
		QRegularExpressionMatchIterator const it =
			re.globalMatch(tx.lines[qsizetype(i)]);
		if (!it.hasNext())
			continue;
		if (!head) {
			k.md += QStringLiteral("\n## ")
			      + QFileInfo(v.video).fileName()
			      + QLatin1Char('\n');
			head = true;
		}
		exportHit(k, tx, i, v);
		attribute(k, it, tx, i, v);
	}
}

} // namespace

transcript const &load(transcripts &cache, QString const &srtPath)
{
	auto at = cache.constFind(srtPath);
	if (at != cache.constEnd())
		return *at;
	QFile f(srtPath);
	if (!f.open(QIODevice::ReadOnly)) {
		// An unreadable file must not poison the cache with a
		// permanent empty: the next touch retries.
		static transcript const none;
		return none;
	}
	transcript t;
	QByteArray const raw = f.readAll();
	// A line-aware sink: the hits export cites srt file lines, so
	// each cue lands with its block's first visible line.
	struct sink : srt::parser<sink> {
		transcript *t;
		void on_cue(double a, double b, std::string &&x, int at)
		{
			t->cues.push_back({a, b, std::move(x)});
			t->cueLine.push_back(at);
		}
	};
	sink s;
	s.t = &t;
	s.parse(srt::to_utf8({raw.constData(), size_t(raw.size())}));
	if (t.cues.empty()) {
		// Readable but not an SRT (yet): cache nothing, so a
		// repaired file is re-read on the next touch.
		static transcript const none;
		return none;
	}
	for (srt::cue const &c : t.cues)
		t.lines << oneLine(c.text);
	return *cache.insert(srtPath, std::move(t));
}

stats run(topics::doc const &corpus, QList<source> const &videos,
          Grabber &grab, QString const &outDir, transcripts &cache)
{
	stats st;
	QHash<QString, QString> partMd;
	QSet<QString> partHead;
	// Sanitized-stem twins across the video list, judged
	// case-folded for case-insensitive target filesystems: their
	// frames carry the discovery id, deterministically, whatever
	// the export order.
	QSet<QString> dup, seen;
	for (source const &v : videos) {
		QString const key = safeStem(v.video).toCaseFolded();
		if (seen.contains(key))
			dup.insert(key);
		else
			seen.insert(key);
	}
	for (topics::export_item const &e : topics::export_plan(corpus)) {
		QString const name = QString::fromStdString(e.name);
		// Stored patterns carry their own case semantics -- the
		// (?i:) wrap, the [Xx] idiom -- and search, evidence and
		// dives compile them bare; a forced flag here made export
		// disagree with all three.
		QRegularExpression const re(
			QString::fromStdString(e.pattern));
		sink k{corpus, e, grab, st, cache, partMd, partHead, dup,
		       outDir, outDir + QLatin1Char('/') + name, {}};
		QDir().mkpath(k.tdir + QStringLiteral("/frames"));
		k.md = QStringLiteral("# ") + name
		     + QStringLiteral("\n\nPattern: `") + re.pattern()
		     + QStringLiteral("`\n");
		if (!re.isValid())
			k.md += QStringLiteral("\n(invalid pattern: ")
			      + re.errorString() + QStringLiteral(")\n");
		for (source const &v : videos)
			if (re.isValid() && (v.topics.isEmpty()
			                     || v.topics.contains(name)))
				exportVideo(k, re, v);
		writeMd(k.tdir + QLatin1Char('/') + name
		        + QStringLiteral(".md"), k.md);
		++st.topics;
	}
	for (auto it = partMd.cbegin(); it != partMd.cend(); ++it) {
		QString const dir = outDir + QLatin1Char('/') + it.key();
		QDir().mkpath(dir);
		writeMd(dir + QLatin1Char('/') + it.key()
		        + QStringLiteral(".md"), it.value());
		++st.topics;
	}
	return st;
}

namespace {

// The last field of a hits entry: whitespace runs -- any mix,
// Unicode classification -- squeeze to one space, leading and
// trailing runs vanish, even when nothing remains.
QString squeeze(QString const &s)
{
	QString out;
	bool gap = false;
	for (QChar const ch : s) {
		if (ch.isSpace()) {
			gap = !out.isEmpty();
			continue;
		}
		if (gap)
			out += QLatin1Char(' ');
		gap = false;
		out += ch;
	}
	return out;
}

// The context span around a match in its display line: up to three
// whole words on the left, six on the right, and a word the match
// cuts through rides free on its own side.  Ellipses mark the
// sides where the cue continues past the span.
QString spanOf(QString const &line, qsizetype s, qsizetype e)
{
	qsizetype a = s;
	while (a > 0 && !line[a - 1].isSpace())
		--a;
	for (int w = 0; w < 3 && a > 0; ++w) {
		qsizetype b = a;
		while (b > 0 && line[b - 1].isSpace())
			--b;
		while (b > 0 && !line[b - 1].isSpace())
			--b;
		a = b;
	}
	qsizetype z = e;
	qsizetype const n = line.size();
	while (z < n && !line[z].isSpace())
		++z;
	for (int w = 0; w < 6 && z < n; ++w) {
		qsizetype c = z;
		while (c < n && line[c].isSpace())
			++c;
		while (c < n && !line[c].isSpace())
			++c;
		z = c;
	}
	auto const inked = [&line](qsizetype from, qsizetype to) {
		for (qsizetype i = from; i < to; ++i)
			if (!line[i].isSpace())
				return true;
		return false;
	};
	QString out = squeeze(line.mid(a, z - a));
	if (inked(0, a))
		out.prepend(QStringLiteral("… "));
	if (inked(z, n))
		out.append(QStringLiteral(" …"));
	return out;
}

// fmt_time with the component count forced: every entry shows the
// same number of colons as the latest stamp needs, the leftmost
// number unpadded so no field ever grows an octal-looking leading
// zero -- vertical alignment comes from space padding against the
// widest stamp instead.
std::string stampOf(double t, bool hours)
{
	long long ms = t > 0.0 ? std::llround(t * 1000.0) : 0;
	long long const h = ms / 3600000;
	ms %= 3600000;
	long long const m = ms / 60000;
	ms %= 60000;
	long long const s = ms / 1000;
	ms %= 1000;
	char buf[48];
	int const n = hours
		? std::snprintf(buf, sizeof buf,
		                "%lld:%02lld:%02lld.%03lld",
		                h, m, s, ms)
		: std::snprintf(buf, sizeof buf, "%lld:%02lld.%03lld",
		                h * 60 + m, s, ms);
	return {buf, std::size_t(n)};
}

QByteArray pad(QByteArray b, qsizetype w)
{
	qsizetype const need = w - b.size();
	return need > 0 ? QByteArray(need, ' ') + b : b;
}

} // namespace

QByteArray hits(QString const &pattern, QRegularExpression const &re,
                QStringList const &srts, transcripts &cache)
{
	// Pass one collects; the columns' widths come from their own
	// longest members, so emission needs every entry first.
	struct entry {
		QString   text;
		double    t = 0;
		qsizetype v = 0, j = 0;
		int       line = 0;
	};
	QList<entry> es;
	double tmax = 0;
	qsizetype vmax = 0, jmax = 0;
	int lmax = 0;
	for (qsizetype v = 0; v < srts.size(); ++v) {
		if (srts[v].isEmpty())
			continue;
		transcript const &tx = load(cache, srts[v]);
		for (qsizetype j = 0; j < tx.lines.size(); ++j) {
			auto m = re.globalMatch(tx.lines[j]);
			while (m.hasNext()) {
				QRegularExpressionMatch const hit =
					m.next();
				entry e;
				e.v = v;
				e.j = j;
				e.t = tx.cues[std::size_t(j)].start;
				e.line = tx.cueLine[std::size_t(j)];
				e.text = spanOf(tx.lines[j],
				                hit.capturedStart(0),
				                hit.capturedEnd(0));
				tmax = std::max(tmax, e.t);
				vmax = std::max(vmax, e.v);
				jmax = std::max(jmax, e.j);
				lmax = std::max(lmax, e.line);
				es << e;
			}
		}
	}
	QByteArray out = pattern.toUtf8();
	out += '\n';
	bool const hours = tmax >= 3600.0;
	qsizetype const tw =
		qsizetype(stampOf(tmax, hours).size());
	qsizetype const vw = QByteArray::number(vmax).size();
	qsizetype const jw = QByteArray::number(jmax).size();
	qsizetype const lw = QByteArray::number(lmax).size();
	for (entry const &e : es) {
		std::string const ts = stampOf(e.t, hours);
		out += pad(QByteArray::number(e.v), vw);
		out += '\t';
		out += pad(QByteArray::number(e.j), jw);
		out += '\t';
		out += pad(QByteArray(ts.data(), qsizetype(ts.size())),
		           tw);
		out += '\t';
		out += pad(QByteArray::number(e.line), lw);
		out += '\t';
		out += e.text.toUtf8();
		out += '\n';
	}
	return out;
}

} // namespace exporter
