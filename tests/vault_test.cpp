// vault_test.cpp -- unit tests for the cache-naming store.  Standard
// C++ against a throwaway rig directory: no Qt, no llm, the hash
// injected as a deterministic mixer.
#include "vault.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

namespace fs = std::filesystem;

int g_fail = 0;

void check(bool ok, char const *what)
{
	std::printf("%s  %s\n", ok ? "OK  " : "FAIL", what);
	if (!ok)
		++g_fail;
}

agenda::id mix(std::string_view s)
{
	std::uint64_t h = 1469598103934665603ull;
	for (unsigned char const c : s) {
		h ^= c;
		h *= 1099511628211ull;
	}
	agenda::id out;
	for (std::size_t i = 0; i < out.b.size(); ++i)
		out.b[i] = std::uint8_t(h >> (8 * i));
	return out;
}

agenda::id gid(unsigned char x)
{
	agenda::id i;
	i.b[0] = x;
	return i;
}

vault::recipes recipes(unsigned char seed)
{
	vault::recipes r;
	for (std::size_t i = 0; i < r.size(); ++i)
		r[i] = gid(static_cast<unsigned char>(seed + i));
	return r;
}

agenda::task task(agenda::id id, agenda::kind k,
                  std::vector<agenda::id> deps = {})
{
	agenda::task t;
	t.id = id;
	t.deps = std::move(deps);
	t.what = k;
	return t;
}

fs::path g_rig;

std::string dir()
{
	return g_rig.string();
}

void reset_rig()
{
	fs::remove_all(g_rig);
	fs::create_directories(g_rig);
}

void put(std::string const &path, std::string_view text)
{
	std::ofstream f(path, std::ios::binary);
	f << text;
}

std::string get(std::string const &path)
{
	std::ifstream f(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(f), {}};
}

std::size_t count(std::string_view hay, std::string_view what)
{
	std::size_t n = 0;
	for (std::size_t p = hay.find(what);
	     p != std::string_view::npos;
	     p = hay.find(what, p + what.size()))
		++n;
	return n;
}

} // namespace

