// srtedit.hpp -- the reading view.
//
// Read-only QTextEdit showing caption text only, one paragraph per
// cue, SRT inline tags rendered.  Cue start times are painted in a
// quiet gutter; hovering shows full cue timing in a tooltip.
//
// Split for deduplication: srt_view_base carries all host-independent
// machinery (document build, gutter, playback highlight, glide),
// compiled once in srtedit.cpp; SrtEdit<P, S> is a header-only
// adapter routing input to concept-constrained playback and search
// hosts.  Controllers hold srt_view_base& and never see the
// template.
#ifndef SRTVIEW_SRC_SRTEDIT_HPP_
#define SRTVIEW_SRC_SRTEDIT_HPP_

#include "concepts.hpp"
#include "srt.hpp"
#include "timefmt.hpp"

#include <QFont>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPropertyAnimation>
#include <QTextBlock>
#include <QTextEdit>

#include <vector>

// Type scale relative to the theme font; make this configurable when
// settings land.
inline constexpr double kFontScale = 1.75;

class srt_view_base : public QTextEdit
{
public:

	void setCues(std::vector<srt::cue> cues);

	int cueCount() const { return int(m_cues.size()); }
	double cueStart(int i) const
	{
		return (i >= 0 && size_t(i) < m_cues.size()) ? m_cues[size_t(i)].start
		                                             : 0.0;
	}
	int currentCue() const { return textCursor().blockNumber(); }
	int playCue() const { return m_playCue; }

	void setMatchSelections(QList<ExtraSelection> const &sel);
	void setCurrentMatchSelection(QList<ExtraSelection> const &sel);

	// Playback following: the cue containing t gets a light
	// full-width background tint; when following is on, the view
	// glides to keep it in the upper third.  Never touches the text
	// cursor, so seek/search anchors stay put.
	void setPlayTime(double t);
	void setFollow(bool on);
	bool follow() const { return m_follow; }

protected:
	explicit srt_view_base(QWidget *parent);

	void resizeEvent(QResizeEvent *ev) override;
	bool event(QEvent *ev) override;

	// For the derived input adapter's gutter event filter.
	void paintGutter();
	int cueAtGutterY(int y);
	QObject const *gutterObject() const { return &m_gutter; }

private:
	static constexpr qreal kCueGap = 14.0;

	void layoutGutter();
	template <typename F> void visitVisibleBlocks(F f);
	int cueAt(double t) const;
	void updateCurrentCueHighlight();
	void updatePlayHighlight();
	void glideTo(int cue);
	void applySelections();

	QWidget                 m_gutter;
	QFont                   m_gutterFont;
	QPropertyAnimation      m_glide;
	int                     m_gutterW = 0;
	int                     m_playCue = -1;
	bool                    m_follow = true;
	std::vector<srt::cue>   m_cues;
	QList<ExtraSelection>   m_lineSel, m_playSel, m_matchSel, m_curSel;
};

// Input adapter: keys, double-click and gutter clicks become direct
// calls on the two hosts.  All state lives in the base.
template <playback_host P, search_host S>
class SrtEdit final : public srt_view_base
{
public:
	SrtEdit(P *playback, S *search, QWidget *parent)
		: srt_view_base(parent), p_(playback), s_(search)
	{
	}

protected:
	void keyPressEvent(QKeyEvent *ev) override
	{
		if (ev->modifiers() & (Qt::ControlModifier | Qt::AltModifier
		                       | Qt::MetaModifier)) {
			QTextEdit::keyPressEvent(ev);
			return;
		}
		switch (ev->key()) {
		case Qt::Key_Return:
		case Qt::Key_Enter:
		case Qt::Key_T:
			if (cueCount() > 0)
				p_->seekCue(currentCue(),
				            ev->text() == QStringLiteral("T"));
			return;
		case Qt::Key_Space:
			p_->togglePause();
			return;
		case Qt::Key_Left:
			p_->seekRel(-5.0);
			return;
		case Qt::Key_Right:
			p_->seekRel(5.0);
			return;
		case Qt::Key_Escape:
			s_->hideSearch();
			return;
		case Qt::Key_F:
			if (ev->text() == QStringLiteral("f")) {
				p_->toggleFollow();
				return;
			}
			break;
		case Qt::Key_C:
			if (ev->text() == QStringLiteral("c")) {
				p_->setPause(false);
				return;
			}
			break;
		case Qt::Key_P:
			if (ev->text() == QStringLiteral("P")) {
				p_->setPause(true);
				return;
			}
			break;
		case Qt::Key_Slash:
			s_->showSearch();
			return;
		case Qt::Key_N:
			s_->findAgain(ev->text() == QStringLiteral("N"));
			return;
		default:
			break;
		}
		QTextEdit::keyPressEvent(ev);
	}

	void mouseDoubleClickEvent(QMouseEvent *ev) override
	{
		QTextEdit::mouseDoubleClickEvent(ev);
		if (cueCount() > 0)
			p_->seekCue(currentCue(), false);
	}

	bool eventFilter(QObject *obj, QEvent *ev) override
	{
		if (obj == gutterObject())
			return handleGutter(ev);
		return QTextEdit::eventFilter(obj, ev);
	}

private:
	bool handleGutter(QEvent *ev)
	{
		if (ev->type() == QEvent::Paint) {
			paintGutter();
			return true;
		}
		if (ev->type() != QEvent::MouseButtonPress)
			return false;
		auto *me = static_cast<QMouseEvent *>(ev);
		int const cue = cueAtGutterY(int(me->position().y()));
		if (cue >= 0) {
			setTextCursor(QTextCursor(
				document()->findBlockByNumber(cue)));
			p_->seekCue(cue, false);
		}
		return true;
	}

	P *p_;
	S *s_;
};

#endif // SRTVIEW_SRC_SRTEDIT_HPP_
