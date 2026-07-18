// trail.hpp -- C++ facade over the fundo C core: the undo tree of
// search / jump / seek breadcrumbs.  Each step carries both the
// before-state (undo applies it) and the after-state (redo applies
// it); steps encode deterministically so that byte-identical
// transitions trigger the tree's branch adoption.  The applying latch
// centrally suppresses recording while a step is being applied.
#ifndef SRTVIEW_SRC_TRAIL_HPP_
#define SRTVIEW_SRC_TRAIL_HPP_

#include "fundo.h"

#include <QString>

#include <cstddef>
#include <optional>

struct trail_step {
	enum kind : int { search_text, search_jump, video_jump, side_seek };

	kind    k = search_text;
	QString textBefore, textAfter;
	int     curBefore = 0, curAfter = 0;
	double  timeBefore = 0.0, timeAfter = 0.0;
};

class Trail
{
public:
	Trail() { fundo_init(&m_f); }
	~Trail() { fundo_fini(&m_f); }

	Trail(Trail const &) = delete;
	Trail &operator=(Trail const &) = delete;

	// Grow a branch, or adopt an identical existing one.
	void act(trail_step const &s);

	// One step toward the past / future; the step to apply, or
	// nothing at the root / tip.
	std::optional<trail_step> undo();
	std::optional<trail_step> redo();

	bool canUndo() const { return fundo_can_undo(&m_f); }
	bool canRedo() const { return fundo_can_redo(&m_f); }
	std::size_t branches() const { return fundo_branches(&m_f); }

	bool applying() const { return m_applying; }
	void setApplying(bool on) { m_applying = on; }

private:
	struct fundo m_f;
	bool         m_applying = false;
};

#endif // SRTVIEW_SRC_TRAIL_HPP_
