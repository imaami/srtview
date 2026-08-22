// knowledge.cpp -- see knowledge.hpp.
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QHeaderView>
#include <QPainter>
#include <QRegularExpression>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

#include "knowledge.hpp"

namespace {

// Preview no more than this many bytes of an artifact: the pane is
// a reading aid, not a pager, and cache files are small anyway.
constexpr qint64 kPreviewCap = 64 * 1024;

// The progress column is a bar, not a word: phases concatenate left
// to right, each sized by its share of the row's units.  Few units
// draw as discrete cells, many as a continuous fill; rows without
// bar data (group headers, focus threads) paint plainly.
class BarDelegate final : public QStyledItemDelegate
{
public:
	using QStyledItemDelegate::QStyledItemDelegate;

	void paint(QPainter *p, QStyleOptionViewItem const &opt,
	           QModelIndex const &idx) const override
	{
		QStyledItemDelegate::paint(p, opt, idx);
		auto const done = idx.data(KnowledgePane::kBarDone)
		                     .value<QList<int>>();
		auto const total = idx.data(KnowledgePane::kBarTotal)
		                      .value<QList<int>>();
		int units = 0;
		for (int const t : total)
			units += t;
		if (!units || done.size() != total.size())
			return;
		static QColor const phase[]{
			{0x3d, 0xae, 0xe9},   // work toward the artifact
			{0xf6, 0x74, 0x00},   // follow-on band
			{0x9b, 0x59, 0xb6}};
		QColor const track = opt.palette.color(QPalette::Mid);
		QRect const r = opt.rect.adjusted(3, 5, -7, -5);
		p->save();
		p->setPen(Qt::NoPen);
		int x = r.x();
		if (units <= 16) {
			int const gap = 2;
			int const cw = std::max(2,
				(r.width() - gap * (units - 1)) / units);
			for (std::size_t i = 0;
			     i < std::size_t(total.size()); ++i)
				for (int k = 0; k < total[qsizetype(i)];
				     ++k) {
					p->setBrush(k < done[qsizetype(i)]
						? phase[i % 3] : track);
					p->drawRect(x, r.y(), cw,
					            r.height());
					x += cw + gap;
				}
		} else {
			for (std::size_t i = 0;
			     i < std::size_t(total.size()); ++i) {
				int const t = total[qsizetype(i)];
				int const w = r.width() * t / units;
				if (!w)
					continue;
				p->setBrush(track);
				p->drawRect(x, r.y(), w, r.height());
				p->setBrush(phase[i % 3]);
				p->drawRect(x, r.y(),
				            w * done[qsizetype(i)]
				              / std::max(1, t),
				            r.height());
				x += w + 1;
			}
		}
		p->restore();
	}

	QSize sizeHint(QStyleOptionViewItem const &opt,
	               QModelIndex const &idx) const override
	{
		QSize s = QStyledItemDelegate::sizeHint(opt, idx);
		if (idx.data(KnowledgePane::kBarDone).isValid())
			s.setWidth(std::max(s.width(),
				6 * opt.fontMetrics.height()));
		return s;
	}
};

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
	                        QStringLiteral("progress"),
	                        QStringLiteral("glossary")});
	m_tree.setItemDelegateForColumn(1, new BarDelegate(&m_tree));
	m_tree.header()->setStretchLastSection(true);
	m_tree.header()->setSectionResizeMode(
		0, QHeaderView::ResizeToContents);
	m_tree.header()->setSectionResizeMode(
		1, QHeaderView::ResizeToContents);
	// A long video name elides instead of shoving the progress
	// and glossary columns off to the right.
	m_tree.header()->setMaximumSectionSize(
		24 * fontMetrics().height());
	m_tree.setRootIsDecorated(true);
	m_tree.setUniformRowHeights(true);
	m_tabs.setParent(split);
	// No header: a timestamp and a quote explain themselves.
	m_hits.setColumnCount(2);
	m_hits.setHeaderHidden(true);
	m_hits.header()->setStretchLastSection(true);
	m_hits.header()->setSectionResizeMode(
		0, QHeaderView::ResizeToContents);
	m_hits.setRootIsDecorated(true);
	m_hits.setUniformRowHeights(true);
	m_preview.setReadOnly(true);
	m_preview.setPlaceholderText(
		QStringLiteral("no artifact cached yet"));
	m_gloss.setReadOnly(true);
	m_gloss.setPlaceholderText(
		QStringLiteral("no glossary entry yet"));
	m_tabs.addTab(&m_hits, QStringLiteral("Matches"));
	m_tabs.addTab(&m_preview, QStringLiteral("Summary"));
	m_tabs.addTab(&m_gloss, QStringLiteral("Glossary"));
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
	QString keepGroup, keepTitle, keepName;
	if (QTreeWidgetItem const *cur = m_tree.currentItem();
	    cur && cur->parent()) {
		QTreeWidgetItem const *top = cur;
		while (top->parent())
			top = top->parent();
		keepGroup = top->text(0);
		keepTitle = cur->text(0);
		keepName = cur->data(0, kName).toString();
	}
	m_rows = std::move(rows);
	m_tree.clear();
	QTreeWidgetItem *byName = nullptr, *byTitle = nullptr;
	QHash<QString, QTreeWidgetItem *> named; // group + '\n' + name
	for (KnowledgeRow const &r : m_rows) {
		QTreeWidgetItem *parent = groupItem(m_tree, r.group);
		if (!r.under.isEmpty())
			parent = named.value(r.group + QLatin1Char('\n')
			                     + r.under, parent);
		auto *it = new QTreeWidgetItem(
			parent, {r.title, QString(), r.gloss});
		if (!r.name.isEmpty())
			named.insert(r.group + QLatin1Char('\n') + r.name, it);
		it->setData(0, kPattern, r.pattern);
		it->setData(0, kPath, r.path);
		it->setData(0, kVideo, r.video);
		it->setData(0, kSrt, r.srt);
		it->setData(0, kName, r.name);
		it->setData(0, kLink, r.link);
		if (!r.total.isEmpty()) {
			it->setData(1, kBarDone,
			            QVariant::fromValue(r.done));
			it->setData(1, kBarTotal,
			            QVariant::fromValue(r.total));
		}
		if (!r.tip.isEmpty())
			it->setToolTip(1, r.tip);
		it->setToolTip(0, r.pattern.isEmpty() ? r.title
		                                      : r.pattern);
		if (!r.gloss.isEmpty())
			it->setToolTip(2, r.gloss);
		if (!byName && !keepName.isEmpty()
		    && r.group == keepGroup && r.name == keepName)
			byName = it;
		if (!byTitle && r.group == keepGroup
		    && r.title == keepTitle)
			byTitle = it;
	}
	// Names outrank titles but may vanish under a rename (a focus
	// row's name is its artifact path): the title match is the
	// fallback, not the loser of a race.
	if (QTreeWidgetItem *const keep = byName ? byName : byTitle)
		m_tree.setCurrentItem(keep);
	applyFilter();
}

