// mpvlink.cpp -- see mpvlink.hpp.
#include <QFile>
#include <QJsonDocument>
#include <QJsonValue>
#include <QProcess>

#include <utility>

#include "mpvlink.hpp"

namespace {

// Spawned players must not yank the keyboard away from the
// transcript.  mpv rejects unknown options fatally and renamed this
// one incompatibly (--focus-on-open was removed in favor of
// --focus-on=never in 0.38), so probe once which spelling this mpv
// accepts.
QString focusFlag()
{
	auto const accepts = [](QString const &opt) {
		QProcess p;
		p.setStandardOutputFile(QProcess::nullDevice());
		p.setStandardErrorFile(QProcess::nullDevice());
		p.start(QStringLiteral("mpv"),
		        {opt, QStringLiteral("--version")});
		return p.waitForFinished(3000)
		    && p.exitStatus() == QProcess::NormalExit
		    && p.exitCode() == 0;
	};
	static QString const flag = [&accepts] {
		QString const modern = QStringLiteral("--focus-on=never");
		if (accepts(modern))
			return modern;
		QString const old = QStringLiteral("--focus-on-open=no");
		if (accepts(old))
			return old;
		return QString();
	}();
	return flag;
}

} // namespace

mpv_link_base::mpv_link_base()
{
	connect(&sock(), &QLocalSocket::connected,
	        this, [this] { onConnected(); });
	m_retry.setParent(this);
	m_retry.setInterval(100);
	connect(&m_retry, &QTimer::timeout, this, [this] { retryTick(); });
}

bool mpv_link_base::setPlaylist(QList<play_entry> const &list,
                                QString const &sock, int index,
                                QString *err)
{
	if (list.isEmpty() || index < 0 || index >= list.size()) {
		*err = QStringLiteral("bad playlist");
		return false;
	}
	if (sock == m_sockPath && (ready() || m_starting)) {
		if (list == m_list) {
			if (index == m_index || !ready()) {
				m_index = index; // bring-up resyncs to this
				return true;
			}
			return playIndex(index, err);
		}
		m_list = list;               // corpus edited: same window,
		m_index = index;             // rebuilt playlist
		if (ready())
			resync(false);
		return true;
	}
	// New target: drop only a player this instance spawned; reused
	// ones belong to everyone and are left running.
	m_retry.stop();
	teardown(m_spawned);
	m_spawned = false;
	m_starting = false;
	m_ready = false;
	m_adopting = false;
	m_txq.clear();
	m_list = list;
	m_index = index;
	m_sockPath = sock;
	m_lastTime = -1.0;
	m_pendingSeek = -1.0;
	m_lastPause = true;
	m_unpause = false;
	return ensureAlive(err);
}

bool mpv_link_base::playIndex(int index, QString *err)
{
	if (index < 0 || index >= int(m_list.size())) {
		*err = QStringLiteral("no such playlist entry");
		return false;
	}
	m_index = index;                     // optimistic; the path echo
	                                     // confirms without an event
	return send(QStringLiteral("playlist-play-index %1").arg(index),
	            err);
}

bool mpv_link_base::ensureAlive(QString *err)
{
	if (connectedNow() || m_starting)
		return true;
	if (m_sockPath.isEmpty() || m_list.isEmpty()) {
		*err = QStringLiteral("no video associated");
		return false;
	}
	bringUp();
	return true;
}

// Non-blocking bring-up: a short grace of pure connect attempts
// adopts a running instance, after which we spawn one ourselves --
// all driven by the retry timer; the UI thread never waits on a
// socket.
void mpv_link_base::bringUp()
{
	m_starting = true;
	m_ready = false;
	m_attempts = 0;
	connectSock(0);
	if (!connectedNow() && m_starting)
		m_retry.start();
}

void mpv_link_base::retryTick()
{
	if (connectedNow() || !m_starting) {
		m_retry.stop();
		return;
	}
	++m_attempts;
	if (m_attempts == 3 && proc().state() == QProcess::NotRunning) {
		QFile::remove(m_sockPath);
		startProcess(spawnArgs());
		m_spawned = true;
	}
	if (m_attempts > 3 && m_spawned
	    && proc().state() == QProcess::NotRunning) {
		giveUp(QStringLiteral("mpv exited before its socket came "
		                      "up"));
		return;
	}
	if (m_attempts > 100) {              // ten seconds of nothing
		giveUp(QStringLiteral("mpv IPC socket never came up: %1")
		       .arg(m_sockPath));
		return;
	}
	connectSock(0);
}

