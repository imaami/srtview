// trail.hpp -- the undo breadcrumbs: a session-long stack of typed
// steps recording what the user did (search-pattern transitions,
// search jumps, video jumps, sideways seeks), each carrying the
// state *before* the step so Ctrl+Z can walk the path backwards.
// Pause/play is deliberately not recorded.  Recording is centrally
// suppressed while a step is being applied.
#ifndef SRTVIEW_SRC_TRAIL_HPP_
#define SRTVIEW_SRC_TRAIL_HPP_

#include <QString>

#include <vector>

struct trail_step {
	enum kind : int { search_text, search_jump, video_jump, side_seek };

	kind    k;
	QString text;          // search_text: pattern before the change
	int     cursor = 0;    // search_jump: cursor position before
	double  time = 0.0;    // video_jump / side_seek: player time before
};

class Trail
{
public:
	void push(trail_step s)
	{
		if (!m_applying)
			m_steps.push_back(std::move(s));
	}

	bool empty() const { return m_steps.empty(); }

	trail_step pop()
	{
		trail_step s = std::move(m_steps.back());
		m_steps.pop_back();
		return s;
	}

	bool applying() const { return m_applying; }
	void setApplying(bool on) { m_applying = on; }

private:
	std::vector<trail_step> m_steps;
	bool m_applying = false;
};

#endif // SRTVIEW_SRC_TRAIL_HPP_
