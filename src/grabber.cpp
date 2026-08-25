// grabber.cpp -- see grabber.hpp for the design.
#include <QDir>
#include <QFile>
#include <QTimer>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "grabber.hpp"

namespace {

// Content-change probing: how far to look for the neighboring
// segments, how fine to localize their boundaries, and how different
// two downscaled thumbs must be to count as different content.
constexpr qint64 kWinMs   = 30000;
constexpr qint64 kStepMs  = 1000;
constexpr int    kDiffMean = 8;

// Qt's PNG quality knob maps inversely onto zlib effort; this sits
// near zlib level 2 -- measured ~1.7x the encode rate of the default
// for a modest size increase, still lossless.
constexpr int kPngQuality = 80;

QString cacheRoot()
{
	QString base = qEnvironmentVariable("XDG_CACHE_HOME");
	if (base.isEmpty())
		base = QDir::homePath() + QStringLiteral("/.cache");
	return base + QStringLiteral("/srtview/frames");
}

bool same(media::thumb const &a, media::thumb const &b)
{
	return media::same(a, b, kDiffMean);
}

} // namespace

Grabber::Grabber()
{
	moveToThread(&m_thread);
	m_thread.start();
}

Grabber::~Grabber()
{
	shutdown();
	m_thread.quit();
	m_thread.wait();
}

void Grabber::setListener(QObject *ctx, grab_listener *l)
{
	m_ctx = ctx;
	m_listener = l;
}

void Grabber::setSink(pick_sink *s)
{
	m_sink = s;
}

void Grabber::setVideo(QString const &path, QString const &id)
{
	QMetaObject::invokeMethod(this, [this, path, id] {
		setVideoImpl(path, id);
	}, Qt::QueuedConnection);
}

void Grabber::setVideoImpl(QString const &path, QString const &id)
{
	if (path.isEmpty() || id.isEmpty()) {
		// An unresolvable identity clears the target: a jump
		// must never grab into the previous video's cache.
		m_path.clear();
		m_id.clear();
		return;
	}
	m_path = path;
	m_id = id;
	loadKnown(id);
	replayPicks(path, id);
}

void Grabber::enqueue(double t)
{
	QMetaObject::invokeMethod(this, [this, t] {
		enqueueImpl(m_path, m_id, t, true);
	}, Qt::QueuedConnection);
}

void Grabber::enqueue(QString const &path, QString const &id, double t)
{
	QMetaObject::invokeMethod(this, [this, path, id, t] {
		enqueueImpl(path, id, t, false);
	}, Qt::QueuedConnection);
}

void Grabber::enqueueImpl(QString const &path, QString const &id,
                          double t, bool rush)
{
	if (addJob(path, id, t, rush) && m_jobs.size() == 1)
		startJob();
}

// The shared tail of demand and backfill: dedupe, then a queued
// job.  True only when a job was actually appended.
bool Grabber::addJob(QString const &path, QString const &id,
                     double t, bool rush)
{
	if (path.isEmpty() || id.isEmpty() || t < 0.0)
		return false;
	loadKnown(id);
	replayPicks(path, id);
	qint64 const ms = qint64(t * 1000.0 + 0.5);
	bool fresh;
	{
		QMutexLocker const lock(&m_lock);
		QSet<qint64> &known = m_known[id];
		fresh = !known.contains(ms);
		if (fresh)
			known.insert(ms);
	}
	if (!fresh) {
		// A known hit is not re-grabbed, but the frame under
		// inspection still deserves its rush: announced again,
		// the scribe promotes a queued twin out of the rest
		// lane or the archive answers on the spot.  Without
		// this, a first-migration session parks the inspected
		// frame behind the whole replay backlog.
		if (rush && m_sink)
			m_sink->pickReady(path, id, ms, true);
		return false;
	}
	Job j;
	j.path = path;
	j.id = id;
	j.hit = ms;
	j.rush = rush;
	m_jobs << j;
	return true;
}

