// trail.hpp -- C++ facade over the fundo C core: the undo tree of
// search / jump / seek breadcrumbs.  Each node stores a *state*, not
// a transition: a bitmask of the facets the step touched plus the
// value of each touched facet after the step.  Undo resolves every
// departed facet to its nearest recorded ancestor value (defaults
// below the root), so both directions apply arrival state; steps
// encode deterministically so that byte-identical actions trigger the
// tree's branch adoption.  The applying latch centrally suppresses
// recording while a step is being applied.
//
// Search hits wrapping around the document form travel cycles on top
// of the tree: hop states (text + cursor + video, all deterministic)
// join a fundo ring as they are discovered, and a hop whose state is
// byte-identical to the ring neighbor in its direction travels the
// ring instead of growing the tree.  On a ring, undo unwinds travel
// while any is outstanding (backward search and undo coincide, as do
// forward search and redo) and drops back to tree walking when the
// ring's net travel reaches zero.
#ifndef SRTVIEW_SRC_TRAIL_HPP_
#define SRTVIEW_SRC_TRAIL_HPP_

#include "fundo.h"

#include <QString>

#include <cstddef>
#include <optional>

struct trail_step {
	enum : unsigned {
		text   = 1u << 0,  // search pattern
		cursor = 1u << 1,  // text cursor position
		video  = 1u << 2,  // playback position
	};

	QString  pattern;
	double   time = 0.0;
	int      cur   = 0;
	unsigned flags = 0;
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

	// Playback drifts between steps without recording; called before
	// a jump is recorded, this drops a breadcrumb at the position
	// actually departed from, so that undoing the jump returns there.
	void driftTo(double t);

	// A video position was applied outside act() (undo/redo): keep
	// the drift baseline in sync.
	void noteVideo(double t) { m_lastVideo = t; }

	// Search-hit cycle: the anchoring act (a full text+cursor+video
	// state with a reproducible cue-start time) begins an episode;
	// hops probe, then travel the ring or grow it.
	void anchorCycle();
	void dropCycle() { m_cycle = nullptr; m_ringAt = nullptr; }
	bool cycled() const { return m_cycle != nullptr; }

	// What a hop toward dir amounts to: nothing (the state is already
	// current), a ring travel, or growth.  Latches the ring position
	// growHop() will splice after, so probe before seeking -- the
	// seek's drift breadcrumb moves the tree off the ring.
	enum class hop { stay, travel, grow };
	hop probeHop(trail_step const &s, int dir);
	void travelHop(int dir);
	void growHop(trail_step const &s, int dir);

	// One step toward the past / future; the state to apply, or
	// nothing at the root / tip.
	std::optional<trail_step> undo();
	std::optional<trail_step> redo();

	bool canUndo() const;
	bool canRedo() const;
	std::size_t branches() const { return fundo_branches(&m_f); }

	bool applying() const { return m_applying; }
	void setApplying(bool on) { m_applying = on; }

private:
	int travelSlack() const;

	struct fundo             m_f;
	std::optional<double>    m_lastVideo; // last recorded/applied position
	struct fundo_node const *m_cycle  = nullptr; // active episode anchor
	struct fundo_node const *m_ringAt = nullptr; // pending growth splice
	bool                     m_applying = false;
};

#endif // SRTVIEW_SRC_TRAIL_HPP_
