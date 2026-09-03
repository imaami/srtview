// ident.cpp -- see ident.hpp.
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <utility>

#include "ident.hpp"

namespace {

// Streaming chunk: big enough to run the disk at speed, small
// enough that a stop request lands within milliseconds.
constexpr qint64 kChunk = qint64{4} << 20;

constexpr char kMemoMagic[] = "srtview-ids-v1";

// A file that keeps changing under the hash gets this many passes
// before it is answered as having no identity yet.
constexpr unsigned kRestamps = 3;

// The memo's view of a file: one stat, taken unlocked.
struct stamp {
	std::int64_t size = 0;
	std::int64_t mtime = 0;
	bool         exists = false;
};

stamp stampOf(QString const &path)
{
	QFileInfo const fi(path);
	return {fi.size(), fi.lastModified().toMSecsSinceEpoch(),
	        fi.exists()};
}

} // namespace

Ident::Ident(QString memoPath, void (*poke)(void *) noexcept,
             void *ctx)
	: m_memoPath(std::move(memoPath)), m_poke(poke), m_ctx(ctx)
{
	loadMemo();
}

Ident::~Ident()
{
	stop();
}

void Ident::stop()
{
	{
		std::lock_guard const lk(m_mtx);
		if (m_stopping)
			return;
		m_stopping = true;
	}
	for (std::jthread &t : m_pool)
		t.request_stop();
	m_cv.notify_all();
	m_pool.clear();          // joins
}

void Ident::post(QString const &path)
{
	{
		std::lock_guard const lk(m_mtx);
		if (m_stopping || path.isEmpty())
			return;
		// One pending slot per path, queued or hashing.  Nothing
		// answered is remembered here: every new post walks the
		// worker's stat-backed memo again, so an unchanged file
		// costs a stat and an edited one re-hashes -- the caches
		// keyed by content stay coherent with the disk.
		if (m_pending.contains(path))
			return;
		m_pending.insert(path);
		// A stale undrained answer must not satisfy the owner's
		// want-set before the re-validated one lands.
		m_fresh.remove(path);
		m_queue.push_back(path);
		// The pool grows to demand and never shrinks: a parked
		// worker is a stack, a re-spawned one is a design smell.
		// Capped at the logical cores.  Demand counts hashing
		// jobs too -- a dequeued path left the queue but still
		// occupies its worker, and a post arriving mid-hash must
		// wake a second worker, not wait behind the first.
		static unsigned const cap = std::max(1u,
			std::thread::hardware_concurrency());
		if (m_pool.size() < cap
		    && m_queue.size() + m_live > m_pool.size())
			m_pool.emplace_back(
				[this](std::stop_token st) {
					work(st);
				});
	}
	m_cv.notify_one();
}

QHash<QString, QString> Ident::drain()
{
	std::lock_guard const lk(m_mtx);
	return std::exchange(m_fresh, {});
}

bool Ident::busy()
{
	std::lock_guard const lk(m_mtx);
	return !m_queue.empty() || m_live != 0;
}

