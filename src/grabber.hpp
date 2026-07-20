// grabber.hpp -- background frame grabs via a private headless mpv.
//
// A shadow player (--vo=null, paused, per-process socket -- never the
// shared viewing instance) follows the study session: every video
// jump enqueues its timestamp, and the grabber extracts frames into a
// persistent cache, $XDG_CACHE_HOME/srtview/frames/<video id>/<ms>.png.
// For each hit it keeps three picks -- the frame at the hit and one
// from the different-looking content on either side -- by bisecting
// for the nearest content-change boundaries (downscaled grayscale
// mean difference; slides and screenshares are near-constant inside a
// segment, so changes localize in a handful of probes).  On footage
// where everything moves the bisection degenerates to nearby offsets
// by itself, so three relevant frames always exist.  Picks append to
// picks.txt beside the frames, one "<hit> <prev> <next>" ms line each
// (-1 for an absent side); the same file is the dedupe record, so
// revisits (ring travel, undo) cost nothing across sessions.
//
// IPC rides the shared mpv_client machinery: raw command lines out,
// mpv's JSON event lines back for sequencing -- a seek is done at
// playback-restart, a screenshot when its file parses as an image.
// One serial queue; a corporate WSL2 box must never see two decoders
// competing.
#ifndef SRTVIEW_SRC_GRABBER_HPP_
#define SRTVIEW_SRC_GRABBER_HPP_

#include "mpvclient.hpp"

#include <QHash>
#include <QImage>
#include <QList>
#include <QSet>
#include <QString>
#include <QTimer>

#include <utility>

// Told about grab completion; implemented by the composition root
// (which may be folding finished frames into an export).  grabsIdle
// fires when the queue drains, grabProgress after each mid-queue job.
struct grab_listener {
	virtual void grabsIdle() = 0;
	virtual void grabProgress() {}

protected:
	~grab_listener() = default;
};

class Grabber : public mpv_client<Grabber>
{
public:
	Grabber();
	~Grabber() override;

	// Event sink for the shared dispatcher.
	void onEvent(QJsonObject const &ev);

	void setListener(grab_listener *l) { m_listener = l; }

	// The video whose jumps are being followed (set on every open).
	void setVideo(QString const &path, QString const &id);

	// A jump landed at t seconds: schedule its three picks.
	void enqueue(double t);

	// Backfill flavor: schedule a hit in any video (export).
	void enqueue(QString const &path, QString const &id, double t);

	// The recorded picks of a hit; false while not yet grabbed.
	bool picksFor(QString const &id, qint64 hitMs,
	              qint64 &prev, qint64 &next);

	// Cache location of one frame (may not exist).
	QString framePath(QString const &id, qint64 ms) const;

	void shutdown();

private:
	struct Job {
		enum Phase : int {
			anchor,     // grab the hit frame itself
			back0,      // probe the far edge of the back window
			backBisect, // localize the boundary behind
			fwd0,       // probe the far edge ahead
			fwdBisect,  // localize the boundary ahead
		};

		QImage  ref;         // hit-frame thumb, the segment identity
		QString path, id;
		qint64  hit = 0, lo = 0, hi = 0;
		qint64  prevMs = -1, nextMs = -1;
		Phase   phase = anchor;
	};

	enum class Stage : int { idle, spawn, load, seekWait, shoot };

	void startJob();
	void want(qint64 ms);
	void pump();
	void tick();
	void onConnected();
	void onRestart();
	void deliver(QImage const &full);
	void advance(QImage const &tn, qint64 ms);
	void bisectBack(Job &j);
	void bisectFwd(Job &j);
	void finish(Job &j);
	void abortJob();
	void drained();
	void ensureProc();
	void armDeadline(qint64 ms);
	void loadKnown(QString const &id);
	QString dir(QString const &id) const;
	QString frameFile(qint64 ms) const;

	using PickMap = QHash<qint64, std::pair<qint64, qint64>>;

	QTimer                       m_poll;
	QElapsedTimer                m_deadline;
	QHash<QString, QSet<qint64>> m_known;   // grabbed or queued hits
	QHash<QString, PickMap>      m_picks;   // finished hits only
	QList<Job>                   m_jobs;
	QString                      m_path, m_id; // the followed video
	QString                      m_loadedId;   // in the shadow player
	grab_listener               *m_listener = nullptr;
	qint64                       m_wantMs = -1;
	qint64                       m_deadlineMs = 0;
	int                          m_strikes = 0;
	Stage                        m_stage = Stage::idle;
};

#endif // SRTVIEW_SRC_GRABBER_HPP_
