#include "mainwin.hpp"

#include "palettefix.hpp"
#include "srt.hpp"

#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QApplication>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QShortcut>
#include <QStatusBar>
#include <QTextDocumentFragment>

#include <cmath>
#include <cstdio>

namespace {

// Platform themes pin per-class fonts (menus, tooltips, message
// boxes) separately from the general application font; the base
// zoom domain must carry them along or "everything" leaves the
// chrome behind.
constexpr char const *kThemedClasses[] = {
	"QMenuBar", "QMenu", "QMessageBox", "QToolTip", "QStatusBar",
};

// One zoom step per Ctrl+-/+ press; 12 steps ~ a factor of four.
constexpr double kZoomStep = 1.125;
constexpr int    kZoomSpan = 12;

double zoomFactor(int steps)
{
	return std::pow(kZoomStep, steps);
}

// Corpus-search diagnostics, SRTVIEW_DEBUG-gated like the mpv
// clients' dbg().
void dbgHop(QString const &msg)
{
	static bool const on =
		qEnvironmentVariableIsSet("SRTVIEW_DEBUG");
	if (on) {
		std::fprintf(stderr, "srtview: %s\n", qPrintable(msg));
		std::fflush(stderr);
	}
}

} // namespace

MainWin::MainWin()
	: m_view(&m_playback, &m_search, this)
	, m_bar(&m_search, &m_view)
	, m_link(&m_playback)
	, m_playback(m_link, m_view, *statusBar(), m_trail, m_grab,
	             this)
	, m_search(m_bar, m_view, *statusBar(), m_prefs, m_trail,
	           m_playback, this)
{
	m_grab.setListener(this, this);
	m_exportTick.start();
	// Clicks on the top or bottom chrome focus the footer: that is
	// the base zoom domain's handle (neither bar is focusable by
	// itself, and focusing the menu bar would hijack plain keys as
	// mnemonics).
	statusBar()->installEventFilter(this);
	menuBar()->installEventFilter(this);
	m_baseFont = QApplication::font();
	for (char const *cls : kThemedClasses)
		m_classFonts << QApplication::font(cls);
	setCentralWidget(&m_view);
	// For < and > (video stepping): as printable characters they
	// cannot be window shortcuts without stealing them from regex
	// typing in the search bar, so they are filtered off the view.
	m_view.installEventFilter(this);
	setAcceptDrops(true);
	resize(1220, 1440);
	setWindowTitle(QStringLiteral("srtview"));

	// Zoom keys route by focus (zoomDomain), so they must fire from
	// anywhere in the application.
	auto const zoomKey = [this](char const *seq, auto fn) {
		auto *sc = new QShortcut(
			QKeySequence(QLatin1StringView(seq)), this);
		sc->setContext(Qt::ApplicationShortcut);
		connect(sc, &QShortcut::activated, this, fn);
	};
	zoomKey("Ctrl++", [this] { zoomStep(1); });
	zoomKey("Ctrl+=", [this] { zoomStep(1); });
	zoomKey("Ctrl+-", [this] { zoomStep(-1); });
	zoomKey("Ctrl+0", [this] { zoomReset(false); });
	zoomKey("Ctrl+Shift+0", [this] { zoomReset(true); });

	// --- menus ---
	auto *file = menuBar()->addMenu(QStringLiteral("&File"));
	file->addAction(QStringLiteral("&Open\u2026"), QKeySequence::Open,
	                this, [this] { openDialog(m_prefs.lastDir()); });
	m_recentMenu = file->addMenu(QStringLiteral("Open &recent"));
	m_recentMenu->installEventFilter(this);
	connect(m_recentMenu, &QMenu::aboutToShow,
	        this, [this] { rebuildRecentMenu(); });
	file->addAction(QStringLiteral("Open p&laylist…"),
	                this, [this] { openPlaylistDialog(); });
	file->addAction(QStringLiteral("&Export frames"),
	                this, [this] { startExport(); });
	file->addAction(QStringLiteral("&Close"), QKeySequence::Close,
	                this, [this] { closeFile(); });
	file->addSeparator();
	file->addAction(QStringLiteral("&Quit"), QKeySequence::Quit,
	                this, [this] { close(); });

	auto *edit = menuBar()->addMenu(QStringLiteral("&Edit"));
	edit->addAction(QStringLiteral("&Undo step"), QKeySequence::Undo,
	                this, [this] { undoStep(); });
	edit->addAction(QStringLiteral("&Redo step"), QKeySequence::Redo,
	                this, [this] { redoStep(); });

	auto *pb = menuBar()->addMenu(QStringLiteral("&Playback"));
	pb->addAction(QStringLiteral("Play/pause\t(Space)"),
	              this, [this] { m_playback.togglePause(); });
	pb->addAction(QStringLiteral("Back 5 s\t(\u2190)"),
	              this, [this] { m_playback.seekRel(-5.0); });
	pb->addAction(QStringLiteral("Forward 5 s\t(\u2192)"),
	              this, [this] { m_playback.seekRel(5.0); });
	pb->addSeparator();
	pb->addAction(QStringLiteral("Seek to cursor cue\t(Return, "
	                             "double-click)"),
	              this, [this] {
		m_playback.seekCue(m_view.currentCue(), false);
	});
	pb->addAction(QStringLiteral("Seek + pause\t(T)"),
	              this, [this] {
		m_playback.seekCue(m_view.currentCue(), true);
	});
	pb->addSeparator();
	pb->addAction(&m_playback.followAction());

	m_videosMenu = menuBar()->addMenu(QStringLiteral("&Videos"));
	connect(m_videosMenu, &QMenu::aboutToShow,
	        this, [this] { rebuildVideosMenu(); });

	auto *search = menuBar()->addMenu(QStringLiteral("&Search"));
	search->addAction(QStringLiteral("&Find\u2026"), QKeySequence::Find,
	                  this, [this] { m_search.showSearch(); });
	search->addAction(&m_search.nextAction());
	search->addAction(&m_search.prevAction());
	search->addSeparator();
	search->addAction(&m_search.nextTextAction());
	search->addAction(&m_search.prevTextAction());

	// --- status bar ---
	statusBar()->addPermanentWidget(&m_info);
	statusBar()->addPermanentWidget(&m_state);
	setState(QStringLiteral("no file"));
	// Search-side changes push updates (searchInfoChanged); the
	// poll covers what only drifts -- timestamp and pause state.
	m_infoTick.setInterval(500);
	connect(&m_infoTick, &QTimer::timeout,
	        this, [this] { updateInfo(); });
	m_infoTick.start();
	m_tallyLag.setSingleShot(true);
	m_tallyLag.setInterval(300);
	connect(&m_tallyLag, &QTimer::timeout,
	        this, [this] { recomputeTally(); });

	repairMenuPalette(menuBar());
}

