// semantic_engine.hpp -- corpus semantic workflow, outside the UI.
// Standard C++23, no Qt.  It cuts transcripts into cue-boundary
// windows, submits schema-constrained extraction through the
// pipeline behind it, validates and persists evidence records, asks
// bounded pairwise consolidation judgments, routes queries over
// records plus raw cues, and validates cited answers against the
// supplied bundle.  A class template over a backend concept of five
// calls rather than a class over Facts: the orchestration -- reset,
// warm replay, pacing, the answer re-ask chain, pending judgments
// -- is where the subtle bugs live, and it has to run against a
// fake in the tests as it runs against the pipeline in the app.
// MainWin owns one engine and provides source snapshots; it does not
// own semantic policy or model-output parsing.
#ifndef SRTVIEW_SRC_SEMANTIC_ENGINE_HPP_
#define SRTVIEW_SRC_SEMANTIC_ENGINE_HPP_

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "semantic.hpp"
#include "slurp.hpp"
#include "timefmt.hpp"

namespace engine {

// What the engine asks of the pipeline behind it: to queue a task
// with its snapshot, to say where a task's artifact lies, to count
// what has landed, to say whether a task failed and was parked,
// and to name the cache root and a kind's recipe.
// Facts is the backend in the app; a directory of canned replies
// is one in the tests.
template <class B>
concept semantic_backend = requires(B &b, agenda::task t,
                                    std::string const &body,
                                    agenda::id id, agenda::kind k) {
	b.offer(std::move(t), body);
	{ b.locate(id, k) } -> std::convertible_to<std::string>;
	{ b.landed() } -> std::convertible_to<std::uint64_t>;
	{ b.parked(id) } -> std::convertible_to<bool>;
	{ b.dir() } -> std::convertible_to<std::string>;
	{ b.recipe(k) } -> std::convertible_to<agenda::id>;
};

namespace detail {
inline constexpr std::size_t kWindowBytes = std::size_t{12} * 1024;
inline constexpr std::size_t kWindowOverlap = 2;
// One window's share of frame text: the earliest frames up to this
// many rendered bytes; the clip happens at assignment, so the body
// and the identity always agree on the clipped set.
inline constexpr std::size_t kFrameBytes = 2048;
inline constexpr std::size_t kJudgeFan = 4;
inline constexpr std::size_t kEntityFan = 2;
// One entity's share of a judge body, in bytes: the executor clips
// a snapshot at 96 KiB, and a verdict over a body whose second
// entity was clipped away would be a verdict over one side.
inline constexpr std::size_t kEntityBytes = std::size_t{12} * 1024;
inline constexpr std::size_t kHarvestPerTick = 4;
inline constexpr std::size_t kAnswerRecords = 12;
inline constexpr std::size_t kAnswerPassages = 12;
inline constexpr std::size_t kAnswerContra = 4;
inline constexpr std::size_t kAnswerBytes = std::size_t{80} * 1024;
inline constexpr std::size_t kQuestionBytes = 8192;
// Attempts a window, a pair or a question gets under one recipe,
// each under its own deterministic id.  The ids are deterministic
// on purpose: at the cap the chain is done for this recipe, and a
// restart replays it from cache without a call -- the same model
// at temperature zero would only answer the same way again.  What
// asks afresh is a new recipe: a changed prompt or schema, or a
// changed model named through SRTVIEW_LLM_MODEL_ID.
inline constexpr unsigned kAttempts = 3;
inline constexpr std::size_t kConversationBytes = std::size_t{16} * 1024;

inline bool debug()
{
	char const *const v = std::getenv("SRTVIEW_DEBUG");
	return v && *v;
}

inline std::string title_line(std::string title)
{
	for (char &c : title)
		if (c == '\r' || c == '\n')
			c = ' ';
	return title;
}



inline agenda::id id_of(std::string_view tag, std::string_view body,
	             semantic::hash8_fn h)
{
	std::string key(tag);
	key += '\n';
	key += body;
	return h(key);
}

inline void add_key(std::vector<agenda::id> &keys, std::string_view source)
{
	agenda::id const id = agenda::id::from_hex(source);
	if (id && std::ranges::find(keys, id) == keys.end())
		keys.push_back(id);
}

inline void add_record(std::string &out, std::string_view label,
	            semantic::record const &r)
{
	out += label;
	out += "\nID: " + r.id.hex();
	out += "\nKIND: ";
	out += semantic::name(r.what);
	out += "\nSUBJECT: " + r.subject;
	out += "\nRELATION: " + r.relation;
	out += "\nOBJECT: " + r.object;
	out += "\nSTATEMENT: " + r.statement;
	out += "\nEVIDENCE\n";
	for (semantic::evidence_span const &e : r.evidence) {
		out += '[' + e.source + '#'
		     + std::to_string(e.first) + '-'
		     + std::to_string(e.last) + "] "
		     + title_line(e.title) + '\n' + e.quote + '\n';
	}
}

inline std::string judge_body(semantic::record const &a,
	                   semantic::record const &b)
{
	std::string out;
	add_record(out, "NEW", a);
	out += "\n---\n";
	add_record(out, "CANDIDATE", b);
	return out;
}

// Byte caps that land on UTF-8 character boundaries: the longest
// prefix, and the longest suffix, of at most cap bytes.
inline std::string_view utf8_head(std::string_view s, std::size_t cap)
{
	if (s.size() <= cap)
		return s;
	while (cap && (static_cast<unsigned char>(s[cap]) & 0xc0) == 0x80)
		--cap;
	return s.substr(0, cap);
}

inline std::string_view utf8_tail(std::string_view s, std::size_t cap)
{
	if (s.size() <= cap)
		return s;
	std::size_t at = s.size() - cap;
	while (at < s.size()
	       && (static_cast<unsigned char>(s[at]) & 0xc0) == 0x80)
		++at;
	return s.substr(at);
}

} // namespace detail

// The text the model is shown for a window, and the numbered cue
// lines that are the window's identity: the terms pass asks over
// the same windows as extraction, so one cut and one body serve
// both.
inline std::string window_lines(semantic::window const &w)
{
	std::string out;
	for (semantic::cue const &c : w.cues) {
		out += '#';
		out += std::to_string(c.number);
		out += " [";
		out += fmt_time(c.start, true);
		out += "] ";
		out += c.text;
		out += '\n';
	}
	return out;
}

// The on-screen text read inside the window's span: one line per
// frame, rendered into the body and hashed into the identity alike
// -- a window with different frame text is a different ask.
inline std::string window_frames(semantic::window const &w)
{
	std::string out;
	for (semantic::frame const &f : w.frames) {
		out += "@ [";
		out += fmt_time(f.at, true);
		out += "] ";
		out += f.text;
		out += '\n';
	}
	return out;
}

inline std::string window_body(semantic::window const &w)
{
	std::string out = "SOURCE: ";
	out += w.source;
	out += "\nTITLE: ";
	out += detail::title_line(std::string(w.title));
	out += '\n';
	out += window_frames(w);
	out += window_lines(w);
	return out;
}

template <semantic_backend Backend>
class SemanticEngine
{
public:
	// One transcript as the owner loaded it: cues numbered 0..n-1
	// in transcript order, the number being the index.  The engine
	// relies on that order and never re-establishes it.
	struct source {
		std::string                  id;
		std::string                  title;
		std::vector<semantic::cue>   cues;
		std::vector<semantic::frame> frames; // sorted by at
	};

