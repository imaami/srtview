// facts.cpp -- see facts.hpp.  Prompts per task kind, transcript
// truncation to the context budget, prompt assembly from snapshots
// and cache files, and the atomic write-on-reply live here.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <string_view>
#include <system_error>

#include "facts.hpp"
#include "llm.h"
#include "semantic.hpp"
#include "slurp.hpp"

namespace {

// The server holds 32k of context; leave room for the reply and the
// reasoning that precedes it, and cut an oversized prompt body.
// The token cap covers the reasoning too, and a hard subject can
// spiral a thinking model: an exhausted budget means an empty reply
// and a parked task, so the cap errs generous.
constexpr std::size_t  kMaxText   = std::size_t{96} * 1024;
constexpr std::int32_t kMaxTokens = 8192;
constexpr std::int32_t kTimeoutS  = 3600;

// The shared stance rule: the observed failure is sentences opening
// with "This ...", "The topic ...", "The thread ..." however firmly
// the individual prompts forbid meta-commentary.
#define SUBJECT_STANCE \
	"Write about the subject, never about the act of reviewing " \
	"it: open every sentence with the subject's own names, " \
	"terms and claims -- never with 'This', 'The " \
	"topic', 'The thread' or any phrase pointing at the material " \
	"or the search. "

constexpr char kLeafPrompt[] =
	"The user message is the complete subtitle text of one video. "
	"Write a condensed description of its factual content: the "
	"subjects covered, claims and decisions made, and the names, "
	"numbers and terms that appear. " SUBJECT_STANCE "Plain text "
	"only; no preamble, no headings, no remarks about the "
	"subtitles themselves.";

constexpr char kNodePrompt[] =
	"Each section of the user message, separated by a line "
	"containing only three dashes, is a condensed summary of one "
	"or more videos from the same collection. Merge the sections "
	"into a single higher-level summary that preserves the "
	"load-bearing facts: subjects, decisions, names, numbers and "
	"terms. Generalize where the sections agree and keep the "
	"notable specifics. " SUBJECT_STANCE "Plain text only; no "
	"preamble, no headings.";

constexpr char kDivePrompt[] =
	"The user message holds labeled sections separated by lines of "
	"three dashes: OVERVIEW summarizes a whole video collection "
	"(the section may be absent), SUMMARIES holds condensed "
	"summaries of the videos where a search pattern matched, and "
	"MATCHES holds the matching subtitle excerpts grouped per "
	"video under == headers. Describe the matched subject itself "
	"in specific detail -- its facts, claims, decisions, numbers "
	"and names, taken from MATCHES only; OVERVIEW and SUMMARIES "
	"are background for interpretation, not sources of extra "
	"facts. End with one sentence on how the subject stands apart "
	"from the collection's other themes, or leave that closing "
	"out if nothing sets it apart. " SUBJECT_STANCE "Plain text "
	"only; no preamble, no headings.";

constexpr char kFocusPrompt[] =
	"The user message holds sections separated by lines of three "
	"dashes: FIRST and SECOND each describe one searched theme "
	"from the same video collection, each may open with a PATTERN "
	"line naming the regex that found it, and MATCHES holds the "
	"subtitle excerpts found by the search you asked for, grouped "
	"per video under == headers. Describe the thread the two "
	"themes share, concretely -- its facts, names and numbers, "
	"taken from MATCHES only -- sharpening toward what is most "
	"distinctive about it; do not average the themes into "
	"generality. Where MATCHES spell one term several ways, never "
	"adopt a mangled spelling as the term: write the likely "
	"correct form and name it once as the intended word behind "
	"the variants. If the excerpts show no genuine "
	"shared thread, reply with the single word NONE. "
	SUBJECT_STANCE "Plain text only; no preamble, no headings.";

#undef SUBJECT_STANCE

constexpr char kProbePrompt[] =
	"The user message holds sections separated by lines of three "
	"dashes. FIRST and SECOND each describe one searched theme "
	"from the same video collection, and each may open with a "
	"PATTERN line naming the regex that found it. TRANSCRIPT "
	"holds raw subtitle lines those patterns matched, grouped per "
	"video under == headers. A FEEDBACK section, when present, "
	"reports what became of your previous attempt; correct "
	"accordingly. Judge whether the two themes genuinely touch. "
	"If they do not, reply with the single word NONE. If they do, "
	"propose one search to run over the collection's subtitle "
	"text to gather concrete evidence of the shared thread. The "
	"subtitles are machine transcriptions of speech, and "
	"TRANSCRIPT shows how the transcriber mangles this "
	"collection's terms: one spoken word or name may surface as "
	"sound-alike respellings written the way the language spells "
	"those sounds, as split or joined compounds, as acronyms or "
	"letter names heard as words, or as a wrong homophone -- a "
	"word from FIRST and a word from SECOND may even be one "
	"intended word under two manglings. For each term your search "
	"leans on -- the thread's own names and terms first, not just "
	"the PATTERN words -- list in your notes its intended form "
	"and every transcription of it you observe or can plausibly "
	"expect, and fold each list into one alternation, the way "
	"h(er|im) collapses two readings into one path. A plausible "
	"variant belongs even when TRANSCRIPT does not show it -- "
	"the search is how you find out -- and a term with no "
	"plausible variants "
	"stays literal. Choose only terms that carry the shared "
	"thread, and join their alternations with | into one flat "
	"pattern: a subtitle line counts as evidence when it touches "
	"any one term, so never demand two terms in one line and "
	"never use lookaheads. A simple pattern is a good answer: "
	"when unsure, prefer a plain union of a few terms over "
	"elaborate structure. Branches that merely repeat a PATTERN "
	"word add nothing -- the collection already runs those "
	"searches; spend them on new terms and their variants. "
	"Note your working briefly, then end with "
	"exactly one line of this form: REGEX: followed by one PCRE2 "
	"regular expression, case-insensitive via (?i:...) where "
	"sensible, no delimiters or flags outside the pattern.";

constexpr char kTermsPrompt[] =
	"The user message is a numbered excerpt from the machine-"
	"transcribed subtitles of one video: lines of the form "
	"#N [H:MM:SS] text. Lines of the form @ [H:MM:SS] text are "
	"on-screen text read from the video image at that moment, "
	"showing the written spelling of "
	"spoken terms: use them to anchor TERM spellings and to unite "
	"mangled spoken variants with the written form, but the reader "
	"misreads too, so weigh them as one more witness, not as truth. "
	"A frame line is never a cue: CUES lists only #numbers. "
	"Identify the terms worth an index entry: recurring "
	"vocabulary specific to this material that a newcomer would "
	"look up -- proper names, coined or specialized terms and "
	"phrases, acronyms and other shortenings. Skip ordinary "
	"words, bare numbers and single "
	"letters. For each, most important first, at most twelve, emit "
	"one block of lines:\n"
	"TERM: the likely correct spelling\n"
	"KIND: one of term, name, acronym, abbreviation, symbol, "
	"other\n"
	"MEANS: the expansion, only for an acronym or abbreviation "
	"the excerpt itself explains\n"
	"SEEN: every spelling observed in the excerpt, verbatim, "
	"separated by |\n"
	"GLOSS: one sentence saying what it is, drawn from the excerpt "
	"only\n"
	"CUES: the #numbers of the lines it appears on, space "
	"separated\n"
	"Blocks are separated by one blank line. The subtitles are "
	"machine transcriptions of speech, so ONE spoken term often "
	"appears under several mangled spellings. Group them into one "
	"block: if the excerpt has kelvane, calvain and kellvayne for "
	"the name Kelvane, that is TERM: Kelvane with SEEN: kelvane "
	"| calvain | kellvayne. A mangled spelling never gets its own "
	"block, and "
	"TERM is the correct spelling even when the excerpt only "
	"misspells it. Only terms the excerpt actually contains; only "
	"cue numbers that appear above. If nothing qualifies, reply "
	"with the single word NONE. No other text.";

constexpr char kMergePrompt[] =
	"The user message lists index terms collected from machine-"
	"transcribed subtitles of one spoken-word corpus, one term "
	"per "
	"line. Because the transcription is automatic, one spoken "
	"term often appears as several separately listed spellings "
	"(Kelvane may also be listed as kelvane, calvain, kellvayne). "
	"Find "
	"such groups. Emit one line per group:\n"
	"MERGE: the correct spelling | wrong spelling | wrong "
	"spelling\n"
	"Every name must be copied exactly from the list. Merge only "
	"spellings and mishearings of the SAME word or name; related "
	"but different things -- two distinct things whose names "
	"merely sound alike -- stay separate. Also judge worth: a "
	"listed "
	"term that is everyday vocabulary of the material's own "
	"field, a word the corpus uses constantly without ever "
	"defining, rather than a specific name or coined term, earns "
	"one "
	"line:\n"
	"DROP: the term\n"
	"Never drop a specific name, however misspelled. If there is "
	"nothing to merge and nothing to drop, reply with the single "
	"word NONE. No other text.";

constexpr char kSpellPrompt[] =
	"The lines are machine transcriptions of speech: the "
	"transcriber writes what it HEARS, so a name it does not know "
	"often comes out as a wrong word that sounds alike -- "
	"Quenneville has been transcribed as when a vill. TERM A is "
	"well attested in these subtitles. TERM B is rare, and the "
	"question is whether B is really the speaker saying A, heard "
	"wrong. Do the substitution test: re-read each of B's lines "
	"with A in B's place (they are shown substituted already). If "
	"the sentences then read as natural speech about A -- same "
	"kind of subject, same kind of actions -- and A and B could "
	"sound alike when spoken quickly, then B is a mistranscription "
	"of A. Spelling distance means nothing. Think it through, then "
	"answer with exactly one word on its own final line: SAME or "
	"DIFFERENT.";

constexpr char kExtractPrompt[] =
	"The user message is one numbered window from one transcript; "
	"lines of the form @ [H:MM:SS] text are on-screen text read "
	"from the video image, context for spellings and names only. "
	"Extract only atomic knowledge directly supported by its cue "
	"lines. A record is one factual claim, relationship, procedure "
	"step, operational rule, or definition; split compound claims. "
	"Each record is a triple and its sentence. SUBJECT is the thing "
	"the record is about, named bare, the way the material names it: "
	"put the aspect into the relation and the object, never into the "
	"subject (subject 'the northern route', relation 'was closed "
	"during', object 'the winter months' -- not subject "
	"'northern-route closure'). RELATION is the verb phrase that "
	"links them. "
	"OBJECT is the complement, itself a bare name when it names a "
	"thing. Use one spelling for one thing across records. STATEMENT "
	"is the whole sentence, standing alone without referring to the "
	"transcript. CUES must contain the exact #numbers that support "
	"the statement. Never cite a number not present in the message "
	"and never infer a missing step. Return the constrained JSON "
	"object only.";

constexpr char kJudgePrompt[] =
	"The user message holds two items with exact transcript evidence: "
	"either NEW and CANDIDATE, two atomic records, or ENTITY A and "
	"ENTITY B, two names with assertions made about each. Judge their "
	"relationship. For records: same means they assert the same "
	"durable fact even if worded differently; related means a useful "
	"connection without identity; contradicts means their supported "
	"assertions conflict; novel means no material relationship. For "
	"names: same means both name one thing -- a transcription may "
	"mangle a name, so weigh what is said about each against the "
	"speaker's own words; related means distinct but connected things; "
	"novel means unrelated; contradicts does not apply. Evidence "
	"outranks labels. Return the constrained JSON object only.";

constexpr char kAnswerPrompt[] =
	"Answer the QUESTION using only the evidence spans in the user "
	"message. Each span carries a CITE line holding the exact JSON "
	"object that cites it; every citation must be a copy of one CITE "
	"object, nothing else -- a RECORD id is never a citation. The "
	"citations array is the only place a citation goes: the answer "
	"text is prose for a reader and carries no CITE objects, ids or "
	"cue numbers. A span under a CONTRADICTS heading belongs to a "
	"record judged to contradict the record it names: where the "
	"sources disagree, say so and cite both sides. Synthesize "
	"procedure steps "
	"in source order when the evidence supports a procedure. If the "
	"bundle cannot support the answer, set insufficient true, explain "
	"briefly what is missing, and cite nothing. Return the constrained "
	"JSON object only.";

// System prompt per task kind, in agenda::kind order.  The views
// wrap NUL-terminated literals, so .data() satisfies the C API
// below.
constexpr std::string_view kPromptOf[] = {
	kLeafPrompt, kNodePrompt, kDivePrompt, kFocusPrompt,
	kProbePrompt, kTermsPrompt, kMergePrompt, kSpellPrompt,
	kExtractPrompt, kJudgePrompt, kAnswerPrompt,
};

// Connect refusals in a row before the pipeline parks itself for
// the session; anything else the server says resets the count.
constexpr int kRefusalCap = 3;


// The response schema a kind is asked under; null for prose.
char const *schema_of(agenda::kind what)
{
	switch (what) {
	case agenda::kind::extract:
		return semantic::records_schema().data();
	case agenda::kind::judge:
		return semantic::verdict_schema().data();
	case agenda::kind::answer:
		return semantic::answer_schema().data();
	default:
		return nullptr;
	}
}

// A reply's context travels as the task's user data, heap-owned:
// exactly one callback per accepted task makes adoption in
// deliver() the release.  The path is the plan-stable tmp target;
// want is the artifact name the chain owed at submit time and epoch
// the reset generation the ask belongs to, so completed() can tell
// a superseded reply apart; the task rides whole, the journal line
// and file head prepared while the task was known.
struct reply_ctx {
	Facts        *self;
	std::string   path;
	std::string   want;
	std::string   line;
	std::string   head;
	agenda::task  t;
	std::uint64_t epoch;
};

// A dive file opens with its regex and a focus file with the regex
// whose search fed it: assembly and harvest downstream want the
// prose and the pattern both, and human readers get the same favor.
// Kinds with an empty prefix carry no head.
constexpr std::string_view kHeadPfx[] = {
	"", "", "PATTERN ", "REGEX: ", "", "", "", "",
	"", "", "",
};
static_assert(std::size(kHeadPfx) == std::size(kPromptOf)
              && std::size(kHeadPfx) == agenda::kind_count,
              "per-kind tables mirror agenda::kind");

std::string head_of(agenda::task const &t)
{
	std::string_view const pfx = kHeadPfx[std::size_t(t.what)];
	return pfx.empty() || t.note.empty()
	       ? std::string()
	       : std::string(pfx) + t.note + "\n\n";
}

bool debug()
{
	char const *v = std::getenv("SRTVIEW_DEBUG");
	return v && *v;
}

std::string cacheDir()
{
	char const *base = std::getenv("XDG_CACHE_HOME");
	std::string dir = base && *base ? base : std::string();
	if (dir.empty()) {
		char const *home = std::getenv("HOME");
		dir = home && *home ? home : ".";
		dir += "/.cache";
	}
	return dir + "/srtview/facts";
}

// SRTVIEW_LLM=[host][:port]; the last colon splits host from port,
// so bare IPv6 literals are not handled (bracket-free by choice:
// this is a loopback-adjacent knob, not a URL parser).
struct endpoint {
	std::string   host;
	std::uint16_t port;
};

endpoint serverEnv()
{
	endpoint ep{};
	char const *v = std::getenv("SRTVIEW_LLM");
	if (!v || !*v)
		return ep;

	std::string_view s(v);
	std::size_t const colon = s.rfind(':');
	if (colon != std::string_view::npos) {
		unsigned long const p =
			std::strtoul(v + colon + 1, nullptr, 10);
		if (p && p <= 65535)
			ep.port = std::uint16_t(p);
		s = s.substr(0, colon);
	}
	ep.host = s;
	return ep;
}

// A recipe is the semantic contract of one cached answer.  It is a
// filename dimension in vault, deliberately separate from content:
// content edits retain the vault's adoption policy, while changing a
// prompt, schema or generation setting must miss.  The endpoint and
// optional operator-supplied model identity keep two servers/models
// from sharing answers accidentally.  llama-server does not expose a
// stable model digest through the chat request itself, hence the
// explicit SRTVIEW_LLM_MODEL_ID escape hatch for a server whose model
// changes in place.
vault::recipes recipe_ids(vault::hash8_fn h)
{
	endpoint const ep = serverEnv();
	char const *const model = std::getenv("SRTVIEW_LLM_MODEL_ID");
	vault::recipes out;
	for (std::size_t i = 0; i < out.size(); ++i) {
		std::string text{"srtview-facts-recipe-v1\nkind="};
		text += agenda::name(agenda::kind(i));
		char const *const schema = schema_of(agenda::kind(i));
		text += "\nschema=";
		text += schema ? schema : "text/plain;utf-8";
		text += "\nmax_tokens=";
		text += std::to_string(kMaxTokens);
		text += "\ntemperature=0\nendpoint=";
		text += ep.host.empty() ? "127.0.0.1" : ep.host;
		text += ':';
		text += std::to_string(ep.port ? ep.port : 8080);
		text += "\nmodel=";
		text += model && *model ? model : "unspecified";
		text += "\nprompt=";
		text += kPromptOf[i];
		out[i] = h(text);
	}
	return out;
}

// Generation is a sustained full-power burn on the accelerator, and
// a corpus queues many in a row: the default gap gives the silicon
// breathing room.  SRTVIEW_LLM_PACE=<seconds> widens it, 0 disables.
constexpr std::int32_t kPaceDefaultS = 3;
constexpr std::int32_t kPaceMaxS     = 3600;

std::int32_t paceEnv()
{
	char const *v = std::getenv("SRTVIEW_LLM_PACE");
	if (!v || !*v)
		return kPaceDefaultS;

	long const s = std::strtol(v, nullptr, 10);
	return s <= 0 ? 0 : s > kPaceMaxS ? kPaceMaxS
	                                  : std::int32_t(s);
}

// Never cut a UTF-8 sequence: back off continuation bytes.  Reading
// text[n] at size() is the const string's terminator, not past-the-
// end.
std::string_view clip_to(std::string const &text, std::size_t limit)
{
	std::size_t n = std::min(text.size(), limit);
	while (n && (static_cast<unsigned char>(text[n]) & 0xc0) == 0x80)
		--n;
	return {text.data(), n};
}

std::string_view clip(std::string const &text)
{
	return clip_to(text, kMaxText);
}

// Atomic cache write: all-or-nothing via .tmp and rename, so a
// reader on any thread sees either nothing or a whole summary.
bool store(std::string const &path, std::string const &head,
           char const *text, std::size_t n)
{
	std::string const tmp = path + ".tmp";
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		out.write(head.data(), std::streamsize(head.size()));
		out.write(text, std::streamsize(n));
		out.put('\n');
		// close() flushes: a full disk surfaces here, not in the
		// destructor after the check.
		out.close();
		if (!out) {
			std::remove(tmp.c_str());
			return false;
		}
	}
	return std::rename(tmp.c_str(), path.c_str()) == 0;
}

