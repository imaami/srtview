// facts.hpp -- the background LLM pipeline over the facts cache.
//
// Executor half of the agenda: owns the llm client, the cache
// directories and an agenda::plan behind one mutex, runs one task
// at a time, and turns each completion into a cache file plus the
// next pick.  Kinds map to prompts and inputs as follows: a *leaf*
// summarizes transcript text snapshot on the offering (UI) thread;
// a *node* merges the cache files of its children, which dependency
// gating guarantees exist; a *dive* explains one topic's matched
// excerpts (snapshot like a leaf) against the summaries of the
// videos they came from (deps, guaranteed) and the corpus overview
// (refs, attached only if already in the cache); a *focus* reads a
// pair of finished dives (deps) and either refuses (NONE) or
// sharpens their common thread, closing with a REGEX line the UI
// layer harvests back into the corpus.  Cache layout, sibling to
// the frame cache:
//   $XDG_CACHE_HOME/srtview/facts/<id>.txt        leaves and nodes
//   $XDG_CACHE_HOME/srtview/facts/dives/<id>.txt  dives
//   $XDG_CACHE_HOME/srtview/facts/focus/<id>.txt  focuses
// File existence is the manifest: done work is marked done instead
// of queued, a failed or cancelled task writes nothing and retries
// next session by its absence, and an empty reply writes nothing so
// the cache can never mask a failure.
//
// Thread rules.  R1: Qt objects never enter here; callers snapshot
// text on their own thread.  R2: cache files are written to .tmp
// and renamed, never modified after, so any thread may read a done
// task's file.  R3: one mutex guards plan, snapshots and the
// in-flight id; lock order is this mutex before the llm client's
// internal one, never the reverse (llm callbacks run unlocked).
// R4: teardown raises m_down under the mutex before destroying the
// client, and completions never advance past it.
//
// SRTVIEW_LLM=[host][:port] points the pipeline at another server;
// the default is llama-server on 127.0.0.1:8080.  Three connect
// refusals in a row latch the pipeline offline for the session --
// nothing is written, so the next session simply retries.  Tasks
// run with a quiet gap in between (SRTVIEW_LLM_PACE=<seconds>,
// default 30, 0 disables): sustained generation is a full-power
// burn, and the accelerator needs breathing room by default.
//
// Standard C++23 over the plain C llm client, no Qt.
#ifndef SRTVIEW_SRC_FACTS_HPP_
#define SRTVIEW_SRC_FACTS_HPP_

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "agenda.hpp"

struct llm;

class Facts
{
public:
	Facts();
	~Facts();

	Facts(Facts const &) = delete;
	Facts &operator=(Facts const &) = delete;

	// Resolves what the cache already answers: an id whose file
	// exists is marked done -- dependents of a cached leaf must
	// unblock even though no offer will follow -- and false says
	// no transcript text is needed.  True means the caller should
	// follow up with offer() and the materialized text.
	bool settle(agenda::id key);

	// Leaf summary of one subtitle file: the id keys both the
	// cache and the heat map; the text is snapshot here.  Ids the
	// plan already knows and ids whose file exists are skipped.
	void offer(agenda::id key, std::string const &utf8Text);

	// Body-less tasks whose inputs are cache files: pyramid nodes
	// and dive-pair focuses.  Tasks whose files exist are marked
	// done instead of queued.
	void corpus(std::vector<agenda::task> nodes);

	// The cache root; never changes after construction.
	std::string const &dir() const { return m_dir; }

	// One topic dive: the caller-built task (id, deps = hit-video
	// leaves, keys, refs = optional overview) plus the matched
	// excerpts, snapshot here.  Cached dives are marked done.
	void dive(agenda::task t, std::string const &utf8Excerpts);

	void heat(agenda::id key, double add);
	void decay(double keep);

	// New corpus: drops pending work, snapshots and heat.  A task
	// in flight keeps running; its completion still lands in the
	// cache and is remembered.
	void reset();

private:
	static void deliver(void *ud, std::uint64_t task, int status,
	                    char const *text, std::size_t size);
	void completed(agenda::id which, int status, bool wrote);
	void advance();
	bool submit(agenda::task const &t);
	std::string assemble(agenda::task const &t);
	std::string assemble_node(agenda::task const &t) const;
	std::string assemble_dive(agenda::task const &t);
	std::string assemble_focus(agenda::task const &t) const;
	std::string spend_body(agenda::id which);
	std::string path_of(agenda::task const &t) const;

	mutable std::mutex m_mtx;
	agenda::plan       m_plan;      // guarded by m_mtx
	std::vector<std::pair<agenda::id, std::string>>
	                   m_bodies;    // snapshots, spent on submit
	std::string        m_dir;       // .../srtview/facts
	agenda::id         m_inflight;  // id at the llm, or none
	llm               *m_llm = nullptr;
	int                m_refused = 0;   // consecutive connect fails
	bool               m_offline = false;
	bool               m_down = false;
};

#endif // SRTVIEW_SRC_FACTS_HPP_
