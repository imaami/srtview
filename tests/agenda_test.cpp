// Unit tests for the agenda scheduler: pyramid shapes and
// determinism, dependency gating, cache-hit satisfaction, scoring
// bands (kind, tier, export edge), heat and decay, transitive
// priority inheritance, failure parking, and plan lifecycle.
#include <cstdio>
#include <string>
#include <vector>

#include "agenda.hpp"

static int g_fail;

static void check(bool ok, char const *what)
{
	std::printf("%s  %s\n", ok ? "OK  " : "FAIL", what);
	if (!ok)
		++g_fail;
}

// Readable fake combiner: parent of a and b is "(a+b)".
static std::string glue(std::vector<std::string> const &kids)
{
	std::string s = "(";
	for (std::size_t i = 0; i < kids.size(); ++i)
		s += (i ? "+" : "") + kids[i];
	return s + ")";
}

static agenda::task const *by_id(std::vector<agenda::task> const &v,
                                 std::string const &id)
{
	for (agenda::task const &t : v)
		if (t.id == id)
			return &t;
	return nullptr;
}

static void test_pyramid_shapes()
{
	check(agenda::pyramid({}, glue).empty()
	      && agenda::pyramid({"a"}, glue).empty(),
	      "no nodes below two leaves");

	auto two = agenda::pyramid({"a", "b"}, glue);
	check(two.size() == 1 && two[0].id == "(a+b)"
	      && two[0].deps == std::vector<std::string>{"a", "b"}
	      && two[0].keys == std::vector<std::string>{"a", "b"}
	      && two[0].what == agenda::kind::node && two[0].tier == 1,
	      "two leaves make one tier-1 node");

	auto three = agenda::pyramid({"a", "b", "c"}, glue);
	auto const *r3 = by_id(three, "((a+b)+c)");
	check(three.size() == 2 && by_id(three, "(a+b)") && r3
	      && r3->deps == std::vector<std::string>{"(a+b)", "c"}
	      && r3->keys == std::vector<std::string>{"a", "b", "c"}
	      && r3->tier == 2,
	      "odd tail carries up into the next level");

	auto five = agenda::pyramid({"a", "b", "c", "d", "e"}, glue);
	auto const *r5 = by_id(five, "(((a+b)+(c+d))+e)");
	check(five.size() == 4 && r5 && r5->tier == 3
	      && r5->keys == std::vector<std::string>{"a", "b", "c",
	                                              "d", "e"},
	      "five leaves: four nodes, root at tier 3, all keys");

	check(agenda::pyramid({"a", "b", "c", "d", "e"}, glue) == five,
	      "pyramid is deterministic");
}

static void test_gating()
{
	agenda::plan p;
	for (auto const &t : agenda::pyramid({"a", "b", "c"}, glue))
		p.add(t);
	p.add({.id = "a"});
	p.add({.id = "b"});
	check(p.take() == "a" && p.take() == "b" && p.take().empty(),
	      "leaves run first; nodes blocked while children pend");
	p.done("a");
	check(p.take().empty(), "one done child is not enough");
	p.done("b");
	check(p.take() == "(a+b)", "both children done unblocks node");
	check(p.take().empty(), "root blocked: leaf c never added");
	p.done("(a+b)");
	p.done("c");                     // cache hit, task never added
	check(p.take() == "((a+b)+c)",
	      "done-before-add satisfies a dependency");
}

static void test_states()
{
	agenda::plan p;
	p.add({.id = "x"});
	check(p.status("x") == agenda::plan::state::pending
	      && p.status("?") == agenda::plan::state::unknown,
	      "status reports pending and unknown");
	check(p.take() == "x"
	      && p.status("x") == agenda::plan::state::running
	      && p.take().empty(),
	      "a running task is not handed out again");
	p.done("x");
	p.add({.id = "x"});
	check(p.status("x") == agenda::plan::state::done,
	      "re-adding a done id leaves it done");
	check(p.backlog() == 0, "done tasks leave the backlog");
}

static void test_order_bands()
{
	agenda::plan p;
	p.add({.id = "d1", .what = agenda::kind::dive});
	p.add({.id = "n1", .what = agenda::kind::node, .tier = 2});
	p.add({.id = "n2", .what = agenda::kind::node, .tier = 1});
	p.add({.id = "l1", .what = agenda::kind::leaf});
	p.add({.id = "l2", .what = agenda::kind::leaf});
	check(p.take() == "l1" && p.take() == "l2",
	      "leaves before nodes, stable by insertion");
	check(p.take() == "n2" && p.take() == "n1",
	      "lower tier first within a kind");
	check(p.take() == "d1", "dives last, all else equal");
}

static void test_export_edge()
{
	agenda::plan p;
	p.add({.id = "hidden", .what = agenda::kind::dive,
	       .exported = false});
	p.add({.id = "public", .what = agenda::kind::dive});
	check(p.take() == "public",
	      "exported dive outranks the supportive one");
}