void add_section(std::string &to, std::string const &sec)
{
	if (sec.empty())
		return;
	if (!to.empty())
		to += "\n---\n";
	to += sec;
}

// One provenance line per cache file, appended when the file is
// created: the dedupe means a file completes once ever, so the
// journal stays one creation-ordered line per artifact.  Human
// eyes only; nothing reads it back.
void journal(std::string const &dir, std::string const &line)
{
	std::ofstream out(dir + "/journal.txt",
	                  std::ios::app | std::ios::binary);
	out << line << '\n';
}

// The journal line for a task: kind (nodes with their tier), id,
// the note (a dive's pattern), the supportive mark, and the inputs.
std::string journal_line(agenda::task const &t)
{
	std::string line{agenda::name(t.what)};
	if (t.what == agenda::kind::node) {
		line += '.';
		line += std::to_string(t.tier);
	}
	line += ' ';
	line += t.id.hex();
	if (!t.note.empty())
		line += " [" + t.note + ']';
	if (!t.exported)
		line += " (supportive)";
	if (t.deps.empty())
		return line;
	line += " <-";
	for (agenda::id const d : t.deps)
		line += ' ' + d.hex();
	return line;
}

} // namespace

Facts::Facts(vault::hash8_fn h)
	: m_dir(cacheDir()), m_vault(m_dir, h, recipe_ids(h)), m_hash(h)
{
	std::error_code const &ec = m_vault.error();
	if (!ec) {
		endpoint const ep = serverEnv();
		m_llm = llm_create(ep.host.empty() ? nullptr
		                                   : ep.host.c_str(),
		                   ep.port);
		llm_pace(m_llm, paceEnv() * 1000);
	} else {
		// The same class of state flip the offline latch logs:
		// a pipeline that will never run must say so once.
		std::fprintf(stderr, "srtview: facts: %s: %s, pipeline "
		             "disabled\n", m_dir.c_str(),
		             ec.message().c_str());
	}
}

