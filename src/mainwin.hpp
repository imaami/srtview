// mainwin.hpp -- composition root: owns the components and the
// controllers, builds the chrome, wires the pieces, and runs the
// open/close flow.  Nothing depends on this header except its
// drivers (main, selftest).
#ifndef SRTVIEW_SRC_MAINWIN_HPP_
#define SRTVIEW_SRC_MAINWIN_HPP_

#include "discovery.hpp"
#include "exporter.hpp"
#include "grabber.hpp"
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

class MainWin : public QMainWindow, private search_nav,
                private grab_listener, private video_sync
{
public:
	MainWin();

	bool openPath(QString const &path, QString const &srtOverride = {});
	bool openAny(QString const &path);
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
	static bool avPath(QString const &p);
	QString srtOf(PlayItem const &it);
	QString videoId(QString const &video);
	bool videoMatches(PlayItem const &it,
	                  QRegularExpression const &re);
	bool hopVideo(QRegularExpression const &re,
	              bool backward) override;
	void mpvSwitched(int index) override;
	bool showDoc(QString const &video, QString const &srt);
	qsizetype playlistIndex(QString const &video);
	qsizetype indexOfId(QString const &id) const;
	QList<play_entry> corpusEntries();
	void grabsIdle() override;
	void grabProgress() override;
	void startExport();
	void runExport(bool drained);
	QString exportDir() const;
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
	discovery                       m_disc;
	topics::doc                     m_corpus;
	QString                         m_corpusPath;
	QList<PlayItem>                 m_playlist;
	QHash<QString, PlayItem>        m_videosById;
	exporter::transcripts           m_transcripts;
	QMenu                          *m_recentMenu = nullptr;
	QMenu                          *m_videosMenu = nullptr;
	SrtEdit<PlaybackCtl, SearchCtl> m_view;
	SearchBar<SearchCtl>            m_bar;
	MpvLink<PlaybackCtl>            m_link;
	Grabber                         m_grab;
	PlaybackCtl                     m_playback;
	SearchCtl                       m_search;
	QLabel                          m_state;
	QElapsedTimer                   m_exportTick;
	int                             m_exportQueued = -1;
	bool                            m_exportPending = false;
};

#endif // SRTVIEW_SRC_MAINWIN_HPP_
