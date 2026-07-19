// mainwin.hpp -- composition root: owns the components and the
// controllers, builds the chrome, wires the pieces, and runs the
// open/close flow.  Nothing depends on this header except its
// drivers (main, selftest).
#ifndef SRTVIEW_SRC_MAINWIN_HPP_
#define SRTVIEW_SRC_MAINWIN_HPP_

#include "mpvlink.hpp"
#include "playback.hpp"
#include "prefs.hpp"
#include "search.hpp"
#include "searchbar.hpp"
#include "srtedit.hpp"
#include "trail.hpp"

#include <QLabel>
#include <QMainWindow>

class MainWin : public QMainWindow
{
public:
	MainWin();

	bool openPath(QString const &path);
	void undoStep();
	void redoStep();

	// Component / controller access (menus are wired internally;
	// these exist for the selftest driver).
	srt_view_base   &view() { return m_view; }
	search_bar_base &bar() { return m_bar; }
	PlaybackCtl     &playback() { return m_playback; }
	SearchCtl       &search() { return m_search; }

protected:
	bool eventFilter(QObject *obj, QEvent *ev) override;
	void dragEnterEvent(QDragEnterEvent *ev) override;
	void dropEvent(QDropEvent *ev) override;
	void closeEvent(QCloseEvent *ev) override;
	void resizeEvent(QResizeEvent *ev) override;

private:
	static bool droppable(QMimeData const *md);
	void applyStep(trail_step const &s, bool undo);
	void openDialog(QString const &startDir);
	void rebuildRecentMenu();
	void closeFile();
	bool fail(QString const &msg);
	void setState(QString const &s);

	Prefs                           m_prefs;
	Trail                           m_trail;
	QMenu                          *m_recentMenu = nullptr;
	SrtEdit<PlaybackCtl, SearchCtl> m_view;
	SearchBar<SearchCtl>            m_bar;
	MpvLink<PlaybackCtl>            m_link;
	PlaybackCtl                     m_playback;
	SearchCtl                       m_search;
	QLabel                          m_state;
};

#endif // SRTVIEW_SRC_MAINWIN_HPP_