static void test_heat()
{
	agenda::plan p;
	p.add({.id = "l1", .keys = {"v1"}});
	p.add({.id = "l2", .keys = {"v2"}});
	p.add({.id = "l3", .keys = {"v3"}});
	p.heat("v3", 2.0);
	check(p.take() == "l3", "heat lifts a later leaf to the front");
	p.heat("v2", 1.0);
	p.decay(1e-7);                   // sweeps every entry cold
	check(p.take() == "l1", "decay restores insertion order");
}

static void test_blend()
{
	agenda::plan p;
	p.add({.id = "l1", .keys = {"v1"}});
	p.add({.id = "l2", .keys = {"v2"}});
	p.heat("v1", 2.0);
	p.decay(0.5);                    // pattern change fades interest
	p.heat("v2", 1.5);
	check(p.take() == "l2", "fresh interest outranks faded");
	check(p.take() == "l1", "faded interest still counts");
}

static void test_inheritance()
{
	agenda::plan p;
	// Chain: hot dive -> node -> leaf "deep"; keys are disjoint so
	// only inheritance can move the leaf.
	p.add({.id = "deep", .what = agenda::kind::leaf});
	p.add({.id = "mid", .deps = {"deep"},
	       .what = agenda::kind::node, .tier = 1});
	p.add({.id = "dive", .deps = {"mid"}, .keys = {"hot"},
	       .what = agenda::kind::dive});
	p.add({.id = "plain", .what = agenda::kind::leaf});
	p.heat("hot", 10.0);
	check(p.take() == "deep",
	      "a hot blocked dive pulls its deepest missing leaf");
	p.done("deep");
	check(p.take() == "mid",
	      "inheritance walks the chain as it unblocks");
	p.done("mid");
	check(p.take() == "dive" && p.take() == "plain",
	      "the hot task itself runs once ready");
}

// Regression, found live: the pyramid root sums every leaf's heat,
// outscores each single leaf, and its lift then flattened all
// leaves to one effective score -- insertion order, heat erased.
// Own-score tie-breaking keeps the within-class order.
static void test_aggregate_feedback()
{
	agenda::plan p;
	char const *leaves[] = {"v1", "v2", "v3", "v4"};
	for (char const *l : leaves)
		p.add({.id = l, .keys = {l}});
	for (auto const &t : agenda::pyramid({"v1", "v2", "v3", "v4"},
	                                     glue))
		p.add(t);
	check(p.take() == "v1", "first leaf starts cold");
	p.heat("v1", 0.5);
	p.decay(0.5);                    // the fixture's exact feed
	p.heat("v1", 0.222);
	p.heat("v3", 0.889);
	p.heat("v4", 0.889);
	p.done("v1");
	check(p.take() == "v3" && p.take() == "v4",
	      "hot leaves precede the cold one despite the root's lift");
	p.done("v3");
	p.done("v4");
	check(p.take() == "(v3+v4)",
	      "the hot subtree's node outranks the cold leaf");
	p.done("(v3+v4)");
	check(p.take() == "v2", "the cold leaf runs when its turn comes");
	p.done("v2");
	check(p.take() == "(v1+v2)", "cold node follows");
	p.done("(v1+v2)");
	check(p.take() == "((v1+v2)+(v3+v4))", "root closes the corpus");
}

static void test_parking()
{
	agenda::plan p;
	for (auto const &t : agenda::pyramid({"a", "b"}, glue))
		p.add(t);
	p.add({.id = "a"});
	p.add({.id = "b"});
	check(p.take() == "a", "leaf a taken");
	p.fail("a");
	check(p.status("a") == agenda::plan::state::parked,
	      "failed task parks");
	check(p.take() == "b", "the rest keeps flowing");
	p.done("b");
	check(p.take().empty() && p.backlog() == 1,
	      "a parked child blocks its parent for the session");
}

static void test_reset()
{
	agenda::plan p;
	p.add({.id = "a"});
	p.add({.id = "b", .keys = {"v"}});
	p.heat("v", 5.0);
	p.done("a");
	p.reset();
	check(p.status("a") == agenda::plan::state::done,
	      "reset keeps done ids: cache files outlive plans");
	check(p.status("b") == agenda::plan::state::unknown
	      && p.backlog() == 0,
	      "reset drops pending tasks");
	p.add({.id = "c", .keys = {"v"}});
	p.add({.id = "d", .keys = {"w"}});
	p.heat("w", 1.0);
	check(p.take() == "d", "reset dropped the old heat too");
}

int main()
{
	test_pyramid_shapes();
	test_gating();
	test_states();
	test_order_bands();
	test_export_edge();
	test_heat();
	test_blend();
	test_inheritance();
	test_aggregate_feedback();
	test_parking();
	test_reset();
	std::printf("%s\n", g_fail ? "FAILURES" : "all passed");
	return g_fail ? 1 : 0;
}