Facts::~Facts()
{
	{
		std::lock_guard const lock(m_mtx);
		m_down = true;
	}
	// Joins the worker; queued tasks cancel through deliver(),
	// which m_down keeps from advancing.
	llm_destroy(&m_llm);
}

void Facts::offer(agenda::id key, std::string const &utf8Text)
{
	if (!key || utf8Text.empty())
		return;

	std::lock_guard const lock(m_mtx);
	if (!m_llm)
		return;
	// The witness registers before the plan is consulted: current
	// inputs define status, never the reverse -- a done id must
	// not keep a changed transcript out of the vault.  The hash
	// covers the full text: clipping is presentation, and a
	// clipped hash would tie identity to the clip limit.
	m_vault.content(key, m_hash(utf8Text));
	agenda::task t;
	t.id = key;
	t.keys = {key};
	stage(std::move(t), utf8Text);
}

void Facts::corpus(std::vector<agenda::task> nodes)
{
	std::lock_guard const lock(m_mtx);
	if (!m_llm)
		return;
	for (agenda::task &t : nodes)
		if (settle(t))
			m_plan.add(std::move(t));
	advance();
}

void Facts::retire(std::vector<agenda::id> const &stale)
{
	std::lock_guard const lock(m_mtx);
	for (agenda::id const id : stale)
		if (m_plan.status(id) == agenda::plan::state::pending)
			m_plan.fail(id);
}

