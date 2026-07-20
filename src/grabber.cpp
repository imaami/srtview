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

void Grabber::setVideo(QString const &path, QString const &id)
{
	QMetaObject::invokeMethod(this, [this, path, id] {
		setVideoImpl(path, id);
	}, Qt::QueuedConnection);
}

void Grabber::setVideoImpl(QString const &path, QString const &id)
{
	if (path.isEmpty() || id.isEmpty())
		return;
	m_path = path;
	m_id = id;
	QDir().mkpath(dir(id));
	loadKnown(id);
}

void Grabber::enqueue(double t)
{
	QMetaObject::invokeMethod(this, [this, t] {
		enqueueImpl(m_path, m_id, t);
	}, Qt::QueuedConnection);
}

void Grabber::enqueue(QString const &path, QString const &id, double t)
{
	QMetaObject::invokeMethod(this, [this, path, id, t] {
		enqueueImpl(path, id, t);
	}, Qt::QueuedConnection);
}

void Grabber::enqueueImpl(QString const &path, QString const &id,
                          double t)
{
	if (path.isEmpty() || id.isEmpty() || t < 0.0)
		return;
	QDir().mkpath(dir(id));
	loadKnown(id);
	qint64 const ms = qint64(t * 1000.0 + 0.5);
	{
		QMutexLocker const lock(&m_lock);
		QSet<qint64> &known = m_known[id];
		if (known.contains(ms))
			return;
		known.insert(ms);
	}
	Job j;
	j.path = path;
	j.id = id;
	j.hit = ms;
	m_jobs << j;
	if (m_jobs.size() == 1)
		startJob();
}

bool Grabber::picksFor(QString const &id, qint64 hitMs,
                       qint64 &prev, qint64 &next)
{
	loadKnown(id);
	QMutexLocker const lock(&m_lock);
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
	m_dec.close();
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
	qint64 prev = -1, next = -1;
	if (!reuseSegment(j, ref, prev, next)) {
		prev = boundary(j.hit, ref, -1);
		next = boundary(j.hit, ref, +1);
	}
	if (!ensurePick(j.id, j.hit)
	    || (prev >= 0 && !ensurePick(j.id, prev))
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
	m_strikes = 0;
	m_jobs.removeFirst();
	if (m_listener && m_ctx && !m_jobs.isEmpty())
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
	if (++m_strikes >= 3)                // decoding is not working
		m_jobs.clear();
	else if (!m_jobs.isEmpty())
		startJob();
	drained();
}

void Grabber::drained()
{
	if (!m_listener || !m_ctx || !m_jobs.isEmpty())
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
		qint64 const hit = col[0].toLongLong();
		set.insert(hit);
		picks.insert(hit, {col[1].toLongLong(),
		                   col[2].toLongLong()});
	}
}

QString Grabber::dir(QString const &id) const
{
	return cacheRoot() + QLatin1Char('/') + id;
}
