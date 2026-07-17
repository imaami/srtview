// srtedit.hpp -- the reading view.
//
// Read-only QTextEdit showing caption text only, one paragraph per
// cue, SRT inline tags rendered.  Cue start times are painted in a
// quiet gutter; hovering shows full cue timing in a tooltip.
//
// Bound to its host window through a forward declaration: direct
// non-virtual calls, headers stay acyclic.
#ifndef SRTVIEW_SRC_SRTEDIT_HPP_
#define SRTVIEW_SRC_SRTEDIT_HPP_

#include "srt.hpp"
#include "timefmt.hpp"

#include <QFont>
#include <QPropertyAnimation>
#include <QTextEdit>

#include <vector>

// Type scale relative to the theme font; make this configurable when
// settings land.
inline constexpr double kFontScale = 1.75;

class MainWin;

class SrtEdit : public QTextEdit
{
public:
	explicit SrtEdit(MainWin *host);

	void setCues(std::vector<srt::cue> cues);

	int cueCount() const { return int(m_cues.size()); }
	double cueStart(int i) const
	{
		return (i >= 0 && size_t(i) < m_cues.size()) ? m_cues[size_t(i)].start
		                                             : 0.0;
	}
	int currentCue() const { return textCursor().blockNumber(); }
	int playCue() const { return m_playCue; }

	void setMatchSelections(const QList<ExtraSelection> &sel);

	// Playback following: the cue containing t gets a light
	// full-width background tint; when following is on, the view
	// glides to keep it in the upper third.  Never touches the text
	// cursor, so seek/search anchors stay put.
	void setPlayTime(double t);
	void setFollow(bool on);
	bool follow() const { return m_follow; }

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
	int cueAt(double t) const;
	void updateCurrentCueHighlight();
	void updatePlayHighlight();
	void glideTo(int cue);
	void applySelections();

	MainWin                *m_host;
	QWidget                 m_gutter;
	QFont                   m_gutterFont;
	QPropertyAnimation      m_glide;
	int                     m_gutterW = 0;
	int                     m_playCue = -1;
	bool                    m_follow = true;
	std::vector<srt::cue>   m_cues;
	QList<ExtraSelection>   m_lineSel, m_playSel, m_matchSel;
};

#endif // SRTVIEW_SRC_SRTEDIT_HPP_
