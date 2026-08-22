// Unit tests for the agenda scheduler: binary id round-trips,
// pyramid shapes and determinism, dependency gating, cache-hit
// satisfaction, scoring bands (kind, tier, export edge), heat and
// decay, transitive priority inheritance with own-score tie-breaks,
// failure parking, and plan lifecycle.
#include <cstdio>
#include <vector>

#include "agenda.hpp"

using V = std::vector<agenda::id>;

static int g_fail;

static void check(bool ok, char const *what)
{
	std::printf("%s  %s\n", ok ? "OK  " : "FAIL", what);
	if (!ok)
		++g_fail;
}

// Readable id literal: the tag's bytes, zero-padded.
static agenda::id tid(char const *tag)
{
	agenda::id out;
	for (std::size_t i = 0; tag[i] && i < out.b.size(); ++i)
		out.b[i] = std::uint8_t(tag[i]);
	return out;
}

// Deterministic, order-sensitive fake combiner standing in for the
// app's BLAKE2b: mixes child bytes position-dependently.
static agenda::id glue(std::vector<agenda::id> const &kids)
{
	agenda::id out = tid("node");
	std::size_t at = 0;
	for (agenda::id const &k : kids)
		for (std::uint8_t const x : k.b) {
			out.b[at % out.b.size()] ^=
				std::uint8_t(x + 89 * (at + 13));
			++at;
		}
	return out;
}

static agenda::task const *by_id(std::vector<agenda::task> const &v,
                                 agenda::id id)
{
	for (agenda::task const &t : v)
		if (t.id == id)
			return &t;
	return nullptr;
}

static void test_id()
{
	agenda::id const k = agenda::id::from_hex("992ef934e71c5c62");
	check(bool(k) && k.hex() == "992ef934e71c5c62",
	      "hex round-trips through the binary id");
	check(agenda::id::from_hex("00000000000000ff").hex()
	      == "00000000000000ff", "leading zeros survive");
	check(!agenda::id::from_hex("992ef934e71c5c6"),
	      "short hex is no id");
	check(!agenda::id::from_hex("992ef934e71c5c6x"),
	      "malformed hex is no id");
	check(!agenda::id{}, "the zero id is falsy");
}

static void test_index()
{
	agenda::index ix;
	check(ix.find(tid("a")) == agenda::index::npos,
	      "an empty index finds nothing");
	// Past several doublings, every key still lands: growth
	// rehashes, probing skips over neighbours, and tags that
	// share low bits collide on purpose.
	for (unsigned i = 1; i <= 300; ++i)
		ix.add(agenda::id{{std::uint8_t(i), std::uint8_t(i >> 8),
		                   1, 0, 0, 0, 0, 0}}, i);
	bool all = true;
	for (unsigned i = 1; i <= 300; ++i)
		all &= ix.find(agenda::id{{std::uint8_t(i),
		                           std::uint8_t(i >> 8), 1, 0, 0,
		                           0, 0, 0}}) == i;
	check(all, "every key survives growth and probing");
	ix.add(tid("a"), 7);
	ix.add(tid("a"), 9);
	check(ix.find(tid("a")) == 9, "adding a known key replaces its value");
	ix.add(agenda::id{}, 3);
	check(ix.find(agenda::id{}) == agenda::index::npos,
	      "the zero id is no key");
	ix.clear();
	check(ix.find(tid("a")) == agenda::index::npos, "clear forgets");
}

static void test_pyramid_shapes()
{
	agenda::id const a = tid("a"), b = tid("b"), c = tid("c"),
	                 d = tid("d"), e = tid("e");
	check(agenda::pyramid({}, glue).empty()
	      && agenda::pyramid({a}, glue).empty(),
	      "no nodes below two leaves");

	auto two = agenda::pyramid({a, b}, glue);
	agenda::id const ab = glue({a, b});
	check(two.size() == 1 && two[0].id == ab
	      && two[0].deps == V{a, b} && two[0].keys == V{a, b}
	      && two[0].what == agenda::kind::node && two[0].tier == 1,
	      "two leaves make one tier-1 node");

	auto three = agenda::pyramid({a, b, c}, glue);
	auto const *r3 = by_id(three, glue({ab, c}));
	check(three.size() == 2 && by_id(three, ab) && r3
	      && r3->deps == V{ab, c} && r3->keys == V{a, b, c}
	      && r3->tier == 2,
	      "odd tail carries up into the next level");

	auto five = agenda::pyramid({a, b, c, d, e}, glue);
	agenda::id const abcd = glue({ab, glue({c, d})});
	auto const *r5 = by_id(five, glue({abcd, e}));
	check(five.size() == 4 && r5 && r5->tier == 3
	      && r5->keys == V{a, b, c, d, e},
	      "five leaves: four nodes, root at tier 3, all keys");

	check(agenda::pyramid({a, b, c, d, e}, glue) == five,
	      "pyramid is deterministic");
}

