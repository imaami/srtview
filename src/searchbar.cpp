#include "searchbar.hpp"

#include "scale.hpp"

#include <QApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QPainter>

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
		"QToolButton { padding: 2px 4px; }"));
	auto *frame = new QHBoxLayout(this);
	frame->setContentsMargins(10, 8, 10, 8);
	frame->setSpacing(2);

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
// application font at the caption scale, the pattern text and its
// field width from the chrome.
void search_bar_base::applyType()
{
	QFont f = QApplication::font();
	f.setPointSizeF(f.pointSizeF() * kFontScale * m_barZoom);
	setFont(f);
	QFont ef = f;
	ef.setPointSizeF(f.pointSizeF() * m_editZoom);
	m_edit.setFont(ef);
	m_edit.setMinimumWidth(QFontMetrics(ef).averageCharWidth() * 32);
	// Reserve for the common case only ("00/00"): the idle em-dash
	// must not hold a wide empty box open between the buttons.
	m_count.setMinimumWidth(
		fontMetrics().horizontalAdvance(QStringLiteral("00/00")));
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
