// ident.hpp -- content identity: a file's id is BLAKE2b-256 over
// its bytes (left 8 bytes hex, "content-v1"), and nothing else --
// never a path, never a name -- so a cache travels to another
// machine, another directory, another filename and still matches.
// The scribe's mailbox idiom over a persistent std::jthread pool:
// no file byte is ever hashed on the owning (UI) thread.  Workers
// grow on demand up to the logical core count and then live
// parked on the condvar for the app's life; a pending set dedupes
// the path queue (queued and hashing posts absorb), finished ids
// drain behind a poke, and a local (size, mtime, hash) memo
// answers unchanged files without a read -- the memo is pure
// acceleration, never identity, so a renamed file simply re-hashes
// once.  Nothing is remembered past the answer: every new post
// re-validates through the memo's stat, so a file edited on disk
// re-hashes and the content-addressed caches stay coherent.  The
// mutex guards the queues and the memo map only: the stat, the
// hash and the memo write all run without it, so a stalled mount
// holds one worker and never the owner, and a file that changes
// while it hashes is stamped again and re-hashed -- three tries,
// then it has no identity yet.  The one path-derived id in the
// program remains discovery's socket scheme: per-machine runtime
// rendezvous, shared with srtjump, and never data identity.
#ifndef SRTVIEW_SRC_IDENT_HPP_
#define SRTVIEW_SRC_IDENT_HPP_

#include <QHash>
#include <QSet>
#include <QString>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

class Ident
{
public:
	// poke fires on a worker thread after results land; it must
	// be cheap, thread-safe and noexcept (the owner wraps it into
	// a queued call).  memoPath names the acceleration file; an
	// unreadable or missing memo just means hashing.
	Ident(QString memoPath, void (*poke)(void *) noexcept,
	      void *ctx);

	// Stops and joins the pool; a hash in flight is abandoned at
	// its next chunk.
	~Ident();

	Ident(Ident const &) = delete;
	Ident &operator=(Ident const &) = delete;

	// Queue a file for identification.  Duplicates of a queued or
	// running path are absorbed; an already-answered path queues
	// again and re-validates against the file, unchanged ones by a
	// stat alone.  A stale undrained answer dies with the re-post.
	void post(QString const &path);

	// Results so far, path -> id; an unreadable file answers with
	// an empty id (the consumers' unresolvable-identity path).
	QHash<QString, QString> drain();

	// True while any queued or running job remains.
	bool busy();

	void stop();

private:
	struct memo_row {
		std::int64_t size = 0;
		std::int64_t mtime = 0;
		QString      hash;
	};

	void work(std::stop_token st);
	QString hashFile(QString const &path, std::stop_token st);
	void loadMemo();
	void saveMemo(QHash<QString, memo_row> const &memo);

	QString                     m_memoPath;
	void                      (*m_poke)(void *) noexcept;
	void                       *m_ctx;
	std::mutex                  m_mtx;
	std::condition_variable_any m_cv;
	std::deque<QString>         m_queue;
	unsigned                    m_live = 0;   // jobs being hashed
	QSet<QString>               m_pending;    // queued or hashing
	QHash<QString, QString>     m_fresh;      // not yet drained
	QHash<QString, memo_row>    m_memo;       // acceleration only
	bool                        m_memoDirty = false;
	bool                        m_stopping = false;
	std::vector<std::jthread>   m_pool;       // grows to the core
	                                          // count, never shrinks
};

#endif // SRTVIEW_SRC_IDENT_HPP_