static void test_gating()
{
	agenda::id const a = tid("a"), b = tid("b"), c = tid("c");
	agenda::id const ab = glue({a, b});
	agenda::plan p;
	for (auto const &t : agenda::pyramid({a, b, c}, glue))
		p.add(t);
	p.add({.id = a});
	p.add({.id = b});
	check(p.take() == a && p.take() == b && !p.take(),
	      "leaves run first; nodes blocked while children pend");
	p.done(a);
	check(!p.take(), "one done child is not enough");
	p.done(b);
	check(p.take() == ab, "both children done unblocks node");
	check(!p.take(), "root blocked: leaf c never added");
	p.done(ab);
	p.done(c);                       // cache hit, task never added
	check(p.take() == glue({ab, c}),
	      "done-before-add satisfies a dependency");
}

static void test_states()
{
	agenda::plan p;
	p.add({.id = tid("x")});
	check(p.status(tid("x")) == agenda::plan::state::pending
	      && p.status(tid("?")) == agenda::plan::state::unknown,
	      "status reports pending and unknown");
	check(p.take() == tid("x")
	      && p.status(tid("x")) == agenda::plan::state::running
	      && !p.take(),
	      "a running task is not handed out again");
	p.done(tid("x"));
	p.add({.id = tid("x")});
	check(p.status(tid("x")) == agenda::plan::state::done,
	      "re-adding a done id leaves it done");
	check(p.backlog() == 0, "done tasks leave the backlog");

	agenda::task a{.id = tid("t")};
	agenda::task b{.id = tid("t")};
	b.note = "journal";
	check(a == b, "note is journal text, not identity");
}

static void test_order_bands()
{
	agenda::plan p;
	p.add({.id = tid("a1"), .what = agenda::kind::answer});
	p.add({.id = tid("p1"), .what = agenda::kind::probe});
	p.add({.id = tid("f1"), .what = agenda::kind::focus});
	p.add({.id = tid("d1"), .what = agenda::kind::dive});
	p.add({.id = tid("j1"), .what = agenda::kind::judge});
	p.add({.id = tid("t1"), .what = agenda::kind::terms});
	p.add({.id = tid("e1"), .what = agenda::kind::extract});
	p.add({.id = tid("n1"), .what = agenda::kind::node, .tier = 2});
	p.add({.id = tid("n2"), .what = agenda::kind::node, .tier = 1});
	p.add({.id = tid("l1"), .what = agenda::kind::leaf});
	p.add({.id = tid("l2"), .what = agenda::kind::leaf});
	check(p.take() == tid("a1"),
	      "an interactive grounded answer jumps the queue");
	check(p.take() == tid("l1") && p.take() == tid("l2"),
	      "leaves before nodes, stable by insertion");
	check(p.take() == tid("n2") && p.take() == tid("n1"),
	      "lower tier first within a kind");
	check(p.take() == tid("e1"),
	      "semantic extraction follows the summary backbone");
	check(p.take() == tid("t1"),
	      "terms between summaries and dives: the index backbone");
	check(p.take() == tid("d1"), "dives after nodes");
	check(p.take() == tid("j1"),
	      "bounded consolidation follows evidence gathering");
	check(p.take() == tid("f1"), "focuses after dives");
	check(p.take() == tid("p1"),
	      "probes last: gathered evidence writes before new "
	      "inquiries open");
}

static void test_export_edge()
{
	agenda::plan p;
	p.add({.id = tid("hid"), .what = agenda::kind::dive,
	       .exported = false});
	p.add({.id = tid("pub"), .what = agenda::kind::dive});
	check(p.take() == tid("pub"),
	      "exported dive outranks the supportive one");
}

static void test_heat()
{
	agenda::plan p;
	p.add({.id = tid("l1"), .keys = {tid("v1")}});
	p.add({.id = tid("l2"), .keys = {tid("v2")}});
	p.add({.id = tid("l3"), .keys = {tid("v3")}});
	p.heat(tid("v3"), 2.0);
	check(p.take() == tid("l3"),
	      "heat lifts a later leaf to the front");
	p.heat(tid("v2"), 1.0);
	p.decay(1e-7);                   // sweeps every entry cold
	check(p.take() == tid("l1"), "decay restores insertion order");
}