void mpv_link_base::onConnected()
{
	m_retry.stop();
	m_starting = false;
	dbg(QStringLiteral("connected to %1").arg(m_sockPath));
	observe();
	if (m_spawned)
		resync(false);
	else
		m_adopting = true;           // resync deferred to first path
	m_ready = true;
	if (m_unpause) {
		QString e;
		setPause(false, &e);
		m_unpause = false;
	}
	for (QString const &line : std::as_const(m_txq))
		writeLine(line);
	m_txq.clear();
	if (m_recovering) {
		note(QStringLiteral("respawn complete"));
		m_recovering = false;
	}
}

void mpv_link_base::giveUp(QString const &why)
{
	m_retry.stop();
	m_starting = false;
	m_ready = false;
	m_recovering = false;
	m_txq.clear();
	note(why);
}

QStringList mpv_link_base::spawnArgs() const
{
	QStringList args{QStringLiteral("--no-terminal"),
	                 QStringLiteral("--idle=yes"),
	                 QStringLiteral("--force-window=yes"),
	                 QStringLiteral("--pause"),
	                 QStringLiteral("--keep-open=yes"),
	                 QStringLiteral("--input-ipc-server=")
	                 + m_sockPath,
	                 QStringLiteral("--sub-auto=no")};
	if (QString const focus = focusFlag(); !focus.isEmpty())
		args << focus;
	// WSLg's PulseAudio bridge can drop stream acknowledgments,
	// wedging mpv's core inside ao_pulse (no-timeout waits) on the
	// seek/pause storms this tool generates; keeping the stream open
	// avoids the restarts that trigger it.  Detect by the presence
	// of the bridge's own socket.  SRTVIEW_MPV_ARGS comes later on
	// the command line, so users can override (mpv is last-wins).
	if (QFile::exists(QStringLiteral("/mnt/wslg/PulseServer"))) {
		args << QStringLiteral("--audio-stream-silence=yes");
		dbg(QStringLiteral("WSLg pulse bridge detected: adding "
		                   "--audio-stream-silence=yes"));
	}
	args += QProcess::splitCommand(
		qEnvironmentVariable("SRTVIEW_MPV_ARGS"));
	return args;
}

// Mirror m_list into the player.  The current entry loads first (or,
// when it is already playing, stays untouched past playlist-clear),
// the rest append in list order, and one playlist-move puts the
// current entry into place -- exactly one file load at most.
void mpv_link_base::resync(bool keepCurrent)
{
	if (keepCurrent)
		writeLine(QStringLiteral("playlist-clear"));
	else
		writeLine(QStringLiteral("loadfile %1 replace")
		          .arg(mpvQuote(m_list[m_index].video)));
	for (qsizetype i = 0; i < m_list.size(); ++i)
		if (int(i) != m_index)
			writeLine(QStringLiteral("loadfile %1 append")
			          .arg(mpvQuote(m_list[i].video)));
	if (m_index > 0)
		writeLine(QStringLiteral("playlist-move 0 %1")
		          .arg(m_index + 1));
}

void mpv_link_base::dispatch()
{
	for (QByteArray l = nextLine(); !l.isNull(); l = nextLine()) {
		QJsonDocument const d = QJsonDocument::fromJson(l);
		if (d.isObject())
			onEvent(d.object());
	}
}

void mpv_link_base::onEvent(QJsonObject const &ev)
{
	QString const type =
		ev.value(QStringLiteral("event")).toString();
	if (type == QStringLiteral("file-loaded")) {
		onLoaded();
		return;
	}
	if (type != QStringLiteral("property-change"))
		return;
	QJsonValue const v = ev.value(QStringLiteral("data"));
	QString const name = ev.value(QStringLiteral("name")).toString();
	if (name == QStringLiteral("pause")) {
		m_lastPause = v.toBool(m_lastPause);
		return;
	}
	if (name == QStringLiteral("playback-time")) {
		if (!v.isDouble())
			return;
		m_lastTime = v.toDouble();
		m_times << m_lastTime;
		return;
	}
	if (name == QStringLiteral("path"))
		onPath(v);
}

// The observed path is the playlist authority: our own navigation
// echoes back silently (the optimistic index already matches), a
// switch made inside mpv surfaces as a pending index for the
// observer, and the initial event resolves a deferred adoption.
void mpv_link_base::onPath(QJsonValue const &v)
{
	bool const adopting = m_adopting;
	m_adopting = false;
	if (!v.isString()) {
		if (adopting)                // adopted an idle instance
			resync(false);
		return;
	}
	QString const path = v.toString();
	for (qsizetype i = 0; i < m_list.size(); ++i) {
		if (m_list[i].video != path)
			continue;
		if (int(i) != m_index) {
			m_index = int(i);
			m_newIndex = m_index;
		}
		if (adopting)                // keep its position, fix the
			resync(true);        // playlist around it
		return;
	}
	if (adopting)                        // playing something foreign
		resync(false);
}