void Grabber::backfill(QList<grab_feed> const &feeds)
{
	QMetaObject::invokeMethod(this, [this, feeds] {
		backfillImpl(feeds);
	}, Qt::QueuedConnection);
}

void Grabber::prefer(QString const &id)
{
	QMetaObject::invokeMethod(this, [this, id] {
		preferImpl(id);
	}, Qt::QueuedConnection);
}

void Grabber::backfillImpl(QList<grab_feed> const &feeds)
{
	m_backfill.clear();
	for (grab_feed const &f : feeds)
		if (!f.path.isEmpty() && !f.id.isEmpty()
		    && !f.times.isEmpty())
			m_backfill << BackFeed{f, 0};
	// An idle worker has nothing left to call drained(): kick
	// the plan off here.  A busy one refills on its own drain.
	if (m_jobs.isEmpty() && refill())
		startJob();
}

void Grabber::preferImpl(QString const &id)
{
	for (qsizetype i = 1; i < m_backfill.size(); ++i) {
		if (m_backfill[i].f.id != id)
			continue;
		m_backfill.move(i, 0);
		return;
	}
}

// One backfill job when demand runs dry: the front feed's moments
// walk through the same dedupe the demand path uses, and only an
// actually-new hit becomes a job.  Cheap by design -- a known
// moment costs a set lookup, an exhausted feed is dropped.
bool Grabber::refill()
{
	while (!m_backfill.isEmpty()) {
		BackFeed &f = m_backfill.first();
		while (f.at < f.f.times.size()) {
			double const t = f.f.times[f.at++];
			if (addJob(f.f.path, f.f.id, t, false))
				return true;
		}
		m_backfill.removeFirst();
	}
	return false;
}

// The manifest replay: every pick of a video goes to the sink once
// per session, deduped across trios, whenever the first
// (path, id) pairing reaches the worker.  Replay state is its own
// latch, not loadKnown's: picksFor() may have loaded the manifest
// earlier from any thread, and that load must not swallow the
// replay.
void Grabber::replayPicks(QString const &path, QString const &id)
{
	if (!m_sink)
		return;
	PickMap picks;
	{
		QMutexLocker const lock(&m_lock);
		if (m_replayed.contains(id))
			return;
		m_replayed.insert(id);
		picks = m_picks.value(id);
	}
	QSet<qint64> seen;
	for (auto it = picks.cbegin(); it != picks.cend(); ++it) {
		auto const [p, n] = it.value();
		for (qint64 const ms : {it.key(), p, n}) {
			if (ms < 0 || seen.contains(ms))
				continue;
			seen.insert(ms);
			m_sink->pickReady(path, id, ms, false);
		}
	}
}

bool Grabber::picksFor(QString const &path, QString const &id,
                       qint64 hitMs, qint64 &prev, qint64 &next)
{
	loadKnown(id);
	QMutexLocker const lock(&m_lock);
	if (!m_replayed.contains(id))
		QMetaObject::invokeMethod(this, [this, path, id] {
			replayPicks(path, id);
		}, Qt::QueuedConnection);
	auto const video = m_picks.constFind(id);
	if (video == m_picks.constEnd())
		return false;
	auto const it = video->constFind(hitMs);
	if (it == video->constEnd())
		return false;
	prev = it->first;
	next = it->second;
	return true;
}

QString Grabber::framePath(QString const &id, qint64 ms) const
{
	return dir(id) + QLatin1Char('/') + QString::number(ms)
	     + QStringLiteral(".png");
}

void Grabber::shutdown()
{
	// The wave-off first: the blocking call below waits for the
	// running job, and a backfill keeps one running as the steady
	// state -- the bail turns seconds of bisection into one probe.
	m_bail.store(true);
	if (QThread::currentThread() == thread()) {
		shutdownImpl();
		return;
	}
	if (!m_thread.isRunning())
		return;
	QMetaObject::invokeMethod(this, [this] { shutdownImpl(); },
	                          Qt::BlockingQueuedConnection);
}

