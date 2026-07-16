// searchbar.hpp -- IDE-style search overlay.
//
// Slides in below the top edge of the view on Ctrl+F, slides away on
// Esc or Enter (Enter accepts: the incremental search has already
// landed on a match, so the next keystroke belongs to the view).
// The pattern keeps working (F3, n/N) while the bar is hidden.
//
// Statically bound to its host: a class template whose member
// definitions live in searchbar.cpp, closed by an explicit
// instantiation for the concrete host -- direct calls, no moc, no
// type erasure, and still one .hpp/.cpp per class.
#pragma once

#include <QLabel>
#include <QLineEdit>
#include <QPoint>
#include <QPropertyAnimation>
#include <QToolButton>
#include <QWidget>

template <class Host>
class SearchBar : public QWidget
{
public:
	SearchBar(Host *host, QWidget *parent);

	QString pattern() const { return m_edit.text(); }
	bool caseSensitive() const { return m_case.isChecked(); }
	bool regexEnabled() const { return m_regex.isChecked(); }
	void setPattern(const QString &s) { m_edit.setText(s); }
	void setRegexEnabled(bool on) { m_regex.setChecked(on); }
	void setCount(int idx, int n);

	void open(const QPoint &target);
	void dismiss();
	void reposition(const QPoint &target);

protected:
	bool eventFilter(QObject *obj, QEvent *ev) override;
	void paintEvent(QPaintEvent *) override;

private:
	void slideTo(const QPoint &to);

	Host               *m_host;
	QLineEdit           m_edit;
	QToolButton         m_regex, m_case, m_prev, m_next, m_close;
	QLabel              m_count;
	QPropertyAnimation  m_anim;
	QPoint              m_target;
};
