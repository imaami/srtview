// srtedit.hpp -- the reading view.
//
// Read-only QTextEdit showing caption text only, one paragraph per
// cue, SRT inline tags rendered.  Cue start times are painted in a
// quiet gutter; hovering shows full cue timing in a tooltip.
//
// Statically bound to its host window (same pattern as SearchBar):
// member definitions live in srtedit.cpp, closed by an explicit
// instantiation for the concrete host.
#pragma once

#include "srt.hpp"

#include <QFont>
#include <QTextEdit>

#include <vector>

// Type scale relative to the theme font; make this configurable when
// settings land.
inline constexpr double kFontScale = 1.75;

template <class Host>
class SrtEdit : public QTextEdit
{
public:
	explicit SrtEdit(Host *host);

	void setCues(std::vector<Cue> cues);

	int cueCount() const { return int(m_cues.size()); }
	double cueStart(int i) const
	{
		return (i >= 0 && size_t(i) < m_cues.size()) ? m_cues[size_t(i)].start
		                                             : 0.0;
	}
	int currentCue() const { return textCursor().blockNumber(); }

	void setMatchSelections(const QList<ExtraSelection> &sel);

protected:
	void keyPressEvent(QKeyEvent *ev) override;
	void mouseDoubleClickEvent(QMouseEvent *ev) override;
	void resizeEvent(QResizeEvent *ev) override;
	bool event(QEvent *ev) override;
	bool eventFilter(QObject *obj, QEvent *ev) override;

private:
	static constexpr qreal kCueGap = 14.0;

	void layoutGutter();
	template <typename F> void visitVisibleBlocks(F f);
	void paintGutter();
	int cueAtGutterY(int y);
	void updateCurrentCueHighlight();
	void applySelections();

	Host                   *m_host;
	QWidget                 m_gutter;
	QFont                   m_gutterFont;
	int                     m_gutterW = 0;
	std::vector<Cue>        m_cues;
	QList<ExtraSelection>   m_lineSel, m_matchSel;
};
