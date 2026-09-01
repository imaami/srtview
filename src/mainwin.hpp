// mainwin.hpp -- composition root: owns the components and the
// controllers, builds the chrome, wires the pieces, and runs the
// open/close flow.  Nothing depends on this header except its
// drivers (main, selftest).
#ifndef SRTVIEW_SRC_MAINWIN_HPP_
#define SRTVIEW_SRC_MAINWIN_HPP_

#include "agenda.hpp"
#include "cubepane.hpp"
#include "discovery.hpp"
#include "exporter.hpp"
#include "facts.hpp"
#include "grabber.hpp"
#include "ident.hpp"
#include "knowledge.hpp"
#include "loom.hpp"
#include "mpvlink.hpp"
#include "ocrq.hpp"
#include "playback.hpp"
#include "prefs.hpp"
#include "refinery.hpp"
#include "search.hpp"
#include "searchbar.hpp"
#include "semantic_engine.hpp"
#include "srtedit.hpp"
#include "topics.hpp"
#include "trail.hpp"

#include <QHash>
#include <QSet>
#include <QLabel>
#include <QMainWindow>
#include <QRegularExpression>

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

class MainWin : public QMainWindow, private search_nav,
                private grab_listener, private ocr_listener,
                private video_sync, private refinery_host
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

	// The dialogless hits export (File > Export search hits… asks
	// for the path and calls this); public for the selftest.
	bool exportHitsTo(QString const &path);

	CubePane &cubes() { return m_cubes; }

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

	// The four zoom domains, nested: captions and the search bar
	// chrome scale from the base (application) font, the pattern
	// text from the chrome.  Ctrl +/-/0 act on the domain under
	// the pointer -- pattern field, bar, captions, base for
	// everything else; Ctrl+Shift+0 resets the lot.
	enum class ZoomDom { base, captions, bar, regex };

	static bool droppable(QMimeData const *md);
	static bool avPath(QString const &p);
	QString srtOf(PlayItem const &it);
	QString videoId(QString const &video);
	bool videoMatches(PlayItem const &it,
	                  QRegularExpression const &re);
	bool hopVideo(QRegularExpression const &re,
	              bool backward) override;
	void searchInfoChanged() override { updateInfo(); }
	void searchCommitted() override;
	void mpvSwitched(int index) override;
	void updateInfo();
	QString matchInfo(qsizetype at);
	void recomputeTally();
	void feedHeat();
	bool showDoc(QString const &video, QString const &srt);
	// The shared switch-then-seek preamble of the knowledge-hit
	// and cube activations: when the target differs from the
	// current video, the departure drift is recorded first --
	// while the trail still names the origin -- and the path
	// opened.  True when the target is current afterward;
	// switched tells the caller's seek to skip its own departure
	// capture, which would stamp the origin's lingering timestamp
	// into the destination video.
	bool visitVideo(QString const &video, QString const &srt,
	                bool &switched);
	void refineryChanged() override;
	void rebuildCorpus(bool fresh);
	void identArrived();
	void identifiedCorpus();
	void adoptVideo(QString const &video, QString const &srt);
	agenda::id offerFacts(QString const &srt);
	void refreshKnowledge();
	void knowledgeSelected(QTreeWidgetItem const *item);
	void semanticSelected(QTreeWidgetItem const *item);
	void chatAsked();
	void semanticStep();
	void showEvidence(std::vector<semantic::citation> const &cites);
	QString glossPath() const;
	void loadGloss();
	void showGloss(QTreeWidgetItem const *item);
	qsizetype playlistIndex(QString const &video,
	                        QString const &srt = {});
	qsizetype indexOfId(QString const &id) const;
	QList<play_entry> corpusEntries();
	void grabsIdle() override;
	void grabProgress() override;
	void ocrReady() override;
	void ocrSettled();
	void rebuildSemantic();
	void startExport();
	void runExport(bool drained);
	void exportHits();
	bool writeHits(QString const &head,
	               QRegularExpression const &re,
	               QString const &path);
	void writePlaylistVersion();
	QString exportDir() const;
	void applyStep(trail_step const &s, bool undo);
	bool applyVideoStep(trail_step const &s);
	void openDialog(QString const &startDir);
	void openPlaylistDialog();
	void stepVideo(int dir);
	bool barFocused() const;
	ZoomDom zoomDomain() const;
	int *zoomOf(ZoomDom d);
	void applyZoom(ZoomDom d);
	void zoomStep(int dir);
	void zoomReset(bool all);
	void rebuildRecentMenu();
	void rebuildVideosMenu();
	void closeFile();
	bool fail(QString const &msg);
	void setState(QString const &s);
	void errState(QString const &s);

	// Frame text folded from drained OCR notes: video discovery
	// id -> seconds -> that reading's confident spans, boxes and
	// all.  On the debounced resnapshot each video's moments weave
	// into regions (loom.hpp) whose consensus lines enter the
	// engine's sources.
	std::map<std::string,
	         std::map<double, std::vector<ocr::span>>> m_frameText;
	// The weave, cached per video and re-run only for videos whose
	// readings actually changed since the last snapshot -- tens of
	// milliseconds per live-corpus video adds up across dozens.
	std::map<std::string, std::vector<ocr::region>> m_regions;
	// Content identity, filled by Ident's workers: path -> hex16
	// id of the file's bytes.  Empty while a hash is in flight.
	QHash<QString, QString>         m_fileIds;
	QSet<QString>                   m_identWant;
	QString                         m_shownVideo, m_shownSrt;
	bool                            m_identPending = false;
	bool                            m_identFresh = false;
	bool                            m_shownIdless = false;
	std::set<std::string>           m_frameDirty;
	bool                            m_ocrDirty = false;
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
	KnowledgePane                   m_know;
	CubePane                        m_cubes;
	MpvLink<PlaybackCtl>            m_link;
	OcrQ                            m_ocr;   // before the grabber:
	                                         // outlives its feeder
	Grabber                         m_grab;
	Facts                           m_facts;
	engine::SemanticEngine<Facts>   m_semantic;
	Refinery                        m_refine;
	Ident                           m_ident;
	PlaybackCtl                     m_playback;
	SearchCtl                       m_search;
	QLabel                          m_state;
	QLabel                          m_info;      // video/time/match, right
	QLabel                          m_pattern;   // live regex, left edge
	QTimer                          m_infoTick;  // time/pause poll
	QTimer                          m_tallyLag;  // debounced tally
	QTimer                          m_ocrSettle; // debounced frame
	                                             // resnapshot
	QElapsedTimer                   m_ocrFirstDirty; // its ceiling
	QTimer                          m_pump;      // engine pump
	std::vector<topics::gloss_entry> m_gloss;    // sidecar, loaded
	agenda::id                      m_rootId;    // pyramid root
	std::vector<agenda::id>         m_chatPending;
	QList<int>                      m_tally;     // hits per video
	QString                         m_tallyKey;  // pattern it is for
	QElapsedTimer                   m_exportTick;
	QFont                           m_baseFont;  // first-launch font
	QList<QFont>                    m_classFonts; // themed classes
	// Zoom state as integer step counts (factor = 1.125^steps):
	// exact comparison, exact reset, no accumulation drift.
	int                             m_zoomBase = 0;
	int                             m_zoomCaptions = 0;
	int                             m_zoomBar = 0;
	int                             m_zoomRegex = 0;
	int                             m_tallyTotal = -1;
	int                             m_exportQueued = -1;
	bool                            m_exportPending = false;
};

#endif // SRTVIEW_SRC_MAINWIN_HPP_