bool MainWin::openPath(QString const &path, QString const &srtOverride)
{
	QString err, video = path, srt = srtOverride;
	std::string story;
	if (path.endsWith(QStringLiteral(".srt"), Qt::CaseInsensitive)) {
		srt   = path;
		video = QString::fromStdString(
			m_disc.video_for_srt(path.toStdString(), story));
		if (video.isEmpty())
			return fail(QString::fromStdString(story));
	} else if (srt.isEmpty()) {
		srt = QString::fromStdString(
			m_disc.srt_for_video(video.toStdString(), story));
		if (srt.isEmpty())
			return fail(QString::fromStdString(story));
	}
	// Player routing: playlist members navigate inside the
	// persistent corpus instance -- same window, no respawn, no
	// focus theft; anything else gets a single-entry playlist on
	// the per-video socket (the srtjump sharing scheme).
	qsizetype const at = playlistIndex(video);
	QString const claim = at >= 0 ? m_corpusPath : video;
	QString const sock = QString::fromStdString(
		m_disc.sock_for_video(claim.toStdString()));
	if (sock.isEmpty())
		return fail(QStringLiteral("cannot resolve path: %1")
		            .arg(claim));
	QList<play_entry> list;
	int index = 0;
	if (at >= 0) {
		list = corpusEntries();
		index = int(at);
	} else {
		list << play_entry{video, srt};
	}
	if (!m_link.setPlaylist(list, sock, index, &err))
		return fail(err);
	return showDoc(video, srt);
}

QString MainWin::videoId(QString const &video)
{
	return QString::fromStdString(
		m_disc.id_for_video(video.toStdString()));
}

qsizetype MainWin::playlistIndex(QString const &video)
{
	return indexOfId(videoId(video));
}

