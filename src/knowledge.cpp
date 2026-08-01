// knowledge.cpp -- see knowledge.hpp.
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QRegularExpression>
#include <QSplitter>
#include <QVBoxLayout>
#include <QWidget>

#include "knowledge.hpp"

namespace {

// Preview no more than this many bytes of an artifact: the pane is
// a reading aid, not a pager, and cache files are small anyway.
constexpr qint64 kPreviewCap = 64 * 1024;

QTreeWidgetItem *groupItem(QTreeWidget &tree, QString const &name)
{
	for (int i = 0; i < tree.topLevelItemCount(); ++i)
		if (tree.topLevelItem(i)->text(0) == name)
			return tree.topLevelItem(i);
	auto *g = new QTreeWidgetItem(&tree, {name});
	g->setFlags(Qt::ItemIsEnabled);
	g->setExpanded(true);
	return g;
}

} // namespace

KnowledgePane::KnowledgePane(QWidget *parent)
	: QDockWidget(QStringLiteral("Knowledge"), parent)
{
	setObjectName(QStringLiteral("knowledge"));
	auto *body = new QWidget(this);
	auto *lay = new QVBoxLayout(body);
	lay->setContentsMargins(4, 4, 4, 4);
	lay->setSpacing(4);

	m_filter.setParent(body);
	m_filter.setPlaceholderText(
		QStringLiteral("filter (regex) — Enter: list"));
	m_filter.setClearButtonEnabled(true);
	lay->addWidget(&m_filter);

	auto *split = new QSplitter(Qt::Vertical, body);
	m_tree.setParent(split);
	m_tree.setColumnCount(3);
	m_tree.setHeaderLabels({QStringLiteral("item"),
	                        QStringLiteral("state"),
	                        QStringLiteral("gloss")});
	m_tree.header()->setStretchLastSection(true);
	m_tree.header()->setSectionResizeMode(
		0, QHeaderView::ResizeToContents);
	m_tree.header()->setSectionResizeMode(
		1, QHeaderView::ResizeToContents);
	m_tree.setRootIsDecorated(true);
	m_tree.setUniformRowHeights(true);
	m_tabs.setParent(split);
	m_hits.setColumnCount(2);
	m_hits.setHeaderLabels({QStringLiteral("time"),
	                        QStringLiteral("cue")});
	m_hits.header()->setStretchLastSection(true);
	m_hits.setRootIsDecorated(true);
	m_hits.setUniformRowHeights(true);
	m_preview.setReadOnly(true);
	m_preview.setPlaceholderText(
		QStringLiteral("no artifact cached yet"));
	m_tabs.addTab(&m_hits, QStringLiteral("Evidence"));
	m_tabs.addTab(&m_preview, QStringLiteral("Prose"));
	m_tabs.addTab(&m_gloss, QStringLiteral("Gloss"));
	split->addWidget(&m_tree);
	split->addWidget(&m_tabs);
	split->setStretchFactor(0, 3);
	split->setStretchFactor(1, 2);
	lay->addWidget(split);
	setWidget(body);

	// Self-wired presentation behavior; activation stays the
	// owner's to connect.
	connect(&m_filter, &QLineEdit::textChanged,
	        this, [this] { applyFilter(); });
	connect(&m_filter, &QLineEdit::returnPressed, this, [this] {
		m_tree.setFocus();
		// A visible current row stands; otherwise land on the
		// first row the filter left visible -- Enter must never
		// activate what the filter just hid.
		if (QTreeWidgetItem const *cur = m_tree.currentItem();
		    cur && !cur->isHidden())
			return;
		for (int g = 0; g < m_tree.topLevelItemCount(); ++g) {
			QTreeWidgetItem const *grp =
				m_tree.topLevelItem(g);
			for (int i = 0; i < grp->childCount(); ++i) {
				QTreeWidgetItem *it = grp->child(i);
				if (it->isHidden())
					continue;
				m_tree.setCurrentItem(it);
				return;
			}
		}
	});
	connect(&m_tree, &QTreeWidget::currentItemChanged,
	        this, [this](QTreeWidgetItem *cur, QTreeWidgetItem *) {
		preview(cur);
	});
}

