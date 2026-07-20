// grabber.cpp -- see grabber.hpp for the design.
#include <QDir>
#include <QFile>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "grabber.hpp"

#include "discovery.hpp"

namespace {

// Content-change probing: how far to look for the neighboring
// segments, how fine to localize their boundaries, and how different
// two downscaled thumbs must be to count as different content.
constexpr qint64 kWinMs   = 30000;
constexpr qint64 kStepMs  = 1000;
constexpr int    kDiffMean = 8;

constexpr qint64 kSpawnTimeout = 15000;
constexpr qint64 kLoadTimeout  = 20000;
constexpr qint64 kSeekTimeout  = 15000;
constexpr qint64 kShotTimeout  = 10000;

QString cacheRoot()
{
	QString base = qEnvironmentVariable("XDG_CACHE_HOME");
	if (base.isEmpty())
		base = QDir::homePath() + QStringLiteral("/.cache");
	return base + QStringLiteral("/srtview/frames");
}

QImage thumb(QImage const &img)
{
	return img.scaled(64, 36, Qt::IgnoreAspectRatio,
	                  Qt::SmoothTransformation)
	          .convertToFormat(QImage::Format_Grayscale8);
}

// Same content?  Mean absolute difference of the tiny grayscale
// thumbs; downscaling has already averaged compression noise away.
bool same(QImage const &a, QImage const &b)
{
	qint64 sum = 0;
	for (int y = 0; y < a.height(); ++y) {
		uchar const *pa = a.constScanLine(y);
		uchar const *pb = b.constScanLine(y);
		for (int x = 0; x < a.width(); ++x)
			sum += std::abs(int(pa[x]) - int(pb[x]));
	}
	return sum < qint64(kDiffMean) * a.width() * a.height();
}

} // namespace

Grabber::Grabber()
{
	// The worker owns all activity: member QObjects are children so
	// moveToThread() takes them along (the base parents its own).
	m_poll.setParent(this);
	connect(&sock(), &QLocalSocket::readyRead,
	        this, [this] { dispatch(); });
	connect(&sock(), &QLocalSocket::connected,
	        this, [this] { onConnected(); });
	connect(&m_poll, &QTimer::timeout, this, [this] { tick(); });
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
	m_wantMs = -1;
	m_stage = Stage::idle;
	m_poll.stop();
	teardown(true);
	m_loadedId.clear();
}

void Grabber::startJob()
{
	if (m_jobs.isEmpty()) {
		m_poll.stop();
		return;
	}
	if (!m_poll.isActive())
		m_poll.start(100);
	want(m_jobs.first().hit);
}

void Grabber::want(qint64 ms)
{
	m_wantMs = ms;
	QTimer::singleShot(0, this, [this] { pump(); });
}

// One step of the pipeline toward the wanted frame; every completion
// signal (cache hit, connect, restart event, file poll) funnels back
// here.
void Grabber::pump()
{
	if (m_stage != Stage::idle || m_wantMs < 0 || m_jobs.isEmpty())
		return;
	QImage full;
	if (full.load(frameFile(m_wantMs))) {
		deliver(full);
		return;
	}
	if (!connectedNow()) {
		ensureProc();
		return;
	}
	Job const &j = m_jobs.first();
	if (m_loadedId != j.id) {
		m_loadedId = j.id;
		writeLine(QStringLiteral("loadfile %1 replace")
		          .arg(mpvQuote(j.path)));
		writeLine(QStringLiteral("set pause yes"));
		m_stage = Stage::load;
		armDeadline(kLoadTimeout);
		return;
	}
	writeLine(QStringLiteral("seek %1 absolute+exact")
	          .arg(double(m_wantMs) / 1000.0, 0, 'f', 3));
	m_stage = Stage::seekWait;
	armDeadline(kSeekTimeout);
}

void Grabber::tick()
{
	if (m_stage == Stage::spawn && !connectedNow())
		connectSock(0);
	if (m_stage == Stage::shoot) {
		QImage full;
		if (full.load(frameFile(m_wantMs))) {
			m_stage = Stage::idle;
			deliver(full);
			return;
		}
	}
	if (m_stage != Stage::idle
	    && m_deadline.elapsed() > m_deadlineMs)
		abortJob();
}

void Grabber::onEvent(QJsonObject const &ev)
{
	if (ev.value(QStringLiteral("event")).toString()
	    == QStringLiteral("playback-restart"))
		onRestart();
}

void Grabber::onConnected()
{
	if (m_stage != Stage::spawn)
		return;
	m_stage = Stage::idle;
	pump();
}

