// engine_test.cpp -- the semantic engine's orchestration against a
// fake backend: a directory of canned replies standing in for the
// pipeline.  Standard C++; no Qt, no model server, no Facts.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

#include "semantic_engine.hpp"

namespace {

namespace fs = std::filesystem;

int g_fail = 0;

void check(bool ok, char const *what)
{
	std::printf("%s  %s\n", ok ? "OK  " : "FAIL", what);
	if (!ok)
		++g_fail;
}

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

// The pipeline as the engine sees it.  An offer whose artifact
// exists lands at once, as a cache hit does; one whose artifact is
// missing is remembered as asked, and the test answers it by
// writing the file -- which is all the real pipeline ever does.
struct fake {
	std::string root;
	std::uint64_t count = 0;
	std::vector<std::pair<agenda::task, std::string>> asked;
	std::vector<agenda::id> failed; // parked, as after a refusal

	explicit fake(std::string r) : root(std::move(r)) {}

	std::string path(agenda::id id, agenda::kind k) const
	{
		return root + "/semantic/" + std::string(agenda::name(k))
		     + '/' + id.hex() + ".txt";
	}

	void offer(agenda::task t, std::string const &body)
	{
		if (fs::exists(path(t.id, t.what)))
			++count;
		else
			asked.emplace_back(std::move(t), body);
	}

	std::string locate(agenda::id id, agenda::kind k) const
	{
		std::string const p = path(id, k);
		return fs::exists(p) ? p : std::string();
	}

	std::uint64_t landed() const { return count; }
	bool parked(agenda::id id) const
	{
		return std::ranges::find(failed, id) != failed.end();
	}
	std::string dir() const { return root; }
	agenda::id recipe(agenda::kind) const { return mix("recipe"); }

	// Answers the n-th asked task of a kind; the body it was asked
	// with is returned so a test can read what the model would.
	std::string answer(agenda::kind k, std::size_t n,
	                   std::string const &reply)
	{
		for (auto const &[t, body] : asked)
			if (t.what == k && !n--) {
				std::ofstream(path(t.id, k)) << reply;
				++count;
				return body;
			}
		return {};
	}

	std::size_t asks(agenda::kind k) const
	{
		return std::size_t(std::ranges::count_if(asked,
			[k](auto const &a) { return a.first.what == k; }));
	}
};

using Engine = engine::SemanticEngine<fake>;

Engine::source lecture(std::string id, std::size_t cues)
{
	Engine::source s{id, "/lectures/" + id + ".mp4", {}, {}};
	for (std::size_t i = 0; i < cues; ++i)
		s.cues.push_back({std::uint32_t(i), double(i) * 5.0,
		                  double(i) * 5.0 + 4.0,
		                  "cue " + std::to_string(i) + " of the "
		                  "lecture speaks of the Ghidra decompiler "
		                  "and the ground truth of machine code"});
	return s;
}

std::string records_json(std::string_view subject,
                         std::string_view statement, unsigned cue)
{
	return std::string(R"({"records":[{"kind":"claim","subject":")")
	     + std::string(subject) + R"(","relation":"says","object":")"
	     + std::string(statement) + R"(","statement":")"
	     + std::string(statement) + R"(","cues":[)"
	     + std::to_string(cue) + "]}]}";
}

} // namespace