	SemanticEngine(Backend &back, semantic::hash8_fn h);

	// corpus is a stable identity for the exact playlist.  Reset is
	// session-local: durable catalog/cache data is loaded, never
	// erased.  Resetting the backend is the composition root's job.
	void reset(std::string corpus, std::vector<source> sources);
	void tick();

	// The corpus cut into windows, in staging order; the text the
	// model is shown for one; and a window's identity under a tag.
	// The identity is the source and its numbered cue lines.  The
	// title is in the text and deliberately not in the identity:
	// it is the video's file name, context worth giving a model
	// that must tell a lecture's subject from its manglings, but
	// keying on it would re-ask every window over an unchanged
	// transcript whenever a file is renamed, and dropping it from
	// the text would buy the rule "same input, same key" with the
	// one hint the model has.  Everything that changes the words
	// -- a retranscribed subtitle is a new source id -- does re-key.
	// Frame text is not like the title: it is substantive input --
	// a reply that never saw the slide is a different answer -- so
	// it rides in the text and the identity both, and a window
	// re-keys when its frames change.
	// The terms pass asks over the same windows, so one cut serves
	// both.
	// The current cut's ground witness (also its catalog
	// generation): frame-sensitive asks list it in deps, and the
	// owner marks it done -- Facts::mark() -- when the cut it
	// names is complete.  Null before the first reset or when the
	// catalog is down.
	agenda::id witness() const { return m_witness; }

	std::size_t windows() const { return m_extract.size(); }
	semantic::window const &window(std::size_t at) const
	{
		return m_extract[at].source;
	}
	agenda::id key(std::string_view tag,
	               semantic::window const &w) const;

	// One evidence-grounded question.  The returned identity is
	// stable for question + retrieved bundle + the conversation so
	// far.  result() is empty while generation is pending;
	// insufficient evidence resolves immediately without spending
	// a model call.
	agenda::id ask(std::string question);
	std::optional<semantic::answer> result(agenda::id id) const;
	std::optional<semantic::evidence_span> evidence(
		semantic::citation const &citation) const;
	// A citation as the conversation and the pane spell it: the
	// source's file name and the cue span.
	std::string label(semantic::citation const &c) const;
	// The engine keeps the conversation, since it is what reads
	// it: every exchange the model completed -- the question, the
	// answer, the citations the answer showed, so a follow-up can
	// say "the second cited lecture" -- bounded to the tail a
	// bundle carries.  It outlives reset(), as a reader's thread
	// survives a corpus adoption; a new corpus starts a new one.
	void new_conversation() { m_conversation.clear(); }

	// The corpus lexicon: groups of spellings the terms pass found
	// for one term and the owner validated against the transcript
	// -- Ghidra, gidra, deidre.  The model was asked which
	// spellings belong together and answered; the names one group
	// holds are one entity on its word, without a verdict, however
	// few words they share.  The judge is asked only about names
	// the lexicon does not group.  Replaces the lexicon before it;
	// reset() drops it, as it belongs to the corpus.
	void lexicon(std::vector<std::vector<std::string>> groups);

	std::vector<semantic::record> const &knowledge() const;
	// The relations a concept of knowledge() stands in, by position.
	semantic::catalog::tie_count ties(std::size_t at) const;
	// knowledge() as a graph of entities, positions into it.
	std::vector<semantic::catalog::entity> const &entities() const;

private:
	// One window: id is its identity (the terms pass keys on the
	// same cut), current the attempt in flight.  A reply that cited
	// outside the window, or was no document at all, is asked again
	// under a new id with feedback, up to the cap; the records each
	// attempt did get right are kept.
	struct extract_work {
		agenda::id id;
		agenda::id current;
		semantic::window source; // views m_sources; cut in reset()
		unsigned attempt = 1;
		bool seen = false;
	};

