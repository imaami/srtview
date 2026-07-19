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

class SearchCtl
{
public:
	SearchCtl(search_bar_base &bar, srt_view_base &view,
	          QStatusBar &status, Prefs &prefs, Trail &trail,
	          PlaybackCtl &playback);

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

	// Selftest hooks.
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
	// syncVideo; also feeds the persistent history.
	void recordUse(bool syncVideo);
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
	QAction            m_nextAct, m_prevAct;
	QAction            m_nextTextAct, m_prevTextAct;
	QTextCursor        m_anchor;
	QString            m_recorded;   // last pattern written to the trail
	QString            m_draft;      // live text while stepping history
	std::vector<int>   m_matchStarts;
	int                m_histPos = -1;
	bool               m_stepping = false;
};

#endif // SRTVIEW_SRC_SEARCH_HPP_