void Ident::work(std::stop_token st)
{
	std::unique_lock lk(m_mtx);
	for (;;) {
		bool const go = m_cv.wait(lk, st, [this] {
			return !m_queue.empty();
		});
		if (!go || st.stop_requested() || m_stopping)
			return;
		QString const path = m_queue.front();
		m_queue.pop_front();
		++m_live;

		// The memo row is copied under the lock and everything
		// that touches the disk runs without it -- the queue owns
		// the path until the answer lands, and a stalled mount
		// must hold this worker alone, never post(), drain() or
		// the rest of the pool.  The memo peek costs a stat, not a
		// read; a hit answers without touching the bytes.
		auto const known = m_memo.constFind(path);
		bool const remembered = known != m_memo.constEnd();
		memo_row const row = remembered ? *known : memo_row{};
		lk.unlock();
		stamp at = stampOf(path);
		QString id;
		bool hashed = false;
		if (remembered && at.exists && row.size == at.size
		    && row.mtime == at.mtime) {
			id = row.hash;
		} else {
			// The stamp is taken again after the hash: a file
			// edited meanwhile would otherwise answer with the
			// hash of mixed bytes under the pre-edit stamp, and
			// the memo would serve it for as long as that stamp
			// held.  A moved stamp hashes again; a file still
			// moving after kRestamps passes has no identity yet.
			for (unsigned pass = 1;; ++pass) {
				id = hashFile(path, st);
				stamp const after = stampOf(path);
				if (id.isEmpty() || (after.size == at.size
				                     && after.mtime == at.mtime))
					break;
				at = after;
				if (pass == kRestamps) {
					id.clear();
					break;
				}
			}
			hashed = true;
		}
		lk.lock();
		if (st.stop_requested() || m_stopping) {
			// An abandoned hash answers nothing: the next
			// session asks again.
			--m_live;
			return;
		}
		m_pending.remove(path);
		m_fresh.insert(path, id);
		if (hashed && !id.isEmpty()) {
			m_memo.insert(path, {at.size, at.mtime, id});
			m_memoDirty = true;
		}
		--m_live;
		bool const settled = m_queue.empty() && m_live == 0;
		if (settled && m_memoDirty) {
			// The write goes out over a snapshot, unlocked: the
			// hash is implicitly shared, so the copy is a
			// pointer, and a save that lands later carries the
			// rows added meanwhile.
			QHash<QString, memo_row> const snap = m_memo;
			m_memoDirty = false;
			lk.unlock();
			saveMemo(snap);
			lk.lock();
		}
		if (m_poke) {
			lk.unlock();
			m_poke(m_ctx);
			lk.lock();
		}
	}
}

QString Ident::hashFile(QString const &path, std::stop_token st)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
		return {};
	QCryptographicHash h(QCryptographicHash::Blake2b_256);
	h.addData(QByteArrayView("content-v1\n"));
	QByteArray buf;
	buf.resize(kChunk);
	while (!f.atEnd()) {
		if (st.stop_requested())
			return {};
		qint64 const n = f.read(buf.data(), kChunk);
		if (n < 0)
			return {};
		h.addData(QByteArrayView(buf.constData(), qsizetype(n)));
	}
	return QString::fromLatin1(h.result().left(8).toHex());
}

// Unlocked, over the caller's snapshot of the map; the write itself
// is small (a line per known file) and atomic via QSaveFile.
void Ident::saveMemo(QHash<QString, memo_row> const &memo)
{
	QSaveFile f(m_memoPath);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
		return;
	QByteArray out(kMemoMagic);
	out += '\n';
	for (auto it = memo.constBegin(); it != memo.constEnd();
	     ++it) {
		out += QByteArray::number(qlonglong(it->size));
		out += '\t';
		out += QByteArray::number(qlonglong(it->mtime));
		out += '\t';
		out += it->hash.toUtf8();
		out += '\t';
		out += it.key().toUtf8();
		out += '\n';
	}
	f.write(out);
	f.commit();
}

void Ident::loadMemo()
{
	QFile f(m_memoPath);
	if (!f.open(QIODevice::ReadOnly))
		return;
	QList<QByteArray> const lines = f.readAll().split('\n');
	if (lines.isEmpty() || lines.front() != kMemoMagic)
		return;
	for (qsizetype i = 1; i < lines.size(); ++i) {
		QList<QByteArray> const c = lines[i].split('\t');
		if (c.size() != 4)
			continue;
		bool okS = false, okT = false;
		memo_row row;
		row.size = c[0].toLongLong(&okS);
		row.mtime = c[1].toLongLong(&okT);
		row.hash = QString::fromUtf8(c[2]);
		if (okS && okT && row.hash.size() == 16)
			m_memo.insert(QString::fromUtf8(c[3]),
			              std::move(row));
	}
}
