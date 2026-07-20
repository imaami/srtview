#include "playback.hpp"

#include "grabber.hpp"
#include "mpvlink.hpp"
#include "srtedit.hpp"
#include "timefmt.hpp"


#include <QStatusBar>

PlaybackCtl::PlaybackCtl(mpv_link_base &link, srt_view_base &view,
                         QStatusBar &status, Trail &trail,
                         Grabber &grab)
	: m_link(link), m_view(view), m_status(status), m_trail(trail),
	  m_grab(grab)
{
	m_followAct.setText(QStringLiteral("&Follow playback\t(f)"));
	m_followAct.setCheckable(true);
	m_followAct.setChecked(true);
	QObject::connect(&m_followAct, &QAction::toggled, &m_followAct,
	                 [this](bool on) { m_view.setFollow(on); });
}

void PlaybackCtl::seekCue(int cue, bool forcePause)
{
	if (cue < 0 || cue >= m_view.cueCount())
		return;
	double const t = m_view.cueStart(cue);
	if (!jumpTo(t, forcePause))
		return;
	trail_step jump;
	jump.flags = trail_step::video;
	jump.time = t;
	m_trail.act(jump);
	m_status.showMessage(QStringLiteral("#%1 \u2192 %2%3")
		.arg(cue + 1).arg(fmtTime(t, true),
		     forcePause ? QStringLiteral("  [paused]") : QString()),
		2000);
}

bool PlaybackCtl::jumpTo(double t, bool forcePause)
{
	QString err;
	// Capture before the seek (which advances lastTime): where the
	// user actually was is what undoing the jump must return to.
	double const before = m_link.lastTime();
	if (!m_link.seek(t, forcePause, &err)) {
		m_status.showMessage(QStringLiteral("mpv: ") + err, 3000);
		return false;
	}
	if (before >= 0.0)
		m_trail.driftTo(before);
	m_view.setPlayTime(t);
	m_grab.enqueue(t);
	return true;
}

void PlaybackCtl::setPause(bool on)
{
	QString err;
	if (!m_link.setPause(on, &err))
		m_status.showMessage(QStringLiteral("mpv: ") + err, 3000);
}

void PlaybackCtl::togglePause()
{
	QString err;
	if (!m_link.cyclePause(&err))
		m_status.showMessage(QStringLiteral("mpv: ") + err, 3000);
}

void PlaybackCtl::seekRel(double dt)
{
	QString err;
	double const before = m_link.lastTime();
	if (!m_link.seekRel(dt, &err)) {
		m_status.showMessage(QStringLiteral("mpv: ") + err, 3000);
		return;
	}
	if (before >= 0.0) {
		m_trail.driftTo(before);
		trail_step seek;
		seek.flags = trail_step::video;
		seek.time = before + dt;
		m_trail.act(seek);
	}
}

bool PlaybackCtl::applyTime(double t)
{
	QString err;
	if (!m_link.seek(t, false, &err)) {
		m_status.showMessage(QStringLiteral("mpv: ") + err, 3000);
		return false;
	}
	// Reflect immediately: mpv's echo is not guaranteed (see
	// mpv_link_base::seek) and the highlight should not wait a
	// round-trip anyway.
	m_view.setPlayTime(t);
	m_trail.noteVideo(t);
	m_grab.enqueue(t);
	return true;
}

void PlaybackCtl::toggleFollow()
{
	m_followAct.toggle();                // toggled() -> setFollow
	m_status.showMessage(m_followAct.isChecked()
		? QStringLiteral("following playback")
		: QStringLiteral("following off"), 1500);
}

void PlaybackCtl::onMpvTime(double t)
{
	m_view.setPlayTime(t);
}

void PlaybackCtl::onMpvState(bool responsive)
{
	if (responsive) {
		m_status.showMessage(QStringLiteral("mpv responding again"),
		                     3000);
		return;
	}
	// Persistent until recovery: this is the "player seized up"
	// failure, now visible instead of silent.
	m_status.showMessage(QStringLiteral(
		"mpv is not responding \u2014 run srtview from a terminal "
		"(SRTVIEW_DEBUG=1) for details"), 0);
}
