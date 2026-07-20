#include "discovery.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QLocalSocket>

namespace {

QString runDir()
{
	QString base = qEnvironmentVariable("XDG_RUNTIME_DIR");
	if (base.isEmpty())
		base = qEnvironmentVariable("TMPDIR", QStringLiteral("/tmp"));
	QString d = base + QStringLiteral("/srtjump");
	QDir().mkpath(d);
	return d;
}

} // namespace

QString idForVideo(QString const &video)
{
	QString const rp = QFileInfo(video).canonicalFilePath();
	if (rp.isEmpty())
		return {};
	QByteArray const h = QCryptographicHash::hash(rp.toUtf8(),
	                     QCryptographicHash::Sha256).toHex().left(16);
	return QString::fromLatin1(h);
}

QString sockForVideo(QString const &video, QString *err)
{
	QString const id = idForVideo(video);
	if (id.isEmpty()) {
		*err = QStringLiteral("cannot resolve path: %1").arg(video);
		return {};
	}
	return runDir() + QLatin1Char('/') + id + QStringLiteral(".sock");
}

bool socketAlive(QString const &sock)
{
	QLocalSocket probe;
	probe.connectToServer(sock);
	bool const ok = probe.waitForConnected(200);
	probe.abort();
	return ok;
}

QString srtForVideo(QString const &video, QString *err)
{
	QString const direct = video + QStringLiteral(".srt");
	if (QFileInfo(direct).isReadable())
		return direct;
	QFileInfo const fi(video);
	if (fi.fileName().contains(QLatin1Char('.'))) {
		QString const alt = fi.path() + QLatin1Char('/')
		                  + fi.completeBaseName() + QStringLiteral(".srt");
		if (QFileInfo(alt).isReadable())
			return alt;
		*err = QStringLiteral("subtitle file not found: %1 or %2")
		       .arg(direct, alt);
		return {};
	}
	*err = QStringLiteral("subtitle file not found: %1").arg(direct);
	return {};
}

QString videoForSrt(QString const &srt, QString *err)
{
	QString x = srt;
	x.chop(4);                                   // ".srt"
	if (QFileInfo(x).isFile())
		return x;
	QFileInfo const fi(x);
	QDir const dir = fi.dir();
	QStringList cands;
	auto const entries = dir.entryList({fi.fileName() + QStringLiteral(".*")},
	                                   QDir::Files);
	for (QString const &e : entries)
		if (!e.endsWith(QStringLiteral(".srt")))
			cands << dir.filePath(e);
	if (cands.isEmpty()) {
		*err = QStringLiteral("no video found for %1").arg(srt);
		return {};
	}
	if (cands.size() == 1)
		return cands.first();
	QStringList live;
	for (QString const &c : cands) {
		QString e2;
		QString const s = sockForVideo(c, &e2);
		if (!s.isEmpty() && socketAlive(s))
			live << c;
	}
	if (live.size() == 1)
		return live.first();
	*err = live.isEmpty()
		? QStringLiteral("ambiguous, no running player for any of:\n%1")
		  .arg(cands.join(QLatin1Char('\n')))
		: QStringLiteral("ambiguous, multiple running players:\n%1")
		  .arg(live.join(QLatin1Char('\n')));
	return {};
}
