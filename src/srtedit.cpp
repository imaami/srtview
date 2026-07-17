#include "srtedit.hpp"

#include "mainwin.hpp"

#include <QAbstractTextDocumentLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTextBlock>
#include <QToolTip>

#include <algorithm>

template <class Host>
SrtEdit<Host>::SrtEdit(Host *host)
	: QTextEdit(host), m_host(host), m_gutter(this)
{
	setReadOnly(true);
	setFrameShape(QFrame::NoFrame);
	setAcceptDrops(false);               // let the host take the drop
	viewport()->setAcceptDrops(false);
	QFont f = font();
	f.setPointSizeF(std::max(f.pointSizeF(), 10.0) * kFontScale);
	setFont(f);
	m_gutterFont = f;
	m_gutterFont.setPointSizeF(f.pointSizeF() * 0.62);
	document()->setDocumentMargin(28);
	m_gutter.installEventFilter(this);
	m_gutter.setCursor(Qt::PointingHandCursor);
	connect(verticalScrollBar(), &QScrollBar::valueChanged,
	        &m_gutter, QOverload<>::of(&QWidget::update));
	connect(this, &QTextEdit::textChanged,
	        &m_gutter, QOverload<>::of(&QWidget::update));
	connect(this, &QTextEdit::cursorPositionChanged,
	        this, [this] { updateCurrentCueHighlight(); });
}

template <class Host>
void SrtEdit<Host>::setCues(std::vector<srt::cue> cues)
{
	m_cues = std::move(cues);
	QTextDocument *doc = document();
	doc->clear();
	doc->setDefaultFont(font());

	QTextBlockFormat bf;
	bf.setLineHeight(126, QTextBlockFormat::ProportionalHeight);
	QTextBlockFormat bfGap = bf;
	bfGap.setTopMargin(kCueGap);

	QTextCursor cur(doc);
	cur.setBlockFormat(bf);
	bool first = true;
	for (const srt::cue &c : m_cues) {
		if (!first)
			cur.insertBlock(bfGap, QTextCharFormat());
		first = false;
		const std::string html = srt::cue_html(c.text);
		cur.insertHtml(QString::fromUtf8(html.data(),
		                                 qsizetype(html.size())));
	}

	// gutter width from the widest start time in this file
	const QString widest = m_cues.empty()
		? QString() : fmtTime(m_cues.back().start, false);
	m_gutterW = QFontMetrics(m_gutterFont).horizontalAdvance(widest) + 26;
	setViewportMargins(m_gutterW, 0, 0, 0);
	layoutGutter();
	moveCursor(QTextCursor::Start);
	updateCurrentCueHighlight();
}

template <class Host>
void SrtEdit<Host>::setMatchSelections(const QList<ExtraSelection> &sel)
{
	m_matchSel = sel;
	applySelections();
}

template <class Host>
void SrtEdit<Host>::keyPressEvent(QKeyEvent *ev)
{
	if (!(ev->modifiers() & (Qt::ControlModifier | Qt::AltModifier
	                         | Qt::MetaModifier))) {
		switch (ev->key()) {
		case Qt::Key_Return:
		case Qt::Key_Enter:
		case Qt::Key_T:
			if (!m_cues.empty())
				m_host->seekCue(currentCue(),
				                ev->text() == QStringLiteral("T"));
			return;
		case Qt::Key_Space:
			m_host->togglePause();
			return;
		case Qt::Key_Left:
			m_host->seekRel(-5.0);
			return;
		case Qt::Key_Right:
			m_host->seekRel(5.0);
			return;
		case Qt::Key_Escape:
			m_host->hideSearch();
			return;
		case Qt::Key_C:
			if (ev->text() == QStringLiteral("c")) {
				m_host->setPause(false);
				return;
			}
			break;
		case Qt::Key_P:
			if (ev->text() == QStringLiteral("P")) {
				m_host->setPause(true);
				return;
			}
			break;
		case Qt::Key_Slash:
			m_host->showSearch();
			return;
		case Qt::Key_N:
			m_host->findAgain(ev->text() == QStringLiteral("N"));
			return;
		default:
			break;
		}
	}
	QTextEdit::keyPressEvent(ev);
}

template <class Host>
void SrtEdit<Host>::mouseDoubleClickEvent(QMouseEvent *ev)
{
	QTextEdit::mouseDoubleClickEvent(ev);
	if (!m_cues.empty())
		m_host->seekCue(currentCue(), false);
}

