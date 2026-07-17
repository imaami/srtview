#include "searchbar.hpp"

#include "mainwin.hpp"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QPainter>

SearchBar::SearchBar(MainWin *host, QWidget *parent)
	: QWidget(parent), m_host(host)
{
	setAutoFillBackground(true);
	setStyleSheet(QStringLiteral(
		"SearchBar, QWidget { background: palette(window); }"
		"QLineEdit { border: none; background: palette(base);"
		"            padding: 3px 6px; border-radius: 4px; }"));
	auto *frame = new QHBoxLayout(this);
	frame->setContentsMargins(10, 8, 10, 8);
	frame->setSpacing(6);

	m_edit.setPlaceholderText(QStringLiteral("search\u2026"));
	m_edit.setMinimumWidth(240);
	m_edit.installEventFilter(this);
	frame->addWidget(&m_edit);

	m_regex.setText(QStringLiteral(".*"));
	m_regex.setCheckable(true);
	m_regex.setChecked(true);           // regexp on by default
	m_regex.setAutoRaise(true);
	m_regex.setToolTip(QStringLiteral("Regular expression"));
	frame->addWidget(&m_regex);

	m_case.setText(QStringLiteral("Aa"));
	m_case.setCheckable(true);
	m_case.setChecked(true);            // case-sensitive
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

	m_count.setMinimumWidth(
		fontMetrics().horizontalAdvance(QStringLiteral("000/000")));
	m_count.setAlignment(Qt::AlignCenter);
	frame->addWidget(&m_count);

	m_close.setText(QStringLiteral("\u2715"));
	m_close.setAutoRaise(true);
	frame->addWidget(&m_close);

	connect(&m_edit, &QLineEdit::textChanged,
	        this, [this] { m_host->searchChanged(); });
	connect(&m_edit, &QLineEdit::returnPressed,
	        this, [this] { m_host->commitSearch(); });
	connect(&m_regex, &QToolButton::toggled,
	        this, [this] { m_host->searchChanged(); });
	connect(&m_case, &QToolButton::toggled,
	        this, [this] { m_host->searchChanged(); });
	connect(&m_prev, &QToolButton::clicked,
	        this, [this] { m_host->findAgain(true); });
	connect(&m_next, &QToolButton::clicked,
	        this, [this] { m_host->findAgain(false); });
	connect(&m_close, &QToolButton::clicked,
	        this, [this] { m_host->hideSearch(); });

	m_anim.setTargetObject(this);
	m_anim.setPropertyName("pos");
	m_anim.setDuration(140);
	m_anim.setEasingCurve(QEasingCurve::OutCubic);
	hide();
}

void SearchBar::setCount(int idx, int n)
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

void SearchBar::open(const QPoint &target)
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

void SearchBar::dismiss()
{
	if (!isVisible())
		return;
	slideTo(QPoint(m_target.x(), -height()));
	connect(&m_anim, &QPropertyAnimation::finished,
	        this, [this] { hide(); }, Qt::SingleShotConnection);
}

void SearchBar::reposition(const QPoint &target)
{
	m_target = target;
	if (isVisible())
		move(target);
}

bool SearchBar::eventFilter(QObject *obj, QEvent *ev)
{
	if (obj == &m_edit && ev->type() == QEvent::KeyPress) {
		auto *ke = static_cast<QKeyEvent *>(ev);
		if (ke->key() == Qt::Key_Escape) {
			m_host->hideSearch();
			return true;
		}
	}
	return QWidget::eventFilter(obj, ev);
}

void SearchBar::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(palette().color(QPalette::Mid));
	p.setBrush(palette().color(QPalette::Window));
	p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
	                  8.0, 8.0);
}

void SearchBar::slideTo(const QPoint &to)
{
	m_anim.stop();
	m_anim.setStartValue(pos());
	m_anim.setEndValue(to);
	m_anim.start();
}
