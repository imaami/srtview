// mpvlink.hpp -- the viewing player: one persistent mpv whose
// internal playlist mirrors the app's.
//
// Switching videos is playlist navigation inside the same window --
// never a respawn, which would tear the window down and throw focus
// (brutal on WSLg).  The mirror is byte-for-byte in set and order,
// so mpv's own playlist keys agree with the app's: the observed path
// property routes mpv-side switches back to the observer, file-loaded
// attaches the entry's subtitles and fires deferred seeks, and
// playback-time / pause feed the transcript as before.  Adopting an
// already-running instance keeps its position when it is playing a
// list member (session resume; the srtjump sharing scheme for single
// videos), resyncing the playlist around the playing entry without
// reloading it.
//
// Bring-up never blocks the UI thread: connecting and spawning run
// off a retry timer, and commands sent meanwhile queue until the
// on-connect setup (observation, playlist resync) has gone out.
//
// Split for deduplication: mpv_link_base compiled once in
// mpvlink.cpp on top of the shared mpv_client machinery; MpvLink<Obs>
// is a header-only adapter adding event delivery to a
// concept-constrained observer and the health watchdog (a wedged core
// keeps the socket connected but answers nothing -- kill and respawn
// at the last observed position and pause state; throttled, and a
// reused player is never killed).
#ifndef SRTVIEW_SRC_MPVLINK_HPP_
#define SRTVIEW_SRC_MPVLINK_HPP_

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTimer>

#include "concepts.hpp"
#include "mpvclient.hpp"

struct play_entry {
	QString video, srt;                  // resolved paths

	bool operator==(play_entry const &) const = default;
};

class mpv_link_base : public mpv_client_base
{
public:
	// (Re)target the player: ensure an instance on sock, mirror
	// list into its internal playlist, and play entry index.  With
	// the same list live on the same socket this is at most
	// playlist navigation.
	bool setPlaylist(QList<play_entry> const &list,
	                 QString const &sock, int index, QString *err);

	// Playlist navigation within the current list.
	bool playIndex(int index, QString *err);
	int currentIndex() const { return m_index; }

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

	// Last observed playback-time, or negative before any event.
	double lastTime() const { return m_lastTime; }

	// Last observed pause state (true until told otherwise).
	bool lastPause() const { return m_lastPause; }

protected:
	mpv_link_base();

	// Pump buffered JSON event lines through onEvent().
	void dispatch();

	// Buffered observations for the adapter's pump.
	bool takeTime(double &t);
	bool takeIndex(int &i);

	// Liveness: any bytes from mpv stamp the receive clock; a cheap
	// get_property ping forces traffic even when nothing is playing.
	void sendPing();
	bool recoverWedged();
	bool recovering() const { return m_recovering; }

private:
	// Ready for commands: connected AND past the on-connect setup
	// (observe + resync); until then commands queue.
	bool ready() const { return connectedNow() && m_ready; }
	void onEvent(QJsonObject const &ev);
	void bringUp();
	void retryTick();
	void onConnected();
	void giveUp(QString const &why);
	QStringList spawnArgs() const;
	void observe();
	void resync(bool keepCurrent);
	void onLoaded();
	void onPath(QJsonValue const &v);

	QList<play_entry> m_list;
	QList<double>     m_times;
	QStringList       m_txq;             // queued during bring-up
	QTimer            m_retry;
	double            m_lastTime = -1.0;
	double            m_pendingSeek = -1.0;
	qint64            m_lastRespawn = -60000;
	int               m_index = 0;
	int               m_newIndex = -1;   // pending observer delivery
	unsigned          m_attempts = 0;
	bool              m_lastPause = true;
	bool              m_spawned = false;
	bool              m_recovering = false;
	bool              m_adopting = false; // resync awaits first path
	bool              m_starting = false; // bring-up in progress
	bool              m_ready = false;
	bool              m_unpause = false;  // restore after respawn
};

template <mpv_observer Obs>
class MpvLink final : public mpv_link_base
{
public:
	explicit MpvLink(Obs *observer)
		: m_obs(observer)
	{
		connect(&sock(), &QLocalSocket::readyRead,
		        this, [this] { pump(); });
		m_health.setInterval(2000);
		connect(&m_health, &QTimer::timeout, this, [this] { tick(); });
		m_health.start();
	}

private:
	void pump()
	{
		dispatch();
		for (double t; takeTime(t);)
			m_obs->onMpvTime(t);
		for (int i; takeIndex(i);)
			m_obs->onMpvIndex(i);
	}

	// Watchdog: ping, then judge by silence.  Flips are reported to
	// the observer and logged.
	void tick()
	{
		if (recovering() || !connectedNow())
			return;
		sendPing();
		dbg(QStringLiteral("tick: rx silence %1 ms")
		    .arg(msecsSinceRx()));
		bool const ok = msecsSinceRx() < kWedgeMs;
		if (ok == m_ok)
			return;
		m_ok = ok;
		note(ok ? QStringLiteral("mpv responding again")
		        : QStringLiteral("mpv unresponsive: no IPC traffic "
		                         "for %1 ms").arg(msecsSinceRx()));
		m_obs->onMpvState(ok);
		if (!ok)
			recoverWedged();
	}

	static constexpr qint64 kWedgeMs = 6000;

	Obs   *m_obs;
	QTimer m_health;
	bool   m_ok = true;
};

#endif // SRTVIEW_SRC_MPVLINK_HPP_