void Grabber::onRestart()
{
	if (m_stage == Stage::load) {
		m_stage = Stage::idle;
		pump();                      // loaded: proceed to the seek
		return;
	}
	if (m_stage != Stage::seekWait)
		return;
	m_stage = Stage::shoot;
	armDeadline(kShotTimeout);
	writeLine(QStringLiteral("screenshot-to-file %1 video")
	          .arg(mpvQuote(frameFile(m_wantMs))));
}

void Grabber::deliver(QImage const &full)
{
	qint64 const ms = m_wantMs;
	m_wantMs = -1;
	advance(thumb(full), ms);
}

// The bisection plan, one delivered frame at a time.  Invariants:
// backward, lo differs from the hit segment and hi belongs to it;
// forward the mirror image.
void Grabber::advance(QImage const &tn, qint64 ms)
{
	Job &j = m_jobs.first();
	switch (j.phase) {
	case Job::anchor:
		j.ref = tn;
		j.lo = std::max<qint64>(0, j.hit - kWinMs);
		j.phase = Job::back0;
		want(j.lo);
		return;
	case Job::back0:
		if (same(tn, j.ref)) {       // one long segment all the way
			j.prevMs = ms;
			bisectFwd(j);        // degenerate: starts the probe
			return;
		}
		j.hi = j.hit;
		j.phase = Job::backBisect;
		bisectBack(j);
		return;
	case Job::backBisect:
		(same(tn, j.ref) ? j.hi : j.lo) = ms;
		bisectBack(j);
		return;
	case Job::fwd0:
		if (same(tn, j.ref)) {
			j.nextMs = ms;
			finish(j);
			return;
		}
		j.hi = ms;
		j.phase = Job::fwdBisect;
		bisectFwd(j);
		return;
	case Job::fwdBisect:
		(same(tn, j.ref) ? j.lo : j.hi) = ms;
		bisectFwd(j);
		return;
	}
}

void Grabber::bisectBack(Job &j)
{
	if (j.hi - j.lo <= kStepMs) {
		j.prevMs = j.lo;             // last known different behind
		bisectFwd(j);                // starts the forward probe
		return;
	}
	want((j.lo + j.hi) / 2);
}

void Grabber::bisectFwd(Job &j)
{
	if (j.phase != Job::fwdBisect) {     // entered from the back side
		j.lo = j.hit;
		j.phase = Job::fwd0;
		want(j.hit + kWinMs);
		return;
	}
	if (j.hi - j.lo <= kStepMs) {
		j.nextMs = j.hi;             // first known different ahead
		finish(j);
		return;
	}
	want((j.lo + j.hi) / 2);
}

void Grabber::finish(Job &j)
{
	QFile f(dir(j.id) + QStringLiteral("/picks.txt"));
	if (f.open(QIODevice::Append | QIODevice::Text))
		f.write(QByteArray::number(j.hit) + ' '
		        + QByteArray::number(j.prevMs) + ' '
		        + QByteArray::number(j.nextMs) + '\n');
	{
		QMutexLocker const lock(&m_lock);
		m_picks[j.id].insert(j.hit, {j.prevMs, j.nextMs});
	}
	m_strikes = 0;
	m_jobs.removeFirst();
	if (m_listener && m_ctx && !m_jobs.isEmpty())
		QMetaObject::invokeMethod(m_ctx, [l = m_listener] {
			l->grabProgress();
		}, Qt::QueuedConnection);
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
	m_wantMs = -1;
	m_stage = Stage::idle;
	teardown(false);
	if (proc().state() != QProcess::NotRunning)
		proc().kill();               // wedged: fresh spawn next time
	m_loadedId.clear();
	if (++m_strikes >= 3) {              // mpv is not cooperating
		m_jobs.clear();
		m_poll.stop();
	} else {
		startJob();
	}
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

void Grabber::ensureProc()
{
	if (m_stage != Stage::idle)
		return;
	m_stage = Stage::spawn;
	armDeadline(kSpawnTimeout);
	if (proc().state() == QProcess::Running) {
		connectSock(0);
		return;
	}
	m_sockPath = grabSock();
	QFile::remove(m_sockPath);
	m_loadedId.clear();
	startProcess({QStringLiteral("--no-terminal"),
	              QStringLiteral("--idle=yes"),
	              QStringLiteral("--pause"),
	              QStringLiteral("--keep-open=yes"),
	              QStringLiteral("--vo=null"),
	              QStringLiteral("--no-audio"),
	              QStringLiteral("--sid=no"),
	              QStringLiteral("--input-ipc-server=")
	              + m_sockPath});
}

void Grabber::armDeadline(qint64 ms)
{
	m_deadline.restart();
	m_deadlineMs = ms;
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

QString Grabber::frameFile(qint64 ms) const
{
	return framePath(m_jobs.first().id, ms);
}