	// One pair: id is the first attempt's identity, current the
	// attempt in flight.  A verdict that is no verdict -- a server
	// ignoring the schema -- is asked again under a new id with
	// feedback, up to the cap, like a window or an answer.
	struct judge_work {
		agenda::id id;
		agenda::id current;
		agenda::id a, b;       // record ids, or entity ids
		unsigned attempt = 1;
		bool entity = false;   // a and b name entities, not records
		bool seen = false;
	};

	// One question: id is the handle the owner polls, current the
	// attempt in flight.  A rejected attempt stages the next one
	// with feedback appended to the same bundle -- a new id, so the
	// rejected artifact is never consulted again -- until the cap.
	struct answer_work {
		agenda::id id;
		agenda::id current;
		std::string body;
		std::string question;
		std::string note;
		std::vector<agenda::id> keys;
		std::vector<semantic::citation> allowed;
		std::optional<semantic::answer> value;
		unsigned attempt = 1;
		bool seen = false;
		bool failed = false; // the pipeline parked it: ask again
	};

	void cut(source const &s);
	void offer_extract(extract_work const &w);
	bool harvest_extract(extract_work &w);
	void stage_judge(semantic::record const &fresh,
	                 semantic::record const &candidate);
	void stage_entities(std::size_t fresh);
	void stage_entity_pair(std::size_t a, std::size_t b);
	std::string entity_body(std::size_t at) const;
	std::string judge_body(judge_work const &w) const;
	void offer_judge(judge_work const &w);
	bool harvest_judge(judge_work &w);
	std::string answer_bundle(std::string_view question,
	                          std::vector<semantic::citation> &allowed,
	                          std::vector<agenda::id> &keys);
	void offer_answer(answer_work const &w, std::string const &body);
	void harvest_answer(answer_work &w);
	void converse(std::string_view speaker, std::string_view text);
	bool citation_allowed(semantic::citation const &c,
	                      answer_work const &w) const;
	bool linked(agenda::id a, agenda::id b) const;
	semantic::record const *record_of(agenda::id id) const;

