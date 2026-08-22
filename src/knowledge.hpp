// knowledge.hpp -- the knowledge pane: a dockable, keyboard-first
// browser over what the background pipeline has already produced.
//
// The pane reads the current corpus topics, harvested focus threads,
// per-video summaries and evidence-backed semantic records, each
// previewing its cache artifact or exact citations and
// activating into the live machinery -- a topic or focus applies
// its exact pattern to the search (which animates matches, the
// corpus tally, and through it the heat that steers the pipeline),
// a video row switches playback.  Rows are handed in prebuilt: the
// pane owns presentation only -- grouping,
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
// preview, empty when nothing is cached yet.  done/total drive the
// progress column's bar, one pair entry per phase (empty = no
// bar); tip is that cell's tooltip -- the words behind the bar.
// under names the row this one nests below within its group (by
// that row's name, which must have been handed in first); empty
// rows sit directly under the group.  link names a row of the same
// group that activating this one jumps to -- a graph's cross-link
// drawn in a tree.
struct KnowledgeRow {
	QString    group;    // tree section: "Topics", "Focuses", ...
	QString    under;    // parent row's name, or empty
	QString    link;     // row to jump to on activation, or empty
	QString    title;
	QString    pattern;  // exact pattern text, never rewritten
	QString    path;     // artifact file for the preview pane
	QString    video;    // playback activation target
	QString    srt;
	QString    tip;      // progress tooltip: "acronym · terms 3/7"
	QString    gloss;    // first line, shown inline
	QString    name;     // corpus name behind a retitled row
	QList<int> done;     // per-phase finished units
	QList<int> total;    // per-phase staged units

	bool operator==(KnowledgeRow const &) const = default;
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
	// row still exists in the same group -- by name when it
	// survived, else by title (an artifact-path name can vanish
	// under a vault rename).  Nested rows start collapsed: a group
	// reads as its unique entries, each opening on its leaves.
	// Cheap at session scale, so refresh is rebuild -- except an
	// unchanged model, which is a no-op: the owner refreshes on a
	// timer, and a gratuitous rebuild would re-emit the selection
	// (rescanning transcripts).
	void setRows(QVector<KnowledgeRow> rows);

	// The selected row's occurrences, grouped per video; a
	// non-empty set raises the Matches tab, an empty one falls
	// back to prose.  The owner computes hits (it owns the
	// transcripts); counts are the true per-video totals, and a
	// capped video row says so ("first N of M") while a complete
	// one is just the name.
	void setMatches(QVector<KnowledgeHit> hits,
	                QHash<QString, int> const &counts);

	// The selected topic's glossary text: the sidecar entry beside
	// the topic file when one exists (external edits always win),
	// else the machine's.  Read-only -- glossary text is generated
	// or edited outside the app, never typed here.
	void setGloss(QString const &text);

	// Activation surfaces for the owner: Enter / double-click.
	// Payload rides in item data (roles below).
	QTreeWidget &tree() { return m_tree; }
	QTreeWidget &hits() { return m_hits; }

	// Show, raise and put the keyboard in the filter box.
	void summon();

	// Makes the named row of the group current, its ancestors
	// opened, and scrolls to it; false when no such row.
	bool jumpTo(QString const &group, QString const &name);

	// Base-domain chrome scaling, child by child: platform themes
	// pin per-class fonts that outrank parent propagation, so a
	// font handed to the dock alone moves nothing inside it.
	void setUiFont(QFont const &f);

	enum Role {
		kPattern = Qt::UserRole,
		kPath,
		kVideo,
		kSrt,
		kCue,
		kName,
		kBarDone,
		kBarTotal,
		kLink,
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
