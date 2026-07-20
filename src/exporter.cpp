// exporter.cpp -- see exporter.hpp.
#include "exporter.hpp"

#include "discovery.hpp"
#include "grabber.hpp"
#include "srt.hpp"
#include "timefmt.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <vector>

namespace exporter {

namespace {

// Cue text as one Markdown-safe line; the raw srt text is the
// source-material truth, only the line structure is folded.
QString oneLine(std::string const &text)
{
	QString s = QString::fromUtf8(text.data(),
	                              qsizetype(text.size()));
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

std::vector<srt::cue> loadCues(QString const &path)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
		return {};
	QByteArray const raw = f.readAll();
	return srt::parse(srt::to_utf8({raw.constData(),
	                                size_t(raw.size())}));
}

// Copy one pick into the topic's frames/ dir, return its image link.
QString frameLink(Grabber &grab, source const &v, qint64 ms,
                  QString const &tdir)
{
	QString const name = safeStem(v.video) + QLatin1Char('-')
	                   + QString::number(ms)
	                   + QStringLiteral(".png");
	QString const dst = tdir + QStringLiteral("/frames/") + name;
	if (!QFile::exists(dst))
		QFile::copy(grab.framePath(v.id, ms), dst);
	return QStringLiteral("![](frames/") + name
	     + QStringLiteral(") ");
}

void exportHit(QString &md, stats &st,
               std::vector<srt::cue> const &cues, std::size_t i,
               source const &v, Grabber &grab, QString const &tdir)
{
	srt::cue const &c = cues[i];
	++st.hits;
	md += QStringLiteral("\n### ") + fmtTime(c.start, true)
	    + QStringLiteral("\n\n");
	if (i > 0)
		md += QStringLiteral("> ") + oneLine(cues[i - 1].text)
		    + QStringLiteral("\n");
	md += QStringLiteral("> **") + oneLine(c.text)
	    + QStringLiteral("**\n");
	if (i + 1 < cues.size())
		md += QStringLiteral("> ") + oneLine(cues[i + 1].text)
		    + QStringLiteral("\n");

	qint64 const ms = qint64(c.start * 1000.0 + 0.5);
	qint64 prev = -1, next = -1;
	if (!grab.picksFor(v.id, ms, prev, next)) {
		grab.enqueue(v.video, v.id, c.start);
		++st.queued;
		md += QStringLiteral("\n*(frames pending)*\n");
		return;
	}
	md += QStringLiteral("\n");
	for (qint64 const f : {prev, ms, next})
		if (f >= 0)
			md += frameLink(grab, v, f, tdir);
	md += QStringLiteral("\n");
	++st.framed;
}

void exportVideo(QString &md, stats &st, QRegularExpression const &re,
                 source const &v, Grabber &grab, QString const &tdir)
{
	QString err, srtPath = v.srt;
	if (srtPath.isEmpty())
		srtPath = srtForVideo(v.video, &err);
	std::vector<srt::cue> const cues = loadCues(srtPath);
	bool head = false;
	for (std::size_t i = 0; i < cues.size(); ++i) {
		if (!re.match(oneLine(cues[i].text)).hasMatch())
			continue;
		if (!head) {
			md += QStringLiteral("\n## ")
			    + QFileInfo(v.video).fileName()
			    + QStringLiteral("\n");
			head = true;
		}
		exportHit(md, st, cues, i, v, grab, tdir);
	}
}

} // namespace

stats run(topics::doc const &corpus, QList<source> const &videos,
          Grabber &grab, QString const &outDir)
{
	stats st;
	for (topics::topic const &t : corpus.topics) {
		QString const name = QString::fromStdString(t.name);
		QRegularExpression const re(
			QString::fromStdString(topics::expand(corpus, t)),
			QRegularExpression::CaseInsensitiveOption);
		QString const tdir = outDir + QLatin1Char('/') + name;
		QDir().mkpath(tdir + QStringLiteral("/frames"));
		QString md = QStringLiteral("# ") + name
		           + QStringLiteral("\n\nPattern: `") + re.pattern()
		           + QStringLiteral("`\n");
		if (!re.isValid())
			md += QStringLiteral("\n(invalid pattern: ")
			    + re.errorString() + QStringLiteral(")\n");
		// Scoping restricts only when present: a video without
		// topic references takes part in every topic, matching the
		// corpus-wide behavior of the interactive search.
		for (source const &v : videos)
			if (re.isValid() && (v.topics.isEmpty()
			                     || v.topics.contains(name)))
				exportVideo(md, st, re, v, grab, tdir);
		QFile f(tdir + QLatin1Char('/') + name
		        + QStringLiteral(".md"));
		if (f.open(QIODevice::WriteOnly | QIODevice::Truncate
		           | QIODevice::Text))
			f.write(md.toUtf8());
		++st.topics;
	}
	return st;
}

} // namespace exporter