void Grabber::shutdownImpl()
{
	m_jobs.clear();
	m_backfill.clear();
	m_dec.close();
	m_bail.store(false);
}

void Grabber::startJob()
{
	// One job per event-loop turn: marshalled calls (enqueue,
	// shutdown) interleave between jobs.
	QTimer::singleShot(0, this, [this] { runJob(); });
}

void Grabber::runJob()
{
	if (m_jobs.isEmpty())
		return;
	Job const j = m_jobs.first();
	media::thumb ref;
	if ((m_dec.path() != j.path && !m_dec.open(j.path))
	    || !m_dec.thumbAt(j.hit, ref)) {
		abortJob();
		return;
	}
	// The inspected frame first, encoded and announced before the
	// boundary probing: its read must not wait on the bisection.
	if (!ensurePick(j.id, j.hit)) {
		abortJob();
		return;
	}
	if (m_sink)
		m_sink->pickReady(j.path, j.id, j.hit, j.rush);
	qint64 prev = -1, next = -1;
	if (!reuseSegment(j, ref, prev, next)) {
		prev = boundary(j.hit, ref, -1);
		next = boundary(j.hit, ref, +1);
	}
	// A shutdown mid-job walks away quietly: no strike, no log,
	// the queue is cleared right behind us.
	if (m_bail.load())
		return;
	if ((prev >= 0 && !ensurePick(j.id, prev))
	    || (next >= 0 && !ensurePick(j.id, next))) {
		abortJob();
		return;
	}
	finishJob(j, prev, next);
}

// Bisect toward the nearest content change in direction dir; the
// pick is the frame just beyond the boundary (in the neighboring
// segment), or the window edge when the segment runs past it.
qint64 Grabber::boundary(qint64 hit, media::thumb const &ref, int dir)
{
	qint64 const edge = std::max<qint64>(0, hit + dir * kWinMs);
	media::thumb probe;
	if (!m_dec.thumbAt(edge, probe))
		return -1;
	if (same(probe, ref))
		return edge;                 // one segment to the window
	qint64 nearMs = hit, farMs = edge;
	while (std::llabs(farMs - nearMs) > kStepMs) {
		if (m_bail.load())           // shutdown is waiting
			return -1;
		qint64 const mid = (nearMs + farMs) / 2;
		if (!m_dec.thumbAt(mid, probe))
			return -1;
		(same(probe, ref) ? nearMs : farMs) = mid;
	}
	return farMs;
}

// A hit inside an already-bisected segment shares its boundaries:
// same content as the other hit's frame plus a position inside its
// boundary window is the same segment.
bool Grabber::reuseSegment(Job const &j, media::thumb const &ref,
                           qint64 &prev, qint64 &next)
{
	PickMap picks;
	{
		QMutexLocker const lock(&m_lock);
		picks = m_picks.value(j.id);
	}
	for (auto it = picks.cbegin(); it != picks.cend(); ++it) {
		auto const [p, n] = it.value();
		if (p < 0 || n < 0 || j.hit <= p || j.hit >= n)
			continue;
		QImage other;
		if (!other.load(framePath(j.id, it.key()))
		    || !same(ref, DecoderQ::toThumb(other)))
			continue;
		prev = p;
		next = n;
		return true;
	}
	return false;
}

// Encode a pick only if the cache does not hold it yet.
bool Grabber::ensurePick(QString const &id, qint64 ms)
{
	QString const path = framePath(id, ms);
	if (QFile::exists(path))
		return true;
	QImage full;
	if (!m_dec.frameAt(ms, full))
		return false;
	return full.save(path, "png", kPngQuality);
}