void Facts::hold(bool on)
{
	std::lock_guard const lock(m_mtx);
	if (m_hold == on)
		return;
	m_hold = on;
	if (!on)
		advance();
}

void Facts::mark(agenda::id fact)
{
	if (!fact)
		return;
	std::lock_guard const lock(m_mtx);
	m_plan.done(fact);
	advance();
}

void Facts::offer(agenda::task t, std::string const &snapshot)
{
	if (!t.id || snapshot.empty())
		return;

	std::lock_guard const lock(m_mtx);
	if (!m_llm)
		return;
	stage(std::move(t), snapshot);
}

// m_mtx held.  Shape before status: the vault registers the task's
// current shape whatever the plan says of it, and a known task
// takes that shape back into the plan with its renewed offer --
// the plan's copy is what submission assembles over, so a dive
// whose video set grew must not be written over the set it had,
// whether that set failed, waits, or is on the wire.  A resolved
// file is marked done rather than queued -- the bookkeeping half
// that must never be skipped, since a dependent of the hit may
// just have gone ready -- and counted as landed.  True when the
// task owes an ask.
bool Facts::settle(agenda::task const &t)
{
	bool const have = !m_vault.resolve(t).empty();
	if (!m_plan.renew(t)
	    && m_plan.status(t.id) != agenda::plan::state::unknown)
		return false;
	if (!have)
		return true;
	m_plan.done(t.id);
	++m_landed;
	return false;
}