template <class Host>
void SrtEdit<Host>::resizeEvent(QResizeEvent *ev)
{
	QTextEdit::resizeEvent(ev);
	layoutGutter();
	m_host->layoutOverlays();
}

template <class Host>
bool SrtEdit<Host>::event(QEvent *ev)
{
	if (ev->type() == QEvent::ToolTip && !m_cues.empty()) {
		auto *he = static_cast<QHelpEvent *>(ev);
		const QPoint vp = viewport()->mapFrom(this, he->pos());
		const int cue = cursorForPosition(vp).blockNumber();
		if (cue >= 0 && size_t(cue) < m_cues.size()) {
			const srt::cue &c = m_cues[size_t(cue)];
			QToolTip::showText(he->globalPos(),
				QStringLiteral("#%1   %2 \u2192 %3")
				.arg(cue + 1)
				.arg(fmtTime(c.start, true), fmtTime(c.end, true)),
				this);
			return true;
		}
	}
	return QTextEdit::event(ev);
}

template <class Host>
bool SrtEdit<Host>::eventFilter(QObject *obj, QEvent *ev)
{
	if (obj == &m_gutter) {
		if (ev->type() == QEvent::Paint) {
			paintGutter();
			return true;
		}
		if (ev->type() == QEvent::MouseButtonPress) {
			auto *me = static_cast<QMouseEvent *>(ev);
			const int cue = cueAtGutterY(int(me->position().y()));
			if (cue >= 0) {
				setTextCursor(QTextCursor(
					document()->findBlockByNumber(cue)));
				m_host->seekCue(cue, false);
			}
			return true;
		}
	}
	return QTextEdit::eventFilter(obj, ev);
}

template <class Host>
void SrtEdit<Host>::layoutGutter()
{
	const QRect cr = contentsRect();
	m_gutter.setGeometry(cr.left(), cr.top(), m_gutterW, cr.height());
}

// Visible blocks under the current scroll offset, gutter-space rects;
// the visitor binds statically.
template <class Host>
template <typename F>
void SrtEdit<Host>::visitVisibleBlocks(F f)
{
	auto *lay = document()->documentLayout();
	const int yoff = verticalScrollBar()->value();
	for (QTextBlock b = document()->firstBlock(); b.isValid();
	     b = b.next()) {
		const QRectF r = lay->blockBoundingRect(b).translated(0, -yoff);
		if (r.top() > m_gutter.height())
			break;
		if (r.bottom() < 0)
			continue;
		if (!f(b, r))
			break;
	}
}

template <class Host>
void SrtEdit<Host>::paintGutter()
{
	QPainter p(&m_gutter);
	// Same background as the text, only quieter ink: the gutter
	// should read as part of the page, not as a panel.
	p.fillRect(m_gutter.rect(), palette().color(QPalette::Base));
	QColor ink = palette().color(QPalette::Text);
	ink.setAlpha(110);
	p.setPen(ink);
	p.setFont(m_gutterFont);
	const int w = m_gutterW - 12;
	const int lineH = fontMetrics().height();
	visitVisibleBlocks([&](const QTextBlock &b, const QRectF &r) {
		const int cue = b.blockNumber();
		if (size_t(cue) < m_cues.size()) {
			const qreal y = r.top() + b.blockFormat().topMargin();
			p.drawText(QRectF(0, y, w, lineH),
			           Qt::AlignRight | Qt::AlignVCenter,
			           fmtTime(m_cues[size_t(cue)].start, false));
		}
		return true;
	});
}

template <class Host>
int SrtEdit<Host>::cueAtGutterY(int y)
{
	int hit = -1;
	visitVisibleBlocks([&](const QTextBlock &b, const QRectF &r) {
		if (y >= r.top() && y < r.bottom()) {
			hit = b.blockNumber();
			return false;
		}
		return true;
	});
	return (hit >= 0 && size_t(hit) < m_cues.size()) ? hit : -1;
}

template <class Host>
void SrtEdit<Host>::updateCurrentCueHighlight()
{
	ExtraSelection sel;
	sel.cursor = textCursor();
	sel.cursor.clearSelection();
	QColor bg = palette().color(QPalette::Highlight);
	bg.setAlpha(34);
	sel.format.setBackground(bg);
	sel.format.setProperty(QTextFormat::FullWidthSelection, true);
	m_lineSel = {sel};
	applySelections();
	m_gutter.update();
}

template <class Host>
void SrtEdit<Host>::applySelections()
{
	setExtraSelections(m_lineSel + m_matchSel);
}

template class SrtEdit<MainWin>;
