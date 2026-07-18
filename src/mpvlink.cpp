#include "mpvlink.hpp"

#include "discovery.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include <chrono>
#include <cstdio>
#include <thread>

namespace {

bool debugEnabled()
{
	static bool const on = qEnvironmentVariableIsSet("SRTVIEW_DEBUG");
	return on;
}

} // namespace

mpv_link_base::mpv_link_base()
{
	m_clock.start();
}

bool mpv_link_base::openFor(QString const &video, QString const &srt,
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

bool mpv_link_base::ensureAlive(QString *err)
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
	if (m_resumeTime >= 0.0)
		args << QStringLiteral("--start=%1").arg(m_resumeTime, 0, 'f', 3);
	// WSLg's PulseAudio bridge can drop stream acknowledgments,
	// wedging mpv's core inside ao_pulse (no-timeout waits) on the
	// seek/pause storms this tool generates; keeping the stream open
	// avoids the restarts that trigger it.  Detect by the presence
	// of the bridge's own socket.  SRTVIEW_MPV_ARGS comes later on
	// the command line, so users can override (mpv is last-wins).
	if (QFile::exists(QStringLiteral("/mnt/wslg/PulseServer"))) {
		args << QStringLiteral("--audio-stream-silence=yes");
		dbg(QStringLiteral("WSLg pulse bridge detected: adding "
		                   "--audio-stream-silence=yes"));
	}
	args += QProcess::splitCommand(qEnvironmentVariable("SRTVIEW_MPV_ARGS"));
	args << QStringLiteral("--") << m_video;
	// Forward mpv's stdout/stderr to ours instead of the QProcess
	// default of capture pipes.  A captured pipe that nobody drains
	// holds 64 KiB; once graphics-stack warnings (chatty on WSLg)
	// fill it, mpv's next write blocks forever and its core wedges.
	// Forwarding removes the pipe entirely and makes mpv's own
	// diagnostics visible when srtview runs from a terminal.
	m_proc.setProcessChannelMode(QProcess::ForwardedChannels);
	m_proc.setProgram(QStringLiteral("mpv"));
	m_proc.setArguments(args);
	dbg(QStringLiteral("spawning mpv, socket %1").arg(m_sock));
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

bool mpv_link_base::send(QString const &line, QString *err)
{
	if (!ensureAlive(err))
		return false;
	dbg(QStringLiteral("tx: ") + line);
	m_conn.write(line.toUtf8() + '\n');
	m_conn.flush();
	// flush() usually drains the whole buffer; only wait if the
	// kernel pushed back.
	return m_conn.bytesToWrite() == 0 || m_conn.waitForBytesWritten(500);
}

bool mpv_link_base::seek(double t, bool forcePause, QString *err)
{
	QString const s = QStringLiteral("no-osd seek %1 absolute+exact")
	                  .arg(t, 0, 'f', 3);
	if (!send(forcePause
	          ? QStringLiteral("no-osd set pause yes; ") + s : s, err))
		return false;
	// Best-known position is the commanded one until mpv reports
	// otherwise: mpv does not always echo a property change for a
	// seek returning to the last value it reported.
	m_lastTime = t;
	return true;
}

bool mpv_link_base::seekRel(double dt, QString *err)
{
	return send(QStringLiteral("no-osd seek %1").arg(dt, 0, 'f', 1), err);
}

bool mpv_link_base::setPause(bool on, QString *err)
{
	return send(QStringLiteral("no-osd set pause %1")
	            .arg(on ? QStringLiteral("yes") : QStringLiteral("no")),
	            err);
}

bool mpv_link_base::cyclePause(QString *err)
{
	return send(QStringLiteral("no-osd cycle pause"), err);
}

void mpv_link_base::shutdown()
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

void mpv_link_base::fill()
{
	QByteArray const chunk = m_conn.readAll();
	if (chunk.isEmpty())
		return;
	m_inbuf += chunk;
	m_lastRx = m_clock.elapsed();
	dbg(QStringLiteral("rx %1 bytes: %2").arg(chunk.size())
	    .arg(QString::fromUtf8(chunk.left(120)).trimmed()));
}

bool mpv_link_base::takeTime(double &t)
{
	while (true) {
		qsizetype const nl = m_inbuf.indexOf('\n');
		if (nl < 0)
			return false;
		QJsonDocument const doc =
			QJsonDocument::fromJson(m_inbuf.left(nl));
		m_inbuf.remove(0, nl + 1);
		if (!doc.isObject())
			continue;
		QJsonObject const ev = doc.object();
		if (ev.value(QStringLiteral("event"))
		    != QStringLiteral("property-change"))
			continue;
		QJsonValue const v = ev.value(QStringLiteral("data"));
		if (ev.value(QStringLiteral("name"))
		    == QStringLiteral("pause")) {
			m_lastPause = v.toBool(m_lastPause);
			continue;
		}
		if (ev.value(QStringLiteral("name"))
		    != QStringLiteral("playback-time") || !v.isDouble())
			continue;
		t = v.toDouble();
		m_lastTime = t;
		return true;
	}
}

bool mpv_link_base::tryConnect()
{
	if (m_sock.isEmpty())
		return false;
	m_conn.abort();
	m_inbuf.clear();
	m_conn.connectToServer(m_sock);
	if (!m_conn.waitForConnected(200))
		return false;
	m_lastRx = m_clock.elapsed();        // fresh connection, fresh clock
	observe();
	dbg(QStringLiteral("connected to %1").arg(m_sock));
	return true;
}

bool mpv_link_base::recoverWedged()
{
	if (!m_spawned) {
		note(QStringLiteral("wedged player was not started by "
		                    "srtview; not killing it"));
		return false;
	}
	if (m_clock.elapsed() - m_lastRespawn < 30000)
		return false;
	m_lastRespawn = m_clock.elapsed();
	m_recovering = true;
	double const t = m_lastTime;
	bool const paused = m_lastPause;
	note(QStringLiteral("killing wedged mpv, respawning at %1s "
	                    "(%2)")
	     .arg(t < 0.0 ? 0.0 : t, 0, 'f', 3)
	     .arg(paused ? QStringLiteral("paused")
	                 : QStringLiteral("playing")));
	m_proc.kill();
	m_proc.waitForFinished(2000);
	m_conn.abort();
	m_inbuf.clear();
	m_spawned = false;
	m_resumeTime = t < 0.0 ? -1.0 : t;
	QString err;
	bool const ok = ensureAlive(&err);
	m_resumeTime = -1.0;
	if (!ok) {
		note(QStringLiteral("respawn failed: ") + err);
		m_recovering = false;
		return true;
	}
	if (!paused)
		setPause(false, &err);
	note(QStringLiteral("respawn complete"));
	m_recovering = false;
	return true;
}

void mpv_link_base::sendPing()
{
	m_conn.write("{\"command\":[\"get_property\",\"pid\"],"
	             "\"request_id\":900913}\n");
	m_conn.flush();
}

bool mpv_link_base::connectedNow() const
{
	return m_conn.state() == QLocalSocket::ConnectedState;
}

qint64 mpv_link_base::msecsSinceRx() const
{
	return m_clock.elapsed() - m_lastRx;
}

void mpv_link_base::note(QString const &msg) const
{
	std::fprintf(stderr, "srtview[%9.3f]: %s\n",
	             m_clock.elapsed() / 1000.0, qPrintable(msg));
	std::fflush(stderr);
}

void mpv_link_base::dbg(QString const &msg) const
{
	if (debugEnabled())
		note(msg);
}

void mpv_link_base::observe()
{
	m_conn.write("{\"command\":[\"observe_property\",1,"
	             "\"playback-time\"]}\n"
	             "{\"command\":[\"observe_property\",2,"
	             "\"pause\"]}\n");
	m_conn.flush();
}
