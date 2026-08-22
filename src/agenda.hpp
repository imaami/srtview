// agenda.hpp -- the LLM pipeline's task graph and scheduler.
// Standard C++, no Qt, no IO, no threads: a pure data structure the
// executor (Facts) drives under its own lock, and tests drive bare.
//
// A task is a unit of background LLM work with a deterministic
// identity: the cache file it will produce.  Kinds: a *leaf*
// summarizes one subtitle file, a *node* summarizes the summaries
// of its children (the abstraction pyramid: pairs joined level by
// level, generalizing toward the root), a *dive* explains one
// topic's hit collection in depth; later kinds extract and reconcile
// evidence-backed records and answer retrieved questions.
// Dependencies gate readiness --
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
// Inheritance ties resolve by the tasks' own scores -- the lift
// says how urgent a chain is, the own score who inside the chain
// serves it best -- so an aggregate's heat never flattens the
// ordering among its own inputs.
#ifndef SRTVIEW_SRC_AGENDA_HPP_
#define SRTVIEW_SRC_AGENDA_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agenda {

// A *leaf* summarizes one subtitle file; a *node* merges child
// summaries (the pyramid, generalizing upward); a *dive* details
// one search pattern's matched material; a *probe* reads two
// finished dives and either refuses or hypothesizes a regex worth
// searching; a *focus* writes the pair's common thread from the
// evidence that search actually found; a *terms* pass reads one
// cue window of raw transcript and proposes index entries -- term,
// observed spellings, gloss -- which the caller validates against
// the window and adopts.  The probe-search-write chain and the
// terms harvest are the executor's caller's to run.  An *extract*
// task emits atomic evidence-cited semantic records from one cue
// window; a *judge* compares one new record with one mechanically
// selected candidate; an *answer* writes a cited response from a
// bounded retrieval bundle.
enum class kind : std::uint8_t {
	leaf, node, dive, focus, probe, terms, merge, spell,
	extract, judge, answer
};

// The one number every per-kind table must match: size arrays with
// it and static_assert against it, so a new kind fails to compile
// instead of corrupting whatever sat past the end.
inline constexpr std::size_t kind_count =
	std::size_t(kind::answer) + 1;

// The kind's name, as journals, debug lines and cache recipes spell
// it: one table beside the enum instead of one per module.
inline constexpr std::string_view kind_names[] = {
	"leaf", "node", "dive", "focus", "probe", "terms", "merge",
	"spell", "extract", "judge", "answer"
};
static_assert(std::size(kind_names) == kind_count,
              "kind_names mirrors agenda::kind");

constexpr std::string_view name(kind k)
{
	return kind_names[std::size_t(k)];
}

// Task identity and heat key: the leading 8 bytes of a BLAKE2b-256,
// kept binary in memory -- hex materializes only at the filesystem
// and log boundary.  The all-zero id means "no id".
struct id {
	std::array<std::uint8_t, 8> b{};

	constexpr auto operator<=>(id const &) const = default;

	constexpr explicit operator bool() const
	{
		for (std::uint8_t const x : b)
			if (x)
				return true;
		return false;
	}

	std::string hex() const;
	static id from_hex(std::string_view s);
};

struct task {
	agenda::id              id{};      // cache identity
	std::vector<agenda::id> deps{};    // must be done first
	std::vector<agenda::id> keys{};    // heat keys: subtitle ids
	std::vector<agenda::id> refs{};    // optional context ids: read
	                                   // at assembly when their
	                                   // files exist, never awaited
	std::string             note{};    // human journal text only:
	                                   // no identity, no scheduling
	kind                    what     = kind::leaf;
	std::uint8_t            tier     = 0;
	bool                    exported = true;

	// note stays out of equality, exactly as its comment promises:
	// journal text is not identity.
	bool operator==(task const &o) const
	{
		return id == o.id && deps == o.deps && keys == o.keys
		    && refs == o.refs && what == o.what
		    && tier == o.tier && exported == o.exported;
	}
};

// Position by id for owners that only ever append.  Ids are hashes
// already, so the table probes on their low bits and needs no hash
// function of its own: linear probing, growth at half load, the
// zero id -- which no live task carries -- marking an empty slot.
// Every id-keyed vector in this program used to be searched end to
// end per lookup; this is their one index.
class index {
public:
	static constexpr std::size_t npos = std::size_t(-1);

	std::size_t find(id key) const;
	// Records key at value, replacing a previous value.
	void add(id key, std::size_t value);
	void clear();

private:
	struct slot {
		id          key;
		std::size_t value;
	};

	std::size_t home(id key) const;

	std::vector<slot> m_slots;
	std::size_t       m_used = 0;
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
	void done(id which);

	// Parks an id for the session; its dependents stay blocked.
	// Unknown ids are remembered as parked -- a failure reported
	// after a reset must not be forgotten into a same-session
	// retry (the mirror of done()'s tombstones).
	void fail(id which);

	void heat(id key, double add);
	void decay(double keep);

	// Highest-scoring ready task among those fit admits (null
	// admits all): peek() names it, start() marks a named pending
	// task running, take() does both.  A caller with more than one
	// lane peeks once per free lane with that lane's fit, so a
	// task the busy lane would take never hides one the free lane
	// could.
	using fit_fn = bool (*)(task const &);
	id peek(fit_fn fit = nullptr) const;
	bool start(id which);
	id take();
	// A known task offered again, in the shape now offered.  A
	// parked one goes back to pending: an offer renewed after the
	// failure that parked it -- a reader asking again once the
	// server is back -- deserves the ask the first offer got.  A
	// pending or running one takes the offered shape, so what is
	// assembled and what is named agree -- a dive whose scan grew a
	// video must cover both; a running one keeps running, its
	// completion is obsolete, and requeue() asks it again.  True
	// when the offer owes a fresh ask: the un-parking, or a new
	// kind or dependency set, the inputs that re-key an artifact;
	// false for a task done, unknown, or as offered already.
	bool renew(task t);
	// A running task back to pending: its generation turned out
	// obsolete by the time it finished.  False unless running.
	bool requeue(id which);

	// Drops every task and all heat; completions of tasks taken
	// before the reset may still be reported and are remembered.
	void reset();

	task const *get(id which) const;
	state status(id which) const;
	std::size_t backlog() const;   // pending + running

private:
	struct entry {
		task  t;
		state s = state::pending;
	};

	static constexpr std::size_t npos = std::size_t(-1);

	std::size_t index_of(id which) const;
	bool ready(entry const &e) const;
	double score(task const &t) const;
	bool lift(std::vector<double> &eff) const;
	bool raise_deps(std::size_t at, std::vector<double> &eff) const;

	// Tasks number in the thousands once every window, pair and
	// question is one: entries append only and the index finds
	// them; heat keys are a few dozen subtitles, scanned.
	std::vector<entry>                 m_entries;
	index                              m_index;
	std::vector<std::pair<id, double>> m_heat;
};

using combine_fn = id (*)(std::vector<id> const &);

// The abstraction pyramid over ordered leaves as node tasks (the
// leaves themselves are offered separately): pairs joined per level,
// an odd tail carried upward unchanged, deps the two children, keys
// the union of covered leaves, tier the level.  combine() names a
// parent from its ordered children; same leaves, same names -- two
// corpora sharing a prefix share the prefix's summaries.
std::vector<task> pyramid(std::vector<id> const &leaves,
                          combine_fn             combine);

} // namespace agenda

#endif // SRTVIEW_SRC_AGENDA_HPP_