qsizetype MainWin::indexOfId(QString const &id) const
{
	if (id.isEmpty())
		return -1;
	for (qsizetype i = 0; i < m_playlist.size(); ++i)
		if (m_playlist[i].id == id)
			return i;
	return -1;
}

// A playlist entry's subtitle file, derived when not explicit.
QString MainWin::srtOf(PlayItem const &it)
{
	if (!it.srt.isEmpty())
		return it.srt;
	std::string story;
	return QString::fromStdString(
		m_disc.srt_for_video(it.video.toStdString(), story));
}

// The player's playlist mirror needs a concrete srt per entry, so
// subtitles attach even for entries reached with mpv's own keys.
QList<play_entry> MainWin::corpusEntries()
{
	QList<play_entry> l;
	for (PlayItem const &it : m_playlist)
		l << play_entry{it.video, srtOf(it)};
	return l;
}

// video_sync: the player moved on its own playlist (its < > keys);
// follow with the document, never commanding the player back.
void MainWin::mpvSwitched(int index)
{
	if (index < 0 || index >= int(m_playlist.size()))
		return;
	PlayItem const &it = m_playlist[qsizetype(index)];
	if (!it.id.isEmpty() && it.id == m_trail.videoId())
		return;                      // our own navigation echoed
	if (QString const srt = srtOf(it); !srt.isEmpty())
		showDoc(it.video, srt);
}

// The document side of opening: transcript, identities, chrome.
bool MainWin::showDoc(QString const &video, QString const &srt)
{
	QFile srtFile(srt);
	if (!srtFile.open(QIODevice::ReadOnly))
		return fail(QStringLiteral("%1: %2").arg(srt,
		                                         srtFile.errorString()));
	QByteArray const raw = srtFile.readAll();
	std::vector<srt::cue> cues = srt::parse(srt::to_utf8(
		{raw.constData(), size_t(raw.size())}));
	if (cues.empty())
		return fail(QStringLiteral("%1: no cues found (not an SRT "
		                           "file?)").arg(srt));

	// Register under the discovery identity: the trail stamps video
	// steps with it, and cross-video undo/redo looks the path up.
	QString const id = videoId(video);
	if (!id.isEmpty()) {
		m_videosById.insert(id, {video, srt, id});
		m_trail.setVideo(id);
		m_grab.setVideo(video, id);
	}

	m_prefs.addRecentFile(video);
	m_prefs.setLastDir(QFileInfo(video).absolutePath());

	auto const n = cues.size();
	m_view.setCues(std::move(cues));
	m_search.refresh();
	setWindowTitle(QStringLiteral("%1 \u2014 srtview")
	               .arg(QFileInfo(video).fileName()));
	setState(QStringLiteral("%1 cues \u00b7 mpv %2")
	         .arg(n)
	         .arg(m_link.spawned() ? QStringLiteral("spawned")
	                               : QStringLiteral("reused")));
	m_view.setFocus();
	return true;
}

// A topic file: the corpus source of videos and composable regexes
// (grammar in topics.hpp).  Loading replaces the playlist; relative
// paths resolve against the file's own directory.
bool MainWin::loadPlaylist(QString const &path)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
		return fail(QStringLiteral("%1: %2").arg(path,
		                                         f.errorString()));
	QByteArray const raw = f.readAll();
	std::string const text = srt::to_utf8(
		{raw.constData(), size_t(raw.size())});
	topics::result r = topics::parse(text);
	if (!r.error.empty())
		return fail(QStringLiteral("%1:%2: %3")
			.arg(path).arg(r.line)
			.arg(QString::fromStdString(r.error)));

	m_corpus = std::move(r.value);
	m_corpusPath = path;
	m_playlist.clear();
	m_transcripts.clear();               // natural refresh point
	QDir const dir = QFileInfo(path).absoluteDir();
	auto const resolve = [&dir](std::string const &p) {
		QString const q = QString::fromStdString(p);
		return q.isEmpty() || !QFileInfo(q).isRelative()
		       ? q : dir.absoluteFilePath(q);
	};
	for (topics::video const &v : m_corpus.videos) {
		PlayItem it{resolve(v.path), resolve(v.srt), {}};
		it.id = videoId(it.video);
		if (!it.id.isEmpty())
			m_videosById.insert(it.id, it);
		m_playlist << it;
	}
	statusBar()->showMessage(QStringLiteral(
		"playlist: %1 videos, %2 topics")
		.arg(m_playlist.size()).arg(m_corpus.topics.size()), 3000);
	if (m_view.cueCount() == 0 && !m_playlist.isEmpty())
		openPath(m_playlist.first().video, m_playlist.first().srt);

	// Topics become the live search vocabulary: every expanded
	// pattern goes into the bar's history in file order (Up in the
	// bar walks from the last backward), and the last one is primed
	// so F3 works the moment the playlist is open.
	for (topics::topic const &t : m_corpus.topics)
		m_prefs.addSearch(QString::fromStdString(
			topics::expand(m_corpus, t)));
	if (!m_corpus.topics.empty()) {
		m_search.setRegexEnabled(true);  // topics are regexes
		m_search.primePattern(QString::fromStdString(
			topics::expand(m_corpus, m_corpus.topics.back())));
	}
	// After the auto-open, so the playlist outranks its own videos.
	m_prefs.addRecentFile(path);
	return true;
}

