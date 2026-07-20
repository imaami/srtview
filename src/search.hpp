// search.hpp -- search mediator: owns the pattern semantics, match
// state and navigation, driving the bar and the view.  Satisfies
// search_host; depends on component bases plus the playback mediator
// (landing on a hit syncs the video by default).
#ifndef SRTVIEW_SRC_SEARCH_HPP_
#define SRTVIEW_SRC_SEARCH_HPP_

#include "trail.hpp"

#include <QAction>
#include <QRegularExpression>
#include <QTextCursor>

#include <vector>

class PlaybackCtl;
class Prefs;
class search_bar_base;
class srt_view_base;
class QStatusBar;

// How the search leaves the current video when a direction is
// exhausted: implemented by the composition root over the playlist;
// the search controller stays ignorant of what a playlist is.
struct search_nav {
	// Switch to the nearest video (in the given direction) whose
	// transcript matches; false when there is nowhere to go.
	virtual bool hopVideo(QRegularExpression const &re,
	                      bool backward) = 0;

protected:
	~search_nav() = default;
};

class SearchCtl
{
public:
	SearchCtl(search_bar_base &bar, srt_view_base &view,
	          QStatusBar &status, Prefs &prefs, Trail &trail,
	          PlaybackCtl &playback, search_nav *nav);

	void showSearch();
	void hideSearch();
	void commitSearch();
	void searchChanged();
	// Jump to the next/previous hit; the video follows unless
	// syncVideo is false (the F4 "text only" flavor).
	void findAgain(bool backward, bool syncVideo = true);
	void historyStep(bool back);

	// Undo appliers: restore prior state without recording.
	void applyPattern(QString const &text);
	void applyCursor(int position);

	// Reposition the overlay over the view (window resize, open).
	void layoutOverlay();

	// Prime a pattern quietly (playlist load): highlights refresh and
	// F3 works immediately, but the cursor stays put -- the topics
	// are a starting vocabulary, not where reading must begin.
	void primePattern(QString const &s);

	// Re-evaluate the live pattern against a fresh document without
	// moving the cursor (video switch).
	void refresh();

	// Selftest hook: like typing, incremental jump included.
	void setSearchText(QString const &s);
	void setRegexEnabled(bool on);
	int matchCount() const { return int(m_matchStarts.size()); }

	QAction &nextAction() { return m_nextAct; }
	QAction &prevAction() { return m_prevAct; }
	QAction &nextTextAction() { return m_nextTextAct; }
	QAction &prevTextAction() { return m_prevTextAct; }

private:
	QRegularExpression pattern() const;
	// Record the pattern's first effective use as one combined step:
	// text change + cursor position, plus the video jump when
	// syncVideo -- which also anchors the hit cycle; also feeds the
	// persistent history.
	void recordUse(bool syncVideo);
	// Route one hit-to-hit hop: travel the ring, grow it, or record
	// a plain step, seeking the video as the route demands.
	void applyHop(trail_step const &s, int dir, bool moved);
	// Seek the video to the cursor's cue; true on success, with the
	// landed-on time in t.
	bool syncCue(double &t);
	void highlightAll();
	void updateCounter(QTextCursor const &cur);
	QPoint target() const;

	search_bar_base   &m_bar;
	srt_view_base     &m_view;
	QStatusBar        &m_status;
	Prefs             &m_prefs;
	Trail             &m_trail;
	PlaybackCtl       &m_playback;
	search_nav        *m_nav;
	QAction            m_nextAct, m_prevAct;
	QAction            m_nextTextAct, m_prevTextAct;
	QTextCursor        m_anchor;
	QString            m_recorded;   // last pattern written to the trail
	QString            m_draft;      // live text while stepping history
	std::vector<int>   m_matchStarts;
	int                m_histPos = -1;
	bool               m_stepping = false;
	bool               m_quiet = false;   // suppress pattern-change jumps
};

#endif // SRTVIEW_SRC_SEARCH_HPP_
