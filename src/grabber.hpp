// grabber.hpp -- background frame grabs via an in-process libav
// decoder.
//
// The grabber shadows the study session from its own worker thread:
// every video jump enqueues its timestamp, and three picks per hit
// -- the hit frame plus one from the different-looking content on
// either side, found by bisecting for content-change boundaries --
// land in $XDG_CACHE_HOME/srtview/frames/<video id>/<ms>.png, with
// picks.txt as both manifest and cross-session dedupe record.
//
// The decode context persists per video (decoder.hpp); bisection
// probes are decoded straight into 64x36 grayscale compare thumbs
// and never touch a PNG encoder or the disk -- only picks are
// encoded.  A hit inside an already-bisected segment reuses that
// segment's boundaries (content-compared), so a cluster of hits on
// one slide costs one encoded frame each.  On footage where
// everything moves the bisection degenerates to nearby offsets by
// itself, so three relevant frames always exist.
//
// Mutating entry points marshal onto the worker, queries read the
// shared maps under a lock, and listener notifications are queued
// into the listener's thread.
#ifndef SRTVIEW_SRC_GRABBER_HPP_
#define SRTVIEW_SRC_GRABBER_HPP_

#include <QHash>
#include <QImage>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThread>

#include <utility>

#include "decoderq.hpp"

// Told about grab completion; implemented by the composition root
// (which may be folding finished frames into an export).  grabsIdle
// fires when the queue drains, grabProgress after each mid-queue job.
struct grab_listener {
	virtual void grabsIdle() = 0;
	virtual void grabProgress() {}

protected:
	~grab_listener() = default;
};

class Grabber : public QObject
{
public:
	Grabber();
	~Grabber() override;

	// The listener is invoked queued in ctx's thread.
	void setListener(QObject *ctx, grab_listener *l);

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
		QString path, id;
		qint64  hit = 0;
	};

	void setVideoImpl(QString const &path, QString const &id);
	void enqueueImpl(QString const &path, QString const &id,
	                 double t);
	void shutdownImpl();
	void startJob();
	void runJob();
	qint64 boundary(qint64 hit, media::thumb const &ref, int dir);
	bool reuseSegment(Job const &j, media::thumb const &ref,
	                  qint64 &prev, qint64 &next);
	bool ensurePick(QString const &id, qint64 ms);
	void finishJob(Job const &j, qint64 prev, qint64 next);
	void abortJob();
	void drained();
	void loadKnown(QString const &id);
	QString dir(QString const &id) const;

	using PickMap = QHash<qint64, std::pair<qint64, qint64>>;

	QThread                      m_thread;  // not a child: stays put
	DecoderQ                     m_dec;
	QMutex                       m_lock;    // the two maps below
	QHash<QString, QSet<qint64>> m_known;   // grabbed or queued hits
	QHash<QString, PickMap>      m_picks;   // finished hits only
	QList<Job>                   m_jobs;
	QString                      m_path, m_id; // the followed video
	QObject                     *m_ctx = nullptr;
	grab_listener               *m_listener = nullptr;
	unsigned                     m_strikes = 0;
};

#endif // SRTVIEW_SRC_GRABBER_HPP_
