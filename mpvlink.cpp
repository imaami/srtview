#include "mpvlink.hpp"

#include "discovery.hpp"

#include <QCoreApplication>
#include <QFile>

#include <chrono>
#include <thread>

MpvLink::MpvLink()
{
	// mpv replies to nothing we care about; keep the buffer empty.
	connect(&m_conn, &QLocalSocket::readyRead,
	        this, [this] { m_conn.readAll(); });
}

bool MpvLink::openFor(const QString &video, const QString &srt, QString *err)
{
	shutdown();
	m_video = video;
	m_srt   = srt;
	m_sock  = sockForVideo(video, err);
	if (m_sock.isEmpty())
		return false;
	return ensureAlive(err);
}

bool MpvLink::ensureAlive(QString *err)
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

bool MpvLink::send(const QString &line, QString *err)
{
	if (!ensureAlive(err))
		return false;
	m_conn.write(line.toUtf8() + '\n');
	m_conn.flush();
	// flush() usually drains the whole buffer; only wait if the
	// kernel pushed back.
	return m_conn.bytesToWrite() == 0 || m_conn.waitForBytesWritten(500);
}

bool MpvLink::seek(double t, bool forcePause, QString *err)
{
	const QString s = QStringLiteral("no-osd seek %1 absolute+exact")
	                  .arg(t, 0, 'f', 3);
	return send(forcePause
		? QStringLiteral("no-osd set pause yes; ") + s : s, err);
}

bool MpvLink::seekRel(double dt, QString *err)
{
	return send(QStringLiteral("no-osd seek %1").arg(dt, 0, 'f', 1), err);
}

bool MpvLink::setPause(bool on, QString *err)
{
	return send(QStringLiteral("no-osd set pause %1")
	            .arg(on ? QStringLiteral("yes") : QStringLiteral("no")),
	            err);
}

bool MpvLink::cyclePause(QString *err)
{
	return send(QStringLiteral("no-osd cycle pause"), err);
}

void MpvLink::shutdown()
{
	if (m_spawned && m_proc.state() == QProcess::Running) {
		QString e;
		send(QStringLiteral("quit"), &e);
		if (!m_proc.waitForFinished(1500))
			m_proc.kill();
		QFile::remove(m_sock);
	}
	m_conn.abort();
	m_spawned = false;
	m_video.clear();
	m_srt.clear();
	m_sock.clear();
}

bool MpvLink::tryConnect()
{
	if (m_sock.isEmpty())
		return false;
	m_conn.abort();
	m_conn.connectToServer(m_sock);
	return m_conn.waitForConnected(200);
}
