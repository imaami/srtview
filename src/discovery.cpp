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

QString sockForVideo(const QString &video, QString *err)
{
	const QString rp = QFileInfo(video).canonicalFilePath();
	if (rp.isEmpty()) {
		*err = QStringLiteral("cannot resolve path: %1").arg(video);
		return {};
	}
	const QByteArray h = QCryptographicHash::hash(rp.toUtf8(),
	                     QCryptographicHash::Sha256).toHex().left(16);
	return runDir() + QLatin1Char('/') + QString::fromLatin1(h)
	                + QStringLiteral(".sock");
}

bool socketAlive(const QString &sock)
{
	QLocalSocket probe;
	probe.connectToServer(sock);
	const bool ok = probe.waitForConnected(200);
	probe.abort();
	return ok;
}

QString srtForVideo(const QString &video, QString *err)
{
	const QString direct = video + QStringLiteral(".srt");
	if (QFileInfo(direct).isReadable())
		return direct;
	const QFileInfo fi(video);
	if (fi.fileName().contains(QLatin1Char('.'))) {
		const QString alt = fi.path() + QLatin1Char('/')
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

QString videoForSrt(const QString &srt, QString *err)
{
	QString x = srt;
	x.chop(4);                                   // ".srt"
	if (QFileInfo(x).isFile())
		return x;
	const QFileInfo fi(x);
	const QDir dir = fi.dir();
	QStringList cands;
	const auto entries = dir.entryList({fi.fileName() + QStringLiteral(".*")},
	                                   QDir::Files);
	for (const QString &e : entries)
		if (!e.endsWith(QStringLiteral(".srt")))
			cands << dir.filePath(e);
	if (cands.isEmpty()) {
		*err = QStringLiteral("no video found for %1").arg(srt);
		return {};
	}
	if (cands.size() == 1)
		return cands.first();
	QStringList live;
	for (const QString &c : cands) {
		QString e2;
		const QString s = sockForVideo(c, &e2);
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
