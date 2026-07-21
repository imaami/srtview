#include "srtedit.hpp"

#include "scale.hpp"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTextBlock>
#include <QToolTip>

#include <algorithm>
#include <cmath>

srt_view_base::srt_view_base(QWidget *parent)
	: QTextEdit(parent), m_gutter(this)
{
	setReadOnly(true);
	setFrameShape(QFrame::NoFrame);
	setAcceptDrops(false);               // let the host take the drop
	viewport()->setAcceptDrops(false);
	applyType(typeFont());
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

// Anchor the reading position through the relayout: the cue at the
// viewport's center stays at the center, instead of the pixel
// offset deciding which lines survive the zoom.  When integer
// rounding lands on the size already shown there is nothing to do,
// so the document never reflows for a no-visible-change step.
void srt_view_base::setTypeZoom(double z)
{
	m_typeZoom = z;
	QFont const f = typeFont();
	if (f == font())
		return;
	QPoint const mid(viewport()->width() / 2,
	                 viewport()->height() / 2);
	int const anchor = cursorForPosition(mid).blockNumber();
	applyType(f);
	QTextBlock const b = document()->findBlockByNumber(anchor);
	if (!b.isValid())
		return;
	QRectF const r = document()->documentLayout()
	               ->blockBoundingRect(b);
	verticalScrollBar()->setValue(
		int(std::lround(r.center().y())) - mid.y());
}

// The caption font, derived in one place from the application font
// (the base zoom domain) times the caption scale and zoom.  Integer
// points, like every derived font in the program.
QFont srt_view_base::typeFont() const
{
	QFont f = QApplication::font();
	f.setPointSize(std::max(1, int(std::lround(
		std::max(f.pointSize(), 10) * kFontScale * m_typeZoom))));
	return f;
}

void srt_view_base::applyType(QFont const &f)
{
	setFont(f);
	m_gutterFont = f;
	m_gutterFont.setPointSize(std::max(1, int(std::lround(
		f.pointSize() * 0.62))));
	document()->setDefaultFont(f);
	refitGutter();
}

void srt_view_base::refitGutter()
{
	QString const widest = m_cues.empty()
		? QString() : fmtTime(m_cues.back().start, false);
	int const w = QFontMetrics(m_gutterFont)
	              .horizontalAdvance(widest) + 26;
	m_gutter.update();     // glyphs may change at the same width
	if (w == m_gutterW)
		return;
	m_gutterW = w;
	setViewportMargins(m_gutterW, 0, 0, 0);
	layoutGutter();
}

void srt_view_base::setCues(std::vector<srt::cue> cues)
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
	for (srt::cue const &c : m_cues) {
		if (!first)
			cur.insertBlock(bfGap, QTextCharFormat());
		first = false;
		std::string const html = srt::cue_html(c.text);
		cur.insertHtml(QString::fromUtf8(html.data(),
		                                 qsizetype(html.size())));
	}

	refitGutter();
	m_glide.stop();
	m_playCue = -1;
	m_playSel.clear();
	moveCursor(QTextCursor::Start);
	updateCurrentCueHighlight();
}

int srt_view_base::cueAt(double t) const
{
	auto const it = std::ranges::upper_bound(m_cues, t, {},
	                                         &srt::cue::start);
	if (it == m_cues.begin())
		return -1;
	auto const prev = it - 1;
	return t < prev->end ? int(prev - m_cues.begin()) : -1;
}

void srt_view_base::setPlayTime(double t)
{
	int const cue = cueAt(t);
	if (cue == m_playCue)
		return;
	m_playCue = cue;
	updatePlayHighlight();
	m_gutter.update();
	if (m_follow && cue >= 0)
		glideTo(cue);
}

void srt_view_base::setFollow(bool on)
{
	m_follow = on;
	if (on && m_playCue >= 0)
		glideTo(m_playCue);
	if (!on)
		m_glide.stop();
}

