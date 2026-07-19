// playback.hpp -- playback mediator: executes transport verbs against
// the mpv link, resolves cues through the view, reports through the
// status bar, and owns the follow toggle.  Satisfies playback_host
// and mpv_observer; depends only on component bases.
#ifndef SRTVIEW_SRC_PLAYBACK_HPP_
#define SRTVIEW_SRC_PLAYBACK_HPP_

#include "trail.hpp"

#include <QAction>

class mpv_link_base;
class srt_view_base;
class QStatusBar;

class PlaybackCtl
{
public:
	PlaybackCtl(mpv_link_base &link, srt_view_base &view,
	            QStatusBar &status, Trail &trail);

	void seekCue(int cue, bool forcePause);
	void setPause(bool on);
	void togglePause();
	void seekRel(double dt);
	void toggleFollow();
	void onMpvTime(double t);
	void onMpvState(bool responsive);

	// Undo applier: return to a recorded position, pause untouched.
	// False when mpv refused the seek and playback did not move.
	bool applyTime(double t);

	QAction &followAction() { return m_followAct; }

private:
	mpv_link_base  &m_link;
	srt_view_base  &m_view;
	QStatusBar     &m_status;
	Trail          &m_trail;
	QAction         m_followAct;
};

#endif // SRTVIEW_SRC_PLAYBACK_HPP_