int main()
{
	g_rig = fs::temp_directory_path()
	      / ("srtview_vault_test." + std::to_string(getpid()));

	agenda::id const l1 = gid(1), l2 = gid(2), n = gid(3);
	agenda::id const da = gid(4), db = gid(5), f = gid(6);
	auto const tl1 = task(l1, agenda::kind::leaf);
	auto const tl2 = task(l2, agenda::kind::leaf);
	auto const tn = task(n, agenda::kind::node, {l1, l2});
	auto const tda = task(da, agenda::kind::dive, {l1});
	auto const tdb = task(db, agenda::kind::dive, {l2});

	// --- recipes are a hard cache boundary -----------------------
	reset_rig();
	{
		vault::store s(dir(), mix, recipes(20));
		s.content(l1, mix("transcript one"));
		std::string const old = dir() + '/' + l1.hex() + ".txt";
		put(old, "leaf summary");
		check(s.resolve(tl1).empty() && fs::exists(old),
		      "a legacy answer never crosses into a recipe");
		std::string const p = s.place(tl1);
		put(p, "leaf summary");
		check(s.resolve(tl1) == p, "the current recipe resolves");
		vault::store s2(dir(), mix, recipes(20));
		s2.content(l1, mix("transcript one"));
		check(s2.resolve(tl1) == p,
		      "a fresh store resolves the identical name");
		check(s2.resolve(l1) == p,
		      "resolution by bare id follows the registry");
		vault::store s3(dir(), mix, recipes(40));
		s3.content(l1, mix("transcript one"));
		check(s3.resolve(tl1).empty() && fs::exists(p),
		      "same source under a changed recipe is a miss");
		vault::store s4(dir(), mix, recipes(20));
		s4.content(l1, mix("transcript one"));
		check(s4.resolve(tl1) == p,
		      "switching back reuses the preserved recipe");
	}

	// --- external srt edit renames the chain ---------------------
	reset_rig();
	std::string p_l1, p_l2, p_n, p_da, p_db;
	{
		vault::store s(dir(), mix, recipes(20));
		s.content(l1, mix("one"));
		s.content(l2, mix("two"));
		put(p_l1 = s.place(tl1), "sum one");
		put(p_l2 = s.place(tl2), "sum two");
		put(p_n = s.place(tn), "node prose");
		put(p_da = s.place(tda), "dive a");
		put(p_db = s.place(tdb), "dive b");
		check(!p_n.empty()
		      && fs::path(p_n).filename().string().size() == 54,
		      "placed names carry plan, recipe and content");
	}
	std::string q_l1, q_n, q_da;
	{
		vault::store s(dir(), mix, recipes(20));
		s.content(l1, mix("one EDITED"));
		s.content(l2, mix("two"));
		q_n = s.resolve(tn);
		check(!q_n.empty() && q_n != p_n && !fs::exists(p_n)
		      && get(q_n) == "node prose",
		      "an edited transcript renames dependents");
		q_l1 = s.resolve(tl1);
		check(!q_l1.empty() && q_l1 != p_l1 && !fs::exists(p_l1)
		      && get(q_l1) == "sum one",
		      "the leaf adopted en route, bottom-up");
		q_da = s.resolve(tda);
		check(!q_da.empty() && q_da != p_da
		      && get(q_da) == "dive a",
		      "dives over the edited leaf rename too");
		check(s.target(tn) == q_n,
		      "target() follows the current generation");
		check(s.resolve(tdb) == p_db && fs::exists(p_db),
		      "an untouched chain keeps its exact name");
		check(count(get(dir() + "/journal.txt"), ": content")
		      == 3,
		      "three content moves journaled, no more");
	}
	{
		vault::store s(dir(), mix, recipes(20));
		s.content(l1, mix("one EDITED"));
		s.content(l2, mix("two"));
		std::size_t const before =
			count(get(dir() + "/journal.txt"), "moved ");
		bool const hit = s.resolve(tn) == q_n
		              && s.resolve(tl1) == q_l1
		              && s.resolve(tda) == q_da
		              && s.resolve(tdb) == p_db;
		check(hit && count(get(dir() + "/journal.txt"),
		                   "moved ") == before,
		      "a converged cache resolves with zero renames");
	}

	// --- external artifact edit ----------------------------------
	std::string r_n, r_db;
	{
		put(p_l2, "sum two REVISED");
		vault::store s(dir(), mix, recipes(20));
		s.content(l1, mix("one EDITED"));
		s.content(l2, mix("two"));
		check(s.resolve(tl2) == p_l2,
		      "an edited artifact keeps its own name");
		r_n = s.resolve(tn);
		check(!r_n.empty() && r_n != q_n && !fs::exists(q_n)
		      && get(r_n) == "node prose",
		      "its dependents re-key and rename");
		r_db = s.resolve(tdb);
		check(!r_db.empty() && r_db != p_db,
		      "so does the dive over the edited leaf summary");
		check(s.resolve(tda) == q_da,
		      "unrelated chains stay untouched");
	}

	// --- probe/focus pair order ----------------------------------
	std::string p_f;
	{
		vault::store s(dir(), mix, recipes(20));
		s.content(l1, mix("one EDITED"));
		s.content(l2, mix("two"));
		s.resolve(tda);
		s.resolve(tdb);
		put(p_f = s.place(task(f, agenda::kind::focus,
		                       {da, db})), "essay");
		check(!p_f.empty()
		      && s.resolve(task(f, agenda::kind::focus,
		                        {db, da})) == p_f,
		      "a reversed dep pair never re-keys a focus");
	}

	// --- place sweeps the plan id --------------------------------
	{
		vault::store s(dir(), mix, recipes(20));
		s.content(l1, mix("one EDITED"));
		s.content(l2, mix("two"));
		// <plan>.<recipe>, as the store names it: the content
		// part is the trailing ".<16 hex>.txt".
		std::string const prefix = s.target(tn).substr(
			0, s.target(tn).size() - 21);
		std::string const junk1 = prefix + ".txt";
		std::string const junk2 = prefix
		                        + ".feedfacefeedface.txt";
		put(junk1, "junk");
		put(junk2, "junk");
		put(s.tmp(tn), "incoming");
		check(s.target(tn) == r_n && fs::exists(junk1),
		      "target() names without sweeping");
		std::string const target = s.place(tn);
		check(target == r_n && !fs::exists(junk1)
		      && !fs::exists(junk2) && fs::exists(s.tmp(tn))
		      && fs::exists(target),
		      "place keeps the target and its tmp, drops the rest");
		check(count(get(dir() + "/journal.txt"), "dropped ")
		      == 2,
		      "the drops are journaled");
		std::error_code ec;
		fs::rename(s.tmp(tn), target, ec);
		check(!ec && get(target) == "incoming",
		      "the executor's tmp rename lands on the target");
	}

	// --- a recipe folds in what its kind reads -------------------
	{
		vault::recipes r = recipes(20);
		r[std::size_t(agenda::kind::leaf)] = gid(99);
		vault::store s(dir(), mix, r);
		s.content(l1, mix("one EDITED"));
		s.content(l2, mix("two"));
		check(s.resolve(tl1).empty(),
		      "a leaf recipe change misses the leaf");
		put(s.place(tl1), "sum one REWRITTEN");
		put(s.place(tl2), "sum two REWRITTEN");
		check(!s.resolve(tl1).empty() && s.resolve(tn).empty()
		      && s.resolve(tda).empty() && fs::exists(r_n)
		      && fs::exists(q_da),
		      "nodes and dives over regenerated leaves miss "
		      "instead of adopting the old artifacts");
		r = recipes(20);
		r[std::size_t(agenda::kind::node)] = gid(98);
		vault::store s2(dir(), mix, r);
		s2.content(l1, mix("one EDITED"));
		s2.content(l2, mix("two"));
		check(s2.resolve(tl1) == q_l1 && s2.resolve(tn).empty()
		      && s2.resolve(tda) == q_da,
		      "a node recipe change leaves leaves and dives be");
		r = recipes(20);
		r[std::size_t(agenda::kind::probe)] = gid(97);
		vault::store s3(dir(), mix, r);
		s3.content(l1, mix("one EDITED"));
		s3.content(l2, mix("two"));
		check(s3.resolve(tda) == q_da
		      && s3.resolve(task(f, agenda::kind::focus,
		                         {da, db})).empty(),
		      "a probe recipe change misses the focus written over "
		      "its regex, the dives stand");
	}

	// --- the effective recipe is what names the artifacts --------
	{
		vault::store s(dir(), mix, recipes(20));
		check(s.recipe(agenda::kind::leaf)
		        == recipes(20)[std::size_t(agenda::kind::leaf)]
		      && s.recipe(agenda::kind::terms)
		        == recipes(20)[std::size_t(agenda::kind::terms)]
		      && s.recipe(agenda::kind::node)
		        != recipes(20)[std::size_t(agenda::kind::node)],
		      "recipe() reports the supplied recipe, folds applied");
	}

	// --- content-keyed kinds omit only the content suffix --------
	{
		vault::store s(dir(), mix, recipes(20));
		agenda::id const tm = gid(7);
		auto const tt = task(tm, agenda::kind::terms);
		check(s.resolve(tt).empty(), "a missing terms reply misses");
		std::string const flat = s.target(tt);
		put(flat, "TERM: x");
		check(s.resolve(tt) == flat && s.place(tt) == flat,
		      "terms resolve by plan and recipe");
	}

	// --- locate ---------------------------------------------------
	{
		// Planted before the store looks: a file appearing behind
		// a live store's back is the documented next-load case.
		agenda::id const lg = gid(8);
		std::string const legacy = dir() + "/dives/" + lg.hex()
		                         + ".txt";
		put(legacy, "old dive");
		vault::store s(dir(), mix, recipes(20));
		check(s.locate(da, agenda::kind::dive) == q_da,
		      "locate finds the current recipe by prefix");
		check(s.locate(gid(99), agenda::kind::dive).empty(),
		      "locate misses politely");
		check(s.locate(lg, agenda::kind::dive).empty()
		      && fs::exists(legacy),
		      "locate never mistakes legacy data for this recipe");
	}

	// --- the shelf: lookups stop re-reading directories ----------
	{
		vault::store s(dir(), mix, recipes(20));
		s.content(l1, mix("one EDITED"));
		s.content(l2, mix("two"));
		std::size_t const s0 = s.scans();
		s.resolve(tn);
		s.resolve(tda);
		s.resolve(tdb);
		check(s.scans() == s0,
		      "clean resolves never touch readdir");
		for (int i = 0; i < 8; ++i)
			s.locate(da, agenda::kind::dive);
		s.locate(db, agenda::kind::dive);
		check(s.scans() == s0 + 1,
		      "lookups share one shelf per directory");
		s.place(tda);
		s.locate(da, agenda::kind::dive);
		check(s.scans() == s0 + 2,
		      "a placement re-reads exactly its directory");
	}

	// --- a placement without its rename stays a miss -------------
	{
		vault::store s(dir(), mix, recipes(20));
		s.content(l1, mix("one EDITED"));
		s.content(l2, mix("two"));
		auto const td9 = task(gid(11), agenda::kind::dive, {l2});
		check(!s.place(td9).empty() && s.resolve(td9).empty(),
		      "no phantom hit before the executor's rename lands");
	}

	// --- completion reads current state, never re-registers ------
	{
		vault::store s(dir(), mix, recipes(20));
		s.content(l1, mix("one EDITED"));
		s.content(l2, mix("two"));
		auto const old = task(gid(12), agenda::kind::dive, {l1});
		std::string const want = s.target(old);
		// The corpus reloads: same plan id, grown dep set.
		s.resolve(task(gid(12), agenda::kind::dive, {l1, l2}));
		check(!want.empty() && s.target(gid(12)) != want,
		      "id-keyed target follows the reloaded shape");
		check(s.place(gid(12)) == s.target(gid(12)),
		      "id-keyed placement names the current generation");
	}

	// --- incomputable chains miss --------------------------------
	{
		vault::store s(dir(), mix, recipes(20));
		check(s.resolve(task(gid(9),
		                     agenda::kind::leaf)).empty(),
		      "a leaf without a witness cannot resolve");
		check(s.resolve(task(gid(10), agenda::kind::node,
		                     {gid(9)})).empty(),
		      "a chain over it cannot either");
		s.content(l1, mix("one EDITED"));
		fs::remove(q_l1);
		check(s.resolve(tda).empty(),
		      "a missing dep artifact blocks its dependents");
	}

	fs::remove_all(g_rig);
	std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED",
	            g_fail, g_fail == 1 ? "" : "s");
	return g_fail ? 1 : 0;
}
