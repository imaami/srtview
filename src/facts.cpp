// facts.cpp -- see facts.hpp.  Prompts per task kind, transcript
// truncation to the context budget, prompt assembly from snapshots
// and cache files, and the atomic write-on-reply live here.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <new>
#include <string_view>
#include <system_error>

#include "facts.hpp"
#include "llm.h"

namespace {

// The server holds 32k of context; leave room for the reply and the
// reasoning that precedes it, and cut an oversized prompt body.
// The token cap covers the reasoning too, and a hard subject can
// spiral a thinking model: an exhausted budget means an empty reply
// and a parked task, so the cap errs generous.
constexpr std::size_t  kMaxText   = std::size_t{96} * 1024;
constexpr std::int32_t kMaxTokens = 8192;
constexpr std::int32_t kTimeoutS  = 3600;

constexpr char kLeafPrompt[] =
	"The user message is the complete subtitle text of one video. "
	"Write a condensed description of its factual content: the "
	"subjects covered, claims and decisions made, and the names, "
	"numbers and terms that appear. Plain text only; no preamble, "
	"no headings, no remarks about the subtitles themselves.";

constexpr char kNodePrompt[] =
	"Each section of the user message, separated by a line "
	"containing only three dashes, is a condensed summary of one "
	"or more videos from the same collection. Merge the sections "
	"into a single higher-level summary that preserves the "
	"load-bearing facts: subjects, decisions, names, numbers and "
	"terms. Generalize where the sections agree and keep the "
	"notable specifics. Plain text only; no preamble, no headings.";

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
	"out if nothing sets it apart. Write about the subject, never "
	"about the act of reviewing it -- no phrases like 'this "
	"material', 'these excerpts', 'the matches show'. Plain text "
	"only; no preamble, no headings.";

// System prompt per task kind (leaf, node, dive).  The views wrap
// NUL-terminated literals, so .data() satisfies the C API below.
constexpr std::string_view kPromptOf[] = {
	kLeafPrompt, kNodePrompt, kDivePrompt,
};

// Connect refusals in a row before the pipeline parks itself for
// the session; anything else the server says resets the count.
constexpr int kRefusalCap = 3;

constexpr char const *kKindName[] = {"leaf", "node", "dive"};

// The path a reply belongs to travels as the task's user data,
// heap-owned: exactly one callback per accepted task makes adoption
// in deliver() the release.  The journal line rides along, prepared
// while the task is known.
struct reply_ctx {
	Facts      *self;
	std::string path;
	std::string line;
	agenda::id  id;
};

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

// Generation is a sustained full-power burn on the accelerator, and
// a corpus queues many in a row: the default gap gives the silicon
// breathing room.  SRTVIEW_LLM_PACE=<seconds> widens it, 0 disables.
constexpr std::int32_t kPaceDefaultS = 30;
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
std::string_view clip(std::string const &text)
{
	std::size_t n = std::min(text.size(), kMaxText);
	while (n && (static_cast<unsigned char>(text[n]) & 0xc0) == 0x80)
		--n;
	return {text.data(), n};
}

// Atomic cache write: all-or-nothing via .tmp and rename, so a
// reader on any thread sees either nothing or a whole summary.
bool store(std::string const &path, char const *text, std::size_t n)
{
	std::string const tmp = path + ".tmp";
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
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

std::string slurp(std::string const &path)
{
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return {};
	return {std::istreambuf_iterator<char>(in), {}};
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
	std::string line = kKindName[std::size_t(t.what)];
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

Facts::Facts()
	: m_dir(cacheDir())
{
	std::error_code ec;
	std::filesystem::create_directories(m_dir + "/dives", ec);
	if (!ec) {
		endpoint const ep = serverEnv();
		m_llm = llm_create(ep.host.empty() ? nullptr
		                                   : ep.host.c_str(),
		                   ep.port);
		llm_pace(m_llm, paceEnv() * 1000);
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

bool Facts::wants(agenda::id key) const
{
	if (!key)
		return false;

	std::lock_guard const lock(m_mtx);
	if (!m_llm || m_plan.status(key) != agenda::plan::state::unknown)
		return false;
	std::error_code ec;
	return !std::filesystem::exists(m_dir + '/' + key.hex() + ".txt",
	                                ec);
}

void Facts::offer(agenda::id key, std::string const &utf8Text)
{
	if (!key || utf8Text.empty())
		return;

	std::lock_guard const lock(m_mtx);
	if (!m_llm || m_plan.status(key) != agenda::plan::state::unknown)
		return;
	std::error_code ec;
	if (std::filesystem::exists(m_dir + '/' + key.hex() + ".txt",
	                            ec)) {
		// A dependent of this cache hit may just have gone ready.
		m_plan.done(key);
		advance();
		return;
	}
	m_plan.add({.id = key, .keys = {key}});
	m_bodies.emplace_back(key, std::string(clip(utf8Text)));
	advance();
}

void Facts::corpus(std::vector<agenda::task> nodes)
{
	std::lock_guard const lock(m_mtx);
	if (!m_llm)
		return;
	for (agenda::task &t : nodes) {
		if (m_plan.status(t.id) != agenda::plan::state::unknown)
			continue;
		std::error_code ec;
		if (std::filesystem::exists(path_of(t), ec))
			m_plan.done(t.id);
		else
			m_plan.add(std::move(t));
	}
	advance();
}

void Facts::dive(agenda::task t, std::string const &utf8Excerpts)
{
	if (!t.id || utf8Excerpts.empty())
		return;

	std::lock_guard const lock(m_mtx);
	if (!m_llm || m_plan.status(t.id) != agenda::plan::state::unknown)
		return;
	t.what = agenda::kind::dive;
	std::error_code ec;
	if (std::filesystem::exists(path_of(t), ec)) {
		m_plan.done(t.id);
		advance();
		return;
	}
	m_bodies.emplace_back(t.id, std::string(clip(utf8Excerpts)));
	m_plan.add(std::move(t));
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
	std::lock_guard const lock(m_mtx);
	m_plan.reset();
	m_bodies.clear();
}

// On the llm worker thread; the file write stays outside the lock.
void Facts::deliver(void *ud, std::uint64_t, int status,
                    char const *text, std::size_t size)
{
	std::unique_ptr<reply_ctx> const ctx(
		static_cast<reply_ctx *>(ud));
	bool const wrote = status == LLM_OK && size
	                && store(ctx->path, text, size);
	if (wrote)
		// m_dir never changes after construction; the worker
		// reads it unlocked.
		journal(ctx->self->m_dir, ctx->line);
	else if (debug())
		std::fprintf(stderr, "srtview: facts: %s: %s\n",
		             ctx->id.hex().c_str(), llm_strerror(status));
	ctx->self->completed(ctx->id, status, wrote);
}

void Facts::completed(agenda::id which, int status, bool wrote)
{
	std::lock_guard const lock(m_mtx);
	if (m_inflight == which)
		m_inflight = {};
	if (wrote)
		m_plan.done(which);
	else
		m_plan.fail(which);

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

// m_mtx held.  Keeps taking until something is in flight or nothing
// is ready; a task whose submission fails parks and the loop moves
// on.
void Facts::advance()
{
	while (!m_down && !m_offline && m_llm && !m_inflight) {
		agenda::id const next = m_plan.take();
		if (!next)
			return;
		agenda::task const *t = m_plan.get(next);
		if (!t || !submit(*t)) {
			m_plan.fail(next);
			continue;
		}
		m_inflight = next;
	}
}

// m_mtx held.
bool Facts::submit(agenda::task const &t)
{
	// The context first: a leaf's snapshot is spent by assemble(),
	// so nothing fallible may sit between spending it and the ask.
	auto *ctx = new (std::nothrow) reply_ctx{this, path_of(t),
	                                         journal_line(t), t.id};
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
		.max_tokens  = kMaxTokens,
		.timeout_s   = kTimeoutS,
		.temperature = 0.0,
	};
	if (!llm_ask(m_llm, &ask, deliver, ctx)) {
		delete ctx;
		return false;
	}
	if (debug())
		std::fprintf(stderr, "srtview: facts: ask %s %s\n",
		             kKindName[std::size_t(t.what)],
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
	}

	return {};
}

std::string Facts::assemble_node(agenda::task const &t) const
{
	std::string all;
	for (agenda::id const dep : t.deps) {
		std::string const part =
			slurp(m_dir + '/' + dep.hex() + ".txt");
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
		std::string const part =
			slurp(m_dir + '/' + ref.hex() + ".txt");
		if (part.empty())
			continue;
		over += over.empty() ? "OVERVIEW\n" : "\n\n";
		over += part;
	}

	std::string sums;
	for (agenda::id const dep : t.deps) {
		std::string const part =
			slurp(m_dir + '/' + dep.hex() + ".txt");
		if (part.empty())
			return {};
		sums += sums.empty() ? "SUMMARIES\n" : "\n\n";
		sums += part;
	}

	std::string const hits = spend_body(t.id);
	if (hits.empty())
		return {};
	std::string all;
	add_section(all, over);
	add_section(all, sums);
	add_section(all, "MATCHES\n" + hits);
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

std::string Facts::path_of(agenda::task const &t) const
{
	return t.what == agenda::kind::dive
	       ? m_dir + "/dives/" + t.id.hex() + ".txt"
	       : m_dir + '/' + t.id.hex() + ".txt";
}
