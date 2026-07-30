// knowledge.hpp -- the knowledge pane: a dockable, keyboard-first
// browser over what the background pipeline has already produced.
//
// Version zero deliberately reads only what exists today: the
// corpus topics (hand-written, committed searches, harvested
// hypotheses -- badged apart), the harvested focus threads, and the
// per-video summaries, each previewing its cache artifact and
// activating into the live machinery -- a topic or focus applies
// its exact pattern to the search (which animates matches, the
// corpus tally, and through it the heat that steers the pipeline),
// a video row switches playback.  No knowledge store yet: rows are
// handed in prebuilt by the owner, which is where corpus and cache
// state already live.  The pane owns presentation only: grouping,
// regex filtering in the app's own pattern dialect, preview, and
// the selection surface the owner wires activation onto (child
// signals + lambdas, the app's mocless convention).
#ifndef SRTVIEW_SRC_KNOWLEDGE_HPP_
#define SRTVIEW_SRC_KNOWLEDGE_HPP_

#include <QDockWidget>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QString>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVector>

// One presentable artifact.  pattern and video/srt are activation
// payloads (either may be empty); path is the cache artifact to
// preview, empty when nothing is cached yet.
struct KnowledgeRow {
	QString group;    // tree section: "Topics", "Focuses", ...
	QString title;
	QString pattern;  // exact pattern text, never rewritten
	QString path;     // artifact file for the preview pane
	QString video;    // playback activation target
	QString srt;
	QString badge;    // "generated", "supportive", "pending", ...
	QString gloss;    // first line, shown inline in the directory
};

// One occurrence of the selected pattern: an exact cue in an exact
// video, carrying everything a playback jump needs.
struct KnowledgeHit {
	QString video;
	QString srt;
	QString line;     // rendered cue text
	double  start = 0.0;
	int     cue   = -1;
};

class KnowledgePane : public QDockWidget
{
public:
	explicit KnowledgePane(QWidget *parent);

	// Replaces the whole model; selection is kept when the same
	// title still exists in the same group.  Cheap at session
	// scale (dozens of rows), so refresh is rebuild.
	void setRows(QVector<KnowledgeRow> rows);

	// The selected row's occurrences, grouped per video; a
	// non-empty set raises the Evidence tab, an empty one falls
	// back to prose.  The owner computes hits (it owns the
	// transcripts); counts are the true per-video totals even when
	// the listing was capped, and the group headers say so.
	void setEvidence(QVector<KnowledgeHit> hits,
	                 QHash<QString, int> const &counts);

	// The selected topic's definition text.  Editable only when
	// the owner says so (a topic row with a durable home to write
	// to); the owner reads the edit back through glossEdit() and
	// its modified flag.
	void setGloss(QString const &text, bool editable);
	QPlainTextEdit &glossEdit() { return m_gloss; }

	// Activation surfaces for the owner: Enter / double-click.
	// Payload rides in item data (roles below).
	QTreeWidget &tree() { return m_tree; }
	QTreeWidget &hits() { return m_hits; }

	// Show, raise and put the keyboard in the filter box.
	void summon();

	enum Role {
		kPattern = Qt::UserRole,
		kPath,
		kVideo,
		kSrt,
		kCue,
	};

private:
	void applyFilter();
	void preview(QTreeWidgetItem const *item);

	QLineEdit               m_filter;
	QTreeWidget             m_tree;
	QTabWidget              m_tabs;
	QTreeWidget             m_hits;
	QPlainTextEdit          m_preview;
	QPlainTextEdit          m_gloss;
	QVector<KnowledgeRow>   m_rows;
};

#endif // SRTVIEW_SRC_KNOWLEDGE_HPP_
