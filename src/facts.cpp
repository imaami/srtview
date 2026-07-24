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
constexpr std::size_t  kMaxText   = std::size_t{96} * 1024;
constexpr std::int32_t kMaxTokens = 4096;
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

// The path a reply belongs to travels as the task's user data,
// heap-owned: exactly one callback per accepted task makes adoption
// in deliver() the release.
struct reply_ctx {
	Facts      *self;
	std::string id;
	std::string path;
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

} // namespace

Facts::Facts()
	: m_dir(cacheDir())
{
	std::error_code ec;
	std::filesystem::create_directories(m_dir + "/dives", ec);
	if (!ec)
		m_llm = llm_create(nullptr, 0);
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

void Facts::offer(std::string const &id, std::string const &utf8Text)
{
	if (id.empty() || utf8Text.empty())
		return;

	std::lock_guard const lock(m_mtx);
	if (!m_llm || m_plan.status(id) != agenda::plan::state::unknown)
		return;
	std::error_code ec;
	if (std::filesystem::exists(m_dir + '/' + id + ".txt", ec)) {
		// A dependent of this cache hit may just have gone ready.
		m_plan.done(id);
		advance();
		return;
	}
	m_plan.add({.id = id, .keys = {id}});
	m_bodies.emplace_back(id, std::string(clip(utf8Text)));
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

void Facts::heat(std::string const &key, double add)
{
	if (key.empty())
		return;

	std::lock_guard const lock(m_mtx);
	m_plan.heat(key, add);
}

void Facts::decay(double keep)
{
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
	bool const ok = status == LLM_OK && size
	             && store(ctx->path, text, size);
	if (!ok && debug())
		std::fprintf(stderr, "srtview: facts: %s: %s\n",
		             ctx->id.c_str(), llm_strerror(status));
	ctx->self->completed(ctx->id, ok);
}

void Facts::completed(std::string const &id, bool ok)
{
	std::lock_guard const lock(m_mtx);
	if (m_inflight == id)
		m_inflight.clear();
	if (ok)
		m_plan.done(id);
	else
		m_plan.fail(id);
	advance();
}

// m_mtx held.  Keeps taking until something is in flight or nothing
// is ready; a task whose submission fails parks and the loop moves
// on.
void Facts::advance()
{
	while (!m_down && m_llm && m_inflight.empty()) {
		std::string const id = m_plan.take();
		if (id.empty())
			return;
		agenda::task const *t = m_plan.get(id);
		if (!t || !submit(*t)) {
			m_plan.fail(id);
			continue;
		}
		m_inflight = id;
	}
}

// m_mtx held.
bool Facts::submit(agenda::task const &t)
{
	std::string const body = assemble(t);
	if (body.empty())
		return false;
	auto *ctx = new (std::nothrow) reply_ctx{this, t.id, path_of(t)};
	if (!ctx)
		return false;
	llm_task const ask = {
		.system      = t.what == agenda::kind::node ? kNodePrompt
		                                            : kLeafPrompt,
		.prompt      = body.c_str(),
		.max_tokens  = kMaxTokens,
		.timeout_s   = kTimeoutS,
		.temperature = 0.0,
	};
	if (!llm_ask(m_llm, &ask, deliver, ctx)) {
		delete ctx;
		return false;
	}
	return true;
}

// m_mtx held.  A leaf spends its snapshot; a node reads its
// children's cache files, present by dependency gating (an empty
// read means a raced cache wipe -- the caller parks the task).
std::string Facts::assemble(agenda::task const &t)
{
	if (t.what != agenda::kind::node) {
		for (std::size_t i = 0; i < m_bodies.size(); ++i) {
			if (m_bodies[i].first != t.id)
				continue;
			std::string spent = std::move(m_bodies[i].second);
			m_bodies.erase(m_bodies.begin()
			               + std::ptrdiff_t(i));
			return spent;
		}
		return {};
	}

	std::string all;
	for (std::string const &dep : t.deps) {
		std::string const part =
			slurp(m_dir + '/' + dep + ".txt");
		if (part.empty())
			return {};
		if (!all.empty())
			all += "\n---\n";
		all += part;
	}
	return std::string(clip(all));
}

std::string Facts::path_of(agenda::task const &t) const
{
	return t.what == agenda::kind::dive
	       ? m_dir + "/dives/" + t.id + ".txt"
	       : m_dir + '/' + t.id + ".txt";
}
