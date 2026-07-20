#include "search.hpp"

#include "playback.hpp"
#include "prefs.hpp"
#include "searchbar.hpp"
#include "srtedit.hpp"

#include <QStatusBar>
#include <QTextDocument>

namespace {

// The current hit must pop against three same-hue washes (cursor
// line, play cue, other hits): rotate the theme highlight to its
// complement and raise the opacity instead of stacking a fourth
// alpha of the same color.
QColor currentHitColor(QPalette const &pal)
{
	QColor c = pal.color(QPalette::Highlight);
	int h = 0, s = 0, v = 0;
	c.getHsv(&h, &s, &v);
	if (h >= 0)                          // achromatic themes keep hue
		c.setHsv((h + 180) % 360, s, v);
	c.setAlpha(170);
	return c;
}

// All matches of re in doc: selections for display, start offsets for
// the position counter.
void collectMatches(QTextDocument *doc, QRegularExpression const &re,
                    QTextCharFormat const &fmt,
                    QList<QTextEdit::ExtraSelection> &sels,
                    std::vector<int> &starts)
{
	QTextCursor c(doc);
	while (true) {
		c = doc->find(re, c);
		if (c.isNull())
			return;
		if (!c.hasSelection() && c.atEnd())
			return;
		if (!c.hasSelection()) {             // zero-length match
			c.movePosition(QTextCursor::NextCharacter);
			continue;
		}
		QTextEdit::ExtraSelection s;
		s.cursor = c;
		s.format = fmt;
		sels << s;
		starts.push_back(c.selectionStart());
	}
}

} // namespace

SearchCtl::SearchCtl(search_bar_base &bar, srt_view_base &view,
                     QStatusBar &status, Prefs &prefs, Trail &trail,
                     PlaybackCtl &playback)
	: m_bar(bar), m_view(view), m_status(status), m_prefs(prefs),
	  m_trail(trail), m_playback(playback)
{
	m_nextAct.setText(QStringLiteral("Find &next"));
	m_nextAct.setShortcut(QKeySequence(Qt::Key_F3));
	QObject::connect(&m_nextAct, &QAction::triggered, &m_nextAct,
	                 [this] { findAgain(false); });
	m_prevAct.setText(QStringLiteral("Find &previous"));
	m_prevAct.setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F3));
	QObject::connect(&m_prevAct, &QAction::triggered, &m_prevAct,
	                 [this] { findAgain(true); });
	m_nextTextAct.setText(QStringLiteral("Find next (&text only)"));
	m_nextTextAct.setShortcut(QKeySequence(Qt::Key_F4));
	QObject::connect(&m_nextTextAct, &QAction::triggered,
	                 &m_nextTextAct, [this] { findAgain(false, false); });
	m_prevTextAct.setText(QStringLiteral("Find previous (te&xt only)"));
	m_prevTextAct.setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F4));
	QObject::connect(&m_prevTextAct, &QAction::triggered,
	                 &m_prevTextAct, [this] { findAgain(true, false); });
}

void SearchCtl::showSearch()
{
	m_anchor = m_view.textCursor();
	// Each opening starts a fresh history walk: recordUse() may have
	// reordered the list since, making the old position meaningless.
	m_histPos = -1;
	m_draft.clear();
	// Size first: the target x depends on the bar's final width,
	// which before the first layout pass is garbage.
	m_bar.adjustSize();
	m_bar.open(target());
}

// Dismissing does not revert anything -- the highlights, pattern
// and cursor all stay -- so leaving the bar is an effective use of
// the pattern and gets recorded like a commit; but unlike Enter it
// does not touch the video.
void SearchCtl::hideSearch()
{
	recordUse(false);
	m_bar.dismiss();
	m_view.setFocus();
}

