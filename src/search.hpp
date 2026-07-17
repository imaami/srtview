// search.hpp -- search mediator: owns the pattern semantics, match
// state and navigation, driving the bar and the view.  Satisfies
// search_host; depends only on component bases.
#ifndef SRTVIEW_SRC_SEARCH_HPP_
#define SRTVIEW_SRC_SEARCH_HPP_

#include <QAction>
#include <QRegularExpression>
#include <QTextCursor>

#include <vector>

class search_bar_base;
class srt_view_base;
class QStatusBar;

class SearchCtl
{
public:
	SearchCtl(search_bar_base &bar, srt_view_base &view,
	          QStatusBar &status);

	void showSearch();
	void hideSearch();
	void commitSearch();
	void searchChanged();
	void findAgain(bool backward);

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
	void highlightAll();
	void updateCounter(QTextCursor const &cur);
	QPoint target() const;

	search_bar_base   &m_bar;
	srt_view_base     &m_view;
	QStatusBar        &m_status;
	QAction            m_nextAct, m_prevAct;
	QTextCursor        m_anchor;
	std::vector<int>   m_matchStarts;
};

#endif // SRTVIEW_SRC_SEARCH_HPP_
