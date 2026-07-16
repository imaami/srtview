// mpvlink.hpp -- owns the (possibly spawned) mpv process and a
// persistent connection to its IPC socket.
//
// Commands go out as single raw input.conf lines: mpv parses non-JSON
// lines as command lists, and one line per action sidesteps mpv
// dropping buffered lines when a client disconnects.  QLocalSocket
// buffers writes internally, so every send() flushes explicitly;
// without it each command reaches mpv one write late.
#pragma once

#include <QLocalSocket>
#include <QObject>
#include <QProcess>
#include <QString>

class MpvLink : public QObject
{
public:
	MpvLink();

	bool openFor(const QString &video, const QString &srt, QString *err);

	// True if a player is (or can be brought) alive on our socket.
	bool ensureAlive(QString *err);

	bool send(const QString &line, QString *err);

	bool seek(double t, bool forcePause, QString *err);
	bool seekRel(double dt, QString *err);
	bool setPause(bool on, QString *err);
	bool cyclePause(QString *err);

	// Quit mpv only if this instance started it.
	void shutdown();

	bool spawned() const { return m_spawned; }

private:
	bool tryConnect();

	QProcess     m_proc;
	QLocalSocket m_conn;
	QString      m_video, m_srt, m_sock;
	bool         m_spawned = false;
};
