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
// (refs, attached only if already in the cache); a *probe* reads a
// pair of finished dives (deps) and either refuses (NONE) or names
// a regex worth searching, its snapshot a raw sample of the pair's
// matched lines plus feedback on an earlier attempt; a *focus*
// writes the pair's common thread
// from the excerpts that search actually found (the snapshot),
// stored under a machine-written REGEX head the UI layer harvests
// back into the corpus.  Evidence *extract* tasks, pairwise *judge*
// tasks and cited *answer* tasks are schema-constrained snapshots
// owned by SemanticEngine.  The probe-search-write chain is the UI
// layer's to run: a probe's reply names a search, not a file the
// plan could gate on.  Cache layout, sibling to the frame cache:
//   $XDG_CACHE_HOME/srtview/facts/<name>.txt        leaves, nodes
//   $XDG_CACHE_HOME/srtview/facts/dives/<name>.txt  dives
//   $XDG_CACHE_HOME/srtview/facts/focus/<name>.txt  focuses
//   $XDG_CACHE_HOME/srtview/facts/probe/<name>.txt  probes
//   $XDG_CACHE_HOME/srtview/facts/terms/<name>.txt  terms
//   $XDG_CACHE_HOME/srtview/facts/semantic/{extract,judge,answer}/
// where <name> includes the vault's plan id, semantic recipe and,
// for dependency-shaped work, content suffix.  The suffix chains
// content, so an external edit to
// an .srt or a cached file renames the dependents instead of
// regenerating them -- see vault.hpp.  File existence is the
// manifest: done work is marked done instead of queued, a failed or
// cancelled task writes nothing and retries next session by its
// absence, and an empty reply writes nothing so the cache can never
// mask a failure.
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
// default 3, 0 disables): sustained generation is a full-power
// burn, and the accelerator needs breathing room by default.
// SRTVIEW_LLM_MODEL_ID supplies a stable model/quant fingerprint for
// recipe-aware caching when the model behind one endpoint may change.
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
#include "vault.hpp"

struct llm;

class Facts
{
public:
	explicit Facts(vault::hash8_fn h);
	~Facts();

	Facts(Facts const &) = delete;
	Facts &operator=(Facts const &) = delete;

	// Leaf summary of one subtitle file: the id keys the cache and
	// the heat map, the text is snapshot here, and its hash is the
	// vault's content witness.  Ids the plan already knows are
	// skipped; ids whose file resolves are marked done instead of
	// queued -- dependents of a cached leaf must unblock even
	// though no ask will follow.
	void offer(agenda::id key, std::string const &utf8Text);

	// Body-less tasks whose inputs are cache files: pyramid
	// nodes.  Tasks whose files exist are marked done instead of
	// queued.
	void corpus(std::vector<agenda::task> nodes);

	// Retire superseded asks: every listed id still waiting parks
	// for the session, so a re-cut's abandoned questions stop
	// competing with the current generation for the model's lane.
	// A task already on the wire finishes and lands harmlessly
	// (artifacts are content-addressed); done and parked ids stay
	// as they are.
	void retire(std::vector<agenda::id> const &stale);

	// The ground-truth hold: while on, the background lane runs
	// only frame-independent work -- leaves, nodes, dives, probes,
	// focus writes -- so the model stays busy with what the frames
	// cannot change while the corpus finishes reading itself, and
	// no cycles burn on extract/judge asks the re-cut would only
	// retire.  User questions answer regardless.  Release advances
	// at once; the toggle is idempotent.
	void hold(bool on);

	// Mark a witnessed fact done: an id that stands for a fact
	// about one cut rather than an artifact -- never staged, never
	// executed, tombstoned into the plan so its dependents come
	// ready and the freed lanes advance.  Marking twice, or before
	// any dependent exists, is harmless by plan::done()'s own
	// contract.
	void mark(agenda::id fact);

