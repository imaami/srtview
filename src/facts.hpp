// facts.hpp -- background factual summaries of subtitle files.
//
// One llm task per unique srt: offer() queues the transcript text to
// the local llama-server (llm.h client, its own worker thread) and
// the condensed summary lands in $XDG_CACHE_HOME/srtview/facts/
// <id>.txt -- the frame cache's sibling.  File existence is the
// manifest: an already-summarized corpus costs one stat per entry to
// reopen, and a failed or cancelled task writes nothing, so the next
// session retries it.  offer() is fire-and-forget on the caller's
// thread; the reply callback runs on the llm worker and touches only
// the filesystem.  Standard C++23 over the plain C llm client, no
// Qt.
#ifndef SRTVIEW_SRC_FACTS_HPP_
#define SRTVIEW_SRC_FACTS_HPP_

#include <string>
#include <vector>

struct llm;

class Facts
{
public:
	Facts();
	~Facts();

	Facts(Facts const &) = delete;
	Facts &operator=(Facts const &) = delete;

	// Queue a summary of utf8Text under id unless the cache already
	// holds one, one is in flight, or either argument is empty.
	void offer(std::string const &id, std::string const &utf8Text);

private:
	std::string              m_dir;    // .../srtview/facts
	std::vector<std::string> m_queued; // ids asked this session
	llm                     *m_llm = nullptr;
};

#endif // SRTVIEW_SRC_FACTS_HPP_