// Enter in the search field: the incremental jump has already landed
// on the match, so this hit is the destination -- sync the video
// like F3 would, accept, and get out of the way.
void SearchCtl::commitSearch()
{
	recordUse(true);
	// On an already-recorded pattern the state may not be current
	// yet: hop to this hit like F3 would, minus the find.
	if (!m_matchStarts.empty() && m_view.cueCount() > 0) {
		trail_step s;
		s.pattern = m_bar.pattern();
		s.time = m_view.cueStart(m_view.currentCue());
		s.cur = m_view.textCursor().position();
		s.flags = trail_step::text | trail_step::cursor
		        | trail_step::video;
		applyHop(s, 1, false);
	}
	m_bar.dismiss();
	m_view.setFocus();
}

// Pattern edited: refresh highlights and jump to the first match at
// or after where the search began.
void SearchCtl::searchChanged()
{
	if (!m_stepping && !m_trail.applying()) {
		m_histPos = -1;              // fresh typing leaves history
		m_trail.dropCycle();         // the ring belongs to the old
		                             // pattern and match set
	}
	highlightAll();
	if (m_trail.applying())
		return;                      // undo restores text only; the
	                                 // cursor has its own steps
	if (m_matchStarts.empty() || m_bar.pattern().isEmpty())
		return;
	QTextCursor const from = m_anchor.isNull()
		? QTextCursor(m_view.document()) : m_anchor;
	QTextCursor hit = m_view.document()->find(pattern(), from);
	if (hit.isNull())
		hit = m_view.document()->find(pattern(),
		                              QTextCursor(m_view.document()));
	if (!hit.isNull()) {
		m_view.setTextCursor(hit);
		updateCounter(hit);
	}
}

void SearchCtl::findAgain(bool backward, bool syncVideo)
{
	QRegularExpression const re = pattern();
	if (!re.isValid() || re.pattern().isEmpty())
		return;
	recordUse(syncVideo);
	int const posBefore = m_view.textCursor().position();
	QTextDocument::FindFlags fl;
	if (backward)
		fl |= QTextDocument::FindBackward;
	bool hit = m_view.find(re, fl);
	if (!hit) {
		m_view.moveCursor(backward ? QTextCursor::End
		                           : QTextCursor::Start);
		hit = m_view.find(re, fl);
		m_status.showMessage(hit
			? QStringLiteral("search wrapped")
			: QStringLiteral("no match"), 1500);
	}
	trail_step s;
	s.cur = m_view.textCursor().position();
	s.flags = trail_step::cursor;
	if (hit && syncVideo && m_view.cueCount() > 0) {
		// Hop states are uniform (text + cursor + video, the time a
		// cue start): byte-identical when a hit is revisited, which
		// is what lets the ring recognize and travel them.
		s.flags |= trail_step::text | trail_step::video;
		s.pattern = m_bar.pattern();
		s.time = m_view.cueStart(m_view.currentCue());
	}
	applyHop(s, backward ? -1 : 1, s.cur != posBefore);
	updateCounter(m_view.textCursor());
}

void SearchCtl::applyHop(trail_step const &s, int dir, bool moved)
{
	if (!(s.flags & trail_step::video)) {
		if (moved)
			m_trail.act(s);      // text-only jump: plain step
		return;
	}
	switch (m_trail.probeHop(s, dir)) {
	case Trail::hop::stay:
		m_playback.applyTime(s.time);    // re-sync drifted playback
		return;
	case Trail::hop::travel:
		// Seek first: travel is infallible, mpv is not, and a
		// refused seek must leave the ring position untouched.
		if (m_playback.applyTime(s.time))
			m_trail.travelHop(dir);
		return;
	case Trail::hop::grow:
		trail_step g = s;
		if (!m_playback.jumpTo(g.time, false))
			g.flags &= ~trail_step::video;
		if (moved || (g.flags & trail_step::video))
			m_trail.growHop(g, dir);
		return;
	}
}

bool SearchCtl::syncCue(double &t)
{
	if (m_view.cueCount() <= 0)
		return false;
	t = m_view.cueStart(m_view.currentCue());
	return m_playback.jumpTo(t, false);
}

void SearchCtl::layoutOverlay()
{
	m_bar.reposition(target());
}

void SearchCtl::setSearchText(QString const &s)
{
	if (m_anchor.isNull() && m_view.cueCount() > 0)
		m_anchor = m_view.textCursor();
	m_bar.setPattern(s);
}

