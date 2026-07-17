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

SrtEdit::SrtEdit(MainWin *host)
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
	m_glide.setTargetObject(verticalScrollBar());
	m_glide.setPropertyName("value");
	m_glide.setDuration(260);
	m_glide.setEasingCurve(QEasingCurve::OutCubic);
	m_gutter.installEventFilter(this);
	m_gutter.setCursor(Qt::PointingHandCursor);
	connect(verticalScrollBar(), &QScrollBar::valueChanged,
	        &m_gutter, QOverload<>::of(&QWidget::update));
	connect(this, &QTextEdit::textChanged,
	        &m_gutter, QOverload<>::of(&QWidget::update));
	connect(this, &QTextEdit::cursorPositionChanged,
	        this, [this] { updateCurrentCueHighlight(); });
}

void SrtEdit::setCues(std::vector<srt::cue> cues)
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
	m_glide.stop();
	m_playCue = -1;
	m_playSel.clear();
	moveCursor(QTextCursor::Start);
	updateCurrentCueHighlight();
}

int SrtEdit::cueAt(double t) const
{
	const auto it = std::ranges::upper_bound(m_cues, t, {},
	                                         &srt::cue::start);
	if (it == m_cues.begin())
		return -1;
	const auto prev = it - 1;
	return t < prev->end ? int(prev - m_cues.begin()) : -1;
}

void SrtEdit::setPlayTime(double t)
{
	const int cue = cueAt(t);
	if (cue == m_playCue)
		return;
	m_playCue = cue;
	updatePlayHighlight();
	m_gutter.update();
	if (m_follow && cue >= 0)
		glideTo(cue);
}

void SrtEdit::setFollow(bool on)
{
	m_follow = on;
	if (on && m_playCue >= 0)
		glideTo(m_playCue);
	if (!on)
		m_glide.stop();
}

void SrtEdit::updatePlayHighlight()
{
	m_playSel.clear();
	if (m_playCue >= 0) {
		const QTextBlock b = document()->findBlockByNumber(m_playCue);
		ExtraSelection sel;
		sel.cursor = QTextCursor(document());
		sel.cursor.setPosition(b.position());
		sel.cursor.setPosition(b.position() + b.length() - 1,
		                       QTextCursor::KeepAnchor);
		QColor bg = palette().color(QPalette::Highlight);
		bg.setAlpha(55);
		sel.format.setBackground(bg);
		sel.format.setProperty(QTextFormat::FullWidthSelection, true);
		m_playSel = {sel};
	}
	applySelections();
}

// Keep the active cue in the upper third, lyrics-style.
void SrtEdit::glideTo(int cue)
{
	const QTextBlock b = document()->findBlockByNumber(cue);
	if (!b.isValid())
		return;
	const QRectF r = document()->documentLayout()->blockBoundingRect(b);
	const int want = int(r.center().y() - viewport()->height() * 0.35);
	const int target = std::clamp(want, 0,
	                              verticalScrollBar()->maximum());
	m_glide.stop();
	m_glide.setStartValue(verticalScrollBar()->value());
	m_glide.setEndValue(target);
	m_glide.start();
}

void SrtEdit::setMatchSelections(const QList<ExtraSelection> &sel)
{
	m_matchSel = sel;
	applySelections();
}

void SrtEdit::keyPressEvent(QKeyEvent *ev)
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
		case Qt::Key_F:
			if (ev->text() == QStringLiteral("f")) {
				m_host->toggleFollow();
				return;
			}
			break;
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

void SrtEdit::mouseDoubleClickEvent(QMouseEvent *ev)
{
	QTextEdit::mouseDoubleClickEvent(ev);
	if (!m_cues.empty())
		m_host->seekCue(currentCue(), false);
}

void SrtEdit::resizeEvent(QResizeEvent *ev)
{
	QTextEdit::resizeEvent(ev);
	layoutGutter();
	m_host->layoutOverlays();
}

bool SrtEdit::event(QEvent *ev)
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

bool SrtEdit::eventFilter(QObject *obj, QEvent *ev)
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

void SrtEdit::layoutGutter()
{
	const QRect cr = contentsRect();
	m_gutter.setGeometry(cr.left(), cr.top(), m_gutterW, cr.height());
}

// Visible blocks under the current scroll offset, gutter-space rects;
// the visitor binds statically.
template <typename F>
void SrtEdit::visitVisibleBlocks(F f)
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

void SrtEdit::paintGutter()
{
	QPainter p(&m_gutter);
	// Same background as the text, only quieter ink: the gutter
	// should read as part of the page, not as a panel.
	p.fillRect(m_gutter.rect(), palette().color(QPalette::Base));
	QColor dim = palette().color(QPalette::Text);
	dim.setAlpha(110);
	const QColor full = palette().color(QPalette::Text);
	p.setFont(m_gutterFont);
	const int w = m_gutterW - 12;
	const int lineH = fontMetrics().height();
	visitVisibleBlocks([&](const QTextBlock &b, const QRectF &r) {
		const int cue = b.blockNumber();
		if (size_t(cue) < m_cues.size()) {
			const qreal y = r.top() + b.blockFormat().topMargin();
			p.setPen(cue == m_playCue ? full : dim);
			p.drawText(QRectF(0, y, w, lineH),
			           Qt::AlignRight | Qt::AlignVCenter,
			           fmtTime(m_cues[size_t(cue)].start, false));
		}
		return true;
	});
}

int SrtEdit::cueAtGutterY(int y)
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

void SrtEdit::updateCurrentCueHighlight()
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

void SrtEdit::applySelections()
{
	setExtraSelections(m_lineSel + m_playSel + m_matchSel);
}