void MainWin::openPlaylistDialog()
{
	QString const p = QFileDialog::getOpenFileName(this,
		QStringLiteral("Open playlist"), m_prefs.lastDir(),
		QStringLiteral("Topic files (*.svt *.txt);;All files (*)"));
	if (p.isEmpty())
		return;
	m_prefs.setLastDir(QFileInfo(p).absolutePath());
	loadPlaylist(p);
}

void MainWin::rebuildVideosMenu()
{
	m_videosMenu->clear();
	m_videosMenu->addAction(QStringLiteral("&Next video\t(>)"),
	                        this, [this] { stepVideo(1); });
	m_videosMenu->addAction(QStringLiteral("&Previous video\t(<)"),
	                        this, [this] { stepVideo(-1); });
	m_videosMenu->addSeparator();
	if (m_playlist.isEmpty()) {
		m_videosMenu->addAction(QStringLiteral("(no playlist)"))
			->setEnabled(false);
		return;
	}
	for (PlayItem const &it : m_playlist) {
		QAction *a = m_videosMenu->addAction(
			QFileInfo(it.video).fileName());
		a->setCheckable(true);
		a->setChecked(!it.id.isEmpty()
		              && it.id == m_trail.videoId());
		QString const v = it.video, s = it.srt;
		connect(a, &QAction::triggered,
		        this, [this, v, s] { openPath(v, s); });
	}
}

// Export as a build: write what the frame cache has, enqueue what it
// lacks, fold finished frames in as they land (throttled), and stop
// when the digest is whole -- or when a drained queue made no
// progress (mpv striking out), which ends the loop with an honest
// "incomplete".
void MainWin::startExport()
{
	if (m_corpusPath.isEmpty()) {
		statusBar()->showMessage(QStringLiteral(
			"no playlist loaded"), 2000);
		return;
	}
	m_exportQueued = -1;
	runExport(true);
}

void MainWin::grabsIdle()
{
	if (m_exportPending)
		runExport(true);
}

void MainWin::grabProgress()
{
	if (m_exportPending && m_exportTick.elapsed() > 15000)
		runExport(false);
}

void MainWin::runExport(bool drained)
{
	QList<exporter::source> vids;
	for (qsizetype i = 0; i < m_playlist.size(); ++i) {
		exporter::source s;
		s.video = m_playlist[i].video;
		s.srt = srtOf(m_playlist[i]);
		s.id = m_playlist[i].id;
		for (std::string const &n : m_corpus.videos[size_t(i)].topics)
			s.topics << QString::fromStdString(n);
		vids << s;
	}
	QString const out = exportDir();
	exporter::stats const st =
		exporter::run(m_corpus, vids, m_grab, out, m_transcripts);
	m_exportTick.restart();
	if (st.queued == 0) {
		m_exportPending = false;
		statusBar()->showMessage(QStringLiteral(
			"export complete: %1 topics, %2 hits → %3")
			.arg(st.topics).arg(st.hits).arg(out), 6000);
		return;
	}
	if (drained && m_exportQueued >= 0
	    && st.queued >= m_exportQueued) {
		m_exportPending = false;
		statusBar()->showMessage(QStringLiteral(
			"export incomplete: %1 hits lack frames → %2")
			.arg(st.queued).arg(out), 6000);
		return;
	}
	m_exportQueued = st.queued;
	m_exportPending = true;
	statusBar()->showMessage(QStringLiteral(
		"export: %1 of %2 hits still grabbing…")
		.arg(st.queued).arg(st.hits), 6000);
}

