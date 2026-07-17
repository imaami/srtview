// mpvlink.hpp -- mpv process ownership and IPC.
//
// Commands go out as single raw input.conf lines: mpv parses non-JSON
// lines as command lists, and one line per action sidesteps mpv
// dropping buffered lines when a client disconnects.  QLocalSocket
// buffers writes internally, so every send() flushes explicitly;
// without it each command reaches mpv one write late.
//
// Split for deduplication: mpv_link_base carries the whole process /
// socket / JSON machinery, compiled once in mpvlink.cpp; MpvLink<Obs>
// is a header-only adapter adding only event delivery to a
// concept-constrained observer.  Controllers hold mpv_link_base& and
// never see the template.
#ifndef SRTVIEW_SRC_MPVLINK_HPP_
#define SRTVIEW_SRC_MPVLINK_HPP_

#include "concepts.hpp"

#include <QByteArray>
#include <QLocalSocket>
#include <QObject>
#include <QProcess>
#include <QString>

class mpv_link_base : public QObject
{
public:
	bool openFor(QString const &video, QString const &srt, QString *err);

	// True if a player is (or can be brought) alive on our socket.
	bool ensureAlive(QString *err);

	bool send(QString const &line, QString *err);

	bool seek(double t, bool forcePause, QString *err);
	bool seekRel(double dt, QString *err);
	bool setPause(bool on, QString *err);
	bool cyclePause(QString *err);

	// Quit mpv only if this instance started it.
	void shutdown();

	bool spawned() const { return m_spawned; }

protected:
	mpv_link_base() = default;

	// Drain the socket into the line buffer.
	void fill();

	// Next observed playback-time value, if a complete event line is
	// buffered.  Property observation is per-client and re-registered
	// on every connect.
	bool takeTime(double &t);

	QLocalSocket &conn() { return m_conn; }

private:
	bool tryConnect();
	void observe();

	QProcess     m_proc;
	QLocalSocket m_conn;
	QByteArray   m_inbuf;
	QString      m_video, m_srt, m_sock;
	bool         m_spawned = false;
};

template <mpv_observer Obs>
class MpvLink final : public mpv_link_base
{
public:
	explicit MpvLink(Obs *observer)
		: m_obs(observer)
	{
		connect(&conn(), &QLocalSocket::readyRead,
		        this, [this] { pump(); });
	}

private:
	void pump()
	{
		fill();
		for (double t; takeTime(t);)
			m_obs->onMpvTime(t);
	}

	Obs *m_obs;
};

#endif // SRTVIEW_SRC_MPVLINK_HPP_
