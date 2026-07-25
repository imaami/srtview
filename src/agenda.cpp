// agenda.cpp -- see agenda.hpp.  Scoring constants, the priority-
// inheritance relaxation, and the pyramid builder live here.
#include <algorithm>

#include "agenda.hpp"

namespace agenda {

namespace {

// Score anatomy, mirrored in the header comment: kinds set the
// neighborhood (leaf, node, dive a unit apart, focus half a band
// under dives), tiers order within a kind, the export edge floats
// a topic over its supportive components, and heat (unbounded)
// moves everything across those bands.
constexpr double kKindBase[]  = {3.0, 2.0, 1.0, 0.5};
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

void plan::add (task t)
{
	if (index_of(t.id) != npos)
		return;

	m_entries.push_back({std::move(t), state::pending});
}

void plan::done (id which)
{
	std::size_t const at = index_of(which);
	if (at == npos) {
		m_entries.push_back({task{.id = which}, state::done});
		return;
	}

	m_entries[at].s = state::done;
}

void plan::fail (id which)
{
	std::size_t const at = index_of(which);
	if (at == npos) {
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
	std::vector<double> own(m_entries.size());
	for (std::size_t i = 0; i < m_entries.size(); ++i)
		own[i] = m_entries[i].s == state::pending
		         ? score(m_entries[i].t)
		         : 0.0;
	std::vector<double> eff(own);
	for (std::size_t pass = 0; pass < kLiftCap && lift(eff); ++pass)
		;

	// Inherited urgency forms the class, the task's own score
	// orders within it.  On effective score alone, a hot aggregate
	// (the pyramid root sums every leaf's heat) would flatten its
	// whole subtree to one value and hand the order back to
	// insertion, erasing exactly the distinctions the heat painted.
	std::size_t best = npos;
	for (std::size_t i = 0; i < m_entries.size(); ++i) {
		if (m_entries[i].s != state::pending || !ready(m_entries[i]))
			continue;
		if (best == npos || eff[i] > eff[best] ||
		    (eff[i] == eff[best] && own[i] > own[best]))
			best = i;
	}
	if (best == npos)
		return {};

	m_entries[best].s = state::running;
	return m_entries[best].t.id;
}

void plan::reset ()
{
	// Ids that already ran to completion stay done: they name cache
	// files, and those outlive any plan.
	std::erase_if(m_entries, [](entry const &e) {
		return e.s != state::done;
	});
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
	for (std::size_t i = 0; i < m_entries.size(); ++i)
		if (m_entries[i].t.id == which)
			return i;

	return npos;
}

bool plan::ready (entry const &e) const
{
	return std::ranges::all_of(e.t.deps,
		[this](id const d) {
			std::size_t const at = index_of(d);
			return at != npos
			    && m_entries[at].s == state::done;
		});
}

double plan::score (task const &t) const
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

	return s;
}

// One relaxation pass of priority inheritance: every pending task
// pulls each of its pending dependencies up to at least its own
// effective score.  Blocked tasks relay what they inherited, so a
// hot root reaches its deepest missing leaf within the pass cap.
bool plan::lift (std::vector<double> &eff) const
{
	bool changed = false;
	for (std::size_t i = 0; i < m_entries.size(); ++i) {
		if (m_entries[i].s != state::pending)
			continue;
		changed |= raise_deps(i, eff);
	}

	return changed;
}

bool plan::raise_deps (std::size_t at, std::vector<double> &eff) const
{
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