void srt_view_base::updatePlayHighlight()
{
	m_playSel.clear();
	if (m_playCue >= 0) {
		QTextBlock const b = document()->findBlockByNumber(m_playCue);
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
void srt_view_base::glideTo(int cue)
{
	QTextBlock const b = document()->findBlockByNumber(cue);
	if (!b.isValid())
		return;
	QRectF const r = document()->documentLayout()->blockBoundingRect(b);
	int const want = int(r.center().y() - viewport()->height() * 0.35);
	int const target = std::clamp(want, 0,
	                              verticalScrollBar()->maximum());
	m_glide.stop();
	m_glide.setStartValue(verticalScrollBar()->value());
	m_glide.setEndValue(target);
	m_glide.start();
}

void srt_view_base::setMatchSelections(QList<ExtraSelection> const &sel)
{
	m_matchSel = sel;
	applySelections();
}

void srt_view_base::setCurrentMatchSelection(QList<ExtraSelection> const &sel)
{
	m_curSel = sel;
	applySelections();
}



void srt_view_base::resizeEvent(QResizeEvent *ev)
{
	QTextEdit::resizeEvent(ev);
	layoutGutter();
}

bool srt_view_base::event(QEvent *ev)
{
	if (ev->type() == QEvent::ToolTip && !m_cues.empty()) {
		auto *he = static_cast<QHelpEvent *>(ev);
		QPoint const vp = viewport()->mapFrom(this, he->pos());
		int const cue = cursorForPosition(vp).blockNumber();
		if (cue >= 0 && size_t(cue) < m_cues.size()) {
			srt::cue const &c = m_cues[size_t(cue)];
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


void srt_view_base::layoutGutter()
{
	QRect const cr = contentsRect();
	m_gutter.setGeometry(cr.left(), cr.top(), m_gutterW, cr.height());
}

// Visible blocks under the current scroll offset, gutter-space rects;
// the visitor binds statically.
template <typename F>
void srt_view_base::visitVisibleBlocks(F f)
{
	auto *lay = document()->documentLayout();
	int const yoff = verticalScrollBar()->value();
	for (QTextBlock b = document()->firstBlock(); b.isValid();
	     b = b.next()) {
		QRectF const r = lay->blockBoundingRect(b).translated(0, -yoff);
		if (r.top() > m_gutter.height())
			break;
		if (r.bottom() < 0)
			continue;
		if (!f(b, r))
			break;
	}
}

void srt_view_base::paintGutter()
{
	QPainter p(&m_gutter);
	// Same background as the text, only quieter ink: the gutter
	// should read as part of the page, not as a panel.
	p.fillRect(m_gutter.rect(), palette().color(QPalette::Base));
	QColor dim = palette().color(QPalette::Text);
	dim.setAlpha(110);
	QColor const full = palette().color(QPalette::Text);
	p.setFont(m_gutterFont);
	int const w = m_gutterW - 12;
	int const lineH = fontMetrics().height();
	visitVisibleBlocks([&](QTextBlock const &b, QRectF const &r) {
		int const cue = b.blockNumber();
		if (size_t(cue) < m_cues.size()) {
			qreal const y = r.top() + b.blockFormat().topMargin();
			p.setPen(cue == m_playCue ? full : dim);
			p.drawText(QRectF(0, y, w, lineH),
			           Qt::AlignRight | Qt::AlignVCenter,
			           fmtTime(m_cues[size_t(cue)].start, false));
		}
		return true;
	});
}

int srt_view_base::cueAtGutterY(int y)
{
	int hit = -1;
	visitVisibleBlocks([&](QTextBlock const &b, QRectF const &r) {
		if (y >= r.top() && y < r.bottom()) {
			hit = b.blockNumber();
			return false;
		}
		return true;
	});
	return (hit >= 0 && size_t(hit) < m_cues.size()) ? hit : -1;
}

void srt_view_base::updateCurrentCueHighlight()
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

void srt_view_base::applySelections()
{
	setExtraSelections(m_lineSel + m_playSel + m_matchSel + m_curSel);
}