QString MainWin::exportDir() const
{
	QFileInfo const fi(m_corpusPath);
	return fi.absolutePath() + QLatin1Char('/')
	     + fi.completeBaseName() + QStringLiteral("-export");
}

// search_nav: leave the current video for the nearest playlist
// neighbor (cyclically, in the search direction) whose transcript
// matches.  The scan reads the srt files directly, so videos without
// a hit are skipped without ever opening them.
bool MainWin::hopVideo(QRegularExpression const &re, bool backward)
{
	qsizetype const n = m_playlist.size();
	qsizetype const at = indexOfId(m_trail.videoId());
	dbgHop(QStringLiteral("hopVideo: at=%1 n=%2 backward=%3 re=%4")
	       .arg(at).arg(n).arg(int(backward))
	       .arg(re.pattern().left(48)));
	// From outside the playlist every entry is a candidate, entered
	// from the end the direction arrives at; from inside, everyone
	// but the current video.
	qsizetype const step = backward ? -1 : 1;
	qsizetype const m = at < 0 ? n : n - 1;
	for (qsizetype k = 1; k <= m; ++k) {
		qsizetype const i = at < 0
			? (backward ? n - k : k - 1)
			: ((at + step * k) % n + n) % n;
		bool const match = videoMatches(m_playlist[i], re);
		dbgHop(QStringLiteral("  candidate %1 (%2): %3")
		       .arg(i).arg(QFileInfo(m_playlist[i].video).fileName(),
		            match ? QStringLiteral("match")
		                  : QStringLiteral("no match")));
		if (match)
			return openPath(m_playlist[i].video,
			                m_playlist[i].srt);
	}
	return false;
}

// One regex pass over the session's rendered transcript (shared
// with the exporter): the offline verdict is exactly what the
// in-document search will see -- raw srt bytes would reject
// patterns that visibly match (anchors, spans touching
// speaker/format markup).
bool MainWin::videoMatches(PlayItem const &it, QRegularExpression const &re)
{
	QString const srt = srtOf(it);
	if (srt.isEmpty()) {
		dbgHop(QStringLiteral("videoMatches: no srt for %1")
		       .arg(it.video));
		return false;
	}
	for (QString const &text
	     : exporter::load(m_transcripts, srt).lines)
		if (re.match(text).hasMatch())
			return true;
	return false;
}

// The always-on status line: pattern, playlist position, timestamp,
// match counters, play state -- assembled whole, set only when it
// actually changed.
void MainWin::updateInfo()
{
	QStringList parts;
	QString const pat = m_search.patternText();
	if (!pat.isEmpty())
		parts << m_info.fontMetrics().elidedText(
			pat, Qt::ElideRight, 320);
	qsizetype const at = indexOfId(m_trail.videoId());
	if (at >= 0)
		parts << QStringLiteral("video %1/%2")
			.arg(at + 1).arg(m_playlist.size());
	if (double const t = m_link.lastTime(); t >= 0.0)
		parts << fmtTime(t, false);
	if (QString const m = matchInfo(at); !m.isEmpty())
		parts << m;
	if (m_view.cueCount() > 0)
		parts << (m_link.lastPause()
			? QStringLiteral("video paused")
			: QStringLiteral("video playing"));
	QString const text = parts.join(QStringLiteral("  ·  "));
	if (text != m_info.text())
		m_info.setText(text);
}

// "Match 3/18 (11/23)": active/total in this video, and across the
// corpus.  The corpus tally is debounced -- typing must not re-scan
// every transcript per keystroke -- and shows an ellipsis while
// pending.
QString MainWin::matchInfo(qsizetype at)
{
	int const n = m_search.matchCount();
	if (n <= 0)
		return {};
	int const idx = m_search.matchIndex();
	QString s = QStringLiteral("Match %1/%2")
		.arg(idx > 0 ? QString::number(idx)
		             : QStringLiteral("?"))
		.arg(n);
	if (at < 0 || m_playlist.size() < 2)
		return s;
	QRegularExpression const re = m_search.effectivePattern();
	QString const key = re.pattern()
	                  + QString::number(int(re.patternOptions()));
	if (key != m_tallyKey) {
		m_tallyKey = key;
		m_tallyTotal = -1;
		m_tallyLag.start();
	}
	if (m_tallyTotal < 0 || m_tally.size() != m_playlist.size())
		return s + QStringLiteral(" (…)");
	int before = 0;
	for (qsizetype i = 0; i < at; ++i)
		before += m_tally[i];
	return s + QStringLiteral(" (%1/%2)")
		.arg(idx > 0 ? QString::number(before + idx)
		             : QStringLiteral("?"))
		.arg(m_tallyTotal);
}

