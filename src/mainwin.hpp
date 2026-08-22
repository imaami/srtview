// mainwin.hpp -- composition root: owns the components and the
// controllers, builds the chrome, wires the pieces, and runs the
// open/close flow.  Nothing depends on this header except its
// drivers (main, selftest).
#ifndef SRTVIEW_SRC_MAINWIN_HPP_
#define SRTVIEW_SRC_MAINWIN_HPP_

#include "agenda.hpp"
#include "discovery.hpp"
#include "exporter.hpp"
#include "facts.hpp"
#include "grabber.hpp"
#include "knowledge.hpp"
#include "mpvlink.hpp"
#include "playback.hpp"
#include "prefs.hpp"
#include "search.hpp"
#include "searchbar.hpp"
#include "semantic_engine.hpp"
#include "srtedit.hpp"
#include "topics.hpp"
#include "trail.hpp"

#include <QHash>
#include <QLabel>
#include <QMainWindow>
#include <QRegularExpression>

#include <cstddef>
#include <set>
#include <string>
#include <vector>

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

	// One background topic scan toward a dive: the corpus hits of
	// one expanded pattern, collected a video per timer tick so a
	// corpus load never stalls the UI thread.  The matcher stays
	// QRegularExpression -- it is the app's pattern dialect -- and
	// everything else is std.
	struct DiveScan {
		QRegularExpression      re;
		std::string             pattern;  // expanded, for the journal
		std::string             parts;    // excerpts, UTF-8
		std::vector<agenda::id> deps;     // leaves of hit videos
		agenda::id              id;       // hash of the pattern
		std::size_t             video     = 0;
		bool                    exported  = true;
		bool                    generated = false; // from a focus
	};

	// A finished first-generation dive, kept for pairing: two that
	// share a hit video stage a probe over their cache files.  The
	// matched lines ride along to ground the pair's probe in raw
	// transcript; scans re-run every session, so retention costs
	// memory only, at most kDiveBudget each.
	struct FinishedDive {
		agenda::id              id;
		std::vector<agenda::id> keys;
		std::string             pattern;
		std::string             parts;    // raw matched lines
	};

	// One staged terms window: a cue range of one transcript whose
	// extraction the harvest maps back onto its video.  Windows
	// re-derive deterministically each session, so records exist
	// for cached replies too.
	struct TermsWork {
		agenda::id id;
		QString    video, srt;
		int        first = 0, last = 0;
	};

	// Directory metadata for one term topic, rebuilt every session
	// from the cached replies by the same harvest that adopts: the
	// display term (the topic name is an opaque termN), its kind,
	// and the machine's gloss (shown unless the external sidecar
	// overrides it).
	struct TermInfo {
		QString term;
		QString kind;
		QString gloss;
	};

	// A dive pair awaiting its interactive focus: the probe asks
	// what to search, the app runs the search, the write turns the
	// evidence into prose.  The probe's ask carries a TRANSCRIPT
	// sample of both dives' matched lines, kept here so a retry
	// re-sends the same evidence.  One feedback retry covers a
	// missing, invalid or matchless regex; cache files gate every
	// step, so records rebuild from them each session.
	struct PendingFocus {
		agenda::id              probe;    // the staged ask
		agenda::id              retry;    // corrected ask, or none
		agenda::id              focus;    // write task / pair id
		std::vector<agenda::id> deps;     // the two dive ids
		std::vector<agenda::id> keys;     // union of pair keys
		std::string             note;     // "apat ~ bpat"
		std::string             raw;      // TRANSCRIPT section
		bool                    scanning = false;
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
	void rebuildCorpus(bool fresh);
	void adoptVideo(QString const &video, QString const &srt);
	agenda::id offerFacts(QString const &srt);
	void queueDives(bool fresh);
	void stageDive(std::string const &pattern, bool exported,
	               bool generated);
	void diveStep();
	void scanDiveVideo(DiveScan &s, PlayItem const &it);
	void finishDive(DiveScan &s);
	void pairFocus(DiveScan const &s);
	void stageProbe(FinishedDive const &a, DiveScan const &b);
	agenda::task probeTask(PendingFocus const &w,
	                       agenda::id ask) const;
	void pumpProbes();
	bool pumpProbe(PendingFocus &w);
	bool retryProbe(PendingFocus &w, std::string const &feedback);
	void stageFocusScan(agenda::id id, std::string const &pattern,
	                    QRegularExpression const &re);
	bool finishProbe(DiveScan const &s, PendingFocus &w);
	std::size_t focusWorkOf(agenda::id id) const;
	void harvestFocus();
	void harvestOne(QString const &file);
	void seedGenerated();
	void queueTerms();
	void harvestTerms();
	bool harvestTermsOne(TermsWork const &w);
	static QRegularExpression termMatcher(QString const &term);
	int corpusHits(QRegularExpression const &re, int cap);
	QStringList termLines(QString const &term, int cap);
	void stageSpellPair(QString const &a, QString const &b,
	                    QString const &title);
	void harvestSpell();
	void tallySpellVote(agenda::id vote, int &same, int &diff);
	std::string expandOf(std::string const &name) const;
	void retireDive(agenda::id id);
	void stageTopic(std::string const &name);
	void indexSpellings(QStringList const &seen,
	                    QString const &owner);
	void stageMerge();
	void harvestMerge();
	void foldLine(QString const &line);
	void dropTopic(QString const &name);
	bool mergeSpelling(QString const &owner, QString const &spell);
	void refreshKnowledge();
	void knowledgeSelected(QTreeWidgetItem const *item);
	void semanticSelected(QTreeWidgetItem const *item);
	void chatAsked();
	void semanticStep();
	void feedLexicon();
	void showEvidence(std::vector<semantic::citation> const &cites);
	QString glossPath() const;
	void loadGloss();
	void showGloss(QTreeWidgetItem const *item);
	qsizetype playlistIndex(QString const &video);
	qsizetype indexOfId(QString const &id) const;
	QList<play_entry> corpusEntries();
	void grabsIdle() override;
	void grabProgress() override;
	void startExport();
	void runExport(bool drained);
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
	MpvLink<PlaybackCtl>            m_link;
	Grabber                         m_grab;
	Facts                           m_facts;
	engine::SemanticEngine<Facts>   m_semantic;
	PlaybackCtl                     m_playback;
	SearchCtl                       m_search;
	QLabel                          m_state;
	QLabel                          m_info;      // video/time/match, right
	QLabel                          m_pattern;   // live regex, left edge
	QTimer                          m_infoTick;  // time/pause poll
	QTimer                          m_tallyLag;  // debounced tally
	QTimer                          m_diveTick;  // topic scan pump
	QTimer                          m_pump;      // harvest pump
	std::vector<DiveScan>           m_diveScans; // staged scans
	std::size_t                     m_diveAt = 0;// scan cursor
	unsigned                        m_pumped = 0;// pump ticks
	std::vector<FinishedDive>       m_dives;     // for pairing
	std::set<std::string>           m_diveRetired; // superseded
	                                             // mid-session
	// A written focus file awaiting harvest, still owned by its
	// pair: the dive ids ride along so retiring a dive can pull
	// the chain's conclusion too.
	struct PendingFile {
		agenda::id              id;
		std::vector<agenda::id> deps;
	};
	std::vector<PendingFocus>       m_focusWork; // probe chains
	std::vector<PendingFile>        m_focusPending; // to harvest
	std::set<std::string>           m_generated; // machine topic names
	std::set<std::string>           m_harvested; // focus ids seen
	std::vector<topics::gloss_entry> m_gloss;    // sidecar, loaded
	std::vector<TermsWork>          m_termsWork; // staged windows
	std::set<std::string>           m_termsSeen; // harvested ids
	// One nominated-pair verdict in flight: three salted votes over
	// the same pre-substituted evidence; two SAME fold the suspect
	// into the anchor's owner (and the nominated corrected
	// spelling takes the title), any settled majority retires the
	// pair.
	struct SpellWork {
		QString    a, b;      // anchor and suspect display terms
		QString    title;     // nominated corrected spelling
		agenda::id vote[3];
	};
	std::vector<SpellWork>          m_spellWork; // verdicts pending
	std::set<QString>               m_spellSeen; // settled pairs
	agenda::id                      m_mergeId;   // directory fold ask
	QHash<QString, QString>         m_mergeSet;  // folded -> staged term
	std::set<std::string>           m_mergeSeen; // folded reply ids
	std::set<std::string>           m_termTopics;// index-only names
	QHash<QString, TermInfo>        m_termInfo;  // name -> directory
	QHash<QString, QString>         m_termIndex; // folded term -> name
	qsizetype                       m_lexiconSize = -1; // spellings
	                                             // the engine has
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