	// The cache root; never changes after construction.
	std::string const &dir() const { return m_dir; }
	// The recipe a kind's artifacts are named under; immutable for
	// the life of the pipeline, so a caller may key durable state
	// derived from those artifacts by it.
	agenda::id recipe(agenda::kind k) const { return m_vault.recipe(k); }

	// Locked front doors for the UI layer's out-of-band reads:
	// whether a task's artifact exists (resolving adopts stale
	// names en passant), its text, and a plan id's file whatever
	// its suffix (best-effort, for previews).
	bool cached(agenda::task const &t);
	std::string fetch(agenda::task const &t);
	std::string locate(agenda::id plan, agenda::kind k) const;

	// Artifacts that have become available since construction --
	// landed from the model or found in the cache at offer time.
	// A harvester that saw the count unchanged need not look again.
	std::uint64_t landed() const;

	// Whether nothing will land for a task: it failed this session
	// -- refused, timed out, errored or cancelled -- and was parked
	// without an artifact, or the pipeline as a whole is parked
	// behind an unreachable server.  A renewed offer un-parks a
	// task; an offered question un-parks the pipeline, once.
	bool parked(agenda::id id) const;

	// The registered task's artifact path by bare id, resolving --
	// and adopting -- under the lock; empty while the file is
	// missing or the chain is still incomputable.  The focus
	// harvest waits on exactly this.
	std::string artifact(agenda::id id);

	// A caller-built task with a snapshot: a dive's matched
	// excerpts, a probe's transcript sample (plus feedback on a
	// retry), a focus write's searched evidence.  The caller sets
	// the kind; cached tasks are marked done.
	void offer(agenda::task t, std::string const &snapshot);

	void heat(agenda::id key, double add);
	void decay(double keep);

	// New corpus: a generation boundary.  Pending work, snapshots,
	// heat and done marks all drop and the epoch advances -- a
	// completion from the old generation discards its reply rather
	// than publish into a corpus it never saw; absence retries it
	// next session.
	void reset();

private:
	static void deliver(void *ud, std::uint64_t task, int status,
	                    char const *text, std::size_t size);
	void completed(agenda::task const &t, std::string const &tmp,
	               std::string const &want, std::string const &line,
	               std::uint64_t epoch, int status, bool wrote);
	bool settle(agenda::task const &t);
	void stage(agenda::task t, std::string const &body);
	void advance();
	bool submit(agenda::task const &t, std::size_t lane);
	std::string assemble(agenda::task const &t);
	std::string assemble_node(agenda::task const &t);
	std::string assemble_dive(agenda::task const &t);
	std::string assemble_probe(agenda::task const &t);
	std::string assemble_focus(agenda::task const &t);
	std::string pair_sections(agenda::task const &t);
	std::string spend_body(agenda::id which);

	mutable std::mutex m_mtx;
	agenda::plan       m_plan;      // guarded by m_mtx
	std::vector<std::pair<agenda::id, std::string>>
	                   m_bodies;    // snapshots, spent on submit
	std::string        m_dir;       // .../srtview/facts
	vault::store       m_vault;     // guarded by m_mtx
	vault::hash8_fn    m_hash;      // H8, injected
	// Two lanes at the llm: background work, one task at a time
	// behind the pace, and urgent work -- a grounded answer -- that
	// queues ahead of a background task not yet on the wire and
	// cuts the gap.  One lane each, so an answer never waits for a
	// generation that was only queued; a generation under way is
	// never interrupted.
	struct lane {
		agenda::id    task;    // id at the llm, or none
		std::uint64_t ask = 0; // llm_ask() id, for cancel
	};
	lane               m_lane[2];   // [0] background, [1] urgent
	std::uint64_t      m_epoch = 0; // reset generation
	std::uint64_t      m_landed = 0; // artifacts made available
	llm               *m_llm = nullptr;
	int                m_refused = 0;   // consecutive connect fails
	bool               m_offline = false;
	bool               m_down = false;
	bool               m_hold = false;
};

#endif // SRTVIEW_SRC_FACTS_HPP_