void mpv_link_base::onLoaded()
{
	QString err;
	if (m_index >= 0 && m_index < int(m_list.size())
	    && !m_list[m_index].srt.isEmpty())
		send(QStringLiteral("sub-add %1 select")
		     .arg(mpvQuote(m_list[m_index].srt)), &err);
	if (m_pendingSeek >= 0.0) {
		seek(m_pendingSeek, false, &err);
		m_pendingSeek = -1.0;
	}
}

bool mpv_link_base::takeTime(double &t)
{
	if (m_times.isEmpty())
		return false;
	t = m_times.takeFirst();
	return true;
}

bool mpv_link_base::takeIndex(int &i)
{
	if (m_newIndex < 0)
		return false;
	i = m_newIndex;
	m_newIndex = -1;
	return true;
}

bool mpv_link_base::send(QString const &line, QString *err)
{
	if (!ensureAlive(err))
		return false;
	if (!ready()) {
		m_txq << line;               // flushed once setup is out
		return true;
	}
	return writeLine(line);
}

bool mpv_link_base::seek(double t, bool forcePause, QString *err)
{
	QString const s = QStringLiteral("no-osd seek %1 absolute+exact")
	                  .arg(t, 0, 'f', 3);
	if (!send(forcePause
	          ? QStringLiteral("no-osd set pause yes; ") + s : s, err))
		return false;
	// Best-known position is the commanded one until mpv reports
	// otherwise: mpv does not always echo a property change for a
	// seek returning to the last value it reported.
	m_lastTime = t;
	return true;
}

bool mpv_link_base::seekRel(double dt, QString *err)
{
	return send(QStringLiteral("no-osd seek %1").arg(dt, 0, 'f', 1),
	            err);
}

bool mpv_link_base::setPause(bool on, QString *err)
{
	return send(QStringLiteral("no-osd set pause %1")
	            .arg(on ? QStringLiteral("yes") : QStringLiteral("no")),
	            err);
}

bool mpv_link_base::cyclePause(QString *err)
{
	return send(QStringLiteral("no-osd cycle pause"), err);
}

void mpv_link_base::shutdown()
{
	m_retry.stop();
	teardown(m_spawned);
	m_spawned = false;
	m_starting = false;
	m_ready = false;
	m_adopting = false;
	m_unpause = false;
	m_list.clear();
	m_times.clear();
	m_txq.clear();
	m_sockPath.clear();
	m_index = 0;
	m_newIndex = -1;
	m_attempts = 0;
	m_lastTime = -1.0;
	m_pendingSeek = -1.0;
}

bool mpv_link_base::recoverWedged()
{
	if (!m_spawned) {
		note(QStringLiteral("wedged player was not started by "
		                    "srtview; not killing it"));
		return false;
	}
	if (clock().elapsed() - m_lastRespawn < 30000)
		return false;
	m_lastRespawn = clock().elapsed();
	m_recovering = true;
	double const t = m_lastTime;
	m_unpause = !m_lastPause;
	note(QStringLiteral("killing wedged mpv, respawning at %1s "
	                    "(%2)")
	     .arg(t < 0.0 ? 0.0 : t, 0, 'f', 3)
	     .arg(m_unpause ? QStringLiteral("playing")
	                    : QStringLiteral("paused")));
	proc().kill();
	proc().waitForFinished(2000);
	teardown(false);
	m_spawned = false;
	m_ready = false;
	m_pendingSeek = t < 0.0 ? -1.0 : t;  // applied on file-loaded
	QString err;
	if (!ensureAlive(&err)) {
		note(QStringLiteral("respawn failed: ") + err);
		m_recovering = false;
	}
	return true;
}

void mpv_link_base::sendPing()
{
	writeRaw("{\"command\":[\"get_property\",\"pid\"],"
	         "\"request_id\":900913}\n");
}

void mpv_link_base::observe()
{
	writeRaw("{\"command\":[\"observe_property\",1,"
	         "\"playback-time\"]}\n"
	         "{\"command\":[\"observe_property\",2,"
	         "\"pause\"]}\n"
	         "{\"command\":[\"observe_property\",3,"
	         "\"path\"]}\n");
}