void SearchCtl::setRegexEnabled(bool on)
{
	m_bar.setRegexEnabled(on);
}

void SearchCtl::recordUse(bool syncVideo)
{
	QString const p = m_bar.pattern();
	bool const changed = p != m_recorded;
	bool const sync = syncVideo && !p.isEmpty()
	               && !m_matchStarts.empty();
	// A synced use with no active cycle must still act: it plants
	// the anchor the episode's ring forms around.
	if (!changed && !(sync && !m_trail.cycled()))
		return;
	trail_step s;
	s.cur = m_view.textCursor().position();
	s.flags = trail_step::text | trail_step::cursor;
	s.pattern = p;
	if (sync && syncCue(s.time))
		s.flags |= trail_step::video;
	m_trail.act(s);
	if (s.flags & trail_step::video)
		m_trail.anchorCycle();       // adoption resumes an old ring
	if (!changed)
		return;
	m_recorded = p;
	m_prefs.addSearch(p);
}

void SearchCtl::historyStep(bool back)
{
	QStringList const h = m_prefs.searchHistory();
	if (h.isEmpty())
		return;
	if (m_histPos < 0)
		m_draft = m_bar.pattern();
	int pos = m_histPos + (back ? 1 : -1);
	if (pos >= h.size())
		return;                              // already at the oldest
	m_histPos = pos < 0 ? -1 : pos;
	m_stepping = true;
	m_bar.setPattern(m_histPos < 0 ? m_draft : h[m_histPos]);
	m_stepping = false;
}

void SearchCtl::applyPattern(QString const &text)
{
	m_bar.setPattern(text);          // trail is in applying mode:
	m_recorded = text;               // highlights refresh, no jump
}

void SearchCtl::applyCursor(int position)
{
	QTextCursor c(m_view.document());
	c.setPosition(std::min(position,
	                       std::max(0, int(m_view.document()
	                                       ->characterCount()) - 1)));
	m_view.setTextCursor(c);
}

QRegularExpression SearchCtl::pattern() const
{
	QString const raw = m_bar.pattern();
	QRegularExpression re(m_bar.regexEnabled()
		? raw : QRegularExpression::escape(raw));
	if (!m_bar.caseSensitive())
		re.setPatternOptions(QRegularExpression::CaseInsensitiveOption);
	return re;
}

void SearchCtl::highlightAll()
{
	QList<QTextEdit::ExtraSelection> sels;
	m_matchStarts.clear();
	QRegularExpression const re = pattern();
	bool const empty = re.pattern().isEmpty();
	m_view.setCurrentMatchSelection({}); // updateCounter re-marks
	if (!re.isValid() && !empty) {
		m_bar.setCount(0, -1);           // invalid-pattern feedback
		m_view.setMatchSelections({});
		return;
	}
	if (!empty) {
		// Alpha over the theme base keeps the theme's own text color
		// readable on both light and dark palettes.
		QColor bg = m_view.palette().color(QPalette::Highlight);
		bg.setAlpha(85);
		QTextCharFormat fmt;
		fmt.setBackground(bg);
		collectMatches(m_view.document(), re, fmt, sels, m_matchStarts);
	}
	m_view.setMatchSelections(sels);
	m_bar.setCount(0, empty ? 0 : int(m_matchStarts.size()));
}

void SearchCtl::updateCounter(QTextCursor const &cur)
{
	int idx = 0;
	int const start = cur.selectionStart();
	for (std::size_t i = 0; i < m_matchStarts.size(); ++i) {
		if (m_matchStarts[i] != start)
			continue;
		idx = int(i) + 1;
		break;
	}
	m_bar.setCount(idx, int(m_matchStarts.size()));
	QList<QTextEdit::ExtraSelection> sel;
	if (idx > 0 && cur.hasSelection()) {
		QTextEdit::ExtraSelection s;
		s.cursor = cur;
		s.format.setBackground(currentHitColor(m_view.palette()));
		sel << s;
	}
	m_view.setCurrentMatchSelection(sel);
}

QPoint SearchCtl::target() const
{
	return {m_view.width() - m_bar.width() - 24, 10};
}