static void test_blend()
{
	agenda::plan p;
	p.add({.id = tid("l1"), .keys = {tid("v1")}});
	p.add({.id = tid("l2"), .keys = {tid("v2")}});
	p.heat(tid("v1"), 2.0);
	p.decay(0.5);                    // pattern change fades interest
	p.heat(tid("v2"), 1.5);
	check(p.take() == tid("l2"), "fresh interest outranks faded");
	check(p.take() == tid("l1"), "faded interest still counts");
}

static void test_inheritance()
{
	agenda::plan p;
	// Chain: hot dive -> node -> leaf "deep"; keys are disjoint so
	// only inheritance can move the leaf.
	p.add({.id = tid("deep"), .what = agenda::kind::leaf});
	p.add({.id = tid("mid"), .deps = {tid("deep")},
	       .what = agenda::kind::node, .tier = 1});
	p.add({.id = tid("dive"), .deps = {tid("mid")},
	       .keys = {tid("hot")}, .what = agenda::kind::dive});
	p.add({.id = tid("plain"), .what = agenda::kind::leaf});
	p.heat(tid("hot"), 10.0);
	check(p.take() == tid("deep"),
	      "a hot blocked dive pulls its deepest missing leaf");
	p.done(tid("deep"));
	check(p.take() == tid("mid"),
	      "inheritance walks the chain as it unblocks");
	p.done(tid("mid"));
	check(p.take() == tid("dive") && p.take() == tid("plain"),
	      "the hot task itself runs once ready");
}

// Regression, found live: the pyramid root sums every leaf's heat,
// outscores each single leaf, and its lift then flattened all
// leaves to one effective score -- insertion order, heat erased.
// Own-score tie-breaking keeps the within-class order.
static void test_aggregate_feedback()
{
	agenda::id const v1 = tid("v1"), v2 = tid("v2"),
	                 v3 = tid("v3"), v4 = tid("v4");
	agenda::id const n12 = glue({v1, v2});
	agenda::id const n34 = glue({v3, v4});
	agenda::plan p;
	for (agenda::id const v : {v1, v2, v3, v4})
		p.add({.id = v, .keys = {v}});
	for (auto const &t : agenda::pyramid({v1, v2, v3, v4}, glue))
		p.add(t);
	check(p.take() == v1, "first leaf starts cold");
	p.heat(v1, 0.5);
	p.decay(0.5);                    // the fixture's exact feed
	p.heat(v1, 0.222);
	p.heat(v3, 0.889);
	p.heat(v4, 0.889);
	p.done(v1);
	check(p.take() == v3 && p.take() == v4,
	      "hot leaves precede the cold one despite the root's lift");
	p.done(v3);
	p.done(v4);
	check(p.take() == n34,
	      "the hot subtree's node outranks the cold leaf");
	p.done(n34);
	check(p.take() == v2, "the cold leaf runs when its turn comes");
	p.done(v2);
	check(p.take() == n12, "cold node follows");
	p.done(n12);
	check(p.take() == glue({n12, n34}), "root closes the corpus");
}

static void test_parking()
{
	agenda::id const a = tid("a"), b = tid("b");
	agenda::plan p;
	for (auto const &t : agenda::pyramid({a, b}, glue))
		p.add(t);
	p.add({.id = a});
	p.add({.id = b});
	check(p.take() == a, "leaf a taken");
	p.fail(a);
	check(p.status(a) == agenda::plan::state::parked,
	      "failed task parks");
	check(p.take() == b, "the rest keeps flowing");
	p.done(b);
	check(!p.take() && p.backlog() == 1,
	      "a parked child blocks its parent for the session");

	p.fail(tid("ghost"));            // e.g. reported after a reset
	p.add({.id = tid("ghost")});
	check(p.status(tid("ghost")) == agenda::plan::state::parked
	      && !p.take(),
	      "a failure reported before add stays parked");
}

static void test_reset()
{
	agenda::plan p;
	p.add({.id = tid("a")});
	p.add({.id = tid("b"), .keys = {tid("v")}});
	p.heat(tid("v"), 5.0);
	p.done(tid("a"));
	p.reset();
	check(p.status(tid("a")) == agenda::plan::state::unknown,
	      "reset clears done ids: resolution re-derives the cache");
	check(p.status(tid("b")) == agenda::plan::state::unknown
	      && p.backlog() == 0,
	      "reset drops pending tasks");
	p.add({.id = tid("c"), .keys = {tid("v")}});
	p.add({.id = tid("d"), .keys = {tid("w")}});
	p.heat(tid("w"), 1.0);
	check(p.take() == tid("d"), "reset dropped the old heat too");
}

int main()
{
	test_id();
	test_index();
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
