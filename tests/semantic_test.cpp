// semantic_test.cpp -- strict extraction, evidence and catalog tests.
// Standard C++; no Qt or model server.
#include "semantic.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

int g_fail = 0;

void check(bool ok, char const *what)
{
	std::printf("%s  %s\n", ok ? "OK  " : "FAIL", what);
	if (!ok)
		++g_fail;
}

// A failed precondition ends the run: the checks behind it would
// index past the end instead of diagnosing.
void require(bool ok, char const *what)
{
	check(ok, what);
	if (!ok) {
		std::printf("FAILED (precondition)\n");
		std::exit(1);
	}
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

std::vector<semantic::cue> const kCues{
	{4, 10.0, 12.0, "Generate the deployment package."},
	{5, 12.0, 15.0, "Upload it through Relay."},
	{6, 15.0, 21.0, "Check the upload log."},
	{7, 21.0, 25.0, "Restart the target and verify status."},
};

semantic::window window()
{
	return {"video-identity", "/lecture/deploy.mp4", kCues, {}};
}

semantic::record parse_one(std::string_view statement,
	                       std::uint32_t cue,
	                       std::string_view subject = "deployment")
{
	std::string json =
		"{\"records\":[{\"kind\":\"procedure_step\","
		"\"subject\":\"";
	json += subject;
	json += "\",\"relation\":\"involves\",\"object\":\"";
	json += statement;
	json += "\",\"statement\":\"";
	json += statement;
	json += "\",\"cues\":[" + std::to_string(cue) + "]}]}";
	auto out = semantic::parse_records(json, window(), mix);
	return out.records.empty() ? semantic::record{}
	                           : std::move(out.records.front());
}

} // namespace

