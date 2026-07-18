#include "playback.hpp"

#include "mpvlink.hpp"
#include "srtedit.hpp"
#include "timefmt.hpp"


#include <QStatusBar>

PlaybackCtl::PlaybackCtl(mpv_link_base &link, srt_view_base &view,
                         QStatusBar &status, Trail &trail)
	: m_link(link), m_view(view), m_status(status), m_trail(trail)
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
	QString err;
	double const t = m_view.cueStart(cue);
	if (m_link.lastTime() >= 0.0) {
		trail_step jump;
		jump.k = trail_step::video_jump;
		jump.timeBefore = m_link.lastTime();
		jump.timeAfter = t;
		m_trail.act(jump);
	}
	if (!m_link.seek(t, forcePause, &err)) {
		m_status.showMessage(QStringLiteral("mpv: ") + err, 3000);
		return;
	}
	m_view.setPlayTime(t);
	m_status.showMessage(QStringLiteral("#%1 \u2192 %2%3")
		.arg(cue + 1).arg(fmtTime(t, true),
		     forcePause ? QStringLiteral("  [paused]") : QString()),
		2000);
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
	if (m_link.lastTime() >= 0.0) {
		trail_step seek;
		seek.k = trail_step::side_seek;
		seek.timeBefore = m_link.lastTime();
		seek.timeAfter = m_link.lastTime() + dt;
		m_trail.act(seek);
	}
	if (!m_link.seekRel(dt, &err))
		m_status.showMessage(QStringLiteral("mpv: ") + err, 3000);
}

void PlaybackCtl::applyTime(double t)
{
	QString err;
	if (!m_link.seek(t, false, &err)) {
		m_status.showMessage(QStringLiteral("mpv: ") + err, 3000);
		return;
	}
	// Reflect immediately: mpv's echo is not guaranteed (see
	// mpv_link_base::seek) and the highlight should not wait a
	// round-trip anyway.
	m_view.setPlayTime(t);
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
