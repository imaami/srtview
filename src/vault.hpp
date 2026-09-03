// vault.hpp -- content-chained cache naming for the facts store.
// Standard C++, no Qt, no llm: the naming and adoption policy is a
// pure filesystem concern, unit-tested without a model.
//
// A cache artifact's filename is
// <planid>.<recipe>.<suffix>.txt.  The plan id is the scheduler's
// structural identity, unchanged and computable before any artifact
// exists; recipe hashes the prompt/schema/generation contract (a
// dependent kind's folds in the recipe of the kind it reads, so an
// upstream prompt change misses downstream too), and the suffix
// chains content:
// a leaf's suffix hashes its body (the rendered transcript and the
// video's frame lines), everything else
// hashes its hard deps' suffixes and artifact bytes, so an external
// edit to an .srt or to a cached file shifts every transitive
// dependent's expected name.  Resolution recomputes the expected
// name from current content and adopts by rename whatever sits
// under the same plan and recipe with a stale content suffix, so
// edits cost renames, never regeneration, while a changed recipe is
// a real cache miss.  Old recipes and legacy names stay untouched:
// switching back can reuse them, and no prompt change destroys user
// cache data.  Renames are atomic and idempotent, adoption is lazy
// and bottom-up (dep resolution recurses first), and every move or
// drop appends a line to the human journal.  Content-keyed artifacts
// omit the suffix but still carry the recipe.  Directory
// listings sit on a per-kind shelf the store keeps current itself
// -- it is the process's only cache writer -- so steady-state
// lookups read no directories at all.
//
// The write protocol is tmp() then place(): the executor writes the
// reply to the plan-stable tmp name, asks place() for the target
// (which sweeps every other file of that plan id -- the
// one-file-per-plan invariant's only home), and renames.
//
// No mutex of its own: Facts::m_mtx guards every call (R1-R4 in
// facts.hpp); tests drive single-threaded.
#ifndef SRTVIEW_SRC_VAULT_HPP_
#define SRTVIEW_SRC_VAULT_HPP_

#include "agenda.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace vault {

// H8: the leading 8 bytes of a BLAKE2b-256 in production, injected
// like agenda::combine_fn so the module stays hash-library-free and
// tests stay deterministic.
using hash8_fn = agenda::id (*)(std::string_view);
using recipes = std::array<agenda::id, agenda::kind_count>;

class store {
public:
	store(std::string dir, hash8_fn h, recipes recipe);

	// The leaf's content witness: H8 of the rendered transcript
	// text actually offered to the model.  A changed witness drops
	// every memo -- the next resolution recomputes the chain.
	void content(agenda::id leaf, agenda::id input);

	// The task's artifact if one exists, adopting stale names by
	// rename; empty on a miss or while the chain is incomputable
	// (no witness yet, dep unregistered, dep artifact missing).
	std::string resolve(agenda::task const &t);

	// The registered task's artifact by id alone -- assembly reads
	// deps it knows only by id.  Unregistered ids miss.
	std::string resolve(agenda::id id);

	// The task's currently owed artifact name.  Never sweeps,
	// publishes or re-keys the task's own entry; resolving the
	// dependency chain may adopt a dep's stale name and memoize --
	// the same rename any reader would perform next, the vault
	// doctrine rather than a side effect.  Empty while the chain
	// is incomputable.  The executor records the submit-time value
	// and publishes only while it still holds.
	std::string target(agenda::task const &t);

	// Current-state reads by plan id, for completion: the
	// registered shape answers, and the caller's task copy never
	// re-registers -- a reload may have re-shaped the id, and a
	// stale copy writing itself back would make the generation
	// comparison vacuous.  Unregistered ids answer empty.
	std::string target(agenda::id id);
	std::string place(agenda::id id);

	// The write target for a finished artifact; unlinks every
	// other file of the plan id except its tmp.  Empty while the
	// chain is incomputable.
	std::string place(agenda::task const &t);

	// The in-progress write target: plan-stable, computable
	// without deps, never adopted or swept.
	std::string tmp(agenda::task const &t) const;

	// Best-effort prefix match for previews and soft refs: any
	// artifact of the plan id, whatever its suffix, verified
	// against nothing.
	std::string locate(agenda::id plan, agenda::kind k) const;

	// The first failure creating a kind directory: the store is the
	// cache's only writer, so it lays the directories out itself,
	// and a caller that cannot have them must not queue work.
	std::error_code const &error() const { return m_error; }

	// A kind's effective recipe, folds included: what names its
	// artifacts, for a caller keying its own durable state by it.
	agenda::id recipe(agenda::kind k) const
	{
		return m_recipe[std::size_t(k)];
	}

	// Readdir passes performed so far: the shelf keeps the steady
	// state scan-free, and the tests hold it to that.
	std::size_t scans() const { return m_scans; }

private:
	struct entry {
		agenda::task t;         // shape as last registered
		agenda::id   input;     // leaf witness
		agenda::id   suffix;    // chain hash; zero = uncomputed
		agenda::id   bytes;     // artifact hash; zero = unread
		std::string  path;      // memoized artifact; empty = none
		bool         walking = false; // chain() re-entry guard
	};

	// One sorted listing per kind directory, filled lazily and
	// kept by the store itself: an adoption rename updates it in
	// place, a placement stales it for one re-read, and nothing
	// else writes these directories in-session (external edits
	// surface at the next load, the documented doctrine).
	struct shelf {
		std::vector<std::string> names;
		bool                     fresh = false;
	};

	static constexpr std::size_t npos = std::size_t(-1);

	std::size_t index_of(agenda::id id) const;
	std::size_t enroll(entry e);
	std::size_t registered(agenda::task const &t);
	void forget();
	std::string resolve_at(std::size_t at);
	agenda::id chain_of(std::size_t at);
	bool chain(std::size_t at);
	bool artifact_bytes(std::size_t at);
	std::string flat(std::size_t at) const;
	std::string two_part(std::size_t at) const;
	std::vector<std::string> siblings(agenda::id plan,
	                                  agenda::kind k) const;
	void journal(std::string const &line) const;

	// Entries append only; the index finds them (the agenda's own
	// convention).
	std::vector<entry>  m_entries;
	agenda::index       m_index;
	mutable shelf       m_shelf[agenda::kind_count];
	std::string         m_dir;
	std::error_code     m_error;
	hash8_fn            m_h;
	recipes             m_recipe;
	mutable std::size_t m_scans = 0;
};

} // namespace vault

#endif // SRTVIEW_SRC_VAULT_HPP_
