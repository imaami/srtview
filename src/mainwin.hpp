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
#include "topics.hpp"
#include "trail.hpp"

#include <QHash>
#include <QLabel>
#include <QMainWindow>

class MainWin : public QMainWindow
{
public:
	MainWin();

	bool openPath(QString const &path, QString const &srtOverride = {});
	bool loadPlaylist(QString const &path);
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
	// A playlist entry / registry row: paths resolved, identity from
	// discovery (empty when the file is currently unresolvable).
	struct PlayItem {
		QString video, srt, id;
	};

	static bool droppable(QMimeData const *md);
	void applyStep(trail_step const &s, bool undo);
	bool applyVideoStep(trail_step const &s);
	void openDialog(QString const &startDir);
	void openPlaylistDialog();
	void stepVideo(int dir);
	void rebuildRecentMenu();
	void rebuildVideosMenu();
	void closeFile();
	bool fail(QString const &msg);
	void setState(QString const &s);

	Prefs                           m_prefs;
	Trail                           m_trail;
	topics::doc                     m_corpus;
	QList<PlayItem>                 m_playlist;
	QHash<QString, PlayItem>        m_videosById;
	QMenu                          *m_recentMenu = nullptr;
	QMenu                          *m_videosMenu = nullptr;
	SrtEdit<PlaybackCtl, SearchCtl> m_view;
	SearchBar<SearchCtl>            m_bar;
	MpvLink<PlaybackCtl>            m_link;
	PlaybackCtl                     m_playback;
	SearchCtl                       m_search;
	QLabel                          m_state;
};

#endif // SRTVIEW_SRC_MAINWIN_HPP_
