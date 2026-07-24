// agenda.hpp -- the LLM pipeline's task graph and scheduler.
// Standard C++, no Qt, no IO, no threads: a pure data structure the
// executor (Facts) drives under its own lock, and tests drive bare.
//
// A task is a unit of background LLM work with a deterministic
// identity: the cache file it will produce.  Kinds: a *leaf*
// summarizes one subtitle file, a *node* summarizes the summaries
// of its children (the abstraction pyramid: pairs joined level by
// level, generalizing toward the root), a *dive* explains one
// topic's hit collection in depth.  Dependencies gate readiness --
// a task is ready when every dependency is done, where "done" means
// its cache file exists (the caller marks cache hits done without
// adding a task; unknown-but-done ids are remembered).
//
// take() hands out the highest-scoring ready task and marks it
// running; done()/fail() retire it.  A failed task parks for the
// session -- absence from the cache retries it next session -- and
// whatever depends on it stays blocked.
//
// Scoring: the kind sets the neighborhood (leaves before nodes
// before dives, all else equal), the tier orders within a kind,
// exported topics float over supportive ones, and heat -- an
// accumulator per subtitle id, fed by user actions -- moves
// everything.  Priority inherits through the graph: a hot blocked
// task lifts its pending dependencies until the chain unblocks, so
// searching a video pulls that video's whole pipeline forward.
#ifndef SRTVIEW_SRC_AGENDA_HPP_
#define SRTVIEW_SRC_AGENDA_HPP_

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace agenda {

enum class kind : std::uint8_t { leaf, node, dive };

struct task {
	std::string              id{};     // cache identity (hex)
	std::vector<std::string> deps{};   // ids that must be done first
	std::vector<std::string> keys{};   // heat keys: subtitle ids
	kind                     what     = kind::leaf;
	std::uint8_t             tier     = 0;
	bool                     exported = true;

	bool operator==(task const &) const = default;
};

class plan {
public:
	enum class state : std::uint8_t {
		unknown, pending, running, done, parked
	};

	// Registers a task; an already-known id (any state) is left
	// untouched, so re-planning a corpus is idempotent.
	void add(task t);

	// Marks an id done; unknown ids are remembered as done, which
	// is how cache hits satisfy dependencies without a task.
	void done(std::string const &id);

	// Parks an id for the session; its dependents stay blocked.
	void fail(std::string const &id);

	void heat(std::string const &key, double add);
	void decay(double keep);

	// Highest-scoring ready task, marked running; empty when no
	// pending task has all dependencies done.
	std::string take();

	// Drops every task and all heat; completions of tasks taken
	// before the reset may still be reported and are remembered.
	void reset();

	task const *get(std::string const &id) const;
	state status(std::string const &id) const;
	std::size_t backlog() const;   // pending + running

private:
	struct entry {
		task  t;
		state s = state::pending;
	};

	static constexpr std::size_t npos = std::size_t(-1);

	std::size_t index_of(std::string const &id) const;
	bool ready(entry const &e) const;
	double score(task const &t) const;
	bool lift(std::vector<double> &eff) const;
	bool raise_deps(std::size_t at, std::vector<double> &eff) const;

	// Session scale is dozens to a few hundred tasks: linear scans,
	// no hashed containers.
	std::vector<entry>                          m_entries;
	std::vector<std::pair<std::string, double>> m_heat;
};

using combine_fn = std::string (*)(std::vector<std::string> const &);

// The abstraction pyramid over ordered leaves as node tasks (the
// leaves themselves are offered separately): pairs joined per level,
// an odd tail carried upward unchanged, deps the two children, keys
// the union of covered leaves, tier the level.  combine() names a
// parent from its ordered children; same leaves, same names -- two
// corpora sharing a prefix share the prefix's summaries.
std::vector<task> pyramid(std::vector<std::string> const &leaves,
                          combine_fn                      combine);

} // namespace agenda

#endif // SRTVIEW_SRC_AGENDA_HPP_
