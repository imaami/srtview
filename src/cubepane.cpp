// cubepane.cpp -- see cubepane.hpp.
#include <QRect>
#include <QSet>

#include "cubepane.hpp"
#include "timefmtq.hpp"

namespace {

QString rowText(ocr::region const &r)
{
	return QStringLiteral("%1–%2  ×%3  %4")
		.arg(fmtTime(r.t0, false), fmtTime(r.t1, false))
		.arg(r.sightings)
		.arg(QString::fromUtf8(r.consensus.data(),
		                       qsizetype(r.consensus.size())));
}

QString rowTip(ocr::region const &r)
{
	return QStringLiteral("box %1,%2 %3×%4  jitter %5\n"
	                      "sightings %6\npattern: %7")
		.arg(r.x).arg(r.y).arg(r.w).arg(r.h).arg(r.jitter)
		.arg(r.sightings)
		.arg(QString::fromUtf8(r.pattern.data(),
		                       qsizetype(r.pattern.size())));
}

} // namespace

CubePane::CubePane(QWidget *parent)
	: QDockWidget(QStringLiteral("Time cubes"), parent),
	  m_tree(this)
{
	setObjectName(QStringLiteral("cubes"));
	m_tree.setHeaderHidden(true);
	m_tree.setRootIsDecorated(true);
	m_tree.setUniformRowHeights(true);
	setWidget(&m_tree);
	// Lazy fill on expand -- but only while the placeholder still
	// sits there (it carries no time data): a video refreshed and
	// re-expanded by setVideos() has real children already, and
	// filling again would double the work.
	connect(&m_tree, &QTreeWidget::itemExpanded, this,
	        [this](QTreeWidgetItem *it) {
		if (!it->parent() && it->childCount() == 1
		    && !it->child(0)->data(0, kTime).isValid())
			fill(it);
	});
}

void CubePane::setFetch(fetch_fn f, void *ctx)
{
	m_fetch = f;
	m_ctx = ctx;
}

void CubePane::fill(QTreeWidgetItem *top)
{
	if (!m_fetch)
		return;
	QString const id = top->data(0, kId).toString();
	std::span<ocr::region const> const regs = m_fetch(m_ctx, id);
	// Rebuild flat: the weave is deterministic and a region list
	// only ever changes wholesale at a resnapshot.  takeChildren()
	// detaches without deleting -- the old items are ours to free.
	qDeleteAll(top->takeChildren());
	QString const video = top->data(0, kVideo).toString();
	QString const srt = top->data(0, kSrt).toString();
	QList<QTreeWidgetItem *> kids;
	kids.reserve(qsizetype(regs.size()));
	for (ocr::region const &r : regs) {
		auto *const kid = new QTreeWidgetItem;
		kid->setText(0, rowText(r));
		kid->setToolTip(0, rowTip(r));
		kid->setData(0, kVideo, video);
		kid->setData(0, kSrt, srt);
		kid->setData(0, kId, id);
		kid->setData(0, kTime, r.t0);
		kid->setData(0, kBox, QRect(r.x, r.y, r.w, r.h));
		kids << kid;
	}
	top->addChildren(kids);
}

void CubePane::setVideos(QList<CubeVideo> const &videos)
{
	// Survive the rebuild: which videos were open, and where the
	// keyboard was -- down to the region row, by its time, since a
	// resnapshot that degrades a selected region to its parent
	// would break the survive-contract above.
	QString cur, curSrt;
	QVariant curT, curBox;
	if (QTreeWidgetItem const *c = m_tree.currentItem()) {
		cur = c->data(0, kId).toString();
		curSrt = c->data(0, kSrt).toString();
		curT = c->data(0, kTime);
		curBox = c->data(0, kBox);
	}
	// Expansion keyed like selection: the (id, srt) entry, not the
	// id alone, so duplicate-video rows keep their own state.
	QSet<QString> open;
	for (int i = 0; i < m_tree.topLevelItemCount(); ++i) {
		QTreeWidgetItem const *t = m_tree.topLevelItem(i);
		if (t->isExpanded())
			open.insert(t->data(0, kId).toString()
			            + QLatin1Char('\n')
			            + t->data(0, kSrt).toString());
	}
	m_tree.clear();
	m_total = 0;
	for (CubeVideo const &v : videos) {
		m_total += v.cubes;
		auto *const top = new QTreeWidgetItem(&m_tree);
		top->setText(0, QStringLiteral("%1  (%2)")
		                .arg(v.title).arg(v.cubes));
		top->setData(0, kVideo, v.video);
		top->setData(0, kSrt, v.srt);
		top->setData(0, kId, v.id);
		// The video-row selection restores whatever the count --
		// a video whose cubes all dissolved keeps the keyboard.
		// Matched by (id, srt): duplicate entries pairing one
		// video's bytes with alternate transcripts are distinct
		// rows, and id alone would hand every one the selection
		// in turn, the last winning.
		bool const mine = !cur.isEmpty() && v.id == cur
		               && v.srt == curSrt;
		if (mine)
			m_tree.setCurrentItem(top);
		if (v.cubes <= 0)
			continue;
		if (open.contains(v.id + QLatin1Char('\n') + v.srt)) {
			fill(top);
			top->setExpanded(true);
			// A selected region survives by (time, box) --
			// t0 alone is shared by every region of one
			// frame; one the weave dissolved falls back to
			// its video row.
			if (mine && curT.isValid() && curBox.isValid())
				for (int k = 0; k < top->childCount(); ++k)
					if (top->child(k)->data(0, kTime)
					    == curT
					    && top->child(k)->data(0, kBox)
					       == curBox) {
						m_tree.setCurrentItem(
							top->child(k));
						break;
					}
		} else {
			// The lazy placeholder: makes the row expandable,
			// swept away by the first real fill.
			new QTreeWidgetItem(top);
		}
	}
}

void CubePane::summon()
{
	show();
	raise();
	m_tree.setFocus();
}