void MainWin::recomputeTally()
{
	m_tally.clear();
	int total = 0;
	QRegularExpression const re = m_search.effectivePattern();
	if (re.isValid() && !re.pattern().isEmpty()) {
		for (PlayItem const &it : m_playlist) {
			int c = 0;
			for (QString const &line : exporter::load(
					m_transcripts, srtOf(it)).lines) {
				auto mi = re.globalMatch(line);
				for (; mi.hasNext(); mi.next())
					++c;
			}
			m_tally << c;
			total += c;
		}
	}
	m_tallyTotal = total;
	updateInfo();
}

// The focused widget names the zoom domain: the pattern field, the
// rest of the search bar, the captions, or everything else (the
// base UI).
MainWin::ZoomDom MainWin::zoomDomain() const
{
	QWidget const *fw = QApplication::focusWidget();
	if (!fw)
		return ZoomDom::base;
	if (m_bar.editFocused())
		return ZoomDom::regex;
	if (fw == &m_bar || m_bar.isAncestorOf(fw))
		return ZoomDom::bar;
	if (fw == &m_view || m_view.isAncestorOf(fw))
		return ZoomDom::captions;
	return ZoomDom::base;
}

int *MainWin::zoomOf(ZoomDom d)
{
	switch (d) {
	case ZoomDom::captions:
		return &m_zoomCaptions;
	case ZoomDom::bar:
		return &m_zoomBar;
	case ZoomDom::regex:
		return &m_zoomRegex;
	case ZoomDom::base:
		break;
	}
	return &m_zoomBase;
}

// The domains nest, so applying is strictly top-down: the base font
// first -- the general font, the theme-pinned class fonts, and our
// own chrome widgets explicitly, so menus, dialogs and the footer
// scale uniformly -- then the widgets that derive from it.  All
// derived sizes are integer points.
void MainWin::applyZoom()
{
	auto const scaled = [this](QFont f) {
		double const z = zoomFactor(m_zoomBase);
		if (f.pixelSize() > 0)
			f.setPixelSize(std::max(1,
				int(std::lround(f.pixelSize() * z))));
		else
			f.setPointSize(std::max(1,
				int(std::lround(f.pointSize() * z))));
		return f;
	};
	QFont const base = scaled(m_baseFont);
	QApplication::setFont(base);
	for (qsizetype i = 0; i < m_classFonts.size(); ++i)
		QApplication::setFont(scaled(m_classFonts[i]),
		                      kThemedClasses[i]);
	// Our own chrome, deterministically: theme-class propagation
	// quirks must not decide whether the footer scales.
	menuBar()->setFont(QApplication::font("QMenuBar"));
	statusBar()->setFont(QApplication::font("QStatusBar"));
	m_info.setFont(base);
	m_state.setFont(base);
	m_view.setTypeZoom(zoomFactor(m_zoomCaptions));
	m_bar.setTypeZoom(zoomFactor(m_zoomBar),
	                  zoomFactor(m_zoomRegex));
	m_search.layoutOverlay();
}

// Constrain first, compare second, act third: a step against a
// domain bound changes nothing and must do nothing.  The pattern
// text cannot outgrow its box, so its domain tops out at 0.
void MainWin::zoomStep(int dir)
{
	ZoomDom const d = zoomDomain();
	int &steps = *zoomOf(d);
	int const top = d == ZoomDom::regex ? 0 : kZoomSpan;
	int const next = std::clamp(steps + dir, -kZoomSpan, top);
	if (next == steps)
		return;
	steps = next;
	applyZoom();
}

void MainWin::zoomReset(bool all)
{
	if (all) {
		if (!m_zoomBase && !m_zoomCaptions && !m_zoomBar
		    && !m_zoomRegex)
			return;
		m_zoomBase = 0;
		m_zoomCaptions = 0;
		m_zoomBar = 0;
		m_zoomRegex = 0;
	} else {
		int &steps = *zoomOf(zoomDomain());
		if (!steps)
			return;
		steps = 0;
	}
	applyZoom();
}