int main()
{
	semantic::window const w = window();
	std::string const good =
		R"({"records":[{"kind":"procedure_step","subject":"deployment","relation":"consists of","object":"generating, uploading and verifying","statement":"Generate and upload the package, then verify the restarted target.","cues":[7,4,5]},{"kind":"definition","subject":"Relay","relation":"is","object":"the package upload path","statement":"Relay is the package upload path.","cues":[5]}]})";
	auto a = semantic::parse_records(good, w, mix);
	require(a && a.rejected == 0 && a.records.size() == 2,
	        "valid atomic records parse");
	check(a.records[0].evidence.size() == 2
	      && a.records[0].evidence[0].first == 4
	      && a.records[0].evidence[0].last == 5
	      && a.records[0].evidence[0].quote
	         == "Generate the deployment package.\n"
	            "Upload it through Relay."
	      && a.records[0].evidence[1].first == 7,
	      "citations sort and consecutive cues collapse");
	check(a.records[0].evidence[0].source == "video-identity"
	      && a.records[0].evidence[0].start == 10.0
	      && a.records[0].evidence[0].end == 15.0,
	      "evidence comes from transcript metadata");

	std::string const mixed =
		R"({"records":[{"kind":"claim","subject":"target","relation":"restarts","object":"itself","statement":"The target restarts.","cues":[7]},{"kind":"claim","subject":"invented","relation":"has","object":"no source","statement":"This has no source.","cues":[9]}]})";
	auto b = semantic::parse_records(mixed, w, mix);
	check(b && b.records.size() == 1 && b.rejected == 1,
	      "an invented cue rejects only its record");
	auto c = semantic::parse_records(
		R"({"records":[{"kind":"claim","subject":"x","relation":"r","object":"o","statement":"y","cues":[4],"surprise":true}]})",
		w, mix);
	check(c && c.records.empty() && c.rejected == 1,
	      "record schema is closed");
	auto d = semantic::parse_records("not json", w, mix);
	check(!d && !d.error.empty(), "malformed document rejects the batch");
	auto const again = semantic::parse_records(good, w, mix);
	require(again.records.size() == 2, "the same document parses again");
	check(again.records[0].id == a.records[0].id,
	      "record identity is deterministic");

	semantic::verdict v;
	check(semantic::parse_verdict(
		R"({"relation":"same","rationale":"Equivalent assertion."})", v)
	      && v.what == semantic::relation::same,
	      "closed consolidation verdict parses");
	check(!semantic::parse_verdict(
		R"({"relation":"same","rationale":"x","extra":0})", v),
	      "verdict rejects extra policy");
	std::string wide;
	for (int i = 0; i < 4096; ++i)
		wide += "\xc3\xa4";
	check(semantic::parse_verdict(
		R"({"relation":"same","rationale":")" + wide + "\"}", v)
	      && v.rationale.size() == 2 * 4096,
	      "the schema's character cap fits the parser's byte cap");
	semantic::answer answer;
	check(semantic::parse_answer(
		R"({"answer":"Restart the target.","citations":[{"source":"0123456789abcdef","first":7,"last":7}],"insufficient":false})",
		answer) && answer.citations.size() == 1,
	      "grounded answer schema parses");
	check(semantic::parse_answer(
		R"({"answer":"Restart the target.","citations":[{"source":"0123456789abcdef","first":7,"last":7},{"source":"0123456789abcdef","first":7,"last":7},{"source":"0123456789abcdef","first":7,"last":8}],"insufficient":false})",
		answer) && answer.citations.size() == 2,
	      "a span cited twice is cited once");
	check(!semantic::parse_answer(
		R"({"answer":"Restart the target.","citations":[{"source":"RECORD 0123456789abcdef","first":7,"last":7}],"insufficient":false})",
		answer), "a citation source is a sixteen-hex identity");
	check(!semantic::parse_answer(
		R"({"answer":"Uncited.","citations":[],"insufficient":false})",
		answer), "a claimed answer requires a citation");
	check(!semantic::parse_answer(
		R"({"answer":"  ","citations":[],"insufficient":true})",
		answer), "an insufficient answer still says what is missing");
	check(!semantic::parse_answer(
		R"({"answer":"Both.","citations":[{"source":"0123456789abcdef","first":7,"last":7}],"insufficient":true})",
		answer), "an insufficient answer cites nothing");
	check(semantic::well_formed(good) && semantic::well_formed("[]")
	      && !semantic::well_formed(good.substr(0, good.size() - 3))
	      && !semantic::well_formed("")
	      && !semantic::well_formed("[" + std::string(1 << 20, ' ')
	                                + "]"),
	      "a cut-off or oversized document is not well formed");
	check(semantic::records_schema().starts_with("{\"type\"")
	      && semantic::verdict_schema().find("contradicts")
	         != std::string_view::npos
	      && semantic::answer_schema().find("insufficient")
	         != std::string_view::npos,
	      "llama-server schemas are raw JSON");

	std::vector<int> best;
	auto const less = [](int a, int b) { return a < b; };
	for (int const x : {5, 3, 9, 1, 7, 3, 8})
		semantic::keep_best(best, x, 3, less);
	check(best == std::vector<int>{1, 3, 3}, "keep_best holds the "
	      "best few in order, ties in arrival order");
	semantic::keep_best(best, 0, 0, less);
	check(best.size() == 3, "a zero limit keeps nothing new");

	semantic::vocab lex;
	auto const wa = lex.words("Ghidra decompiles the binary; ÄMPÄRI!");
	auto const wb = lex.words("the ghidra ämpäri");
	check(wa.size() == 5 && wb.size() == 3 && lex.overlap(wa, wb) == 3.0,
	      "words fold case in ASCII and Latin-1 and count alike "
	      "without corpus statistics");
	check(lex.overlap(lex.words("\u0391\u0398\u0397\u039d\u0391 "
	                            "\u041c\u041e\u0421\u041a\u0412\u0410 "
	                            "\u0160AKKI \u1e9e"),
	                  lex.words("\u03b1\u03b8\u03b7\u03bd\u03b1 "
	                            "\u043c\u043e\u0441\u043a\u0432\u0430 "
	                            "\u0161akki \u00df")) == 4.0,
	      "Greek, Cyrillic and Latin Extended capitals fold too, "
	      "one code point to one");
	auto const wc = lex.words("alpha\u2014beta, ghidra\u2019s "
	                          "x\u00a0y \u300c\u6771\u4eac\u300d "
	                          "\u0645\u0631\u062d\u0628\u0627\u060c"
	                          "\u0627\u0644\u0639\u0627\u0644\u0645 "
	                          "\uff08\u65e5\u672c\uff0c\u8a9e\uff09");
	check(wc.size() == 13 && lex.overlap(wc, lex.words("beta")) == 1.0
	      && lex.overlap(wc, lex.words("ghidra")) == 1.0
	      && lex.overlap(wc, lex.words("\u6771\u4eac")) == 2.0
	      && lex.overlap(wc, lex.words("\u0645\u0631\u062d\u0628\u0627"))
	         == 1.0
	      && lex.overlap(wc, lex.words("\u65e5\u672c")) == 2.0,
	      "every script's punctuation and spaces bound words");
	// Han, kana and Thai write no spaces: each character is a word,
	// a Thai vowel or tone mark stays on its consonant, and a run
	// ends where a spaced script begins.
	auto const wd = lex.words("\u6771\u4eac\u30bf\u30ef\u30fctower "
	                          "\u0e20\u0e32\u0e29\u0e32\u0e44\u0e17\u0e22 "
	                          "\u0e1c\u0e39\u0e49 \ud55c\uad6d\uc5b4");
	check(wd.size() == 14
	      && lex.overlap(wd, lex.words("\u6771\u4eac\u3078")) == 2.0
	      && lex.overlap(wd, lex.words("tower")) == 1.0
	      && lex.overlap(wd, lex.words("\u0e44\u0e17\u0e22")) == 3.0
	      && lex.overlap(wd, lex.words("\u0e17")) == 1.0
	      && lex.overlap(wd, lex.words("\u0e1c\u0e39\u0e49")) == 1.0
	      && lex.overlap(wd, lex.words("\u0e1c")) == 0.0
	      && lex.overlap(wd, lex.words("\ud55c\uad6d\uc5b4")) == 1.0
	      && lex.overlap(wd, lex.words("\ud55c\uad6d")) == 0.0,
	      "unspaced scripts split by character, marks attached, "
	      "spaced ones by run");
	for (int i = 0; i < 9; ++i)
		lex.count(lex.words("the the the"));
	lex.count(wa);
	double const rare = 1.0 + std::log(11.0 / 2.0);
	check(std::abs(lex.overlap(wa, wb) - (1.0 + 2.0 * rare)) < 1e-9,
	      "a word in every document weighs a bare one, a rare word "
	      "its frequency");

	fs::path const rig = fs::temp_directory_path()
	                   / ("srtview_semantic_test."
	                      + std::to_string(getpid()));
	fs::remove_all(rig);
	semantic::vocab words;
	semantic::catalog cat(rig.string(), mix, words,
	                      {"video-identity", "second-video"});
	semantic::record r1 = parse_one("Upload the deployment package.", 5);
	semantic::record r2 = parse_one("Restart the target and verify status.", 7);
	semantic::record r3 = parse_one("A completely unrelated subject.",
	                                4, "astronomy");
	require(r1.id && r2.id && r3.id && r1.evidence.size() == 1,
	        "the fixture records parse");
	semantic::window other = window();
	other.source = "second-video";
	other.title = "/lecture/deploy-continued.mp4";
	auto same = semantic::parse_records(
		R"({"records":[{"kind":"procedure_step","subject":"deployment","relation":"involves","object":"Upload the deployment package.","statement":"Upload the deployment package.","cues":[5]}]})",
		other, mix);
	require(same.records.size() == 1, "a second source parses");
	semantic::record r4 = same.records.front();
	semantic::record corrupt = r1;
	corrupt.id = mix("wrong record identity");
	check(!cat.put(corrupt), "catalog rejects a forged record identity");
	check(cat.put(r1) && cat.put(r2) && cat.put(r3) && cat.put(r4)
	      && cat.put(r1) && cat.records().size() == 4,
	      "catalog publication is idempotent");
	check(cat.consolidated().size() == 3 && cat.equivalent(r1.id, r4.id)
	      && !cat.equivalent(r1.id, r2.id),
	      "the same assertion from two sources is one concept "
	      "without a verdict");
	auto const entities = cat.entities();
	{
		// An object that names an entity links to it; a definition
		// is the entity's gloss.
		auto linked = semantic::parse_records(
			R"({"records":[{"kind":"claim","subject":"Ghidra","relation":"features","object":"Memory Manager","statement":"Ghidra features a Memory Manager.","cues":[4]},{"kind":"definition","subject":"memory manager","relation":"is","object":"the tool that lays out memory regions","statement":"The Memory Manager lays out memory regions.","cues":[5]}]})",
			w, mix);
		require(linked.records.size() == 2, "two linked records parse");
		fs::path const rig2 = rig / "graph";
		semantic::catalog graph(rig2.string(), mix, words);
		check(graph.put(linked.records[0]) && graph.put(linked.records[1]),
		      "linked records publish");
		auto const g = graph.entities();
		check(g.size() == 2 && g[0].title == "Ghidra"
		      && g[0].predicates.size() == 1
		      && g[0].predicates[0].objects.size() == 1
		      && g[0].predicates[0].objects[0].entity == 1
		      && g[1].title == "memory manager"
		      && g[1].definition == g[1].predicates[0].objects[0].at
		      && g[0].definition == semantic::catalog::npos,
		      "an object naming an entity links to it, a definition "
		      "glosses its entity");
		// A same verdict between two names makes one entity, the
		// links following it.
		auto more = semantic::parse_records(
			R"({"records":[{"kind":"claim","subject":"gidra","relation":"runs on","object":"the JVM","statement":"Gidra runs on the JVM.","cues":[6]}]})",
			w, mix);
		require(more.records.size() == 1 && graph.put(more.records[0]),
		        "a misspelt subject publishes");
		check(graph.entities().size() == 3
		      && graph.entity_of(graph.entity_id("GIDRA")) == 2,
		      "a misspelt subject is its own entity until judged");
		check(graph.link({graph.entity_id("Ghidra"),
		                  graph.entity_id("gidra"),
		                  semantic::relation::same, "One tool."}),
		      "an entity verdict links");
		auto const h = graph.entities();
		check(h.size() == 2 && h[0].title == "Ghidra"
		      && h[0].predicates.size() == 2
		      && graph.entity_of(graph.entity_id("gidra")) == 0,
		      "a same verdict unites two names into one entity");
		// Restated later at greater length, a concept carries the
		// longer wording but keeps its first statement's place.
		auto again = semantic::parse_records(
			R"({"records":[{"kind":"claim","subject":"Ghidra","relation":"features","object":"Memory Manager","statement":"Ghidra features a Memory Manager, as said before.","cues":[7]}]})",
			w, mix);
		require(again.records.size() == 1 && graph.put(again.records[0]),
		        "a restatement publishes");
		auto const k = graph.entities();
		check(k.size() == 2 && k[0].predicates.size() == 2
		      && k[0].predicates[0].title == "features"
		      && k[0].predicates[1].title == "runs on"
		      && graph.consolidated()[k[0].predicates[0].objects[0].at]
		             .statement.ends_with("before."),
		      "a restated concept keeps its first-spoken place");
		// The lexicon's word is identity: names it spells as one
		// term are one entity without a verdict, a spelling no
		// record names is absent, and the groups replace each other.
		auto spelt = semantic::parse_records(
			R"({"records":[{"kind":"claim","subject":"Deidre","relation":"runs on","object":"the JVM","statement":"Deidre runs on the JVM.","cues":[5]}]})",
			w, mix);
		require(spelt.records.size() == 1 && graph.put(spelt.records[0]),
		        "a third spelling publishes");
		require(graph.entities().size() == 3,
		        "a third spelling is a third entity until the lexicon");
		graph.aliases({{graph.entity_id("ghidra"),
		                graph.entity_id("DEIDRE"),
		                graph.entity_id("nobody says this")}});
		check(graph.entities().size() == 2
		      && graph.entity_of(graph.entity_id("Deidre")) == 0
		      && graph.entities()[0].predicates.size() == 2
		      && graph.entities()[0].predicates[1].objects.size() == 2,
		      "a lexicon group unites its names without a verdict");
		graph.aliases({});
		check(graph.entities().size() == 3,
		      "a lexicon replaced by none leaves the verdicts standing");
	}
	// astronomy is said at cue 4, deployment from cue 5 on: the
	// entities come in the order the corpus first speaks of them.
	check(entities.size() == 2 && entities[0].title == "astronomy"
	      && entities[0].predicates[0].objects.size() == 1
	      && entities[1].title == "deployment"
	      && entities[1].predicates.size() == 1
	      && entities[1].predicates[0].title == "involves"
	      && entities[1].predicates[0].objects.size() == 2,
	      "entities group the view by subject and predicate, in the "
	      "order first spoken");
	{
		// The corpus order decides between sources, not their
		// names, and a concept's earliest span decides for it, not
		// the span of the member whose wording it carries: listed
		// the other way round, deployment -- restated in the second
		// video -- comes first.
		semantic::catalog flipped(rig.string(), mix, words,
		                          {"second-video", "video-identity"});
		flipped.load();
		check(flipped.entities().size() == 2
		      && flipped.entities()[0].title == "deployment",
		      "sources order as the corpus lists them");
	}
	semantic::record retitled = r1;
	retitled.evidence[0].title = "/moved/deploy.mp4";
	check(cat.put(retitled) && cat.records().size() == 4
	      && cat.find(r1.id)->evidence[0].title == "/moved/deploy.mp4",
	      "a span's title is presentation, not identity, and the "
	      "record takes the current one");
	{
		semantic::catalog moved(rig.string(), mix, words);
		moved.load();
		check(moved.find(r1.id)
		      && moved.find(r1.id)->evidence[0].title
		         == "/moved/deploy.mp4",
		      "the current presentation is what the disk holds");
	}
	auto const cand = cat.candidates(r1, 4);
	check(!cand.empty() && cat.records()[cand.front()].id == r4.id,
	      "lexical candidates route the closest record");
	auto const hits = cat.search("restart verification status", 4);
	check(!hits.empty() && cat.consolidated()[hits.front()].id == r2.id,
	      "query routing finds evidence-backed procedure step");
	semantic::edge e{r1.id, r4.id, semantic::relation::same,
	                 "Equivalent upload instruction."};
	check(cat.link(e) && cat.link(e) && cat.edges().size() == 1,
	      "consolidation edge publication is idempotent");
	auto const concepts = cat.consolidated();
	check(concepts.size() == 3
	      && std::ranges::any_of(concepts, [](semantic::record const &r) {
			return r.statement == "Upload the deployment package."
			    && r.evidence.size() == 2;
	      }), "same records crystallize into one evidence union");
	check(cat.link({r1.id, r2.id, semantic::relation::contradicts,
	                "One uploads, one restarts."}),
	      "a contradiction links");
	std::size_t at2 = cat.consolidated().size();
	for (std::size_t i = 0; i < cat.consolidated().size(); ++i)
		if (cat.consolidated()[i].id == r2.id)
			at2 = i;
	require(at2 < cat.consolidated().size(),
	        "the contradicted concept is in the view");
	check(cat.ties(cat.consolidated().size()).contradicts == 0
	      && cat.partners(cat.consolidated().size(),
	                      semantic::relation::related).empty(),
	      "a position past the view reads as nothing");
	check(cat.ties(at2).contradicts == 1 && cat.ties(at2).related == 0
	      && cat.partners(at2, semantic::relation::contradicts)
	         == std::vector<agenda::id>{r1.id}
	      && cat.partners(at2, semantic::relation::related).empty(),
	      "the relation graph is readable per concept");

	check(cat.stage(r2.id, r3.id) && cat.stage(r3.id, r2.id)
	      && !cat.stage(r2.id, r2.id) && cat.staged().size() == 1
	      && cat.staged().front().a == std::min(r2.id, r3.id),
	      "a staged pair is remembered once, ordered");
	semantic::catalog replay(rig.string(), mix, words);
	replay.load();
	check(replay.records().size() == 4 && replay.edges().size() == 2
	      && replay.consolidated().size() == 3,
	      "catalog replays records and relations from disk");
	check(replay.staged().size() == 1
	      && replay.staged().front().b == std::max(r2.id, r3.id),
	      "staged pairs outlive the session that asked");
	check(replay.link({std::min(r2.id, r3.id), std::max(r2.id, r3.id),
	                   semantic::relation::novel, {}})
	      && replay.staged().empty(),
	      "linking retires the staged pair");
	semantic::catalog compact(rig.string(), mix, words);
	compact.load();
	std::error_code fec;
	auto const left = fs::file_size(rig / "staged", fec);
	check(compact.staged().empty() && !fec && left == 0,
	      "load compacts the staged file to the pending pairs");
	auto const back = replay.search("upload package", 1);
	require(!back.empty()
	        && !replay.consolidated()[back.front()].evidence.empty(),
	        "retrieval returns an evidence-backed concept");
	check(replay.consolidated()[back.front()].evidence[0].quote
	      == "Upload it through Relay.",
	      "retrieval retains exact source quote");

	fs::path const torn = rig / "records" / (r2.id.hex() + ".record");
	std::ofstream(torn, std::ios::trunc) << "{\"id\":";
	semantic::catalog healed(rig.string(), mix, words);
	healed.load();
	check(healed.records().size() == 3,
	      "a torn record file loads as absent");
	auto const grown = fs::file_size(torn, fec);
	check(healed.put(r2) && healed.records().size() == 4
	      && !fec && fs::file_size(torn, fec) > grown && !fec,
	      "publication rewrites a torn record file");

	// A stranger's edge under this pair's name is no edge of it.
	semantic::edge stray{std::min(r1.id, r3.id), std::max(r1.id, r3.id),
	                     semantic::relation::related, {}};
	auto const slot_of = [&rig](semantic::edge const &x) {
		return rig / "edges"
		     / (mix("semantic-edge-v1" + std::min(x.a, x.b).hex()
		            + std::max(x.a, x.b).hex()).hex() + ".edge");
	};
	fs::path const slot = slot_of(stray);
	fs::path const src = slot_of(e);
	std::error_code ec;
	fs::copy_file(src, slot, fs::copy_options::overwrite_existing, ec);
	require(!ec, "a published edge is copied into another slot");
	check(healed.link(stray) && healed.edges().size() == 4,
	      "an edge file naming another pair is rewritten");
	fs::copy_file(slot, rig / "edges" / "feedfacefeedface.edge", ec);
	require(!ec, "an edge is copied outside its slot");
	fs::copy_file(rig / "records" / (r1.id.hex() + ".record"),
	              rig / "records" / "feedfacefeedface.record", ec);
	require(!ec, "a record is copied outside its slot");
	semantic::catalog strays(rig.string(), mix, words);
	strays.load();
	check(strays.records().size() == 4 && strays.edges().size() == 4,
	      "entries outside their own slot are strays, not loaded");
	fs::copy_file(rig / "records" / (r1.id.hex() + ".record"),
	              rig / "records" / (r3.id.hex() + ".record"),
	              fs::copy_options::overwrite_existing, ec);
	require(!ec, "a record is copied over another's slot");
	semantic::catalog usurped(rig.string(), mix, words);
	usurped.load();
	check(usurped.records().size() == 3 && usurped.put(r3)
	      && usurped.records().size() == 4,
	      "a record file under another identity's slot is rewritten");

	fs::remove_all(rig);
	std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED",
	            g_fail, g_fail == 1 ? "" : "s");
	return g_fail ? 1 : 0;
}
