// searchbar.hpp -- IDE-style search overlay.
//
// Slides in below the top edge of the view on Ctrl+F, slides away on
// Esc or Enter (Enter accepts: the incremental search has already
// landed on a match, so the next keystroke belongs to the view).
// The pattern keeps working (F3, n/N) while the bar is hidden.
//
// Split for deduplication: search_bar_base carries the widgets,
// styling and slide animation, compiled once in searchbar.cpp;
// SearchBar<S> is a header-only adapter wiring edits and buttons to a
// concept-constrained search host.  Controllers hold
// search_bar_base& and never see the template.
#ifndef SRTVIEW_SRC_SEARCHBAR_HPP_
#define SRTVIEW_SRC_SEARCHBAR_HPP_

#include "concepts.hpp"

#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPoint>
#include <QPropertyAnimation>
#include <QToolButton>
#include <QWidget>

class search_bar_base : public QWidget
{
public:

	QString pattern() const { return m_edit.text(); }
	bool caseSensitive() const { return m_case.isChecked(); }
	bool regexEnabled() const { return m_regex.isChecked(); }
	void setPattern(QString const &s) { m_edit.setText(s); }
	void setRegexEnabled(bool on) { m_regex.setChecked(on); }
	void setCount(int idx, int n);

	void open(QPoint const &target);
	void dismiss();
	void reposition(QPoint const &target);

	// Zoom domains: the bar's chrome scales from the application
	// font (defaulting to the caption scale), the pattern text from
	// the chrome; the caller repositions after a change.
	void setTypeZoom(double bar, double edit);
	bool editFocused() const { return m_edit.hasFocus(); }

protected:
	explicit search_bar_base(QWidget *parent);

	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *ev) override;

	QLineEdit          &edit() { return m_edit; }
	QToolButton        &regexButton() { return m_regex; }
	QToolButton        &caseButton() { return m_case; }
	QToolButton        &prevButton() { return m_prev; }
	QToolButton        &nextButton() { return m_next; }
	QToolButton        &closeButton() { return m_close; }

private:
	void applyType();
	void slideTo(QPoint const &to);

	QLineEdit           m_edit;
	QToolButton         m_regex, m_case, m_prev, m_next, m_close;
	QLabel              m_count;
	QPropertyAnimation  m_anim;
	double              m_barZoom = 1.0;
	double              m_editZoom = 1.0;
	QPoint              m_target;
};

// Wiring adapter: edits and buttons become direct calls on the host.
template <search_host S>
class SearchBar final : public search_bar_base
{
public:
	SearchBar(S *host, QWidget *parent)
		: search_bar_base(parent), s_(host)
	{
		connect(&edit(), &QLineEdit::textChanged,
		        this, [this] { s_->searchChanged(); });
		connect(&edit(), &QLineEdit::returnPressed,
		        this, [this] { s_->commitSearch(); });
		connect(&regexButton(), &QToolButton::toggled,
		        this, [this] { s_->searchChanged(); });
		connect(&caseButton(), &QToolButton::toggled,
		        this, [this] { s_->searchChanged(); });
		connect(&prevButton(), &QToolButton::clicked,
		        this, [this] { s_->findAgain(true); });
		connect(&nextButton(), &QToolButton::clicked,
		        this, [this] { s_->findAgain(false); });
		connect(&closeButton(), &QToolButton::clicked,
		        this, [this] { s_->hideSearch(); });
	}

protected:
	bool eventFilter(QObject *obj, QEvent *ev) override
	{
		if (obj == &edit() && ev->type() == QEvent::KeyPress) {
			auto *ke = static_cast<QKeyEvent *>(ev);
			if (ke->key() == Qt::Key_Escape) {
				s_->hideSearch();
				return true;
			}
			if (ke->key() == Qt::Key_Up) {
				s_->historyStep(true);
				return true;
			}
			if (ke->key() == Qt::Key_Down) {
				s_->historyStep(false);
				return true;
			}
		}
		return QWidget::eventFilter(obj, ev);
	}

private:
	S *s_;
};

#endif // SRTVIEW_SRC_SEARCHBAR_HPP_
