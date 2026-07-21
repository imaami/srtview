// exporter.cpp -- see exporter.hpp.
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QTextDocumentFragment>

#include <vector>

#include "exporter.hpp"

#include "grabber.hpp"
#include "srt.hpp"
#include "timefmt.hpp"

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

QString frameName(source const &v, qint64 ms)
{
	return safeStem(v.video) + QLatin1Char('-')
	     + QString::number(ms) + QStringLiteral(".png");
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
	QString name = frameName(v, ms);
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
	k.canon.insert(frameName(v, ms), name);
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
	QString const name = k.canon.value(frameName(v, ms),
	                                   frameName(v, ms));
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
	if (!k.grab.picksFor(v.id, ms, prev, next)) {
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
	if (!k.grab.picksFor(v.id, ms, prev, next)) {
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
	transcript t;
	QFile f(srtPath);
	if (f.open(QIODevice::ReadOnly)) {
		QByteArray const raw = f.readAll();
		t.cues = srt::parse(srt::to_utf8({raw.constData(),
		                                  size_t(raw.size())}));
		for (srt::cue const &c : t.cues)
			t.lines << oneLine(c.text);
	}
	return *cache.insert(srtPath, std::move(t));
}

stats run(topics::doc const &corpus, QList<source> const &videos,
          Grabber &grab, QString const &outDir, transcripts &cache)
{
	stats st;
	QHash<QString, QString> partMd;
	QSet<QString> partHead;
	for (topics::export_item const &e : topics::export_plan(corpus)) {
		QString const name = QString::fromStdString(e.name);
		QRegularExpression const re(
			QString::fromStdString(e.pattern),
			QRegularExpression::CaseInsensitiveOption);
		sink k{corpus, e, grab, st, cache, partMd, partHead,
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

} // namespace exporter