// m_mtx held.  Only a miss spends a body; a task offered again --
// un-parked, or reshaped while it waits or runs -- spends one too,
// in place of any it still had.  A question is the
// reader's own retry of a pipeline parked behind an unreachable
// server: the latch opens for it, and for the background behind
// it, which closes it again after the next run of refusals --
// each question costs one connection attempt while the server is
// down, which is what asking again means.
void Facts::stage(agenda::task t, std::string const &body)
{
	if (m_offline && t.what == agenda::kind::answer) {
		m_offline = false;
		m_refused = 0;
		std::fprintf(stderr, "srtview: facts: asked again, "
		             "pipeline resumed\n");
	}
	if (settle(t)) {
		std::erase_if(m_bodies, [&t](auto const &b) {
			return b.first == t.id;
		});
		m_bodies.emplace_back(t.id, std::string(clip(body)));
		m_plan.add(std::move(t));
	}
	advance();
}

void Facts::heat(agenda::id key, double add)
{
	if (!key)
		return;

	if (debug())
		std::fprintf(stderr, "srtview: facts: heat %s +%.3f\n",
		             key.hex().c_str(), add);
	std::lock_guard const lock(m_mtx);
	m_plan.heat(key, add);
}

void Facts::decay(double keep)
{
	if (debug())
		std::fprintf(stderr, "srtview: facts: decay %.3f\n",
		             keep);
	std::lock_guard const lock(m_mtx);
	m_plan.decay(keep);
}

