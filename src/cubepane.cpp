// cubepane.cpp -- see cubepane.hpp.
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
		kids << kid;
	}
	top->addChildren(kids);
}

void CubePane::setVideos(QList<CubeVideo> const &videos)
{
	// Survive the rebuild: which videos were open, and where the
	// keyboard was.
	QString cur;
	if (QTreeWidgetItem const *c = m_tree.currentItem())
		cur = c->data(0, kId).toString();
	QSet<QString> open;
	for (int i = 0; i < m_tree.topLevelItemCount(); ++i) {
		QTreeWidgetItem const *t = m_tree.topLevelItem(i);
		if (t->isExpanded())
			open.insert(t->data(0, kId).toString());
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
		if (v.cubes <= 0)
			continue;
		if (open.contains(v.id)) {
			fill(top);
			top->setExpanded(true);
		} else {
			// The lazy placeholder: makes the row expandable,
			// swept away by the first real fill.
			new QTreeWidgetItem(top);
		}
		if (!cur.isEmpty() && v.id == cur)
			m_tree.setCurrentItem(top);
	}
}

void CubePane::summon()
{
	show();
	raise();
	m_tree.setFocus();
}