int main()
{
	fs::path const rig = fs::temp_directory_path()
	                   / ("srtview_engine_test." + std::to_string(getpid()));
	fs::remove_all(rig);
	for (char const *k : {"extract", "judge", "answer"})
		fs::create_directories(rig / "semantic" / k);
	fake back{rig.string()};

	// --- reset cuts windows and asks for each ---------------------
	Engine eng(back, mix);
	std::vector<Engine::source> sources;
	sources.push_back(lecture("aaaaaaaaaaaaaaa1", 300));
	sources.push_back(lecture("aaaaaaaaaaaaaaa2", 40));
	eng.reset("corpus-one", sources);
	require(eng.windows() >= 3 && back.asks(agenda::kind::extract)
	        == eng.windows(), "every window is asked once");
	std::string const body = back.asked.front().second;
	check(body.starts_with("SOURCE: aaaaaaaaaaaaaaa1\nTITLE: ")
	      && body.find("#0 [0:00.000] cue 0 of") != std::string::npos,
	      "a window body names its source, title and numbered cues");
	check(engine::window_body(eng.window(0)) == body
	      && eng.key("terms", eng.window(0))
	         != eng.key("semantic-extract-v1", eng.window(0)),
	      "the same cut serves another tag under another key");
	Engine::source moved = lecture("aaaaaaaaaaaaaaa1", 300);
	moved.title = "/elsewhere/renamed.mp4";
	Engine renamed(back, mix);
	renamed.reset("corpus-one", {moved});
	check(renamed.key("semantic-extract-v1", renamed.window(0))
	      == eng.key("semantic-extract-v1", eng.window(0)),
	      "a renamed video keeps its window identity");

	// --- frame text joins the body and the identity ---------------
	{
		fs::path const frig = rig / "frames";
		fs::create_directories(frig);
		fake fback{frig.string()};
		Engine feng(fback, mix);
		Engine::source src = lecture("aaaaaaaaaaaaaaa1", 300);
		src.frames.push_back({5.0, "P-code | SLEIGH"});
		src.frames.push_back({6.0, std::string(3000, 'x')});
		src.frames.push_back({7.0, "clipped neighbor"});
		src.frames.push_back({999999.0, "orphan slide"});
		feng.reset("corpus-one", {src});
		require(feng.windows() >= 2, "the lecture cuts to windows");
		std::string const w0 =
			engine::window_body(feng.window(0));
		check(w0.find("@ [0:05.000] P-code | SLEIGH\n")
		      != std::string::npos,
		      "frame text renders into the window body");
		check(w0.find("@ [") > w0.find("TITLE: ")
		      && w0.find("@ [") < w0.find("#0 ["),
		      "frame lines sit between the title and the cues");
		check(w0.find(std::string(64, 'x')) == std::string::npos
		      && w0.find("clipped neighbor") == std::string::npos,
		      "the clip keeps the earliest frames and stops");
		check(engine::window_body(feng.window(1)).find("@ [0:05")
		      == std::string::npos,
		      "a frame lands in its own window only");
		std::string const wl = engine::window_body(
			feng.window(feng.windows() - 1));
		check(wl.find("orphan slide") != std::string::npos,
		      "a stray past the last cue lands in the last window");
		check(feng.key("semantic-extract-v1", feng.window(0))
		      != eng.key("semantic-extract-v1", eng.window(0)),
		      "frame text re-keys its window");
		check(feng.key("semantic-extract-v1", feng.window(1))
		      == eng.key("semantic-extract-v1", eng.window(1)),
		      "a frameless window keeps its identity");

		// An oversized FIRST frame shrinks the budget instead
		// of zeroing the window's evidence.
		Engine beng(fback, mix);
		Engine::source big = lecture("aaaaaaaaaaaaaaa2", 300);
		big.frames.push_back({5.0, std::string(3000, 'y')});
		big.frames.push_back({6.0, "starved neighbor"});
		beng.reset("corpus-big", {big});
		std::string const wb =
			engine::window_body(beng.window(0));
		check(wb.find(std::string(64, 'y')) != std::string::npos
		      && wb.find("starved neighbor") == std::string::npos,
		      "an oversized first frame still rides, alone");
	}

	// --- extraction lands, records show, pairs are judged ---------
	back.answer(agenda::kind::extract, 0,
	            records_json("Ghidra", "Ghidra decompiles machine code "
	                                   "into C.", 3));
	back.answer(agenda::kind::extract, 1,
	            records_json("Ghidra", "Ghidra's decompiler emits C "
	                                   "from machine code.", 120));
	eng.tick();
	check(eng.knowledge().size() == 2, "landed records are knowledge");
	check(back.asks(agenda::kind::judge) == 1,
	      "two overlapping records stage one judgment");
	std::string const judge = back.asked.back().second;
	check(judge.starts_with("NEW\nID: ")
	      && judge.find("\n---\nCANDIDATE\nID: ") != std::string::npos,
	      "a judgment sees both records with their evidence");

	// --- an unlanded judgment survives a restart ------------------
	{
		fake again{rig.string()};
		Engine replay(again, mix);
		replay.reset("corpus-one", sources);
		replay.tick();
		check(again.asks(agenda::kind::extract) == eng.windows() - 2
		      && again.asks(agenda::kind::judge) == 1
		      && replay.knowledge().size() == 2,
		      "a warm cache re-asks only the unanswered windows and "
		      "the pending pair");
	}

	// A re-cut corpus -- same id, same recipes, different window
	// identities (frames arrived) -- starts its own catalog view
	// instead of inheriting the pre-frames generation's records.
	{
		fake tail{rig.string()};
		Engine gen2(tail, mix);
		Engine::source fsrc = lecture("aaaaaaaaaaaaaaa1", 300);
		fsrc.frames.push_back({5.0, "P-code | SLEIGH"});
		gen2.reset("corpus-one", {fsrc,
		                          lecture("aaaaaaaaaaaaaaa2", 40)});
		check(gen2.knowledge().empty(),
		      "a re-cut corpus starts its own catalog view");
	}
	back.answer(agenda::kind::judge, 0,
	            R"({"relation":"same","rationale":"One assertion."})");
	eng.tick();
	check(eng.knowledge().size() == 1
	      && eng.knowledge().front().evidence.size() == 2,
	      "a same verdict crystallizes two records into one concept");
	{
		fake again{rig.string()};
		Engine replay(again, mix);
		replay.reset("corpus-one", sources);
		replay.tick();
		check(again.asks(agenda::kind::judge) == 0
		      && replay.knowledge().size() == 1,
		      "a linked pair is never asked again");
	}

	// --- the harvest is paced ------------------------------------
	{
		fs::path const rig2 = rig / "paced";
		for (char const *k : {"extract", "judge", "answer"})
			fs::create_directories(rig2 / "semantic" / k);
		fake paced{rig2.string()};
		Engine e2(paced, mix);
		std::vector<Engine::source> many;
		for (int i = 0; i < 6; ++i)
			many.push_back(lecture("bbbbbbbbbbbbbbb" + std::to_string(i),
			                       30));
		e2.reset("corpus-two", many);
		require(e2.windows() == 6, "six single-window sources");
		for (std::size_t i = 0; i < 6; ++i)
			paced.answer(agenda::kind::extract, i,
			             records_json("topic " + std::to_string(i),
			                          "Statement number "
			                          + std::to_string(i) + " about "
			                          "a distinct matter.", 2));
		e2.tick();
		std::size_t const first = e2.knowledge().size();
		e2.tick();
		e2.tick();
		check(first == 4 && e2.knowledge().size() == 6,
		      "a warm cache harvests a few windows per tick, then "
		      "the rest");
		std::size_t const asks = paced.asked.size();
		e2.tick();
		check(paced.asked.size() == asks,
		      "a tick with nothing landed asks nothing");
	}

	// --- a question: bundle, rejection, re-ask, acceptance ---------
	agenda::id const q = eng.ask("What does Ghidra do with machine code?");
	require(bool(q) && back.asks(agenda::kind::answer) == 1,
	        "a question with evidence is asked");
	std::string const bundle = back.asked.back().second;
	check(bundle.find("QUESTION\nWhat does Ghidra") != std::string::npos
	      && bundle.find("RECORD ") != std::string::npos
	      && bundle.find("\nCITE {\"source\":\"aaaaaaaaaaaaaaa1\",")
	         != std::string::npos
	      && bundle.find("RAW PASSAGE") != std::string::npos,
	      "the bundle carries records, raw passages and CITE objects");
	{
		// Passages are chosen by score and told in corpus order.
		std::vector<std::pair<std::string, unsigned>> told;
		for (std::size_t at = bundle.find("RAW PASSAGE\nCITE ");
		     at != std::string::npos;
		     at = bundle.find("RAW PASSAGE\nCITE ", at + 1)) {
			std::size_t const src = bundle.find("\"source\":\"", at) + 10;
			std::size_t const first = bundle.find("\"first\":", at) + 8;
			told.emplace_back(bundle.substr(src, 16),
			                  std::stoul(bundle.substr(first)));
		}
		check(told.size() > 1 && std::ranges::is_sorted(told),
		      "raw passages come in corpus order");
	}
	check(eng.ask("What does Ghidra do with machine code?") == q
	      && back.asks(agenda::kind::answer) == 1,
	      "the same question is the same ask");
	agenda::id const none = eng.ask("xq zq wq");
	check(none && eng.result(none) && eng.result(none)->insufficient
	      && back.asks(agenda::kind::answer) == 1,
	      "a question nothing matches is insufficient without a call");
	// Cue 250 is in the corpus but in no record and no raw passage
	// of this bundle: a true citation the bundle never offered.
	back.answer(agenda::kind::answer, 0,
	            R"({"answer":"Decompiles it.","citations":[{"source":"aaaaaaaaaaaaaaa1","first":250,"last":250}],"insufficient":false})");
	eng.tick();
	check(!eng.result(q) && back.asks(agenda::kind::answer) == 2
	      && back.asked.back().second.find("\nATTEMPT 2\n")
	         != std::string::npos
	      && back.asked.back().second.starts_with(bundle),
	      "a citation outside the bundle is re-asked with feedback "
	      "on the same bundle");
	back.answer(agenda::kind::answer, 1,
	            R"({"answer":"Ghidra decompiles machine code into C.","citations":[{"source":"aaaaaaaaaaaaaaa1","first":3,"last":3}],"insufficient":false})");
	eng.tick();
	auto const got = eng.result(q);
	require(got && got->citations.size() == 1,
	        "the second attempt answers the first id");
	check(!got->insufficient && got->citations[0].first == 3,
	      "the answer is grounded on the cited cue");
	auto const span = eng.evidence(got->citations[0]);
	check(span && span->source == "aaaaaaaaaaaaaaa1"
	      && span->quote.starts_with("cue 3 of"),
	      "a citation resolves to the source's own text");
	// A follow-up reads the exchange before it -- the question,
	// the answer and the citations it showed -- and retrieves over
	// it too, which is how "says so" finds anything; a question
	// nobody answered never joined.
	agenda::id const more = eng.ask("Which cue says so?");
	check(more && more != q && back.asks(agenda::kind::answer) == 3
	      && back.asked.back().second.starts_with(
		"CONVERSATION\nUSER: What does Ghidra do with machine "
		"code?\nASSISTANT: Ghidra decompiles machine code into C.\n"
		"[aaaaaaaaaaaaaaa1.mp4 #3\u20133]\n\n")
	      && back.asked.back().second.find("xq zq wq") == std::string::npos,
	      "a follow-up carries the completed exchange, citations "
	      "included");

	// --- a verdict that is no verdict is asked again --------------
	{
		fs::path const rig4 = rig / "judge";
		for (char const *k : {"extract", "judge", "answer"})
			fs::create_directories(rig4 / "semantic" / k);
		fake jury{rig4.string()};
		Engine e4(jury, mix);
		e4.reset("corpus-four", {lecture("ddddddddddddddd1", 30)});
		jury.answer(agenda::kind::extract, 0,
		            R"({"records":[{"kind":"claim","subject":"Ghidra","relation":"decompiles","object":"machine code","statement":"Ghidra decompiles machine code.","cues":[2]},{"kind":"claim","subject":"Ghidra","relation":"turns","object":"machine code into C","statement":"Ghidra turns machine code into C.","cues":[8]}]})");
		e4.tick();
		require(jury.asks(agenda::kind::judge) == 1, "one pair judged");
		jury.answer(agenda::kind::judge, 0, "{}");
		e4.tick();
		check(jury.asks(agenda::kind::judge) == 2
		      && jury.asked.back().second.find("\nATTEMPT 2\n")
		         != std::string::npos
		      && e4.knowledge().size() == 2,
		      "an empty verdict is re-asked with feedback");
		jury.answer(agenda::kind::judge, 1,
		            R"({"relation":"same","rationale":"One claim."})");
		e4.tick();
		check(e4.knowledge().size() == 1,
		      "the second verdict links and crystallizes");
	}

	// --- a request the pipeline parks is answered with the failure --
	{
		agenda::id const qf = eng.ask("machine code ground truth");
		require(bool(qf) && back.asks(agenda::kind::answer) == 4
		        && !eng.result(qf), "a fresh question is asked");
		back.failed.push_back(back.asked.back().first.id);
		eng.tick();
		auto const lost = eng.result(qf);
		check(lost && lost->insufficient
		      && lost->text.find("did not answer") != std::string::npos,
		      "a parked request resolves with the failure");
		back.failed.clear();
		check(eng.ask("machine code ground truth") == qf
		      && back.asks(agenda::kind::answer) == 5
		      && back.asked.back().first.id == qf && !eng.result(qf),
		      "asking again after a failure offers the task anew");
	}

	// --- two names that share words are weighed as one thing ------
	{
		fs::path const rig6 = rig / "names";
		for (char const *k : {"extract", "judge", "answer"})
			fs::create_directories(rig6 / "semantic" / k);
		fake names{rig6.string()};
		Engine e6(names, mix);
		e6.reset("corpus-six", {lecture("fffffffffffffff1", 30)});
		names.answer(agenda::kind::extract, 0,
		             R"({"records":[{"kind":"definition","subject":"p-code","relation":"is","object":"Ghidra's intermediate representation","statement":"p-code is Ghidra's intermediate representation.","cues":[2]},{"kind":"claim","subject":"P code","relation":"is used across","object":"every processor","statement":"P code is used across every processor.","cues":[8]}]})");
		e6.tick();
		require(e6.entities().size() == 2, "two names, two entities");
		// The two records are judged as records (one ask), and the
		// two names as entities (one more).
		check(names.asks(agenda::kind::judge) == 2
		      && names.asked.back().second.starts_with("ENTITY A\nNAME: ")
		      && names.asked.back().second.find("\n---\nENTITY B\nNAME: ")
		         != std::string::npos,
		      "names sharing a word are put to the judge as entities");
		check(names.asked.size() >= 2
		      && names.asked.back().first.tier == 0
		      && names.asked[names.asked.size() - 2].first.tier == 1,
		      "a verdict on names outranks one on records");
		names.answer(agenda::kind::judge, 1,
		             R"({"relation":"same","rationale":"One name, two spellings."})");
		e6.tick();
		check(e6.entities().size() == 1
		      && e6.entities()[0].predicates.size() == 2,
		      "a same verdict folds two names into one entity");
		fake again{rig6.string()};
		Engine replay(again, mix);
		replay.reset("corpus-six", {lecture("fffffffffffffff1", 30)});
		replay.tick();
		check(again.asks(agenda::kind::extract) == 0
		      && replay.entities().size() == 1,
		      "the folded entity replays from cache");
	}

	// --- the lexicon's word is identity ----------------------------
	{
		fs::path const rig7 = rig / "lexicon";
		for (char const *k : {"extract", "judge", "answer"})
			fs::create_directories(rig7 / "semantic" / k);
		fake terms{rig7.string()};
		Engine e7(terms, mix);
		e7.reset("corpus-seven", {lecture("1111111111111111", 30),
		                          lecture("2222222222222222", 30)});
		terms.answer(agenda::kind::extract, 0,
		             R"({"records":[{"kind":"claim","subject":"Ghidra","relation":"decompiles","object":"binaries","statement":"Ghidra decompiles binaries.","cues":[2]},{"kind":"claim","subject":"gidra","relation":"supports","object":"patch diffing","statement":"gidra supports patch diffing.","cues":[8]}]})");
		e7.tick();
		require(e7.entities().size() == 2
		        && terms.asks(agenda::kind::judge) == 0,
		        "names sharing no word are two entities nobody "
		        "thought to compare");
		e7.lexicon({{"ghidra", "Gidra", "deidre"}});
		check(e7.entities().size() == 1
		      && terms.asks(agenda::kind::judge) == 0,
		      "a lexicon group is one entity on the model's word, "
		      "no verdict asked");
		terms.answer(agenda::kind::extract, 1,
		             R"({"records":[{"kind":"claim","subject":"Deidre","relation":"runs on","object":"the JVM","statement":"Deidre runs on the JVM.","cues":[5]}]})");
		e7.tick();
		check(e7.entities().size() == 1
		      && e7.entities().front().predicates.size() == 3
		      && terms.asks(agenda::kind::judge) == 0,
		      "a name that appears later joins its group at once");
		e7.lexicon({});
		check(e7.entities().size() == 3,
		      "a lexicon withdrawn leaves the names apart again");
	}

	// --- contradictions ride into the bundle ----------------------
	back.answer(agenda::kind::extract, 2,
	            records_json("Ghidra", "Ghidra never emits C from "
	                                   "machine code.", 250));
	eng.tick();
	require(back.asks(agenda::kind::judge) >= 2,
	        "a third record is judged against the first two");
	std::size_t const judges = back.asks(agenda::kind::judge);
	for (std::size_t i = 1; i < judges; ++i)
		back.answer(agenda::kind::judge, i,
		            R"({"relation":"contradicts","rationale":"Opposite."})");
	eng.tick();
	bool contradicted = false;
	for (std::size_t i = 0; i < eng.knowledge().size(); ++i)
		contradicted |= eng.ties(i).contradicts > 0;
	check(contradicted, "a contradicts verdict counts on the concept");
	eng.ask("Does Ghidra emit C from machine code?");
	check(back.asked.back().second.find("\nCONTRADICTS ")
	      != std::string::npos,
	      "a retrieved concept brings its contradiction along");

	// --- a window cited outside itself is asked again -------------
	{
		fs::path const rig3 = rig / "retry";
		for (char const *k : {"extract", "judge", "answer"})
			fs::create_directories(rig3 / "semantic" / k);
		fake redo{rig3.string()};
		Engine e3(redo, mix);
		e3.reset("corpus-three", {lecture("ccccccccccccccc1", 30)});
		require(e3.windows() == 1 && redo.asks(agenda::kind::extract) == 1,
		        "one window, one ask");
		// One good record, one citing cue 999: the good one lands,
		// the window is re-asked with feedback under a new id.
		redo.answer(agenda::kind::extract, 0,
		            R"({"records":[{"kind":"claim","subject":"good","relation":"says","object":"something supported","statement":"A statement the window supports.","cues":[4]},{"kind":"claim","subject":"bad","relation":"says","object":"nothing","statement":"A statement citing nowhere.","cues":[999]}]})");
		e3.tick();
		check(e3.knowledge().size() == 1
		      && redo.asks(agenda::kind::extract) == 2
		      && redo.asked.back().first.id != redo.asked.front().first.id
		      && redo.asked.back().second.find("\nATTEMPT 2\n")
		         != std::string::npos
		      && redo.asked.back().second.find("from #0-29 above")
		         != std::string::npos,
		      "a rejected record keeps the good one and re-asks with "
		      "the window's range");
		redo.answer(agenda::kind::extract, 1,
		            R"({"records":[{"kind":"claim","subject":"good","relation":"says","object":"something supported","statement":"A statement the window supports.","cues":[4]},{"kind":"claim","subject":"fixed","relation":"says","object":"something cited","statement":"A statement citing the window.","cues":[9]}]})");
		e3.tick();
		check(e3.knowledge().size() == 2
		      && redo.asks(agenda::kind::extract) == 2,
		      "a clean second attempt lands and ends the chain");
		fake cold{rig3.string()};
		Engine replay(cold, mix);
		replay.reset("corpus-three", {lecture("ccccccccccccccc1", 30)});
		replay.tick();
		replay.tick();
		check(cold.asks(agenda::kind::extract) == 0
		      && replay.knowledge().size() == 2,
		      "a restart replays the chain from cache without a call");
	}

	// --- a catalog that cannot publish is no catalog ------------
	{
		fs::path const rig5 = rig / "broken";
		fs::create_directories(rig5 / "semantic" / "extract");
		// A file where the catalog directory must go.
		fs::create_directories(rig5 / "semantic");
		std::ofstream(rig5 / "semantic" / "catalog") << "in the way";
		fake bare{rig5.string()};
		Engine e5(bare, mix);
		e5.reset("corpus-five", {lecture("eeeeeeeeeeeeeee1", 30)});
		agenda::id const none = e5.ask("Ghidra machine code");
		check(e5.windows() == 1 && bare.asked.empty() && !none
		      && e5.knowledge().empty(),
		      "a broken catalog cuts windows for the terms pass and "
		      "asks nothing for knowledge");
	}

	fs::remove_all(rig);
	std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED",
	            g_fail, g_fail == 1 ? "" : "s");
	return g_fail ? 1 : 0;
}