void Facts::reset()
{
	std::uint64_t cancel[2] = {};
	{
		std::lock_guard const lock(m_mtx);
		++m_epoch;
		for (std::size_t i = 0; i < 2; ++i)
			cancel[i] = m_lane[i].ask;
		m_plan.reset();
		m_bodies.clear();
	}
	// The epoch already condemns the in-flight reply; cancelling
	// the request too means the new corpus never waits out a
	// generation nobody will keep.  The cancel runs OUTSIDE the
	// mutex, exactly like ~Facts(): a still-queued task's callback
	// runs synchronously on this thread inside llm_cancel(), and
	// completed() takes the lock.  Capturing the id makes the
	// cancel at-most-once and immune to the unlock races -- ids
	// are never reused, a retired id no-ops, and a fresh ask
	// submitted meanwhile carries an id this value cannot touch.
	for (std::uint64_t const id : cancel)
		if (id)
			llm_cancel(m_llm, id);
}

bool Facts::cached(agenda::task const &t)
{
	std::lock_guard const lock(m_mtx);
	return !m_vault.resolve(t).empty();
}

std::string Facts::fetch(agenda::task const &t)
{
	std::lock_guard const lock(m_mtx);
	return slurp(m_vault.resolve(t));
}

std::uint64_t Facts::landed() const
{
	std::lock_guard const lock(m_mtx);
	return m_landed;
}

bool Facts::parked(agenda::id id) const
{
	std::lock_guard const lock(m_mtx);
	return m_offline || m_plan.status(id) == agenda::plan::state::parked;
}

std::string Facts::locate(agenda::id plan, agenda::kind k) const
{
	std::lock_guard const lock(m_mtx);
	return m_vault.locate(plan, k);
}

std::string Facts::artifact(agenda::id id)
{
	std::lock_guard const lock(m_mtx);
	return m_vault.resolve(id);
}

// On the llm worker thread; the tmp write stays outside the lock,
// and the artifact is named under it in completed().
// A tiny model wedged in a generation loop pads its reply with one
// line repeated hundreds of times; caching that would enshrine the
// wedge as knowledge.  Eight identical consecutive non-empty lines
// is far past anything a legitimate reply produces (structured
// replies always interleave field lines), and such a reply is
// treated as a failed ask: nothing stored, the task parks, and a
// later session -- or a better model behind the same content id --
// answers it properly.
static bool degenerate(char const *text, std::size_t size)
{
	std::string_view rest(text, size);
	std::string_view prev;
	std::size_t run = 0;
	while (!rest.empty()) {
		std::size_t const nl = rest.find('\n');
		std::string_view const line = rest.substr(0, nl);
		rest = nl == std::string_view::npos
		       ? std::string_view() : rest.substr(nl + 1);
		if (line.empty())
			continue;
		run = line == prev ? run + 1 : 1;
		if (run >= 8)
			return true;
		prev = line;
	}
	return false;
}

void Facts::deliver(void *ud, std::uint64_t, int status,
                    char const *text, std::size_t size)
{
	std::unique_ptr<reply_ctx> const ctx(
		static_cast<reply_ctx *>(ud));
	bool const looped = status == LLM_OK && size
	                 && degenerate(text, size);
	// A schema-constrained reply the token cap cut short is not a
	// document at all; like a loop it is a failed ask, never an
	// artifact.
	bool const cut = status == LLM_OK && size && !looped
	              && schema_of(ctx->t.what)
	              && !semantic::well_formed({text, size});
	if ((looped || cut) && debug())
		std::fprintf(stderr, "srtview: facts: %s: %s reply "
		             "refused\n", ctx->t.id.hex().c_str(),
		             looped ? "degenerate" : "truncated");
	bool const wrote = status == LLM_OK && size && !looped && !cut
	                && store(ctx->path, ctx->head, text, size);
	if (!wrote && !looped && !cut && debug())
		std::fprintf(stderr, "srtview: facts: %s: %s\n",
		             ctx->t.id.hex().c_str(),
		             llm_strerror(status));
	ctx->self->completed(ctx->t, ctx->path, ctx->want, ctx->line,
	                     ctx->epoch, status, wrote);
}

void Facts::completed(agenda::task const &t, std::string const &tmp,
                      std::string const &want, std::string const &line,
                      std::uint64_t epoch, int status, bool wrote)
{
	std::lock_guard const lock(m_mtx);
	for (lane &l : m_lane)
		if (l.task == t.id)
			l = {};
	// Naming is the locked half of the write: place() computes the
	// content-chained target, sweeps the plan id down to one file,
	// and the rename publishes (R2).  Both vault reads go by id --
	// the submitted copy must never re-register, or a reload's
	// re-shaped entry would be overwritten and the generation
	// comparison would satisfy itself.  An obsolete completion
	// must not be credited: done() would hand the replacement
	// generation a stale artifact, fail() would park it for the
	// session.  One reshaped under way -- its target moved while
	// it ran, in this epoch -- goes back to pending and is asked
	// again over its current inputs; the epoch closes the reset
	// window itself, where between reset() and the re-offers the
	// vault still describes the old corpus and the plan knows no
	// such task.  The tmp dies; absence retries.
	bool const fresh = epoch == m_epoch && !want.empty();
	bool const current = fresh && m_vault.target(t.id) == want;
	std::string const target = wrote && current
		? m_vault.place(t.id) : std::string();
	if (!target.empty()
	    && std::rename(tmp.c_str(), target.c_str()) == 0) {
		journal(m_dir, line);
		m_plan.done(t.id);
		++m_landed;
	} else {
		if (wrote)
			std::remove(tmp.c_str());
		if (current)
			m_plan.fail(t.id);
		else if (fresh && m_plan.requeue(t.id)) {
			if (debug())
				std::fprintf(stderr, "srtview: facts: %s: "
				             "reshaped under way, asked again\n",
				             t.id.hex().c_str());
		} else if (debug())
			std::fprintf(stderr, "srtview: facts: %s: obsolete "
			             "completion dropped\n",
			             t.id.hex().c_str());
	}

	// A cancelled task proves nothing about the server; anything
	// answered, even an error, proves it is there.
	if (status == LLM_ERR_CONNECT)
		++m_refused;
	else if (status != LLM_ERR_CANCEL)
		m_refused = 0;
	if (!m_offline && m_refused >= kRefusalCap) {
		m_offline = true;
		std::fprintf(stderr, "srtview: facts: server "
		             "unreachable, pipeline parked\n");
	}
	advance();
}

