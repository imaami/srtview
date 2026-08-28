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

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ocr.hpp"

namespace ocr {

// One video's share of the corpus reading plan: the prototype
// request, stamped with each listed moment in turn.
struct feed {
	request                   proto;  // its ms is ignored
	std::vector<std::int64_t> times;
};

// poke fires on the worker thread after notes land; it must be
// cheap and thread-safe, and calling drain() from inside it is
// fine -- no lock is held.  Function pointer plus context on
// purpose: the Qt shim of a later phase wraps it into a queued
// signal.  The pointer type says noexcept, so a throwing callback
// cannot even be wired: this is a C-shaped boundary, and errors
// travel through the notes' err field, never by unwinding into
// the worker.  The backend and the poke context outlive the
// scribe.
template <class B> requires backend<B>
class scribe
{
public:
	scribe(B &b, void (*poke)(void *) noexcept, void *ctx)
		: m_b(b), m_poke(poke), m_ctx(ctx),
		  m_thr([this](std::stop_token st) { work(st); }) {}

	// jthread stops and joins; the interruptible wait wakes on
	// the stop request, queued jobs are dropped, a running one
	// finishes into the void.
	~scribe() { stop(); }

	// Wind down ahead of destruction: the running job finishes
	// and delivers, queued jobs are dropped, later posts are
	// refused with ticket 0.  The destructor implies it.  The
	// stop flag flips under the same mutex post() admits under:
	// a bare request_stop() would let a racing post win its
	// check, return a live ticket, and watch the worker exit
	// without ever serving it.
	void stop()
	{
		{
			std::lock_guard const lk(m_mtx);
			if (m_stopping)
				return;
			m_stopping = true;
		}
		m_thr.request_stop();
	}

	scribe(scribe const &) = delete;
	scribe &operator=(scribe const &) = delete;

	// Any thread.  An == request already queued or running gains
	// the new ticket instead of a second job; a rush post hoists
	// a queued speculative twin into the rush lane.  Once the
	// wind-down is requested the post is refused with ticket 0 --
	// admission and the stop flag share one mutex, so a ticket is
	// either accepted before the stop or refused, never accepted
	// into a worker already told to leave.
	ticket post(request r, bool rush = false)
	{
		ticket t;
		{
			std::lock_guard const lk(m_mtx);
			if (m_stopping)
				return 0;
			t = ++m_last;
			if (!adopt(r, rush, t))
				(rush ? m_rush : m_rest)
					.push_back({std::move(r), {t}});
		}
		m_cv.notify_one();
		return t;
	}

	// The corpus reads itself: whenever both lanes run dry the
	// worker performs the plan's next moment -- posted work
	// always goes first -- and the notes arrive under ticket 0,
	// the planned reading's mark.  A new plan replaces the old
	// (a corpus swap); prefer() moves one video's remainder to
	// the front.
	void plan(std::vector<feed> feeds)
	{
		{
			std::lock_guard const lk(m_mtx);
			m_plan.clear();
			for (feed &f : feeds)
				if (!f.proto.video.empty()
				    && !f.times.empty())
					m_plan.push_back(
						{std::move(f), 0});
		}
		m_cv.notify_one();
	}

	void prefer(std::string const &id)
	{
		std::lock_guard const lk(m_mtx);
		for (std::size_t i = 1; i < m_plan.size(); ++i) {
			if (m_plan[i].f.proto.id != id)
				continue;
			lot moved = std::move(m_plan[i]);
			m_plan.erase(m_plan.begin()
			             + std::ptrdiff_t(i));
			m_plan.push_front(std::move(moved));
			return;
		}
	}

	// Unhooks the ticket wherever it is: a queued job left
	// ownerless is dropped, a running one delivers to nobody,
	// and a finished note not yet drained is scrubbed -- after
	// cancel(t), no note for t ever surfaces.  Ticket 0 is the
	// planned readings' mark, never cancellable.
	bool cancel(ticket t)
	{
		if (!t)
			return false;
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

	// Any thread: true while the reading plan still has work in
	// any stage -- feeds on the bench, a planned reading in the
	// worker's hands, or ANY finished note not yet drained.  The
	// last leg matters twice over: a tiny or archive-warm plan can
	// finish entirely between the owner's feed and its next probe,
	// and a late demand read's note is unpublished news just the
	// same -- until drained, it may still change the owner's cut,
	// and a probe taken in the poke-to-drain window must not call
	// the world quiet.  A drain-then-probe caller therefore sees
	// false exactly when every note has reached it.  Queued and
	// live demand work still never counts: a read the user asked
	// for is not the corpus reading itself.
	bool planning()
	{
		std::lock_guard const lk(m_mtx);
		if (!m_plan.empty())
			return true;
		if (m_live
		    && std::ranges::find(m_live->owners, ticket(0))
		       != m_live->owners.end())
			return true;
		return !m_done.empty();
	}

private:
	struct job {
		request             r;
		std::vector<ticket> owners;
	};

	// One consumed plan entry: the feed plus its cursor.
	struct lot {
		feed        f;
		std::size_t at;
	};

	// Under the lock, both lanes empty: the plan's next moment
	// as a job owned by ticket 0, husk feeds pruned on the way.
	// A lot whose last moment is being pulled leaves with it:
	// planning() reads m_plan between the final note's poke and
	// the worker's next loop, and a lingering husk there would
	// report a drained plan as still reading -- the release that
	// callback was waiting for would never come.
	std::optional<job> pulled()
	{
		while (!m_plan.empty()) {
			lot &l = m_plan.front();
			if (l.at < l.f.times.size()) {
				request r = l.f.proto;
				r.ms = l.f.times[l.at++];
				if (l.at == l.f.times.size())
					m_plan.pop_front();
				return job{std::move(r), {0}};
			}
			m_plan.pop_front();
		}
		return std::nullopt;
	}

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
				return !m_rush.empty() || !m_rest.empty()
				    || !m_plan.empty();
			});
			// m_stopping guards the dequeue as well as the
			// door: between the flag and the token there is a
			// gap, and a queued job must not start inside it.
			if (!go || m_stopping || st.stop_requested())
				return;
			if (!m_rush.empty() || !m_rest.empty()) {
				auto &lane = m_rush.empty() ? m_rest
				                            : m_rush;
				m_live.emplace(std::move(lane.front()));
				lane.pop_front();
			} else if (std::optional<job> j = pulled()) {
				m_live.emplace(std::move(*j));
			} else
				continue;    // husks only: wait again
			request const r = m_live->r;
			lk.unlock();
			// The worker never dies of a backend's exception:
			// whatever escaped becomes an error note, and the
			// error is never cached, so a later session tries
			// again.
			result res;
			try {
				res = m_b.perform(r);
			} catch (...) {
				res = {};
				res.err = "backend exception";
			}
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
	void                      (*m_poke)(void *) noexcept;
	void                       *m_ctx;
	std::mutex                  m_mtx;
	std::condition_variable_any m_cv;
	std::deque<job>             m_rush;
	std::deque<job>             m_rest;
	std::deque<lot>             m_plan;
	std::vector<note>           m_done;
	std::optional<job>          m_live;
	bool                        m_stopping = false;
	ticket                      m_last = 0;
	std::jthread                m_thr;  // last: starts after the
	                                    // state above exists
};

} // namespace ocr

#endif // SRTVIEW_SRC_SCRIBE_HPP_
