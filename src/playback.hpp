// playback.hpp -- playback mediator: executes transport verbs against
// the mpv link, resolves cues through the view, reports through the
// status bar, and owns the follow toggle.  Satisfies playback_host
// and mpv_observer; depends only on component bases.
#ifndef SRTVIEW_SRC_PLAYBACK_HPP_
#define SRTVIEW_SRC_PLAYBACK_HPP_

#include <QAction>

class mpv_link_base;
class srt_view_base;
class QStatusBar;

class PlaybackCtl
{
public:
	PlaybackCtl(mpv_link_base &link, srt_view_base &view,
	            QStatusBar &status);

	void seekCue(int cue, bool forcePause);
	void setPause(bool on);
	void togglePause();
	void seekRel(double dt);
	void toggleFollow();
	void onMpvTime(double t);

	QAction &followAction() { return m_followAct; }

private:
	mpv_link_base  &m_link;
	srt_view_base  &m_view;
	QStatusBar     &m_status;
	QAction         m_followAct;
};

#endif // SRTVIEW_SRC_PLAYBACK_HPP_