// What each lane takes: answers are urgent, everything else is
// background.  Heat is unbounded, so a hot background task can
// outrank an answer; each free lane therefore peeks among its own
// candidates, and a busy lane's favourite never hides the other
// lane's.
static bool background(agenda::task const &t)
{
	return t.what != agenda::kind::answer;
}

static bool urgent(agenda::task const &t)
{
	return t.what == agenda::kind::answer;
}

// The semantic chain -- extraction and the judgments that
// consolidate it -- is what a user waits hours for on a slow
// machine; summaries, dives and terms can follow it.
static bool ground(agenda::task const &t)
{
	return t.what == agenda::kind::extract
	    || t.what == agenda::kind::judge;
}

// What the background lane may run while the corpus is still
// reading itself: everything that depends on transcripts and
// topics alone -- leaves, nodes, dives, probes, focus writes.
// The frame-keyed asks wait for the cut they will actually be
// about: extract and judge (the ground chain), and terms, whose
// windows re-key with the frames just the same -- the startup cut
// can stage terms before the reading plan exists, and the re-cut
// retires those strays before they ever cost the model a token.
// Everything else keeps the model busy with what it can use,
// immediately and all the time.
static bool patient(agenda::task const &t)
{
	return background(t) && !ground(t)
	    && t.what != agenda::kind::terms;
}

// m_mtx held.  Fills every free lane until nothing is ready for it;
// a task whose submission fails parks and the loop moves on.
void Facts::advance()
{
	static constexpr agenda::plan::fit_fn kFit[2] = {background, urgent};
	for (std::size_t i = 0; i < 2; ++i) {
		lane &l = m_lane[i];
		while (!m_down && !m_offline && m_llm && !l.task) {
			// The background lane's policy: while the hold is
			// on, frame-independent work only -- the hold
			// reshapes the lane, never silences it.  Off the
			// hold, ground truth leads: extraction and
			// judgment run before the rest.  Heat still
			// orders within each class, and the urgent lane
			// answers questions regardless.
			agenda::id next{};
			if (i == 1)
				next = m_plan.peek(urgent);
			else if (m_hold)
				next = m_plan.peek(patient);
			else if (!(next = m_plan.peek(ground)))
				next = m_plan.peek(kFit[i]);
			if (!next)
				break;
			m_plan.start(next);
			agenda::task const *t = m_plan.get(next);
			if (!t || !submit(*t, i)) {
				// A submission that dies here parks silently
				// for the session; that must at least be on
				// the record when anyone is watching.
				if (debug())
					std::fprintf(stderr, "srtview: facts: "
					             "park %s at submit\n",
					             next.hex().c_str());
				m_plan.fail(next);
				continue;
			}
			l.task = next;
		}
	}
}

// m_mtx held.
bool Facts::submit(agenda::task const &t, std::size_t lane)
{
	// The context first: a leaf's snapshot is spent by assemble(),
	// so nothing fallible may sit between spending it and the ask.
	auto *ctx = new (std::nothrow) reply_ctx{this, m_vault.tmp(t),
	                                         m_vault.target(t),
	                                         journal_line(t),
	                                         head_of(t), t,
	                                         m_epoch};
	if (!ctx)
		return false;
	std::string const body = assemble(t);
	if (body.empty()) {
		delete ctx;
		return false;
	}
	llm_task const ask = {
		.system      = kPromptOf[std::size_t(t.what)].data(),
		.prompt      = body.c_str(),
		.json_schema = schema_of(t.what),
		.temperature = 0.0,
		.max_tokens  = kMaxTokens,
		.timeout_s   = kTimeoutS,
		.urgent      = lane == 1,
	};
	// The pace is a thermal policy for autonomous background work,
	// not UI latency: an answer goes urgent, which the llm puts
	// ahead of any task not yet on the wire and in front of no gap.
	std::uint64_t const id = llm_ask(m_llm, &ask, deliver, ctx);
	if (!id) {
		delete ctx;
		return false;
	}
	m_lane[lane].ask = id;
	if (debug())
		std::fprintf(stderr, "srtview: facts: ask %s %s\n",
		             agenda::name(t.what).data(),
		             t.id.hex().c_str());
	return true;
}