// mpv's own playlist keys: > forward, < back, wrapping around.  A
// current video from outside the playlist enters at the ends.
void MainWin::stepVideo(int dir)
{
	if (m_playlist.isEmpty()) {
		statusBar()->showMessage(QStringLiteral("no playlist"),
		                         1500);
		return;
	}
	qsizetype const n = m_playlist.size();
	qsizetype const at = indexOfId(m_trail.videoId());
	qsizetype const to = at < 0 ? (dir > 0 ? 0 : n - 1)
	                            : (at + dir + n) % n;
	openPath(m_playlist[to].video, m_playlist[to].srt);
}

// Fundo: walk the undo tree.  Both directions receive the state to
// arrive at -- undo resolves each departed facet to its nearest
// recorded ancestor value, redo gets the node ascended into -- and
// apply exactly the facets the step touched.  Side branches persist;
// retracing an identical action adopts its old branch.  Application
// runs with recording suppressed.
void MainWin::undoStep()
{
	std::optional<trail_step> const s = m_trail.undo();
	if (!s) {
		statusBar()->showMessage(QStringLiteral("nothing to undo"),
		                         1500);
		return;
	}
	m_trail.setApplying(true);
	applyStep(*s, true);
	m_trail.setApplying(false);
}

void MainWin::redoStep()
{
	std::optional<trail_step> const s = m_trail.redo();
	if (!s) {
		statusBar()->showMessage(QStringLiteral("nothing to redo"),
		                         1500);
		return;
	}
	m_trail.setApplying(true);
	applyStep(*s, false);
	m_trail.setApplying(false);
}

// Video first: it is the one applier that can fail (mpv refuses),
// and a refused step must bail before any facet has been touched.
void MainWin::applyStep(trail_step const &s, bool undo)
{
	QStringList parts;
	if (s.flags & trail_step::video) {
		if (!applyVideoStep(s)) {
			// Playback never moved: put the tree back by taking
			// the opposite step of the one being applied.
			if (undo)
				m_trail.redo();
			else
				m_trail.undo();
			return;
		}
		parts << fmtTime(s.time, true);
	}
	if (s.flags & trail_step::text) {
		m_search.applyPattern(s.pattern);
		parts << QStringLiteral("search \"%1\"").arg(s.pattern);
	}
	if (s.flags & trail_step::cursor)
		m_search.applyCursor(s.cur);
	if (parts.isEmpty())                 // cursor-only: stay quiet
		return;
	statusBar()->showMessage(QStringLiteral("%1 \u2192 %2")
		.arg(undo ? QStringLiteral("undo") : QStringLiteral("redo"),
		     parts.join(QStringLiteral(" \u00b7 "))), 2000);
}

// The trail spans the corpus: a step recorded in another video first
// switches to it (registry: playlist entries plus every video opened
// this session), then seeks.
bool MainWin::applyVideoStep(trail_step const &s)
{
	if (s.vid != m_trail.videoId()) {
		auto const it = m_videosById.constFind(s.vid);
		if (it == m_videosById.constEnd()) {
			statusBar()->showMessage(QStringLiteral(
				"this step's video is not in the playlist"),
				3000);
			return false;
		}
		if (!openPath(it->video, it->srt))
			return false;
	}
	return m_playback.applyTime(s.time);
}

void MainWin::dragEnterEvent(QDragEnterEvent *ev)
{
	if (droppable(ev->mimeData()))
		ev->acceptProposedAction();
}

void MainWin::dropEvent(QDropEvent *ev)
{
	auto const urls = ev->mimeData()->urls();
	if (!urls.isEmpty())
		openPath(urls.first().toLocalFile());
}

void MainWin::closeEvent(QCloseEvent *ev)
{
	m_grab.shutdown();
	m_link.shutdown();
	ev->accept();
}

void MainWin::resizeEvent(QResizeEvent *ev)
{
	QMainWindow::resizeEvent(ev);
	m_search.layoutOverlay();
}

bool MainWin::droppable(QMimeData const *md)
{
	if (!md->hasUrls() || md->urls().isEmpty())
		return false;
	return avPath(md->urls().first().toLocalFile());
}