	Backend &m_back;
	semantic::hash8_fn m_hash;
	std::string m_corpus;
	agenda::id m_witness{};
	std::vector<source> m_sources;
	// The corpus vocabulary and every cue's words in it, parallel
	// to m_sources: tokenized once per load, not once per question.
	// The catalog ranks records on the same vocabulary.
	semantic::vocab m_vocab;
	std::vector<std::vector<std::vector<std::uint32_t>>> m_words;
	std::unique_ptr<semantic::catalog> m_catalog;
	std::vector<extract_work> m_extract;
	std::vector<judge_work> m_judge;
	agenda::index m_judgeAt; // judge id -> m_judge
	std::uint64_t m_swept = 0;  // backend landed() at the last sweep
	bool m_more = true;         // the last sweep spent its budget
	bool m_retry = false;       // a publication failed this sweep
	std::vector<answer_work> m_answers;
	std::string m_conversation;
};

template <semantic_backend Backend>
agenda::id SemanticEngine<Backend>::key(std::string_view tag,
	                           semantic::window const &w) const
{
	std::string k(tag);
	k += '\n';
	k += w.source;
	k += '\n';
	k += window_lines(w);
	k += window_frames(w);
	return m_hash(k);
}



template <semantic_backend Backend>
SemanticEngine<Backend>::SemanticEngine(Backend &back,
                                        semantic::hash8_fn h)
	: m_back(back), m_hash(h)
{
}

template <semantic_backend Backend>
void SemanticEngine<Backend>::reset(std::string corpus,
	                       std::vector<source> sources)
{
	m_corpus = std::move(corpus);
	m_sources = std::move(sources);
	m_extract.clear();
	m_judge.clear();
	m_judgeAt.clear();
	m_answers.clear();
	m_more = true;
	m_vocab = {};
	m_words.clear();
	std::vector<std::string> order;
	order.reserve(m_sources.size());
	for (source const &s : m_sources) {
		cut(s);
		order.push_back(s.id);
		auto &words = m_words.emplace_back();
		words.reserve(s.cues.size());
		for (semantic::cue const &c : s.cues)
			m_vocab.count(words.emplace_back(m_vocab.words(c.text)));
	}
	// The catalog is keyed by the corpus, the recipes AND the
	// complete set of current window identities: records and
	// verdicts extracted under another prompt, schema, model --
	// or another cut, frames having arrived -- must not mix into
	// this view, just as the vault misses them.  Untouched
	// windows replay their cached artifacts into the new view;
	// the old catalog stays on disk, and switching back reuses
	// it.
	std::string keyed = "semantic-catalog-v3\n" + m_corpus + '\n'
		+ m_back.recipe(agenda::kind::extract).hex()
		+ m_back.recipe(agenda::kind::judge).hex();
	for (extract_work const &w : m_extract) {
		keyed += '\n';
		keyed += w.id.hex();
	}
	// The same hash is the cut's ground witness: an id standing
	// for the fact "this exact cut contains every planned OCR
	// result, drained and folded."  Frame-sensitive asks carry it
	// in deps, and the owner marks it done at publication of a
	// complete cut -- an incomplete cut's asks simply never come
	// ready, and its successor's witness is a different id, so
	// the mark, once made, never needs unmaking.
	m_witness = m_hash(keyed);
	std::string const key = m_witness.hex();
	m_catalog = std::make_unique<semantic::catalog>(
		m_back.dir() + "/semantic/catalog/" + key, m_hash, m_vocab,
		std::move(order));
	// A catalog that cannot publish is no catalog: the windows are
	// cut -- the terms pass asks over them regardless -- but nothing
	// is asked or harvested for knowledge, and the reason is said
	// once, the state flip the pipeline itself logs when its cache
	// root is unusable.
	if (std::error_code const &ec = m_catalog->error(); ec) {
		std::fprintf(stderr, "srtview: semantic: %s: %s, knowledge "
		             "disabled\n", m_back.dir().c_str(),
		             ec.message().c_str());
		m_catalog.reset();
		return;
	}
	m_catalog->load();
	// Every pair ever asked and not yet linked is asked again: a
	// verdict that landed after its session ended is a cache hit
	// and links on the first tick, whatever the candidate ranking
	// says today.
	for (semantic::edge const &e : m_catalog->staged()) {
		semantic::record const *a = record_of(e.a);
		semantic::record const *b = record_of(e.b);
		if (a && b) {
			stage_judge(*a, *b);
			continue;
		}
		std::size_t const ea = m_catalog->entity_of(e.a);
		std::size_t const eb = m_catalog->entity_of(e.b);
		if (ea != semantic::catalog::npos
		    && eb != semantic::catalog::npos && ea != eb)
			stage_entity_pair(ea, eb);
	}
	for (extract_work const &w : m_extract)
		offer_extract(w);
}

template <semantic_backend Backend>
void SemanticEngine<Backend>::cut(source const &s)
{
	std::vector<semantic::window> ws;
	for (std::size_t first = 0; first < s.cues.size();) {
		std::size_t last = first;
		std::size_t bytes = 0;
		while (last < s.cues.size()) {
			std::size_t const add = s.cues[last].text.size() + 48;
			if (last > first && bytes + add > detail::kWindowBytes)
				break;
			bytes += add;
			++last;
		}
		ws.push_back({s.id, s.title,
		              std::span(s.cues).subspan(first, last - first),
		              {}});
		if (last == s.cues.size())
			break;
		std::size_t const next = last - first > detail::kWindowOverlap
		                       ? last - detail::kWindowOverlap : last;
		first = next > first ? next : last;
	}

	// Frames attach by interval overlap: window i takes every
	// frame whose [at, until] stretch crosses its own cue range,
	// so a slide read across a window boundary greets both sides
	// -- the temporal region carries, the point observation never
	// could.  A stray before the first window or past the last
	// lands in the nearest one, and the per-window clip keeps the
	// earliest frames.  Identities are computed only after the
	// frames are attached: frame text keys (unlike the title --
	// see the accessor comment).
	for (std::size_t i = 0; i < ws.size(); ++i) {
		double const from = ws[i].cues.front().start;
		double const thru = ws[i].cues.back().end;
		std::size_t bytes = 0;
		for (semantic::frame const &f : s.frames) {
			double const end = std::max(f.at, f.until);
			bool const stray =
				(i == 0 && end < from)
				|| (i + 1 == ws.size() && f.at > thru);
			if (!stray && (end < from || f.at > thru))
				continue;
			std::size_t const add = f.text.size() + 16;
			// The first frame rides regardless: one oversized
			// consensus line must shrink the window's frame
			// budget, never empty its evidence entirely.
			if (!ws[i].frames.empty()
			    && bytes + add > detail::kFrameBytes)
				break;
			bytes += add;
			ws[i].frames.push_back(f);
		}
	}

	for (semantic::window const &w : ws) {
		agenda::id const id = key("semantic-extract-v1", w);
		m_extract.push_back({id, id, w, 1, false});
	}
}

template <semantic_backend Backend>
void SemanticEngine<Backend>::offer_extract(extract_work const &w)
{
	if (!w.current || w.source.cues.empty())
		return;
	std::string const range =
		'#' + std::to_string(w.source.cues.front().number) + '-'
		+ std::to_string(w.source.cues.back().number);
	agenda::task t;
	t.id = w.current;
	t.what = agenda::kind::extract;
	t.exported = false;
	t.note = std::string(w.source.source) + range;
	detail::add_key(t.keys, w.source.source);
	// Frame-sensitive: not ready until this cut's ground witness
	// is marked done -- an incomplete cut's asks never run, and
	// no executor-side state has to say so.
	if (m_witness)
		t.deps.push_back(m_witness);
	std::string body = window_body(w.source);
	if (w.attempt > 1)
		body += "\n---\nATTEMPT " + std::to_string(w.attempt)
		      + "\nA previous reply to this exact window was not "
		        "usable: a record cited a cue number that is not in "
		        "it, or the reply was not one JSON object. Cite only "
		        "numbers from " + range + " above.\n";
	m_back.offer(std::move(t), body);
}

template <semantic_backend Backend>
void SemanticEngine<Backend>::tick()
{
	if (!m_catalog)
		return;
	// Nothing landed since the last sweep, and that sweep ran to
	// the end: every lookup would answer as it did.  Otherwise a
	// warm cache answers every window and pair at once, and
	// harvesting them all in one tick would parse the corpus's
	// worth of replies and rank every record against the catalog
	// on the UI thread in one go -- a few of each per tick keeps
	// the replay smooth; the records themselves are loaded and on
	// display from the start.
	// A question whose request failed -- refused, timed out,
	// errored -- lands nothing, ever; it is answered with the
	// failure so the reader can ask again, checked before the
	// gate below, which waits for landings.
	for (answer_work &w : m_answers)
		if (!w.seen && m_back.parked(w.current)) {
			w.value = semantic::answer{
				"The model did not answer: the request failed or "
				"the server was unreachable. Ask again to retry.",
				{}, true};
			w.seen = w.failed = true;
		}
	std::uint64_t const landed = m_back.landed();
	if (landed == m_swept && !m_more)
		return;
	m_swept = landed;
	m_retry = false;
	std::size_t budget = detail::kHarvestPerTick;
	for (extract_work &w : m_extract)
		if (budget && harvest_extract(w))
			--budget;
	m_more = !budget;
	budget = detail::kHarvestPerTick;
	for (judge_work &w : m_judge)
		if (budget && harvest_judge(w))
			--budget;
	m_more |= !budget;
	for (answer_work &w : m_answers)
		harvest_answer(w);
	// Work the disk refused this sweep is owed another.
	m_more |= m_retry;
}

template <semantic_backend Backend>
bool SemanticEngine<Backend>::harvest_extract(extract_work &w)
{
	if (w.seen)
		return false;
	std::string const path = m_back.locate(w.current,
	                                      agenda::kind::extract);
	if (path.empty())
		return false;
	semantic::parse_result parsed = semantic::parse_records(
		slurp(path, semantic::kMaxFile), w.source, m_hash);
	if (!parsed && detail::debug())
		std::fprintf(stderr, "srtview: semantic: %s: %s\n",
		             w.current.hex().c_str(), parsed.error.c_str());
	// The whole reply is published before any of it is judged:
	// every put() stales the concept view, and the judging below
	// reads the view, so interleaving the two rebuilt it once per
	// record -- a whole-catalog union-find and re-tokenization,
	// sixty-four times a window on a warm replay.  Published as a
	// batch, the window costs one rebuild.
	bool published = true;
	for (semantic::record &r : parsed.records)
		published = m_catalog->put(r) && published;
	// Known records stage their candidates too: a judgment that
	// finished after the session that asked it has an artifact
	// nobody harvested, and a record's nearest neighbours change
	// as the catalog grows.  Judged pairs cost a lookup, cached
	// verdicts link on the next tick, and only unjudged pairs ask.
	// Then each name the window asserted about is weighed against
	// the names nearest it in words: a verdict that two names mean
	// one thing folds two entities into one.
	for (semantic::record const &r : parsed.records) {
		if (!m_catalog->find(r.id))
			continue;
		for (std::size_t const at :
		     m_catalog->candidates(r, detail::kJudgeFan))
			stage_judge(r, m_catalog->records()[at]);
		if (std::size_t const at = m_catalog->entity_of(
			    m_catalog->entity_id(r.subject));
		    at != semantic::catalog::npos)
			stage_entities(at);
	}
	// A record the disk would not take leaves the window unseen,
	// and the next sweep reads the reply again.
	if (!published) {
		m_retry = true;
		return true;
	}
	if (detail::debug() && parsed.rejected)
		std::fprintf(stderr, "srtview: semantic: %s: rejected %zu "
		             "record(s)\n", w.current.hex().c_str(),
		             parsed.rejected);
	// A reply that cited outside the window, or was no document,
	// is cached under its id like any other and would be rejected
	// again on every restart; the window is asked again under a
	// new id, told what went wrong, until the cap, then left to
	// the next recipe.  What every attempt got right has already
	// been kept.
	if ((!parsed || parsed.rejected) && w.attempt < detail::kAttempts) {
		++w.attempt;
		w.current = key("semantic-extract-v1-attempt-"
		                + std::to_string(w.attempt), w.source);
		offer_extract(w);
		return true;
	}
	w.seen = true;
	return true;
}

template <semantic_backend Backend>
bool SemanticEngine<Backend>::linked(agenda::id a, agenda::id b) const
{
	return m_catalog && m_catalog->linked(a, b);
}

template <semantic_backend Backend>
void SemanticEngine<Backend>::stage_judge(semantic::record const &fresh,
	                              semantic::record const &candidate)
{
	if (fresh.id == candidate.id || linked(fresh.id, candidate.id)
	    || m_catalog->equivalent(fresh.id, candidate.id))
		return;
	semantic::record const *a = &fresh, *b = &candidate;
	if (b->id < a->id)
		std::swap(a, b);
	std::string const body = detail::judge_body(*a, *b);
	agenda::id const id = detail::id_of("semantic-judge-v1", body, m_hash);
	if (m_judgeAt.find(id) != agenda::index::npos)
		return;
	// The pair is recorded before it is asked: an ask the disk
	// could not note would be a verdict nobody could replay, so
	// on a disk that refuses, nothing is asked and a later session
	// stages the pair again.
	if (!m_catalog->stage(a->id, b->id))
		return;
	m_judgeAt.add(id, m_judge.size());
	m_judge.push_back({id, id, a->id, b->id, 1, false, false});
	offer_judge(m_judge.back());
}

// The entities nearest a fresh one by the words of their names,
// each asked whether it is the same thing.  A mangled name shares
// no word with the one it mangles and finds no partner here; the
// lexicon, where the model already said which spellings belong
// together, is identity itself and needs no partner.
template <semantic_backend Backend>
void SemanticEngine<Backend>::stage_entities(std::size_t fresh)
{
	auto const &ents = m_catalog->entities();
	auto const needle = m_vocab.words(ents[fresh].title);
	struct near { std::size_t at; double score; };
	std::vector<near> best;
	for (std::size_t i = 0; i < ents.size(); ++i) {
		if (i == fresh)
			continue;
		double const score = m_vocab.overlap(
			needle, m_vocab.words(ents[i].title));
		if (score > 0)
			semantic::keep_best(best, {i, score}, detail::kEntityFan,
				[](near const &x, near const &y) {
					return x.score > y.score;
				});
	}
	for (near const &n : best)
		stage_entity_pair(fresh, n.at);
}

template <semantic_backend Backend>
void SemanticEngine<Backend>::lexicon(
	std::vector<std::vector<std::string>> groups)
{
	if (!m_catalog)
		return;
	std::vector<std::vector<agenda::id>> ids;
	ids.reserve(groups.size());
	for (std::vector<std::string> const &group : groups) {
		auto &out = ids.emplace_back();
		out.reserve(group.size());
		for (std::string const &spelling : group)
			out.push_back(m_catalog->entity_id(spelling));
	}
	m_catalog->aliases(std::move(ids));
}

template <semantic_backend Backend>
void SemanticEngine<Backend>::stage_entity_pair(std::size_t a,
	                                            std::size_t b)
{
	auto const &ents = m_catalog->entities();
	agenda::id ia = ents[a].id, ib = ents[b].id;
	if (ib < ia) {
		std::swap(ia, ib);
		std::swap(a, b);
	}
	if (linked(ia, ib))
		return;
	judge_work w{{}, {}, ia, ib, 1, true, false};
	w.id = w.current = detail::id_of("semantic-entity-judge-v1",
	                                 judge_body(w), m_hash);
	if (m_judgeAt.find(w.id) != agenda::index::npos)
		return;
	if (!m_catalog->stage(ia, ib))
		return;
	m_judgeAt.add(w.id, m_judge.size());
	m_judge.push_back(w);
	offer_judge(m_judge.back());
}

// An entity as the judge sees it: its name and as much of what is
// asserted about it as its share of the body holds, each with the
// transcript's own words, so a mangled spelling shows beside what
// the speaker said.  Bounded in bytes, not lines: both entities
// must reach the model whole.
template <semantic_backend Backend>
std::string SemanticEngine<Backend>::entity_body(std::size_t at) const
{
	semantic::catalog::entity const &e = m_catalog->entities()[at];
	std::vector<semantic::record> const &view = m_catalog->consolidated();
	std::string out = "NAME: " + e.title + '\n';
	for (semantic::catalog::predicate const &p : e.predicates)
		for (semantic::catalog::object const &o : p.objects) {
			if (out.size() >= detail::kEntityBytes)
				return out;
			semantic::record const &r = view[o.at];
			out += "- " + r.statement + '\n';
			if (!r.evidence.empty())
				out += "  [" + r.evidence.front().source + '#'
				     + std::to_string(r.evidence.front().first)
				     + "] " + r.evidence.front().quote + '\n';
		}
	return out;
}

template <semantic_backend Backend>
std::string SemanticEngine<Backend>::judge_body(judge_work const &w) const
{
	if (!w.entity) {
		semantic::record const *const a = record_of(w.a);
		semantic::record const *const b = record_of(w.b);
		return a && b ? detail::judge_body(*a, *b) : std::string();
	}
	std::size_t const ea = m_catalog->entity_of(w.a);
	std::size_t const eb = m_catalog->entity_of(w.b);
	if (ea == semantic::catalog::npos || eb == semantic::catalog::npos)
		return {};
	return "ENTITY A\n" + entity_body(ea) + "\n---\nENTITY B\n"
	     + entity_body(eb);
}

template <semantic_backend Backend>
void SemanticEngine<Backend>::offer_judge(judge_work const &w)
{
	std::string body = judge_body(w);
	if (body.empty())
		return;
	if (w.attempt > 1)
		body += "\n---\nATTEMPT " + std::to_string(w.attempt)
		      + "\nA previous reply to this exact pair was not one "
		        "JSON object with relation and rationale. Return the "
		        "constrained JSON object only.\n";
	agenda::task t;
	t.id = w.current;
	t.what = agenda::kind::judge;
	// A verdict on two names restructures the tree; one on two
	// records adds a tie.  Names go first within the kind.
	t.tier = w.entity ? 0 : 1;
	t.exported = false;
	t.note = w.a.hex() + " ~ " + w.b.hex();
	// Frame-sensitive like extract: gated on the cut's witness.
	if (m_witness)
		t.deps.push_back(m_witness);
	if (!w.entity) {
		for (semantic::evidence_span const &e : record_of(w.a)->evidence)
			detail::add_key(t.keys, e.source);
		for (semantic::evidence_span const &e : record_of(w.b)->evidence)
			detail::add_key(t.keys, e.source);
	}
	m_back.offer(std::move(t), body);
}

template <semantic_backend Backend>
bool SemanticEngine<Backend>::harvest_judge(judge_work &w)
{
	if (w.seen)
		return false;
	std::string const path = m_back.locate(w.current,
	                                      agenda::kind::judge);
	if (path.empty())
		return false;
	semantic::verdict v;
	if (!semantic::parse_verdict(slurp(path, semantic::kMaxFile), v)) {
		if (detail::debug())
			std::fprintf(stderr, "srtview: semantic: %s: invalid "
			             "judgment\n", w.current.hex().c_str());
		// Cached under its id, it would be invalid on every
		// restart; the pair is asked again under a new id with
		// feedback until the cap, then left to the next recipe.
		if (std::string const body = judge_body(w);
		    w.attempt < detail::kAttempts && !body.empty()) {
			++w.attempt;
			w.current = detail::id_of(
				"semantic-judge-v1-attempt-"
				+ std::to_string(w.attempt), body, m_hash);
			offer_judge(w);
			return true;
		}
		w.seen = true;
		return true;
	}
	// Seen means linked: a verdict the disk would not take stays
	// unseen, and the next sweep reads it again.
	w.seen = m_catalog->link({w.a, w.b, v.what,
	                          std::move(v.rationale)});
	m_retry |= !w.seen;
	return true;
}

template <semantic_backend Backend>
std::string SemanticEngine<Backend>::answer_bundle(
	std::string_view question,
	std::vector<semantic::citation> &allowed,
	std::vector<agenda::id> &keys)
{
	std::string out;
	if (!m_conversation.empty()) {
		out = "CONVERSATION\n";
		out += m_conversation;
		out += "\n---\n";
	}
	out += "QUESTION\n";
	out += question;
	auto add_span = [&](semantic::evidence_span const &e,
	                    std::string_view head) {
		semantic::citation const c{e.source, e.first, e.last};
		if (std::ranges::find(allowed, c) != allowed.end())
			return;
		// The citation travels as the literal object the reply
		// must copy: a model that has to assemble one from a
		// RECORD line and a bracketed span reaches for the wrong
		// id.
		std::string part = "\n---\n";
		part += head;
		part += "\nCITE {\"source\":\"" + e.source + "\",\"first\":"
		      + std::to_string(e.first) + ",\"last\":"
		      + std::to_string(e.last) + "}\n" + detail::title_line(e.title)
		      + '\n' + e.quote + '\n';
		if (out.size() + part.size() > detail::kAnswerBytes)
			return;
		out += part;
		allowed.push_back(c);
		detail::add_key(keys, e.source);
	};

	std::string retrieval = m_conversation;
	retrieval += '\n';
	retrieval += question;
	auto const head_of = [](semantic::record const &r,
	                        std::string_view label) {
		std::string head(label);
		head += ' ' + r.id.hex() + '\n'
		      + std::string(semantic::name(r.what)) + ": "
		      + r.subject + " — " + r.relation + " — " + r.object
		      + '\n' + r.statement;
		return head;
	};
	// A retrieved concept brings the records judged to contradict
	// it: the reader of a lecture series is owed "X here, Y there"
	// more than either half alone, and a verdict nobody reads was
	// not worth asking for.  Bounded, so the question's own
	// evidence is never crowded out.
	auto const found = m_catalog->search(retrieval, detail::kAnswerRecords);
	std::vector<semantic::record> const &view = m_catalog->consolidated();
	std::size_t contra = detail::kAnswerContra;
	for (std::size_t const at : found) {
		semantic::record const &r = view[at];
		std::string const head = head_of(r, "RECORD");
		for (semantic::evidence_span const &e : r.evidence)
			add_span(e, head);
		for (agenda::id const id : m_catalog->partners(
			     at, semantic::relation::contradicts)) {
			semantic::record const *const o = m_catalog->find(id);
			if (!o || !contra)
				continue;
			--contra;
			std::string const against = head_of(
				*o, "CONTRADICTS " + r.id.hex() + ", RECORD");
			for (semantic::evidence_span const &e : o->evidence)
				add_span(e, against);
		}
	}

	struct raw_hit {
		std::size_t source, cue;
		double score;
	};
	auto const before = [this](raw_hit const &a, raw_hit const &b) {
		if (a.score != b.score)
			return a.score > b.score;
		semantic::cue const &ca = m_sources[a.source].cues[a.cue];
		semantic::cue const &cb = m_sources[b.source].cues[b.cue];
		return m_sources[a.source].id != m_sources[b.source].id
		     ? m_sources[a.source].id < m_sources[b.source].id
		     : ca.number < cb.number;
	};
	std::vector<raw_hit> raw;
	auto const words = m_vocab.words(retrieval);
	for (std::size_t si = 0; si < m_sources.size(); ++si)
		for (std::size_t ci = 0; ci < m_sources[si].cues.size(); ++ci)
			if (double const score = m_vocab.overlap(
				words, m_words[si][ci]); score > 0)
				semantic::keep_best(raw, {si, ci, score},
				                    detail::kAnswerPassages, before);
	// Chosen by score, told in corpus order: the prompt asks for
	// procedure steps in source order, and the bundle is the only
	// place the model can read that order from.
	std::ranges::sort(raw, [](raw_hit const &a, raw_hit const &b) {
		return a.source != b.source ? a.source < b.source
		                            : a.cue < b.cue;
	});
	for (raw_hit const &h : raw) {
		source const &s = m_sources[h.source];
		semantic::cue const &c = s.cues[h.cue];
		add_span({s.id, s.title, c.number, c.number,
		          c.start, c.end, c.text}, "RAW PASSAGE");
	}
	return out;
}

template <semantic_backend Backend>
agenda::id SemanticEngine<Backend>::ask(std::string question)
{
	if (!m_catalog)
		return {};
	std::size_t a = 0, b = question.size();
	while (a < b && (question[a] == ' ' || question[a] == '\t'
	                 || question[a] == '\r' || question[a] == '\n'))
		++a;
	while (b > a && (question[b - 1] == ' '
	                 || question[b - 1] == '\t'
	                 || question[b - 1] == '\r'
	                 || question[b - 1] == '\n'))
		--b;
	question = std::string(detail::utf8_head(
		std::string_view(question).substr(a, b - a), detail::kQuestionBytes));
	if (question.empty())
		return {};
	std::vector<semantic::citation> allowed;
	std::vector<agenda::id> keys;
	std::string const body = answer_bundle(question, allowed, keys);
	agenda::id const id = detail::id_of("semantic-answer-v1", body, m_hash);
	// The same question is the same ask -- unless the last one
	// failed in the pipeline, in which case asking again is the
	// retry, and the renewed offer un-parks the task.
	for (std::size_t i = 0; i < m_answers.size(); ++i)
		if (m_answers[i].id == id) {
			if (!m_answers[i].failed)
				return id;
			m_answers.erase(m_answers.begin() + std::ptrdiff_t(i));
			break;
		}
	answer_work w{id, id, body, question,
	              std::string(detail::utf8_head(question, 120)),
	              std::move(keys), std::move(allowed), {}, 1, false,
	              false};
	if (w.allowed.empty()) {
		w.value = semantic::answer{
			"No indexed record or transcript passage matched the "
			"question closely enough to ground an answer.", {}, true};
		w.seen = true;
		m_answers.push_back(std::move(w));
		return id;
	}
	offer_answer(w, body);
	m_answers.push_back(std::move(w));
	return id;
}

template <semantic_backend Backend>
void SemanticEngine<Backend>::offer_answer(answer_work const &w,
	                              std::string const &body)
{
	agenda::task t;
	t.id = w.current;
	t.keys = w.keys;
	t.what = agenda::kind::answer;
	t.note = w.note;
	t.exported = true;
	m_back.offer(std::move(t), body);
}

template <semantic_backend Backend>
bool SemanticEngine<Backend>::citation_allowed(semantic::citation const &c,
	                                   answer_work const &w) const
{
	return std::ranges::find(w.allowed, c) != w.allowed.end();
}

template <semantic_backend Backend>
void SemanticEngine<Backend>::harvest_answer(answer_work &w)
{
	if (w.seen)
		return;
	std::string const path = m_back.locate(w.current,
	                                      agenda::kind::answer);
	if (path.empty())
		return;
	semantic::answer value;
	bool const parsed = semantic::parse_answer(
		slurp(path, semantic::kMaxFile), value);
	bool const grounded = parsed
		&& std::ranges::all_of(value.citations,
			[this, &w](semantic::citation const &c) {
				return citation_allowed(c, w);
			});
	if (grounded) {
		std::string said = value.text;
		for (semantic::citation const &c : value.citations)
			said += '\n' + label(c);
		converse("USER", w.question);
		converse("ASSISTANT", said);
		w.value = std::move(value);
		w.seen = true;
		return;
	}
	// The rejected artifact stays where it is and is never read
	// again: the next attempt answers the same bundle under a new
	// id, told what went wrong.  A warm replay of the whole chain
	// is cache hits only.
	if (w.attempt < detail::kAttempts) {
		++w.attempt;
		std::string body = w.body;
		body += "\n---\nATTEMPT " + std::to_string(w.attempt)
		      + "\nA previous reply to this exact message was "
		        "discarded: it cited a span that is not in it, or "
		        "cited while insufficient. Copy CITE objects from "
		        "this message only, or set insufficient true and "
		        "cite nothing.\n";
		w.current = detail::id_of("semantic-answer-v1", body, m_hash);
		offer_answer(w, body);
		return;
	}
	w.value = semantic::answer{
		"The model returned an invalid or unsupported citation "
		"three times; the answer was discarded.", {}, true};
	w.seen = true;
}

// One turn into the conversation, which keeps only the tail a
// bundle carries: what fell off it could never be consulted again.
template <semantic_backend Backend>
void SemanticEngine<Backend>::converse(std::string_view speaker,
	                              std::string_view text)
{
	m_conversation += speaker;
	m_conversation += ": ";
	m_conversation += text;
	m_conversation += '\n';
	std::string_view const kept = detail::utf8_tail(
		m_conversation, detail::kConversationBytes);
	m_conversation.erase(0, std::size_t(kept.data()
	                                    - m_conversation.data()));
}

template <semantic_backend Backend>
std::string SemanticEngine<Backend>::label(semantic::citation const &c) const
{
	auto const e = evidence(c);
	return '[' + (e ? std::filesystem::path(e->title).filename().string()
	                : c.source)
	     + " #" + std::to_string(c.first) + "\u2013"
	     + std::to_string(c.last) + ']';
}

template <semantic_backend Backend>
std::optional<semantic::answer> SemanticEngine<Backend>::result(
	agenda::id id) const
{
	for (answer_work const &w : m_answers)
		if (w.id == id)
			return w.value;
	return {};
}

template <semantic_backend Backend>
std::optional<semantic::evidence_span> SemanticEngine<Backend>::evidence(
	semantic::citation const &citation) const
{
	for (source const &s : m_sources) {
		if (s.id != citation.source)
			continue;
		semantic::evidence_span e{s.id, s.title, citation.first,
		                          citation.last, 0.0, 0.0, {}};
		bool first = true;
		for (semantic::cue const &c : s.cues) {
			if (c.number < citation.first || c.number > citation.last)
				continue;
			if (first) {
				e.start = c.start;
				first = false;
			}
			e.end = c.end;
			if (!e.quote.empty())
				e.quote += '\n';
			e.quote += c.text;
		}
		if (!first && citation.last >= citation.first)
			return e;
		return {};
	}
	return {};
}

template <semantic_backend Backend>
semantic::record const *SemanticEngine<Backend>::record_of(agenda::id id) const
{
	return m_catalog ? m_catalog->find(id) : nullptr;
}

template <semantic_backend Backend>
std::vector<semantic::catalog::entity> const &
SemanticEngine<Backend>::entities() const
{
	static std::vector<semantic::catalog::entity> const empty;
	return m_catalog ? m_catalog->entities() : empty;
}

template <semantic_backend Backend>
semantic::catalog::tie_count SemanticEngine<Backend>::ties(std::size_t at) const
{
	return m_catalog ? m_catalog->ties(at)
	                 : semantic::catalog::tie_count{};
}

template <semantic_backend Backend>
std::vector<semantic::record> const &SemanticEngine<Backend>::knowledge() const
{
	static std::vector<semantic::record> const empty;
	return m_catalog ? m_catalog->consolidated() : empty;
}

} // namespace engine

#endif // SRTVIEW_SRC_SEMANTIC_ENGINE_HPP_