// m_mtx held.  A leaf spends its snapshot; a node reads its
// children's cache files, present by dependency gating (an empty
// read means a raced cache wipe -- the caller parks the task); a
// dive layers required and best-effort context under its snapshot.
std::string Facts::assemble(agenda::task const &t)
{
	switch (t.what) {
	case agenda::kind::leaf:
		return spend_body(t.id);

	case agenda::kind::node:
		return assemble_node(t);

	case agenda::kind::dive:
		return assemble_dive(t);

	case agenda::kind::focus:
		return assemble_focus(t);

	case agenda::kind::probe:
		return assemble_probe(t);

	case agenda::kind::terms:
	case agenda::kind::merge:
	case agenda::kind::spell:
	case agenda::kind::extract:
	case agenda::kind::judge:
	case agenda::kind::answer:
		// A caller-built snapshot -- the numbered window or the
		// term directory listing -- nothing to layer.
		return spend_body(t.id);
	}

	return {};
}

// FIRST and SECOND are the pair's finished dives, read whole: their
// PATTERN heads travel along, so the model sees both the prose and
// the regexes whose threads it is asked to join or refuse.
std::string Facts::pair_sections(agenda::task const &t)
{
	std::string all;
	for (agenda::id const dep : t.deps) {
		std::string const part = slurp(m_vault.resolve(dep));
		if (part.empty())
			return {};
		all += all.empty() ? "FIRST\n" : "\n---\nSECOND\n";
		all += part;
	}
	return all;
}

// The probe reads the pair plus the caller's snapshot: a raw
// TRANSCRIPT sample of both dives' matched lines, joined by a
// FEEDBACK tail on a retry.  The snapshot sits last, so clipping
// trims the sample before it ever touches the prose.
std::string Facts::assemble_probe(agenda::task const &t)
{
	std::string all = pair_sections(t);
	if (all.empty())
		return {};
	add_section(all, spend_body(t.id));
	return std::string(clip(all));
}

// The write pairs the dives with the excerpts the probe's search
// found (the snapshot, required).  MATCHES is the sanctioned fact
// source, so it claims the window first, exactly as in a dive; the
// 5 covers the "\n---\n" joint add_section() will spend.
std::string Facts::assemble_focus(agenda::task const &t)
{
	std::string const pair = pair_sections(t);
	if (pair.empty())
		return {};
	std::string const hits = spend_body(t.id);
	if (hits.empty())
		return {};
	std::string const m = "MATCHES\n" + hits;
	std::string all(clip_to(pair, m.size() + 5 < kMaxText
	                              ? kMaxText - m.size() - 5 : 0));
	add_section(all, m);
	return std::string(clip(all));
}

std::string Facts::assemble_node(agenda::task const &t)
{
	std::string all;
	for (agenda::id const dep : t.deps) {
		std::string const part = slurp(m_vault.resolve(dep));
		if (part.empty())
			return {};
		if (!all.empty())
			all += "\n---\n";
		all += part;
	}
	return std::string(clip(all));
}

// OVERVIEW (refs, attached only when cached) --- SUMMARIES (deps,
// dependency-gated, missing parks the task) --- MATCHES (the
// snapshot).  The snapshot is spent last, so parking on a missing
// input keeps it for a later plan.
std::string Facts::assemble_dive(agenda::task const &t)
{
	std::string over;
	for (agenda::id const ref : t.refs) {
		std::string const part = slurp(
			m_vault.locate(ref, agenda::kind::node));
		if (part.empty())
			continue;
		over += over.empty() ? "OVERVIEW\n" : "\n\n";
		over += part;
	}

	std::string sums;
	for (agenda::id const dep : t.deps) {
		std::string const part = slurp(m_vault.resolve(dep));
		if (part.empty())
			return {};
		sums += sums.empty() ? "SUMMARIES\n" : "\n\n";
		sums += part;
	}

	std::string const hits = spend_body(t.id);
	if (hits.empty())
		return {};
	// The excerpts are the sanctioned fact source: they claim the
	// window first and the background context gets what remains,
	// so clipping can only ever trim background -- never the one
	// section the prompt tells the model to take facts from.  The
	// 5 covers the "\n---\n" joint add_section() will spend.
	std::string const m = "MATCHES\n" + hits;
	std::string ctx;
	add_section(ctx, over);
	add_section(ctx, sums);
	std::string all(clip_to(ctx, m.size() + 5 < kMaxText
	                             ? kMaxText - m.size() - 5 : 0));
	add_section(all, m);
	return std::string(clip(all));
}

// m_mtx held.  Takes and erases the snapshot stored under the id.
std::string Facts::spend_body(agenda::id which)
{
	for (std::size_t i = 0; i < m_bodies.size(); ++i) {
		if (m_bodies[i].first != which)
			continue;
		std::string spent = std::move(m_bodies[i].second);
		m_bodies.erase(m_bodies.begin() + std::ptrdiff_t(i));
		return spent;
	}
	return {};
}
