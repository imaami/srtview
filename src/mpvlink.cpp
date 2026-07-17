#include "mpvlink.hpp"

#include "discovery.hpp"
#include "mainwin.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include <chrono>
#include <thread>

template <class Host>
MpvLink<Host>::MpvLink(Host *host)
	: m_host(host)
{
	connect(&m_conn, &QLocalSocket::readyRead,
	        this, &MpvLink::onReadyRead);
}

template <class Host>
bool MpvLink<Host>::openFor(const QString &video, const QString &srt,
                            QString *err)
{
	shutdown();
	m_video = video;
	m_srt   = srt;
	m_sock  = sockForVideo(video, err);
	if (m_sock.isEmpty())
		return false;
	return ensureAlive(err);
}

template <class Host>
bool MpvLink<Host>::ensureAlive(QString *err)
{
	if (m_conn.state() == QLocalSocket::ConnectedState)
		return true;
	if (tryConnect())
		return true;                     // reuse a running instance
	if (m_video.isEmpty()) {
		*err = QStringLiteral("no video associated");
		return false;
	}
	QFile::remove(m_sock);               // stale leftover
	QStringList args{QStringLiteral("--no-terminal"),
	                 QStringLiteral("--pause"),
	                 QStringLiteral("--keep-open=yes"),
	                 QStringLiteral("--input-ipc-server=") + m_sock,
	                 QStringLiteral("--sub-file=") + m_srt,
	                 QStringLiteral("--sub-auto=no")};
	args += QProcess::splitCommand(qEnvironmentVariable("SRTVIEW_MPV_ARGS"));
	args << QStringLiteral("--") << m_video;
	m_proc.setProgram(QStringLiteral("mpv"));
	m_proc.setArguments(args);
	m_proc.start();
	if (!m_proc.waitForStarted(3000)) {
		*err = QStringLiteral("cannot start mpv: %1")
		       .arg(m_proc.errorString());
		return false;
	}
	m_spawned = true;
	for (int i = 0; i < 50; ++i) {       // wait for the socket
		if (tryConnect())
			return true;
		if (m_proc.state() != QProcess::Running) {
			*err = QStringLiteral("mpv exited before its socket came up");
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		QCoreApplication::processEvents();
	}
	*err = QStringLiteral("mpv IPC socket never came up: %1").arg(m_sock);
	return false;
}

template <class Host>
bool MpvLink<Host>::send(const QString &line, QString *err)
{
	if (!ensureAlive(err))
		return false;
	m_conn.write(line.toUtf8() + '\n');
	m_conn.flush();
	// flush() usually drains the whole buffer; only wait if the
	// kernel pushed back.
	return m_conn.bytesToWrite() == 0 || m_conn.waitForBytesWritten(500);
}

template <class Host>
bool MpvLink<Host>::seek(double t, bool forcePause, QString *err)
{
	const QString s = QStringLiteral("no-osd seek %1 absolute+exact")
	                  .arg(t, 0, 'f', 3);
	return send(forcePause
		? QStringLiteral("no-osd set pause yes; ") + s : s, err);
}

template <class Host>
bool MpvLink<Host>::seekRel(double dt, QString *err)
{
	return send(QStringLiteral("no-osd seek %1").arg(dt, 0, 'f', 1), err);
}

template <class Host>
bool MpvLink<Host>::setPause(bool on, QString *err)
{
	return send(QStringLiteral("no-osd set pause %1")
	            .arg(on ? QStringLiteral("yes") : QStringLiteral("no")),
	            err);
}

template <class Host>
bool MpvLink<Host>::cyclePause(QString *err)
{
	return send(QStringLiteral("no-osd cycle pause"), err);
}

template <class Host>
void MpvLink<Host>::shutdown()
{
	if (m_spawned && m_proc.state() == QProcess::Running) {
		QString e;
		send(QStringLiteral("quit"), &e);
		if (!m_proc.waitForFinished(1500))
			m_proc.kill();
		QFile::remove(m_sock);
	}
	m_conn.abort();
	m_inbuf.clear();
	m_spawned = false;
	m_video.clear();
	m_srt.clear();
	m_sock.clear();
}

template <class Host>
bool MpvLink<Host>::tryConnect()
{
	if (m_sock.isEmpty())
		return false;
	m_conn.abort();
	m_inbuf.clear();
	m_conn.connectToServer(m_sock);
	if (!m_conn.waitForConnected(200))
		return false;
	observe();
	return true;
}

// Property observation is per-client: register on every connect.
template <class Host>
void MpvLink<Host>::observe()
{
	m_conn.write("{\"command\":[\"observe_property\",1,"
	             "\"playback-time\"]}\n");
	m_conn.flush();
}

template <class Host>
void MpvLink<Host>::onReadyRead()
{
	m_inbuf += m_conn.readAll();
	while (true) {
		const qsizetype nl = m_inbuf.indexOf('\n');
		if (nl < 0)
			return;
		const QJsonDocument doc =
			QJsonDocument::fromJson(m_inbuf.left(nl));
		m_inbuf.remove(0, nl + 1);
		if (doc.isObject())
			dispatch(doc.object());
	}
}

template <class Host>
void MpvLink<Host>::dispatch(const QJsonObject &ev)
{
	if (ev.value(QStringLiteral("event"))
	    != QStringLiteral("property-change"))
		return;
	if (ev.value(QStringLiteral("name"))
	    != QStringLiteral("playback-time"))
		return;
	const QJsonValue v = ev.value(QStringLiteral("data"));
	if (v.isDouble())
		m_host->onMpvTime(v.toDouble());
}

template class MpvLink<MainWin>;
