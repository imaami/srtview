// mpvlink.hpp -- owns the (possibly spawned) mpv process and a
// persistent connection to its IPC socket.
//
// Commands go out as single raw input.conf lines: mpv parses non-JSON
// lines as command lists, and one line per action sidesteps mpv
// dropping buffered lines when a client disconnects.  QLocalSocket
// buffers writes internally, so every send() flushes explicitly;
// without it each command reaches mpv one write late.
//
// The link also listens: on every connect it registers a JSON
// observe_property for playback-time (raw and JSON lines coexist on
// one IPC connection), and forwards each update to the host --
// statically bound, same pattern as the widgets: member definitions
// live in mpvlink.cpp, closed by an explicit instantiation.
#pragma once

#include <QByteArray>
#include <QLocalSocket>
#include <QObject>
#include <QProcess>
#include <QString>

class QJsonObject;

template <class Host>
class MpvLink : public QObject
{
public:
	explicit MpvLink(Host *host);

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
	void observe();
	void onReadyRead();
	void dispatch(const QJsonObject &ev);

	Host        *m_host;
	QProcess     m_proc;
	QLocalSocket m_conn;
	QByteArray   m_inbuf;
	QString      m_video, m_srt, m_sock;
	bool         m_spawned = false;
};
