// agenda.cpp -- see agenda.hpp.  Scoring constants, the priority-
// inheritance relaxation, and the pyramid builder live here.
#include <algorithm>
#include <cstring>
#include <iterator>

#include "agenda.hpp"

namespace agenda {

namespace {

// Score anatomy, mirrored in the header comment: kinds set the
// neighborhood (leaf, node, dive a unit apart, focus half a band
// under dives, probe a notch under focus: evidence already gathered
// turns into prose before new inquiries open; terms sit between
// summaries and dives -- the index backbone outranks topic prose
// but never starves the pyramid), tiers order within a kind, the
// export edge floats a topic over its supportive components, and
// heat (unbounded) moves everything within its rank.  The rank is
// the dominant, lexicographic half of the key: the released band
// -- the summary backbone (leaf, node) first, then the semantic
// chain (extract, judge) -- outranks all other background work by
// construction, however hot a dive burns, and an answer tops even
// that -- policy the executor once expressed as a second peek, now
// the score's own shape.  Leaves wait on their video's read as
// extraction waits on the cut's, so a video's summary is the first
// thing asked once its read completes, and the pyramid follows
// before any window is extracted.
// Evidence extraction sits between nodes and the lexical terms pass;
// pair judgments sit between terms and dives -- a verdict is a
// short call that reshapes the knowledge tree, a dive a long one
// that adds an essay, and the lexicon the terms pass builds answers
// many a name question before the judge sees it.  Judgments are
// never exported, dives of a top-level topic are, so the judge
// band clears a dive's base plus the export edge with its own tier
// step to spare; an interactive answer jumps the queue.  merge sits
// at the bottom: the fold judgment over the term directory is most
// useful once the queue has otherwise drained and the directory has
// stopped moving.
constexpr double kKindBase[]  = {3.0, 2.0, 1.0, 0.5, 0.45, 1.5,
                                 0.4, 0.42, 1.75, 1.4, 4.0};
static_assert(std::size(kKindBase) == kind_count,
              "kKindBase mirrors agenda::kind");
constexpr int    kKindRank[]  = {1, 1, 0, 0, 0, 0,
                                 0, 0, 1, 1, 2};
static_assert(std::size(kKindRank) == kind_count,
              "kKindRank mirrors agenda::kind");
constexpr double kTierStep    = 1.0 / 32.0;
constexpr double kExportEdge  = 1.0 / 4.0;

// Relaxation passes cover any dependency chain the pipeline builds:
// pyramid depth is log2 of the corpus and dives hang off leaves.
constexpr std::size_t kLiftCap = 32;

// Heat entries below this are noise; decay() sweeps them out.
constexpr double kColdFloor = 1e-6;

constexpr char kHexDig[] = "0123456789abcdef";

// Hex digit values; -1 fills the invalid rest.
constexpr auto kUnhex = [] {
	std::array<signed char, 256> t{};
	for (auto &x : t)
		x = -1;
	for (int c = '0'; c <= '9'; ++c)
		t[std::size_t(c)] = char(c - '0');
	for (int c = 'a'; c <= 'f'; ++c)
		t[std::size_t(c)] = char(c - 'a' + 10);
	for (int c = 'A'; c <= 'F'; ++c)
		t[std::size_t(c)] = char(c - 'A' + 10);
	return t;
}();

} // namespace

std::string id::hex () const
{
	std::string s(2 * b.size(), '0');
	for (std::size_t i = 0; i < b.size(); ++i) {
		s[2 * i]     = kHexDig[b[i] >> 4];
		s[2 * i + 1] = kHexDig[b[i] & 15];
	}

	return s;
}

id id::from_hex (std::string_view s)
{
	id out;
	if (s.size() != 2 * out.b.size())
		return {};
	for (std::size_t i = 0; i < out.b.size(); ++i) {
		int const hi = kUnhex[std::uint8_t(s[2 * i])];
		int const lo = kUnhex[std::uint8_t(s[2 * i + 1])];
		if (hi < 0 || lo < 0)
			return {};
		out.b[i] = std::uint8_t(hi << 4 | lo);
	}

	return out;
}

std::size_t index::home (id key) const
{
	std::uint64_t h;
	std::memcpy(&h, key.b.data(), sizeof h);
	return h & (m_slots.size() - 1);
}

std::size_t index::find (id key) const
{
	if (!key || m_slots.empty())
		return npos;
	for (std::size_t at = home(key);;
	     at = (at + 1) & (m_slots.size() - 1)) {
		if (m_slots[at].key == key)
			return m_slots[at].value;
		if (!m_slots[at].key)
			return npos;
	}
}

void index::add (id key, std::size_t value)
{
	if (!key)
		return;
	if ((m_used + 1) * 2 > m_slots.size()) {
		std::vector<slot> const old = std::move(m_slots);
		m_slots.assign(old.empty() ? 16 : old.size() * 2, {});
		m_used = 0;
		for (slot const &s : old)
			if (s.key)
				add(s.key, s.value);
	}
	for (std::size_t at = home(key);;
	     at = (at + 1) & (m_slots.size() - 1)) {
		if (m_slots[at].key == key) {
			m_slots[at].value = value;
			return;
		}
		if (!m_slots[at].key) {
			m_slots[at] = {key, value};
			++m_used;
			return;
		}
	}
}

void index::clear ()
{
	m_slots.clear();
	m_used = 0;
}

void plan::add (task t)
{
	if (index_of(t.id) != npos)
		return;

	m_index.add(t.id, m_entries.size());
	m_entries.push_back({std::move(t), state::pending});
}

void plan::done (id which)
{
	std::size_t const at = index_of(which);
	if (at == npos) {
		m_index.add(which, m_entries.size());
		m_entries.push_back({task{.id = which}, state::done});
		return;
	}

	m_entries[at].s = state::done;
}

void plan::fail (id which)
{
	std::size_t const at = index_of(which);
	if (at == npos) {
		m_index.add(which, m_entries.size());
		m_entries.push_back({task{.id = which}, state::parked});
		return;
	}

	m_entries[at].s = state::parked;
}

void plan::heat (id key, double add)
{
	for (auto &[k, w] : m_heat) {
		if (k != key)
			continue;
		w += add;
		return;
	}

	m_heat.push_back({key, add});
}

void plan::decay (double keep)
{
	for (auto &[k, w] : m_heat)
		w *= keep;
	std::erase_if(m_heat, [](std::pair<id, double> const &h) {
		return h.second < kColdFloor;
	});
}

id plan::take ()
{
	id const next = peek();
	start(next);
	return next;
}

bool plan::renew (task t)
{
	std::size_t const at = index_of(t.id);
	if (at == npos)
		return false;
	entry &e = m_entries[at];
	if (e.s == state::parked) {
		e = {std::move(t), state::pending};
		return true;
	}
	if (e.s == state::done) {
		// A cache hit re-offered by a successor cut takes the
		// new shape -- above all its new witness -- and stays
		// done: complete() must judge the artifact by the gates
		// of the cut that wants it now, not the one that first
		// saw it.  Refusing here once pinned a warm window to a
		// mid-read cut's witness, which never marks -- cached
		// knowledge stayed gated for the whole session.  No ask
		// is owed, so the caller hears nothing new.
		e.t = std::move(t);
		return false;
	}
	if (e.s != state::pending && e.s != state::running)
		return false;
	bool const rekeyed = e.t.what != t.what || e.t.deps != t.deps;
	e.t = std::move(t);
	return rekeyed;
}

bool plan::requeue (id which)
{
	std::size_t const at = index_of(which);
	if (at == npos || m_entries[at].s != state::running)
		return false;
	m_entries[at].s = state::pending;
	return true;
}

bool plan::start (id which)
{
	std::size_t const at = index_of(which);
	if (at == npos || m_entries[at].s != state::pending)
		return false;
	m_entries[at].s = state::running;
	return true;
}

id plan::peek (fit_fn fit) const
{
	std::vector<rank_key> own(m_entries.size());
	for (std::size_t i = 0; i < m_entries.size(); ++i)
		own[i] = m_entries[i].s == state::pending
		         ? score(m_entries[i].t)
		         : rank_key{};
	std::vector<rank_key> eff(own);
	for (std::size_t pass = 0; pass < kLiftCap && lift(eff); ++pass)
		;

	// Inherited urgency forms the class, the task's own score
	// orders within it.  On effective score alone, a hot aggregate
	// (the pyramid root sums every leaf's heat) would flatten its
	// whole subtree to one value and hand the order back to
	// insertion, erasing exactly the distinctions the heat painted.
	std::size_t best = npos;
	for (std::size_t i = 0; i < m_entries.size(); ++i) {
		if (m_entries[i].s != state::pending || !ready(m_entries[i])
		    || (fit && !fit(m_entries[i].t)))
			continue;
		if (best == npos || eff[i] > eff[best] ||
		    (eff[i] == eff[best] && own[i] > own[best]))
			best = i;
	}
	return best == npos ? id{} : m_entries[best].t.id;
}

void plan::reset ()
{
	// Everything goes, done ids included: cache truth is re-derived
	// by vault resolution at the next offer, and a retained done
	// tombstone would let a stale completion satisfy a generation
	// it never saw, then keep the changed input from re-offering.
	m_entries.clear();
	m_index.clear();
	m_heat.clear();
}

task const *plan::get (id which) const
{
	std::size_t const at = index_of(which);
	return at == npos ? nullptr : &m_entries[at].t;
}

plan::state plan::status (id which) const
{
	std::size_t const at = index_of(which);
	return at == npos ? state::unknown : m_entries[at].s;
}

std::size_t plan::backlog () const
{
	return std::size_t(std::ranges::count_if(m_entries,
		[](entry const &e) {
			return e.s == state::pending
			    || e.s == state::running;
		}));
}

std::size_t plan::index_of (id which) const
{
	return m_index.find(which);
}

bool plan::ready (entry const &e) const
{
	// Transitive on purpose: a dependency that is done but not yet
	// complete -- a cached artifact behind a pending gate -- must
	// hold its dependents exactly as a pending one would.
	return std::ranges::all_of(e.t.deps,
		[this](id const d) {
			return complete(d);
		});
}

bool plan::complete (id which) const
{
	std::size_t const at = index_of(which);
	return at != npos && m_entries[at].s == state::done
	    && ready(m_entries[at]);
}

plan::rank_key plan::score (task const &t) const
{
	double s = kKindBase[std::size_t(t.what)]
	         - kTierStep * t.tier
	         + (t.exported ? kExportEdge : 0.0);
	for (id const &key : t.keys) {
		for (auto const &[k, w] : m_heat) {
			if (k != key)
				continue;
			s += w;
			break;
		}
	}

	return {kKindRank[std::size_t(t.what)], s};
}

// One relaxation pass of priority inheritance: every pending task
// pulls each of its pending dependencies up to at least its own
// effective score.  Blocked tasks relay what they inherited, so a
// hot root reaches its deepest missing leaf within the pass cap.
bool plan::lift (std::vector<rank_key> &eff) const
{
	bool changed = false;
	for (std::size_t i = 0; i < m_entries.size(); ++i) {
		if (m_entries[i].s != state::pending)
			continue;
		changed |= raise_deps(i, eff);
	}

	return changed;
}

bool plan::raise_deps (std::size_t at, std::vector<rank_key> &eff) const
{
	// The whole key travels: the blocker of ranked work runs with
	// that rank, or the rank would starve behind its own inputs.
	bool changed = false;
	for (id const d : m_entries[at].t.deps) {
		std::size_t const j = index_of(d);
		if (j == npos || m_entries[j].s != state::pending
		    || eff[j] >= eff[at])
			continue;
		eff[j] = eff[at];
		changed = true;
	}

	return changed;
}

std::vector<task> pyramid (std::vector<id> const &leaves,
                           combine_fn             combine)
{
	// Working item: a produced-or-carried id plus the leaves it
	// covers, which become the parent's heat keys.
	struct item {
		id              v;
		std::vector<id> keys;
	};

	std::vector<item> level;
	for (id const l : leaves)
		level.push_back({l, {l}});

	std::vector<task> out;
	for (std::uint8_t tier = 1; level.size() > 1; ++tier) {
		std::vector<item> next;
		for (std::size_t i = 0; i + 1 < level.size(); i += 2) {
			item &a = level[i];
			item &b = level[i + 1];
			item joined{combine({a.v, b.v}), std::move(a.keys)};
			joined.keys.insert(joined.keys.end(),
			                   b.keys.begin(), b.keys.end());
			out.push_back({
				.id   = joined.v,
				.deps = {a.v, b.v},
				.keys = joined.keys,
				.what = kind::node,
				.tier = tier,
			});
			next.push_back(std::move(joined));
		}
		if (level.size() % 2)
			next.push_back(std::move(level.back()));
		level = std::move(next);
	}

	return out;
}

} // namespace agenda
