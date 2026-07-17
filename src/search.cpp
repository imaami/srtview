#include "search.hpp"

#include "searchbar.hpp"
#include "srtedit.hpp"

#include <QStatusBar>
#include <QTextDocument>

namespace {

// All matches of re in doc: selections for display, start offsets for
// the position counter.
void collectMatches(QTextDocument *doc, QRegularExpression const &re,
                    QTextCharFormat const &fmt,
                    QList<QTextEdit::ExtraSelection> &sels,
                    std::vector<int> &starts)
{
	QTextCursor c(doc);
	while (true) {
		c = doc->find(re, c);
		if (c.isNull())
			return;
		if (!c.hasSelection() && c.atEnd())
			return;
		if (!c.hasSelection()) {             // zero-length match
			c.movePosition(QTextCursor::NextCharacter);
			continue;
		}
		QTextEdit::ExtraSelection s;
		s.cursor = c;
		s.format = fmt;
		sels << s;
		starts.push_back(c.selectionStart());
	}
}

} // namespace

SearchCtl::SearchCtl(search_bar_base &bar, srt_view_base &view,
                     QStatusBar &status)
	: m_bar(bar), m_view(view), m_status(status)
{
	m_nextAct.setText(QStringLiteral("Find &next"));
	m_nextAct.setShortcut(QKeySequence(Qt::Key_F3));
	QObject::connect(&m_nextAct, &QAction::triggered, &m_nextAct,
	                 [this] { findAgain(false); });
	m_prevAct.setText(QStringLiteral("Find &previous"));
	m_prevAct.setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F3));
	QObject::connect(&m_prevAct, &QAction::triggered, &m_prevAct,
	                 [this] { findAgain(true); });
}

void SearchCtl::showSearch()
{
	m_anchor = m_view.textCursor();
	// Size first: the target x depends on the bar's final width,
	// which before the first layout pass is garbage.
	m_bar.adjustSize();
	m_bar.open(target());
}

void SearchCtl::hideSearch()
{
	m_bar.dismiss();
	m_view.setFocus();
}

// Enter in the search field: the incremental jump has already landed
// on the match, so accept and get out of the way -- the next
// keystroke (t, Space, ...) belongs to the view.
void SearchCtl::commitSearch()
{
	hideSearch();
}

// Pattern edited: refresh highlights and jump to the first match at
// or after where the search began.
void SearchCtl::searchChanged()
{
	highlightAll();
	if (m_matchStarts.empty() || m_bar.pattern().isEmpty())
		return;
	QTextCursor const from = m_anchor.isNull()
		? QTextCursor(m_view.document()) : m_anchor;
	QTextCursor hit = m_view.document()->find(pattern(), from);
	if (hit.isNull())
		hit = m_view.document()->find(pattern(),
		                              QTextCursor(m_view.document()));
	if (!hit.isNull()) {
		m_view.setTextCursor(hit);
		updateCounter(hit);
	}
}

void SearchCtl::findAgain(bool backward)
{
	QRegularExpression const re = pattern();
	if (!re.isValid() || re.pattern().isEmpty())
		return;
	QTextDocument::FindFlags fl;
	if (backward)
		fl |= QTextDocument::FindBackward;
	if (!m_view.find(re, fl)) {
		m_view.moveCursor(backward ? QTextCursor::End
		                           : QTextCursor::Start);
		m_status.showMessage(m_view.find(re, fl)
			? QStringLiteral("search wrapped")
			: QStringLiteral("no match"), 1500);
	}
	updateCounter(m_view.textCursor());
}

void SearchCtl::layoutOverlay()
{
	m_bar.reposition(target());
}

void SearchCtl::setSearchText(QString const &s)
{
	if (m_anchor.isNull() && m_view.cueCount() > 0)
		m_anchor = m_view.textCursor();
	m_bar.setPattern(s);
}

void SearchCtl::setRegexEnabled(bool on)
{
	m_bar.setRegexEnabled(on);
}

QRegularExpression SearchCtl::pattern() const
{
	QString const raw = m_bar.pattern();
	QRegularExpression re(m_bar.regexEnabled()
		? raw : QRegularExpression::escape(raw));
	if (!m_bar.caseSensitive())
		re.setPatternOptions(QRegularExpression::CaseInsensitiveOption);
	return re;
}

void SearchCtl::highlightAll()
{
	QList<QTextEdit::ExtraSelection> sels;
	m_matchStarts.clear();
	QRegularExpression const re = pattern();
	bool const empty = re.pattern().isEmpty();
	if (!re.isValid() && !empty) {
		m_bar.setCount(0, -1);           // invalid-pattern feedback
		m_view.setMatchSelections({});
		return;
	}
	if (!empty) {
		// Alpha over the theme base keeps the theme's own text color
		// readable on both light and dark palettes.
		QColor bg = m_view.palette().color(QPalette::Highlight);
		bg.setAlpha(85);
		QTextCharFormat fmt;
		fmt.setBackground(bg);
		collectMatches(m_view.document(), re, fmt, sels, m_matchStarts);
	}
	m_view.setMatchSelections(sels);
	m_bar.setCount(0, empty ? 0 : int(m_matchStarts.size()));
}

void SearchCtl::updateCounter(QTextCursor const &cur)
{
	int idx = 0;
	int const start = cur.selectionStart();
	for (std::size_t i = 0; i < m_matchStarts.size(); ++i) {
		if (m_matchStarts[i] != start)
			continue;
		idx = int(i) + 1;
		break;
	}
	m_bar.setCount(idx, int(m_matchStarts.size()));
}

QPoint SearchCtl::target() const
{
	return {m_view.width() - m_bar.width() - 24, 10};
}
