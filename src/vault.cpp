// vault.cpp -- see vault.hpp for the naming scheme.
#include "vault.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>

namespace vault {

namespace {

namespace fs = std::filesystem;

// Kind names salt the suffix; subdirs mirror the facts layout.
constexpr std::string_view kKind[]{
	"leaf", "node", "dive", "focus", "probe", "terms"
};
constexpr std::string_view kSub[]{
	"", "", "dives/", "focus/", "probe/", "terms/"
};

static_assert(std::size(kKind) == std::size(kSub)
              && std::size(kKind)
                 == std::size_t(agenda::kind::terms) + 1,
              "the tables mirror agenda::kind");

std::string_view sub(agenda::kind k)
{
	return kSub[std::size_t(k)];
}

// Whole small file as bytes; missing tells apart from empty.
bool slurp(std::string const &path, std::string &out)
{
	std::ifstream f(path, std::ios::binary);
	if (!f)
		return false;
	out.assign(std::istreambuf_iterator<char>(f), {});
	return true;
}

void append(std::string &s, agenda::id const &id)
{
	s.append(reinterpret_cast<char const *>(id.b.data()),
	         id.b.size());
}

} // namespace

store::store(std::string dir, hash8_fn h)
	: m_dir(std::move(dir)), m_h(h)
{
}

std::size_t store::index_of(agenda::id id) const
{
	for (std::size_t i = 0; i < m_entries.size(); ++i)
		if (m_entries[i].t.id == id)
			return i;
	return npos;
}

// Drops every memo: a changed witness or a reshaped task re-keys
// arbitrary transitive dependents, and entries hold no reverse
// edges to chase -- wholesale is both correct and cheap.
void store::forget()
{
	for (entry &e : m_entries) {
		e.suffix = {};
		e.bytes = {};
		e.path.clear();
	}
}

void store::content(agenda::id leaf, agenda::id input)
{
	std::size_t const at = index_of(leaf);
	if (at == npos) {
		agenda::task t;
		t.id = leaf;
		m_entries.push_back({std::move(t), input,
		                     {}, {}, {}, false});
		return;
	}
	if (m_entries[at].input == input)
		return;
	// A new witness re-keys the whole chain: every memo drops, and
	// the next resolutions adopt whatever the renames now owe.
	m_entries[at].input = input;
	forget();
}

std::size_t store::registered(agenda::task const &t)
{
	std::size_t const at = index_of(t.id);
	if (at == npos) {
		m_entries.push_back({t, {}, {}, {}, {}, false});
		return m_entries.size() - 1;
	}
	entry &e = m_entries[at];
	// Only the suffix inputs re-key: a reshaped dep set (a dive
	// whose scan grew a video) recomputes, changed keys or notes
	// do not.  The reshape re-keys its transitive dependents too,
	// so the memos drop wholesale, like a changed witness.
	if (e.t.what != t.what || e.t.deps != t.deps)
		forget();
	e.t = t;
	return at;
}

std::string store::flat(std::size_t at) const
{
	entry const &e = m_entries[at];
	return m_dir + '/' + std::string(sub(e.t.what))
	     + e.t.id.hex() + ".txt";
}

std::string store::two_part(std::size_t at) const
{
	entry const &e = m_entries[at];
	return m_dir + '/' + std::string(sub(e.t.what))
	     + e.t.id.hex() + '.' + e.suffix.hex() + ".txt";
}

std::string store::tmp(agenda::task const &t) const
{
	return m_dir + '/' + std::string(sub(t.what))
	     + t.id.hex() + ".tmp";
}

// Every artifact-shaped name of the plan id in its kind's dir --
// "<hex>.txt" and "<hex>.<suffix>.txt", never the tmp -- in sorted
// order, so adoption picks deterministically.  Served off the
// shelf: one readdir fills it, and every lookup in between is a
// binary search.
std::vector<std::string> store::siblings(agenda::id plan,
                                         agenda::kind k) const
{
	shelf &s = m_shelf[std::size_t(k)];
	if (!s.fresh) {
		s.names.clear();
		std::error_code ec;
		fs::directory_iterator it(m_dir + '/'
		                          + std::string(sub(k)), ec);
		for (fs::directory_iterator const end;
		     !ec && it != end; it.increment(ec)) {
			std::string n = it->path().filename().string();
			if (n.ends_with(".txt"))
				s.names.push_back(std::move(n));
		}
		std::ranges::sort(s.names);
		s.fresh = true;
		++m_scans;
	}
	std::string const hex = plan.hex();
	std::vector<std::string> out;
	for (auto it = std::ranges::lower_bound(s.names, hex);
	     it != s.names.end() && it->starts_with(hex); ++it)
		if (it->size() > hex.size() && (*it)[hex.size()] == '.')
			out.push_back(*it);
	return out;
}

void store::journal(std::string const &line) const
{
	std::ofstream f(m_dir + "/journal.txt",
	                std::ios::app | std::ios::binary);
	f << line << '\n';
}

bool store::artifact_bytes(std::size_t at)
{
	if (m_entries[at].bytes)
		return true;
	std::string const p = resolve_at(at);
	std::string data;
	if (p.empty() || !slurp(p, data))
		return false;
	m_entries[at].bytes = m_h(data);
	return true;
}

// The chain hash proper; zero on an incomputable input.
agenda::id store::chain_of(std::size_t at)
{
	agenda::kind const what = m_entries[at].t.what;
	if (what == agenda::kind::leaf) {
		if (!m_entries[at].input)
			return {};
		std::string acc{kKind[0]};
		append(acc, m_entries[at].input);
		return m_h(acc);
	}
	std::vector<agenda::id> deps = m_entries[at].t.deps;
	if (what == agenda::kind::focus
	    || what == agenda::kind::probe)
		// Pairing records arrival order; the pair is the
		// identity, so the suffix sorts like the plan id does.
		std::ranges::sort(deps);
	std::string acc{kKind[std::size_t(what)]};
	append(acc, m_entries[at].t.id);
	for (agenda::id const d : deps) {
		std::size_t const i = index_of(d);
		if (i == npos || !chain(i) || !artifact_bytes(i))
			return {};
		append(acc, m_entries[i].suffix);
		append(acc, m_entries[i].bytes);
	}
	return m_h(acc);
}

bool store::chain(std::size_t at)
{
	if (m_entries[at].suffix)
		return true;
	// A dependency loop would recurse forever; the walking mark
	// turns re-entry into an incomputable chain instead.  Plans
	// are acyclic by construction -- this is the crash-to-miss
	// conversion for the day something is not.
	if (m_entries[at].walking)
		return false;
	m_entries[at].walking = true;
	agenda::id const sfx = chain_of(at);
	m_entries[at].walking = false;
	if (!sfx)
		return false;
	m_entries[at].suffix = sfx;
	return true;
}

// Resolution by index: the recursion registers nothing, so indexes
// stay stable below the public surface.  A miss is never memoized
// -- the artifact may be generated later in the session.
std::string store::resolve_at(std::size_t at)
{
	if (!m_entries[at].path.empty())
		return m_entries[at].path;
	std::error_code ec;
	if (m_entries[at].t.what == agenda::kind::terms) {
		std::string p = flat(at);
		if (!fs::exists(p, ec))
			return {};
		m_entries[at].path = std::move(p);
		return m_entries[at].path;
	}
	if (!chain(at))
		return {};
	std::string const target = two_part(at);
	if (!fs::exists(target, ec)) {
		auto const cand = siblings(m_entries[at].t.id,
		                           m_entries[at].t.what);
		if (cand.empty())
			return {};
		std::string const dir = m_dir + '/'
			+ std::string(sub(m_entries[at].t.what));
		fs::rename(dir + cand.front(), target, ec);
		if (ec)
			return {};
		// The shelf follows the rename in place: adoption bursts
		// (a whole legacy cache, an edited chain) stay at one
		// readdir per directory.
		std::string const moved =
			fs::path(target).filename().string();
		shelf &sh = m_shelf[std::size_t(m_entries[at].t.what)];
		std::erase(sh.names, cand.front());
		sh.names.insert(std::ranges::lower_bound(sh.names,
		                                         moved), moved);
		bool const legacy = cand.front().size()
			== m_entries[at].t.id.hex().size() + 4;
		journal("moved " + std::string(sub(m_entries[at].t.what))
		        + cand.front() + " -> " + moved
		        + (legacy ? ": adopt" : ": content"));
	}
	m_entries[at].path = target;
	return m_entries[at].path;
}

std::string store::resolve(agenda::task const &t)
{
	return resolve_at(registered(t));
}

std::string store::resolve(agenda::id id)
{
	std::size_t const at = index_of(id);
	return at == npos ? std::string() : resolve_at(at);
}

std::string store::place(agenda::task const &t)
{
	std::size_t const at = registered(t);
	std::string target;
	if (t.what == agenda::kind::terms) {
		target = flat(at);
	} else {
		if (!chain(at))
			return {};
		target = two_part(at);
	}
	std::string const keep = fs::path(target).filename().string();
	std::string const dir = m_dir + '/'
	                      + std::string(sub(t.what));
	for (std::string const &n : siblings(t.id, t.what)) {
		if (n == keep)
			continue;
		std::error_code ec;
		fs::remove(dir + n, ec);
		if (!ec)
			journal("dropped " + std::string(sub(t.what))
			        + n);
	}
	// Fresh bytes are about to land -- dependents must hash them,
	// not a memo of the old artifact -- and the file itself is not
	// there until the caller's rename lands: no path memo either,
	// or a failed rename would leave cached() swearing by a
	// phantom.  The next resolve reads the disk and memoizes
	// truth; the shelf re-reads its directory once for the same
	// reason.
	m_entries[at].bytes = {};
	m_entries[at].path.clear();
	m_shelf[std::size_t(t.what)].fresh = false;
	return target;
}

std::string store::locate(agenda::id plan, agenda::kind k) const
{
	auto const cand = siblings(plan, k);
	if (cand.empty())
		return {};
	return m_dir + '/' + std::string(sub(k)) + cand.front();
}

} // namespace vault