void Grabber::finishJob(Job const &j, qint64 prev, qint64 next)
{
	QFile f(dir(j.id) + QStringLiteral("/picks.txt"));
	if (f.open(QIODevice::Append | QIODevice::Text))
		f.write(QByteArray::number(j.hit) + ' '
		        + QByteArray::number(prev) + ' '
		        + QByteArray::number(next) + '\n');
	{
		QMutexLocker const lock(&m_lock);
		m_picks[j.id].insert(j.hit, {prev, next});
	}
	if (m_sink) {
		if (prev >= 0 && prev != j.hit)
			m_sink->pickReady(j.path, j.id, prev, false);
		if (next >= 0 && next != j.hit)
			m_sink->pickReady(j.path, j.id, next, false);
	}
	m_strikes = 0;
	m_jobs.removeFirst();
	// Progress means "a job landed and more work remains" -- the
	// backfill counts: during it the queue holds one job at a
	// time, and the export continuation rides these ticks.
	if (m_listener && m_ctx
	    && (!m_jobs.isEmpty() || !m_backfill.isEmpty()))
		QMetaObject::invokeMethod(m_ctx, [l = m_listener] {
			l->grabProgress();
		}, Qt::QueuedConnection);
	if (!m_jobs.isEmpty())
		startJob();
	drained();
}

void Grabber::abortJob()
{
	if (m_jobs.isEmpty())
		return;
	Job const j = m_jobs.takeFirst();
	{
		QMutexLocker const lock(&m_lock);
		m_known[j.id].remove(j.hit); // retryable later
	}
	std::fprintf(stderr, "srtview: frame grab aborted at %lld ms\n",
	             static_cast<long long>(j.hit));
	m_dec.close();                       // suspect file: fresh open
	if (++m_strikes >= 3) {              // decoding is not working
		// Clearing the queue must not poison its moments:
		// un-know each so a jump or a later session retries.
		// The offending video's backfill remainder leaves the
		// plan -- one bad file must neither stall the corpus
		// nor grind through its own every cue breaker-blind --
		// and the strikes reset so the survivors get their
		// own three chances.
		{
			QMutexLocker const lock(&m_lock);
			for (Job const &q : m_jobs)
				m_known[q.id].remove(q.hit);
		}
		m_jobs.clear();
		for (qsizetype i = 0; i < m_backfill.size(); ++i) {
			if (m_backfill[i].f.id != j.id)
				continue;
			m_backfill.removeAt(i);
			break;
		}
		m_strikes = 0;
	} else if (!m_jobs.isEmpty())
		startJob();
	drained();
}

void Grabber::drained()
{
	if (!m_jobs.isEmpty())
		return;
	// Demand ran dry: the backfill keeps the worker fed, and
	// only a truly finished corpus reads as idle.
	if (refill()) {
		startJob();
		return;
	}
	if (!m_listener || !m_ctx)
		return;
	QMetaObject::invokeMethod(m_ctx, [l = m_listener] {
		l->grabsIdle();
	}, Qt::QueuedConnection);
}

void Grabber::loadKnown(QString const &id)
{
	QMutexLocker const lock(&m_lock);
	if (m_known.contains(id))
		return;
	QDir().mkpath(dir(id));              // first touch owns the dir
	QSet<qint64> &set = m_known[id];
	QFile f(dir(id) + QStringLiteral("/picks.txt"));
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
		return;
	PickMap &picks = m_picks[id];
	while (!f.atEnd()) {
		QList<QByteArray> const col =
			f.readLine().simplified().split(' ');
		if (col.size() != 3)
			continue;
		// A torn append parses as zeros under toLongLong's
		// no-questions default, and the manifest replay would
		// activate the fabricated frame; only rows the writer
		// could have produced are believed.
		bool hitOk = false, prevOk = false, nextOk = false;
		qint64 const hit = col[0].toLongLong(&hitOk);
		qint64 const prev = col[1].toLongLong(&prevOk);
		qint64 const next = col[2].toLongLong(&nextOk);
		if (!hitOk || !prevOk || !nextOk || hit < 0
		    || prev < -1 || next < -1
		    || (prev >= 0 && prev > hit)
		    || (next >= 0 && next < hit))
			continue;
		set.insert(hit);
		picks.insert(hit, {prev, next});
	}
}

QString Grabber::dir(QString const &id) const
{
	return cacheRoot() + QLatin1Char('/') + id;
}
