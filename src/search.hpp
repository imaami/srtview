// search.hpp -- search mediator: owns the pattern semantics, match
// state and navigation, driving the bar and the view.  Satisfies
// search_host; depends only on component bases.
#ifndef SRTVIEW_SRC_SEARCH_HPP_
#define SRTVIEW_SRC_SEARCH_HPP_

#include "trail.hpp"

#include <QAction>
#include <QRegularExpression>
#include <QTextCursor>

#include <vector>

class Prefs;
class search_bar_base;
class srt_view_base;
class QStatusBar;

class SearchCtl
{
public:
	SearchCtl(search_bar_base &bar, srt_view_base &view,
	          QStatusBar &status, Prefs &prefs, Trail &trail);

	void showSearch();
	void hideSearch();
	void commitSearch();
	void searchChanged();
	void findAgain(bool backward);
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

private:
	QRegularExpression pattern() const;
	// Record the pattern's first effective use: one coalesced
	// text-transition step plus one jump step back to the search
	// anchor; also feeds the persistent history.
	void recordUse();
	void highlightAll();
	void updateCounter(QTextCursor const &cur);
	QPoint target() const;

	search_bar_base   &m_bar;
	srt_view_base     &m_view;
	QStatusBar        &m_status;
	Prefs             &m_prefs;
	Trail             &m_trail;
	QAction            m_nextAct, m_prevAct;
	QTextCursor        m_anchor;
	QString            m_recorded;   // last pattern written to the trail
	QString            m_draft;      // live text while stepping history
	std::vector<int>   m_matchStarts;
	int                m_histPos = -1;
	bool               m_stepping = false;
};

#endif // SRTVIEW_SRC_SEARCH_HPP_