void KnowledgePane::setMatches(QVector<KnowledgeHit> hits,
                               QHash<QString, int> const &counts)
{
	m_hits.clear();
	// Grouped by VIDEO, not by contiguous run: a playlist listing
	// one path twice delivers its hits in separate runs, which
	// must land in one group under one honest cap note.  A video
	// row is its (long) name across the full width.
	QHash<QString, QTreeWidgetItem *> groups;
	QHash<QString, int> listed;
	for (KnowledgeHit const &h : hits) {
		QTreeWidgetItem *&grp = groups[h.video];
		if (!grp) {
			grp = new QTreeWidgetItem(&m_hits,
				{QFileInfo(h.video).fileName()});
			grp->setFlags(Qt::ItemIsEnabled);
			grp->setFirstColumnSpanned(true);
			grp->setExpanded(true);
		}
		++listed[h.video];
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
	// Only a capped listing earns a note; a complete one says
	// nothing.
	for (auto it = groups.cbegin(); it != groups.cend(); ++it)
		if (int const n = counts.value(it.key());
		    n > listed.value(it.key()))
			it.value()->setText(0, it.value()->text(0)
				+ QStringLiteral(" — first %1 of %2")
				  .arg(listed.value(it.key())).arg(n));
	if (!hits.isEmpty())
		m_tabs.setCurrentWidget(&m_hits);
	else
		m_tabs.setCurrentWidget(&m_preview);
}

void KnowledgePane::setGloss(QString const &text)
{
	m_gloss.setPlainText(text);
}

bool KnowledgePane::jumpTo(QString const &group, QString const &name)
{
	auto const find = [&](auto const &self, QTreeWidgetItem *it)
		-> QTreeWidgetItem * {
		if (it->data(0, kName).toString() == name)
			return it;
		for (int i = 0; i < it->childCount(); ++i)
			if (QTreeWidgetItem *hit = self(self, it->child(i)))
				return hit;
		return nullptr;
	};
	for (int g = 0; g < m_tree.topLevelItemCount(); ++g) {
		QTreeWidgetItem *grp = m_tree.topLevelItem(g);
		if (grp->text(0) != group)
			continue;
		QTreeWidgetItem *hit = find(find, grp);
		if (!hit || hit == grp)
			return false;
		// The row asked for is shown whatever the filter hides:
		// a cross-link followed on purpose outranks a pattern
		// typed earlier, and the pattern applies again on its
		// next edit.
		hit->setHidden(false);
		for (QTreeWidgetItem *up = hit->parent(); up; up = up->parent()) {
			up->setHidden(false);
			up->setExpanded(true);
		}
		m_tree.setCurrentItem(hit);
		m_tree.scrollToItem(hit);
		return true;
	}
	return false;
}

void KnowledgePane::summon()
{
	show();
	raise();
	m_filter.setFocus();
	m_filter.selectAll();
}

void KnowledgePane::setUiFont(QFont const &f)
{
	setFont(f);
	m_filter.setFont(f);
	m_tree.setFont(f);
	m_tree.header()->setFont(f);
	m_tree.header()->setMaximumSectionSize(
		24 * QFontMetrics(f).height());
	m_tabs.setFont(f);
	m_tabs.tabBar()->setFont(f);
	m_hits.setFont(f);
	m_preview.setFont(f);
	m_gloss.setFont(f);
}

// Hide what the pattern misses, in the app's own dialect; an
// invalid or empty pattern hides nothing.  Groups follow their
// children.
void KnowledgePane::applyFilter()
{
	QRegularExpression const re(m_filter.text(),
		QRegularExpression::CaseInsensitiveOption);
	bool const all = m_filter.text().isEmpty() || !re.isValid();
	// A row shows when it matches or any row below it does; the
	// rows below it show on their own merits.
	auto const show = [&](auto const &self,
	                      QTreeWidgetItem *it) -> bool {
		bool hit = all
			|| re.match(it->text(0)).hasMatch()
			|| re.match(it->text(2)).hasMatch()
			|| re.match(it->data(0, kPattern).toString()).hasMatch();
		for (int i = 0; i < it->childCount(); ++i)
			hit |= self(self, it->child(i));
		it->setHidden(!hit);
		return hit;
	};
	for (int g = 0; g < m_tree.topLevelItemCount(); ++g) {
		QTreeWidgetItem *grp = m_tree.topLevelItem(g);
		bool visible = false;
		for (int i = 0; i < grp->childCount(); ++i)
			visible |= show(show, grp->child(i));
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
