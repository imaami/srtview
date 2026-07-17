// searchbar.hpp -- IDE-style search overlay.
//
// Slides in below the top edge of the view on Ctrl+F, slides away on
// Esc or Enter (Enter accepts: the incremental search has already
// landed on a match, so the next keystroke belongs to the view).
// The pattern keeps working (F3, n/N) while the bar is hidden.
//
// Bound to its host through a forward declaration: direct
// non-virtual calls, no moc, no type erasure, headers stay acyclic.
#ifndef SRTVIEW_SRC_SEARCHBAR_HPP_
#define SRTVIEW_SRC_SEARCHBAR_HPP_

#include <QLabel>
#include <QLineEdit>
#include <QPoint>
#include <QPropertyAnimation>
#include <QToolButton>
#include <QWidget>

class MainWin;

class SearchBar : public QWidget
{
public:
	SearchBar(MainWin *host, QWidget *parent);

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

	MainWin            *m_host;
	QLineEdit           m_edit;
	QToolButton         m_regex, m_case, m_prev, m_next, m_close;
	QLabel              m_count;
	QPropertyAnimation  m_anim;
	QPoint              m_target;
};

#endif // SRTVIEW_SRC_SEARCHBAR_HPP_