// A path the direct video/subtitle open flow handles; anything else
// reappearing from recents is a topic file.
bool MainWin::avPath(QString const &p)
{
	static constexpr QLatin1StringView exts[]{
		QLatin1StringView(".srt"),  QLatin1StringView(".mp4"),
		QLatin1StringView(".mkv"),  QLatin1StringView(".webm"),
		QLatin1StringView(".avi"),  QLatin1StringView(".mov"),
		QLatin1StringView(".m4v"),  QLatin1StringView(".mpg"),
		QLatin1StringView(".mpeg"), QLatin1StringView(".ts"),
		QLatin1StringView(".wmv")};
	for (auto const e : exts)
		if (p.endsWith(e, Qt::CaseInsensitive))
			return true;
	return false;
}

bool MainWin::openAny(QString const &path)
{
	return avPath(path) ? openPath(path) : loadPlaylist(path);
}

void MainWin::openDialog(QString const &startDir)
{
	QFileDialog dlg(this, QStringLiteral("Open video or subtitle"),
	                startDir);
	dlg.setFileMode(QFileDialog::ExistingFile);
	dlg.setNameFilter(
		QStringLiteral("Video / SRT (*.mp4 *.mkv *.webm *.avi *.mov "
		               "*.m4v *.mpg *.mpeg *.ts *.wmv *.srt);;"
		               "All files (*)"));
	int const r = dlg.exec();
	// Remember where the user browsed to, accepted or not: cancel
	// after navigating still means "continue from there next time".
	m_prefs.setLastDir(dlg.directory().absolutePath());
	if (r != QDialog::Accepted || dlg.selectedFiles().isEmpty())
		return;
	openPath(dlg.selectedFiles().first());
}

void MainWin::rebuildRecentMenu()
{
	m_recentMenu->clear();
	QStringList const files = m_prefs.recentFiles();
	if (files.isEmpty()) {
		m_recentMenu->addAction(QStringLiteral("(empty)"))
			->setEnabled(false);
		return;
	}
	for (QString const &path : files) {
		QString name = QFileInfo(path).fileName();
		if (!avPath(path))
			name += QStringLiteral("  [playlist]");
		QAction *act = m_recentMenu->addAction(name);
		act->setToolTip(path);
		act->setData(path);
		connect(act, &QAction::triggered,
		        this, [this, path] { openAny(path); });
	}
}

// Right-click on a recent entry: offer to open the file dialog in
// that entry's directory, for revisiting previously used places.
// Keys < and > on the view: step the playlist.
bool MainWin::eventFilter(QObject *obj, QEvent *ev)
{
	if ((obj == statusBar() || obj == menuBar())
	    && ev->type() == QEvent::MouseButtonPress) {
		statusBar()->setFocus(Qt::MouseFocusReason);
		return false;                // and let the click proceed
	}
	if (obj == &m_view && ev->type() == QEvent::KeyPress) {
		auto const *ke = static_cast<QKeyEvent *>(ev);
		if (ke->text() == QStringLiteral(">")) {
			stepVideo(1);
			return true;
		}
		if (ke->text() == QStringLiteral("<")) {
			stepVideo(-1);
			return true;
		}
		return QMainWindow::eventFilter(obj, ev);
	}
	if (obj != m_recentMenu || ev->type() != QEvent::ContextMenu)
		return QMainWindow::eventFilter(obj, ev);
	auto *ce = static_cast<QContextMenuEvent *>(ev);
	QAction *act = m_recentMenu->actionAt(ce->pos());
	if (!act || act->data().toString().isEmpty())
		return true;
	QMenu ctx;
	QAction *view = ctx.addAction(
		QStringLiteral("&View file location\u2026"));
	if (ctx.exec(ce->globalPos()) != view)
		return true;
	QString const dir = QFileInfo(act->data().toString())
	                    .absolutePath();
	m_recentMenu->close();
	openDialog(dir);
	return true;
}

void MainWin::closeFile()
{
	m_link.shutdown();
	m_view.setCues({});
	m_view.clear();
	setWindowTitle(QStringLiteral("srtview"));
	setState(QStringLiteral("no file"));
}

bool MainWin::fail(QString const &msg)
{
	QMessageBox::warning(this, QStringLiteral("srtview"), msg);
	return false;
}

void MainWin::setState(QString const &s)
{
	m_state.setText(s);
}
