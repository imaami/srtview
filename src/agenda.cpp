// agenda.cpp -- see agenda.hpp.  Scoring constants, the priority-
// inheritance relaxation, and the pyramid builder live here.
#include <algorithm>

#include "agenda.hpp"

namespace agenda {

namespace {

// Score anatomy, mirrored in the header comment: kinds set the
// neighborhood a full unit apart, tiers order within a kind, the
// export edge floats a topic over its supportive components, and
// heat (unbounded) moves everything across those bands.
constexpr double kKindBase[]  = {3.0, 2.0, 1.0};
constexpr double kTierStep    = 1.0 / 32.0;
constexpr double kExportEdge  = 1.0 / 4.0;

// Relaxation passes cover any dependency chain the pipeline builds:
// pyramid depth is log2 of the corpus and dives hang off leaves.
constexpr std::size_t kLiftCap = 32;

// Heat entries below this are noise; decay() sweeps them out.
constexpr double kColdFloor = 1e-6;

} // namespace

void plan::add (task t)
{
	if (index_of(t.id) != npos)
		return;

	m_entries.push_back({std::move(t), state::pending});
}

void plan::done (std::string const &id)
{
	std::size_t const at = index_of(id);
	if (at == npos) {
		m_entries.push_back({task{.id = id}, state::done});
		return;
	}

	m_entries[at].s = state::done;
}

void plan::fail (std::string const &id)
{
	std::size_t const at = index_of(id);
	if (at != npos)
		m_entries[at].s = state::parked;
}

void plan::heat (std::string const &key, double add)
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
	std::erase_if(m_heat, [](std::pair<std::string, double> const &h) {
		return h.second < kColdFloor;
	});
}

std::string plan::take ()
{
	std::vector<double> eff(m_entries.size());
	for (std::size_t i = 0; i < m_entries.size(); ++i)
		eff[i] = m_entries[i].s == state::pending
		         ? score(m_entries[i].t)
		         : 0.0;
	for (std::size_t pass = 0; pass < kLiftCap && lift(eff); ++pass)
		;

	std::size_t best = npos;
	for (std::size_t i = 0; i < m_entries.size(); ++i) {
		if (m_entries[i].s != state::pending || !ready(m_entries[i]))
			continue;
		if (best == npos || eff[i] > eff[best])
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

task const *plan::get (std::string const &id) const
{
	std::size_t const at = index_of(id);
	return at == npos ? nullptr : &m_entries[at].t;
}

plan::state plan::status (std::string const &id) const
{
	std::size_t const at = index_of(id);
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

std::size_t plan::index_of (std::string const &id) const
{
	for (std::size_t i = 0; i < m_entries.size(); ++i)
		if (m_entries[i].t.id == id)
			return i;

	return npos;
}

bool plan::ready (entry const &e) const
{
	return std::ranges::all_of(e.t.deps,
		[this](std::string const &d) {
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
	for (std::string const &key : t.keys) {
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
	for (std::string const &d : m_entries[at].t.deps) {
		std::size_t const j = index_of(d);
		if (j == npos || m_entries[j].s != state::pending
		    || eff[j] >= eff[at])
			continue;
		eff[j] = eff[at];
		changed = true;
	}

	return changed;
}

std::vector<task> pyramid (std::vector<std::string> const &leaves,
                           combine_fn                      combine)
{
	// Working item: a produced-or-carried id plus the leaves it
	// covers, which become the parent's heat keys.
	struct item {
		std::string              id;
		std::vector<std::string> keys;
	};

	std::vector<item> level;
	for (std::string const &l : leaves)
		level.push_back({l, {l}});

	std::vector<task> out;
	for (std::uint8_t tier = 1; level.size() > 1; ++tier) {
		std::vector<item> next;
		for (std::size_t i = 0; i + 1 < level.size(); i += 2) {
			item &a = level[i];
			item &b = level[i + 1];
			item joined{combine({a.id, b.id}), std::move(a.keys)};
			joined.keys.insert(joined.keys.end(),
			                   b.keys.begin(), b.keys.end());
			out.push_back({
				.id   = joined.id,
				.deps = {std::move(a.id), std::move(b.id)},
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