void KnowledgePane::setRows(QVector<KnowledgeRow> rows)
{
	if (rows == m_rows)
		return;
	QString keepGroup, keepTitle;
	if (QTreeWidgetItem const *cur = m_tree.currentItem();
	    cur && cur->parent()) {
		keepGroup = cur->parent()->text(0);
		keepTitle = cur->text(0);
	}
	m_rows = std::move(rows);
	m_tree.clear();
	QTreeWidgetItem *reselect = nullptr;
	for (KnowledgeRow const &r : m_rows) {
		auto *it = new QTreeWidgetItem(
			groupItem(m_tree, r.group),
			{r.title, r.badge, r.gloss});
		it->setData(0, kPattern, r.pattern);
		it->setData(0, kPath, r.path);
		it->setData(0, kVideo, r.video);
		it->setData(0, kSrt, r.srt);
		it->setData(0, kName, r.name);
		it->setToolTip(0, r.pattern.isEmpty() ? r.title
		                                      : r.pattern);
		if (!r.gloss.isEmpty())
			it->setToolTip(2, r.gloss);
		if (r.group == keepGroup && r.title == keepTitle)
			reselect = it;
	}
	if (reselect)
		m_tree.setCurrentItem(reselect);
	applyFilter();
}

void KnowledgePane::setEvidence(QVector<KnowledgeHit> hits,
                                QHash<QString, int> const &counts)
{
	m_hits.clear();
	QTreeWidgetItem *grp = nullptr;
	QString video;
	int listed = 0;
	for (KnowledgeHit const &h : hits) {
		if (!grp || h.video != video) {
			if (grp && counts.value(video) > listed)
				grp->setText(1, grp->text(1)
					+ QStringLiteral(", first %1")
					  .arg(listed));
			video = h.video;
			listed = 0;
			grp = new QTreeWidgetItem(&m_hits,
				{QFileInfo(video).fileName(),
				 QStringLiteral("%1 cues")
				 .arg(counts.value(video))});
			grp->setFlags(Qt::ItemIsEnabled);
			grp->setExpanded(true);
		}
		++listed;
		qint64 const s = qint64(h.start);
		auto *it = new QTreeWidgetItem(grp,
			{QStringLiteral("%1:%2:%3")
			 .arg(s / 3600)
			 .arg(s / 60 % 60, 2, 10, QLatin1Char('0'))
			 .arg(s % 60, 2, 10, QLatin1Char('0')),
			 h.line});
		it->setData(0, kVideo, h.video);
		it->setData(0, kSrt, h.srt);
		it->setData(0, kCue, h.cue);
		it->setToolTip(1, h.line);
	}
	if (grp && counts.value(video) > listed)
		grp->setText(1, grp->text(1)
			+ QStringLiteral(", first %1").arg(listed));
	if (!hits.isEmpty())
		m_tabs.setCurrentWidget(&m_hits);
	else
		m_tabs.setCurrentWidget(&m_preview);
}

void KnowledgePane::setGloss(QString const &text, bool editable)
{
	m_gloss.setPlainText(text);
	m_gloss.setReadOnly(!editable);
	m_gloss.setPlaceholderText(editable
		? QStringLiteral("no gloss yet — write one (one line "
		                 "per point; saved beside the topic "
		                 "file on leaving the entry)")
		: QStringLiteral("glosses live beside the topic file "
		                 "— none is loaded"));
	m_gloss.document()->setModified(false);
}

void KnowledgePane::summon()
{
	show();
	raise();
	m_filter.setFocus();
	m_filter.selectAll();
}

// Hide what the pattern misses, in the app's own dialect; an
// invalid or empty pattern hides nothing.  Groups follow their
// children.
void KnowledgePane::applyFilter()
{
	QRegularExpression const re(m_filter.text(),
		QRegularExpression::CaseInsensitiveOption);
	bool const all = m_filter.text().isEmpty() || !re.isValid();
	for (int g = 0; g < m_tree.topLevelItemCount(); ++g) {
		QTreeWidgetItem *grp = m_tree.topLevelItem(g);
		int visible = 0;
		for (int i = 0; i < grp->childCount(); ++i) {
			QTreeWidgetItem *it = grp->child(i);
			bool const hit = all
				|| re.match(it->text(0)).hasMatch()
				|| re.match(it->text(1)).hasMatch()
				|| re.match(it->text(2)).hasMatch()
				|| re.match(it->data(0, kPattern)
				              .toString()).hasMatch();
			it->setHidden(!hit);
			visible += hit;
		}
		grp->setHidden(!visible);
	}
}

void KnowledgePane::preview(QTreeWidgetItem const *item)
{
	QString const path = item
		? item->data(0, kPath).toString() : QString();
	if (path.isEmpty()) {
		m_preview.clear();
		return;
	}
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly)) {
		m_preview.setPlainText(
			QStringLiteral("(cannot read %1)").arg(path));
		return;
	}
	m_preview.setPlainText(
		QString::fromUtf8(f.read(kPreviewCap)));
}
