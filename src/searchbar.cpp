#include "searchbar.hpp"

#include "scale.hpp"

#include <QApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

search_bar_base::search_bar_base(QWidget *parent)
	: QWidget(parent)
{
	// Clicking the bar's own surface (not a child) focuses the bar:
	// that is how the chrome zoom domain is reached by mouse.
	setFocusPolicy(Qt::ClickFocus);
	setAutoFillBackground(true);
	// Buttons keep tight padding: the chrome font matches the
	// caption scale, and style-proportional gaps grow too wide at
	// that size.
	setStyleSheet(QStringLiteral(
		"SearchBar, QWidget { background: palette(window); }"
		"QLineEdit { border: none; background: palette(base);"
		"            padding: 3px 6px; border-radius: 4px; }"
		"QToolButton { padding: 1px 3px; }"));
	auto *frame = new QHBoxLayout(this);
	frame->setContentsMargins(10, 8, 10, 8);
	frame->setSpacing(1);

	m_edit.setPlaceholderText(QStringLiteral("search\u2026"));
	m_edit.installEventFilter(this);
	frame->addWidget(&m_edit);

	m_regex.setText(QStringLiteral(".*"));
	m_regex.setCheckable(true);
	m_regex.setChecked(true);       // regexp on by default
	m_regex.setAutoRaise(true);
	m_regex.setToolTip(QStringLiteral("Regular expression"));
	frame->addWidget(&m_regex);

	m_case.setText(QStringLiteral("Aa"));
	m_case.setCheckable(true);
	m_case.setChecked(false);       // case-insensitive by default
	m_case.setAutoRaise(true);
	m_case.setToolTip(QStringLiteral("Match case"));
	frame->addWidget(&m_case);

	m_prev.setText(QStringLiteral("\u25b2"));
	m_prev.setAutoRaise(true);
	m_prev.setToolTip(QStringLiteral("Previous match (Shift+F3)"));
	m_next.setText(QStringLiteral("\u25bc"));
	m_next.setAutoRaise(true);
	m_next.setToolTip(QStringLiteral("Next match (F3)"));
	frame->addWidget(&m_prev);
	frame->addWidget(&m_next);
	// Navigating by mouse selects the bar's zoom domain, exactly
	// like a press on the bar's own surface.  The toggles stay
	// non-focusing so flipping them mid-typing keeps the cursor.
	connect(&m_prev, &QToolButton::pressed,
	        this, [this] { setFocus(Qt::MouseFocusReason); });
	connect(&m_next, &QToolButton::pressed,
	        this, [this] { setFocus(Qt::MouseFocusReason); });

	m_count.setAlignment(Qt::AlignCenter);
	frame->addWidget(&m_count);

	m_close.setText(QStringLiteral("\u2715"));
	m_close.setAutoRaise(true);
	frame->addWidget(&m_close);

	m_anim.setTargetObject(this);
	m_anim.setPropertyName("pos");
	m_anim.setDuration(140);
	m_anim.setEasingCurve(QEasingCurve::OutCubic);
	applyType();
	hide();
}

void search_bar_base::setTypeZoom(double bar, double edit)
{
	m_barZoom = bar;
	m_editZoom = edit;
	applyType();
}

// Everything font-sized, derived in one place: chrome from the
// application font at the caption scale, the pattern text within
// the chrome's box.  Integer points throughout; the zoom domain
// guarantees the text factor never exceeds 1.  Each widget is only
// touched when its rounded font actually moved, so a pattern-text
// step reduces to one setFont and the chrome geometry stays put.
void search_bar_base::applyType()
{
	QFont f = QApplication::font();
	f.setPointSize(std::max(1, int(std::lround(
		f.pointSize() * kFontScale * m_barZoom))));
	// The pattern box belongs to the bar's geometry: its size comes
	// from the chrome font alone, so the text zoom moves nothing
	// but the glyphs.
	QFont ef = f;
	ef.setPointSize(std::max(1, int(std::lround(
		f.pointSize() * m_editZoom))));
	if (ef != m_edit.font())
		m_edit.setFont(ef);
	// Compare against the font last *applied*, never the widget's
	// current one: the bar inherits the caption font from its
	// parent, which can coincide with the chrome default.
	if (f == m_chromeFont)
		return;
	m_chromeFont = f;
	setFont(f);
	// The style sheet stops parent-font propagation to children:
	// hand each of them its font explicitly.  Buttons and counter
	// dress at 0.8x the chrome so the pattern box stays the bar's
	// protagonist; they still follow every zoom at that proportion.
	QFont bf = f;
	bf.setPointSize(std::max(1, int(std::lround(
		f.pointSize() * 0.8))));
	m_regex.setFont(bf);
	m_case.setFont(bf);
	m_prev.setFont(bf);
	m_next.setFont(bf);
	m_close.setFont(bf);
	m_count.setFont(bf);
	QFontMetrics const fm(f);
	m_edit.setMinimumWidth(fm.averageCharWidth() * 32);
	m_edit.setFixedHeight(fm.height() + 8);
	// Reserve for the common case only ("00/00"): the idle em-dash
	// must not hold a wide empty box open between the buttons.
	m_count.setMinimumWidth(QFontMetrics(bf)
		.horizontalAdvance(QStringLiteral("00/00")));
	adjustSize();
}

void search_bar_base::setCount(int idx, int n)
{
	m_count.setText(n <= 0 ? QStringLiteral("\u2014")
		: QStringLiteral("%1/%2")
		  .arg(idx > 0 ? QString::number(idx) : QStringLiteral("?"))
		  .arg(n));
	// invalid-pattern feedback: theme text color vs. plain red
	QPalette pal;
	if (n < 0)
		pal.setColor(QPalette::Text, QColor(214, 72, 72));
	m_edit.setPalette(pal);
}

void search_bar_base::open(QPoint const &target)
{
	// A pending dismiss()-finished hide() would fire when slideTo()
	// stops the running animation (stop() emits finished); cancel it
	// or reopening mid-slide hides the bar.
	disconnect(&m_anim, &QPropertyAnimation::finished, this, nullptr);
	m_target = target;
	if (!isVisible()) {
		move(target.x(), -height());
		show();
	}
	raise();
	slideTo(target);
	m_edit.setFocus();
	m_edit.selectAll();
}

void search_bar_base::dismiss()
{
	if (!isVisible())
		return;
	slideTo(QPoint(m_target.x(), -height()));
	connect(&m_anim, &QPropertyAnimation::finished,
	        this, [this] { hide(); }, Qt::SingleShotConnection);
}

void search_bar_base::reposition(QPoint const &target)
{
	m_target = target;
	if (isVisible())
		move(target);
}


// A press on the bar's own surface (margins, gaps, the count label
// passing through) lands here: focusing the bar is how the chrome
// zoom domain is reached by mouse.
void search_bar_base::mousePressEvent(QMouseEvent *ev)
{
	setFocus(Qt::MouseFocusReason);
	QWidget::mousePressEvent(ev);
}

void search_bar_base::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(palette().color(QPalette::Mid));
	p.setBrush(palette().color(QPalette::Window));
	p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
	                  8.0, 8.0);
}

void search_bar_base::slideTo(QPoint const &to)
{
	m_anim.stop();
	m_anim.setStartValue(pos());
	m_anim.setEndValue(to);
	m_anim.start();
}
