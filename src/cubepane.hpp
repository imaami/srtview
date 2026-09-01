// cubepane.hpp -- the time-cube browser: the woven OCR regions
// (loom.hpp) as a dockable tree, one top-level row per video, one
// child per region -- deduped, de-glitched, consensus text with
// its temporal extent.  The raw archive is hundreds of
// near-identical glitchy fragments per slide; this pane shows the
// clear signal the weave already recovers in memory, and fills
// live as resnapshots land.  Children build lazily on expand (a
// live corpus carries thousands of regions per video), and the
// region lists stay with the owner -- the pane borrows them
// through a fetch callback and keeps no second copy.
#ifndef SRTVIEW_SRC_CUBEPANE_HPP_
#define SRTVIEW_SRC_CUBEPANE_HPP_

#include <QDockWidget>
#include <QList>
#include <QString>
#include <QTreeWidget>

#include <span>

#include "loom.hpp"

// One video row: display title, activation payload, and the
// region count shown beside the title.
struct CubeVideo {
	QString title;
	QString video;   // path, for activation
	QString srt;
	QString id;      // content id -- the fetch key
	int     cubes = 0;
};

class CubePane : public QDockWidget
{
public:
	// Item data roles; children carry all five, tops the first
	// three.  Time alone is no identity -- every region first
	// sighted in one frame shares its t0 -- so selection identity
	// is (time, box).
	static constexpr int kVideo = Qt::UserRole;
	static constexpr int kSrt   = Qt::UserRole + 1;
	static constexpr int kId    = Qt::UserRole + 2;
	static constexpr int kTime  = Qt::UserRole + 3;
	static constexpr int kBox   = Qt::UserRole + 4;

	explicit CubePane(QWidget *parent);

	// The owner lends the woven regions on demand -- called on
	// expand and on refresh of an expanded video.  The span must
	// stay valid until the next setVideos()/setFetch() call.
	using fetch_fn = std::span<ocr::region const>
		(*)(void *ctx, QString const &id);
	void setFetch(fetch_fn f, void *ctx);

	// Replace the video rows.  Expanded videos re-fill from the
	// fetch, collapsed ones keep a lazy placeholder; expansion
	// state and the current row survive when the same id remains.
	void setVideos(QList<CubeVideo> const &videos);

	int totalCubes() const { return m_total; }

	// Activation surface for the owner: double-click / Enter on a
	// child seeks its video to the region's first sighting.
	QTreeWidget &tree() { return m_tree; }

	void summon();

private:
	void fill(QTreeWidgetItem *top);

	QTreeWidget m_tree;
	fetch_fn    m_fetch = nullptr;
	void       *m_ctx = nullptr;
	int         m_total = 0;
};

#endif // SRTVIEW_SRC_CUBEPANE_HPP_
