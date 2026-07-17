#include "playback.hpp"

#include "mpvlink.hpp"
#include "srtedit.hpp"
#include "timefmt.hpp"

#include <QStatusBar>

PlaybackCtl::PlaybackCtl(mpv_link_base &link, srt_view_base &view,
                         QStatusBar &status)
	: m_link(link), m_view(view), m_status(status)
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
	if (!m_link.seek(t, forcePause, &err)) {
		m_status.showMessage(QStringLiteral("mpv: ") + err, 3000);
		return;
	}
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
	if (!m_link.seekRel(dt, &err))
		m_status.showMessage(QStringLiteral("mpv: ") + err, 3000);
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
