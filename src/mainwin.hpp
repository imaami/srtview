// mainwin.hpp -- application window: menus, status, search state, and
// the host interface that SrtEdit and SearchBar bind to statically.
#ifndef SRTVIEW_SRC_MAINWIN_HPP_
#define SRTVIEW_SRC_MAINWIN_HPP_

#include "mpvlink.hpp"
#include "searchbar.hpp"
#include "srtedit.hpp"

#include <QAction>
#include <QLabel>
#include <QMainWindow>
#include <QRegularExpression>
#include <QTextCursor>

#include <vector>

class MainWin : public QMainWindow
{
public:
	MainWin();

	bool openPath(const QString &path);

	// ---- host interface for SrtEdit and SearchBar; --selftest too ----
	void showSearch();
	void hideSearch();
	void commitSearch();
	void layoutOverlays();
	void searchChanged();
	void findAgain(bool backward);
	void seekCue(int cue, bool forcePause);
	void setPause(bool on);
	void togglePause();
	void seekRel(double dt);
	void toggleFollow();
	void onMpvTime(double t);

	// ---- selftest hooks ----
	void setSearchText(const QString &s);
	void setRegexEnabled(bool on);
	int matchCount() const { return int(m_matchStarts.size()); }
	int playCue() const { return m_edit.playCue(); }
	bool searchBarVisible() const { return m_searchBar.isVisible(); }
	QRect searchBarGeometry() const { return m_searchBar.geometry(); }
	auto &edit() { return m_edit; }

protected:
	void dragEnterEvent(QDragEnterEvent *ev) override;
	void dropEvent(QDropEvent *ev) override;
	void closeEvent(QCloseEvent *ev) override;
	void resizeEvent(QResizeEvent *ev) override;

private:
	static bool droppable(const QMimeData *md);
	QPoint searchBarTarget() const;
	QRegularExpression pattern() const;
	void highlightAll();
	void updateCounter(const QTextCursor &cur);
	void openDialog();
	void closeFile();
	bool fail(const QString &msg);
	void setState(const QString &s);

	SrtEdit    m_edit;
	SearchBar  m_searchBar;
	MpvLink    m_mpv;
	QLabel     m_state;
	QAction    m_nextAct, m_prevAct, m_followAct;
	QTextCursor m_searchAnchor;
	std::vector<int> m_matchStarts;
};

#endif // SRTVIEW_SRC_MAINWIN_HPP_
