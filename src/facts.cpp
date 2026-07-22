// facts.cpp -- see facts.hpp.  The summarization prompt, transcript
// truncation to the model's context budget, and the atomic
// write-on-reply live here.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string_view>
#include <system_error>

#include "facts.hpp"
#include "llm.h"

namespace {

// The server holds 32k of context; leave room for the reply and the
// reasoning that precedes it, and cut an oversized transcript.
constexpr std::size_t kMaxText   = std::size_t{96} * 1024;
constexpr std::int32_t kMaxTokens = 4096;
constexpr std::int32_t kTimeoutS  = 3600;

constexpr char kPrompt[] =
	"The user message is the complete subtitle text of one video. "
	"Write a condensed description of its factual content: the "
	"subjects covered, claims and decisions made, and the names, "
	"numbers and terms that appear. Plain text only; no preamble, "
	"no headings, no remarks about the subtitles themselves.";

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

// On the llm worker thread.  The path travels as the task's user
// data, heap-owned: exactly one callback per accepted task, so
// adoption here is the release.  An empty reply (the reasoning ate
// the whole token budget) writes nothing -- a file must mean a
// summary, or the cache would mask the failure forever.
void deliver(void *ud, std::uint64_t, int status, char const *text,
             std::size_t size)
{
	std::unique_ptr<std::string> const path(
		static_cast<std::string *>(ud));
	if (status != LLM_OK || !size) {
		if (debug())
			std::fprintf(stderr, "srtview: facts: %s: %s\n",
			             path->c_str(), llm_strerror(status));
		return;
	}
	std::string const tmp = *path + ".tmp";
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		out.write(text, std::streamsize(size));
		out.put('\n');
		if (!out) {
			std::remove(tmp.c_str());
			return;
		}
	}
	std::rename(tmp.c_str(), path->c_str());
}

} // namespace

Facts::Facts()
	: m_dir(cacheDir())
{
	std::error_code ec;
	std::filesystem::create_directories(m_dir, ec);
	if (!ec)
		m_llm = llm_create(nullptr, 0);
}

Facts::~Facts()
{
	llm_destroy(&m_llm);
}

void Facts::offer(std::string const &id, std::string const &utf8Text)
{
	if (!m_llm || id.empty() || utf8Text.empty())
		return;
	if (std::ranges::find(m_queued, id) != m_queued.end())
		return;
	std::string path = m_dir + '/' + id + ".txt";
	std::error_code ec;
	if (std::filesystem::exists(path, ec))
		return;
	std::string const body(clip(utf8Text));
	auto ud = std::make_unique<std::string>(std::move(path));
	llm_task const task = {
		.system      = kPrompt,
		.prompt      = body.c_str(),
		.max_tokens  = kMaxTokens,
		.timeout_s   = kTimeoutS,
		.temperature = 0.0,
	};
	if (!llm_ask(m_llm, &task, deliver, ud.get()))
		return;
	ud.release();
	m_queued.push_back(id);
}
