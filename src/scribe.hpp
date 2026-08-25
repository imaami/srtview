// scribe.hpp -- the OCR mailbox: a worker thread taking (video,
// frame, hints) requests from any thread and delivering finished
// readings as drainable notes.  Standard C++23 threads only -- no
// Qt, no tesseract, no FFmpeg; the backend behind the one-call
// concept does the decoding and reading, so the mailbox runs
// against a fake in tests exactly as against the lector in the
// app.  Identical requests coalesce into one job however many
// tickets await them, a rush post hoists a queued speculative
// twin, a cancelled ticket silently unhooks.  One worker today;
// the lanes-plus-live-slot shape permits a pool without API change.
#ifndef SRTVIEW_SRC_SCRIBE_HPP_
#define SRTVIEW_SRC_SCRIBE_HPP_

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

#include "ocr.hpp"

namespace ocr {

// poke fires on the worker thread after notes land; it must be
// cheap and thread-safe, and calling drain() from inside it is
// fine -- no lock is held.  Function pointer plus context on
// purpose: the Qt shim of a later phase wraps it into a queued
// signal.  The backend and the poke context outlive the scribe.
template <class B> requires backend<B>
class scribe
{
public:
	scribe(B &b, void (*poke)(void *), void *ctx)
		: m_b(b), m_poke(poke), m_ctx(ctx),
		  m_thr([this](std::stop_token st) { work(st); }) {}

	// jthread stops and joins; the interruptible wait wakes on
	// the stop request, queued jobs are dropped, a running one
	// finishes into the void.
	~scribe() = default;

	// Wind down ahead of destruction: the running job finishes
	// and delivers, queued jobs are dropped, later posts are
	// refused with ticket 0.  The destructor implies it.
	void stop() { m_thr.request_stop(); }

	scribe(scribe const &) = delete;
	scribe &operator=(scribe const &) = delete;

	// Any thread.  An == request already queued or running gains
	// the new ticket instead of a second job; a rush post hoists
	// a queued speculative twin into the rush lane.  Once the
	// wind-down is requested the post is refused with ticket 0 --
	// the jthread's stop state is the single source of truth.
	ticket post(request r, bool rush = false)
	{
		ticket t;
		{
			std::lock_guard const lk(m_mtx);
			if (m_thr.get_stop_token().stop_requested())
				return 0;
			t = ++m_last;
			if (!adopt(r, rush, t))
				(rush ? m_rush : m_rest)
					.push_back({std::move(r), {t}});
		}
		m_cv.notify_one();
		return t;
	}

	// Unhooks the ticket wherever it is: a queued job left
	// ownerless is dropped, a running one delivers to nobody,
	// and a finished note not yet drained is scrubbed -- after
	// cancel(t), no note for t ever surfaces.
	bool cancel(ticket t)
	{
		std::lock_guard const lk(m_mtx);
		if (m_live && std::erase(m_live->owners, t))
			return true;
		if (pluck(m_rush, t) || pluck(m_rest, t))
			return true;
		return std::erase_if(m_done, [t](note const &n) {
			return n.t == t;
		}) != 0;
	}

	// The finished notes so far, in completion order.
	std::vector<note> drain()
	{
		std::vector<note> out;
		std::lock_guard const lk(m_mtx);
		out.swap(m_done);
		return out;
	}

private:
	struct job {
		request             r;
		std::vector<ticket> owners;
	};

	// Under the lock: attach t to an == twin anywhere it sits.
	bool adopt(request const &r, bool rush, ticket t)
	{
		if (m_live && m_live->r == r) {
			m_live->owners.push_back(t);
			return true;
		}
		for (job &j : m_rush) {
			if (j.r == r) {
				j.owners.push_back(t);
				return true;
			}
		}
		for (auto it = m_rest.begin(); it != m_rest.end(); ++it) {
			if (it->r != r)
				continue;
			it->owners.push_back(t);
			if (rush) {
				m_rush.push_back(std::move(*it));
				m_rest.erase(it);
			}
			return true;
		}
		return false;
	}

	static bool pluck(std::deque<job> &lane, ticket t)
	{
		for (auto it = lane.begin(); it != lane.end(); ++it) {
			if (!std::erase(it->owners, t))
				continue;
			if (it->owners.empty())
				lane.erase(it);
			return true;
		}
		return false;
	}

	void work(std::stop_token st)
	{
		std::unique_lock lk(m_mtx);
		for (;;) {
			bool const go = m_cv.wait(lk, st, [this] {
				return !m_rush.empty() || !m_rest.empty();
			});
			if (!go || st.stop_requested())
				return;
			auto &lane = m_rush.empty() ? m_rest : m_rush;
			m_live.emplace(std::move(lane.front()));
			lane.pop_front();
			request const r = m_live->r;
			lk.unlock();
			result const res = m_b.perform(r);
			lk.lock();
			bool told = false;
			for (ticket const t : m_live->owners) {
				m_done.push_back({r, res, t});
				told = true;
			}
			m_live.reset();
			if (told && m_poke) {
				lk.unlock();
				m_poke(m_ctx);
				lk.lock();
			}
		}
	}

	B                          &m_b;
	void                      (*m_poke)(void *);
	void                       *m_ctx;
	std::mutex                  m_mtx;
	std::condition_variable_any m_cv;
	std::deque<job>             m_rush;
	std::deque<job>             m_rest;
	std::vector<note>           m_done;
	std::optional<job>          m_live;
	ticket                      m_last = 0;
	std::jthread                m_thr;  // last: starts after the
	                                    // state above exists
};

} // namespace ocr

#endif // SRTVIEW_SRC_SCRIBE_HPP_
