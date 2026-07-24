#include "mainwin.hpp"

#include "agenda.hpp"
#include "palettefix.hpp"
#include "srt.hpp"

#include <QCryptographicHash>
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

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

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

// Focus bias on the facts queue: opening a video warms its leaf.
constexpr double kFocusHeat = 0.5;

// Search-tally bias: scale spreads a pattern's total weight across
// its hit videos by share; keep is the fade per pattern change.
constexpr double kSearchHeat = 2.0;
constexpr double kSearchKeep = 0.5;

// The leading 8 bytes of a finished BLAKE2b-256 as a pipeline id.
agenda::id takeId(QCryptographicHash &h)
{
	agenda::id out;
	std::memcpy(out.b.data(), h.result().constData(), out.b.size());
	return out;
}

// Pyramid-node identity from ordered children: two corpora sharing
// a prefix of leaves share the prefix's summary files.  Children
// are fixed-width raw ids, so no separator is needed.
agenda::id treeId(std::vector<agenda::id> const &kids)
{
	QCryptographicHash h(QCryptographicHash::Blake2b_256);
	h.addData(QByteArrayView("tree"));
	for (agenda::id const &k : kids)
		h.addData(QByteArrayView(
			reinterpret_cast<char const *>(k.b.data()),
			qsizetype(k.b.size())));
	return takeId(h);
}

// Dive identity is the expanded pattern's hash: editing a topic
// re-dives it, and identical patterns share one cache file.
agenda::id diveId(std::string const &pattern)
{
	QCryptographicHash h(QCryptographicHash::Blake2b_256);
	h.addData(QByteArrayView("dive\n"));
	h.addData(QByteArrayView(pattern.data(),
	                         qsizetype(pattern.size())));
	return takeId(h);
}

// Excerpt budget per dive, in UTF-8 bytes: roughly half the llm
// clip so the context sections keep their share of the window.
constexpr std::size_t kDiveBudget = std::size_t{48} * 1024;

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
	// main() has normalized a pixel-sized platform font to integer
	// points before any widget constructed.
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
	m_diveTick.setInterval(0);
	connect(&m_diveTick, &QTimer::timeout,
	        this, [this] { diveStep(); });

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
	// On-the-spot videos join the corpus: a bare open founds an
	// implicit playlist of one, an open beside a loaded corpus
	// extends it, and the knowledge pipeline follows either way.
	adoptVideo(video, srt);
	// Player routing: playlist members navigate inside the
	// persistent corpus instance -- same window, no respawn, no
	// focus theft.  The corpus claims the topic file's socket, or,
	// for an implicit corpus with no file, the founding video's --
	// which keeps a bare single-video open byte-compatible with
	// the srtjump sharing scheme.
	qsizetype const at = playlistIndex(video);
	QString const claim = at < 0 ? video
	                    : m_corpusPath.isEmpty()
	                      ? m_playlist.first().video
	                      : m_corpusPath;
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
	m_facts.heat(offerFacts(srt), kFocusHeat);

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
	// A search-driven hop must not pull focus out of the bar; only
	// a switch made from elsewhere hands the keyboard to the view.
	if (!barFocused())
		m_view.setFocus();
	return true;
}

// The facts cache is keyed by the srt file's own discovery identity
// (its hex socket hash, rehydrated to bytes at this boundary): one
// summary per unique srt however many entries share it.  The
// rendered transcript (tags consumed) is what the model reads.  The
// returned id feeds the corpus pyramid and the heat map.
agenda::id MainWin::offerFacts(QString const &srt)
{
	agenda::id const key = agenda::id::from_hex(
		m_disc.id_for_video(srt.toStdString()));
	if (key)
		m_facts.offer(key, exporter::load(m_transcripts, srt)
		                   .lines.join(QLatin1Char('\n'))
		                   .toStdString());
	return key;
}

// Re-derive everything the corpus defines: the playlist and the
// id registry, then the facts pipeline -- leaf offers, the
// abstraction pyramid, the topic dive scans.  Runs after any corpus
// mutation, a loaded file or an on-the-spot adoption; the transcript
// cache and the facts plan dedupe, so re-derivation is idempotent
// and pending work from an older shape simply finishes into the
// cache.  (The Videos menu rebuilds itself on show.)
void MainWin::rebuildCorpus()
{
	m_playlist.clear();
	QDir const dir = QFileInfo(m_corpusPath).absoluteDir();
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
	// One leaf offer per srt not yet in the facts cache, then the
	// pyramid over them in playlist order.  The transcript cache is
	// shared with the tally and the exporter: nothing parses twice.
	std::vector<agenda::id> leaves;
	for (PlayItem const &it : m_playlist) {
		agenda::id const key = offerFacts(srtOf(it));
		if (key && std::ranges::find(leaves, key) == leaves.end())
			leaves.push_back(key);
	}
	std::vector<agenda::task> nodes = agenda::pyramid(leaves, treeId);
	m_rootId = nodes.empty() ? agenda::id{} : nodes.back().id;
	m_facts.corpus(std::move(nodes));
	queueDives();
	updateInfo();
}

// A video seen outside the playlist joins the corpus in memory: a
// bare open founds an implicit playlist, later opens and drops
// extend whatever is loaded.  The topic file on disk is never
// touched -- export writes versions.
void MainWin::adoptVideo(QString const &video, QString const &srt)
{
	if (playlistIndex(video) >= 0)
		return;
	m_corpus.videos.push_back(
		{video.toStdString(), srt.toStdString(), {}});
	rebuildCorpus();
}

// Enter kept a pattern: adopt it as an implicitly exported topic
// and restage the dive scans.  Case-insensitivity folds into the
// pattern as an inline flag, since topic files carry bare PCRE2;
// adoption dedupes, so re-committing a known pattern is free.
void MainWin::searchCommitted()
{
	if (m_playlist.isEmpty())
		return;
	QRegularExpression const re = m_search.effectivePattern();
	if (!re.isValid() || re.pattern().isEmpty())
		return;
	std::string pat = re.pattern().toStdString();
	if (re.patternOptions()
	    & QRegularExpression::CaseInsensitiveOption)
		pat = "(?i:" + pat + ")";
	if (topics::adopt(m_corpus, pat))
		queueDives();
}

// Stage the corpus topic dives: one scan per exported grouping,
// chewed a video per tick.  Reopening a summarized corpus still
// scans (a few ms per cell, spread out); Facts drops the finished
// scan against its cache.
void MainWin::queueDives()
{
	m_diveScans.clear();
	m_diveAt = 0;
	m_diveTick.stop();
	for (topics::export_item const &e : topics::export_plan(m_corpus))
		stageDive(e.pattern, true);
	// The supportive layer: referenced topics dive too, a band
	// lower and unexported -- the nested regexes reveal semantic
	// structure inside the tops, and the queue can lean on it.
	for (topics::topic const *t : topics::components(m_corpus))
		stageDive(topics::expand(m_corpus, *t), false);
	if (!m_diveScans.empty())
		m_diveTick.start();
}

void MainWin::stageDive(std::string const &pattern, bool exported)
{
	DiveScan s;
	s.re = QRegularExpression(QString::fromStdString(pattern));
	if (!s.re.isValid())
		return;
	s.id = diveId(pattern);
	s.pattern = pattern;
	s.exported = exported;
	m_diveScans.push_back(std::move(s));
}

void MainWin::diveStep()
{
	if (m_diveAt >= m_diveScans.size() || m_playlist.isEmpty()) {
		m_diveScans.clear();
		m_diveAt = 0;
		m_diveTick.stop();
		return;
	}
	DiveScan &s = m_diveScans[m_diveAt];
	if (s.video >= std::size_t(m_playlist.size())) {
		finishDive(s);
		++m_diveAt;
		return;
	}
	scanDiveVideo(s, m_playlist[qsizetype(s.video)]);
	++s.video;
}

// One (topic, video) cell: matched cue lines become an excerpt
// section and the video's leaf a dependency.  Past the budget a
// video is dropped whole -- section and dependency both -- so the
// dive never cites a video it did not quote.
void MainWin::scanDiveVideo(DiveScan &s, PlayItem const &it)
{
	if (s.parts.size() > kDiveBudget)
		return;
	QString const srt = srtOf(it);
	std::string hits;
	for (QString const &line :
	     exporter::load(m_transcripts, srt).lines) {
		if (!s.re.match(line).hasMatch())
			continue;
		hits += line.toStdString();
		hits += '\n';
	}
	if (hits.empty())
		return;
	agenda::id const key = agenda::id::from_hex(
		m_disc.id_for_video(srt.toStdString()));
	if (!key || std::ranges::find(s.deps, key) != s.deps.end())
		return;
	s.deps.push_back(key);
	s.parts += "== ";
	s.parts += QFileInfo(it.video).fileName().toStdString();
	s.parts += '\n';
	s.parts += hits;
}

// A finished scan becomes a dive task: deps gate on the hit videos'
// leaf summaries, heat follows the same videos, and the pyramid
// root rides along as optional overview context.
void MainWin::finishDive(DiveScan const &s)
{
	if (s.deps.empty())
		return;
	agenda::task t;
	t.id = s.id;
	t.deps = s.deps;
	t.keys = s.deps;
	t.note = s.pattern;
	t.what = agenda::kind::dive;
	t.exported = s.exported;
	if (m_rootId)
		t.refs.push_back(m_rootId);
	m_facts.dive(std::move(t), s.parts);
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
	m_transcripts.clear();               // natural refresh point
	m_facts.reset();
	rebuildCorpus();
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
	if (m_playlist.isEmpty()) {
		statusBar()->showMessage(QStringLiteral(
			"nothing to export"), 2000);
		return;
	}
	writePlaylistVersion();
	m_exportQueued = -1;
	runExport(true);
}

// The corpus as it stands -- implicit adoptions included -- is part
// of the exported artifact.  Versions land inside the export
// directory, and the file a playlist was loaded from is never
// written: loading a previously exported version gets a ".new"
// sibling instead of an overwrite.
void MainWin::writePlaylistVersion()
{
	QString const name = m_corpusPath.isEmpty()
		? QStringLiteral("playlist.topics")
		: QFileInfo(m_corpusPath).fileName();
	QDir().mkpath(exportDir());
	QString target = exportDir() + QLatin1Char('/') + name;
	if (!m_corpusPath.isEmpty()
	    && QFileInfo(target).canonicalFilePath()
	       == QFileInfo(m_corpusPath).canonicalFilePath())
		target += QStringLiteral(".new");
	QFile f(target);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
		return;
	std::string const text = topics::write(m_corpus);
	f.write(text.data(), qint64(text.size()));
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

// Beside the topic file, or -- for an implicit corpus -- beside its
// founding video.
QString MainWin::exportDir() const
{
	QString const base = !m_corpusPath.isEmpty() ? m_corpusPath
	                   : m_playlist.isEmpty()    ? QString()
	                   : m_playlist.first().video;
	QFileInfo const fi(base);
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
	// The tally -- and through it the facts heat map -- keys on
	// the pattern alone: a search that misses this video but hits
	// others must still recompute and bias the queue.
	if (!m_playlist.isEmpty()) {
		QRegularExpression const re = m_search.effectivePattern();
		QString const key = re.pattern()
		                  + QString::number(int(re.patternOptions()));
		if (key != m_tallyKey) {
			m_tallyKey = key;
			m_tallyTotal = -1;
			m_tallyLag.start();
		}
	}

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
	feedHeat();
	updateInfo();
}

// Search interest becomes queue bias: each recompute (one per
// pattern change) fades what the previous pattern contributed and
// adds the new tally's shares, so the background pipeline drifts
// toward the videos the user's regex is lighting up -- and, through
// priority inheritance in the agenda, so do the summaries built on
// top of them.
void MainWin::feedHeat()
{
	m_facts.decay(kSearchKeep);
	if (m_tallyTotal <= 0)
		return;

	for (qsizetype i = 0; i < m_tally.size(); ++i) {
		if (!m_tally[i])
			continue;
		m_facts.heat(agenda::id::from_hex(m_disc.id_for_video(
		             	srtOf(m_playlist[i]).toStdString())),
		             kSearchHeat * m_tally[i] / m_tallyTotal);
	}
}

// The focused widget names the zoom domain: the pattern field, the
// rest of the search bar, the captions, or everything else (the
// base UI).
bool MainWin::barFocused() const
{
	QWidget const *fw = QApplication::focusWidget();
	return fw && (fw == &m_bar || m_bar.isAncestorOf(fw));
}

MainWin::ZoomDom MainWin::zoomDomain() const
{
	QWidget const *fw = QApplication::focusWidget();
	if (!fw)
		return ZoomDom::base;
	if (m_bar.editFocused())
		return ZoomDom::regex;
	if (barFocused())
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

// The domains nest, so a change applies strictly top-down from the
// changed domain: only a base step rewrites the application and
// chrome fonts (menus, dialogs and the footer scale uniformly with
// it), captions and the bar touch just their own widgets, and a
// pattern-text step never reflows the captions or moves the bar.
// All derived sizes are integer points.
void MainWin::applyZoom(ZoomDom d)
{
	if (d == ZoomDom::base) {
		// An open menu popup is positioned for the old chrome
		// metrics; close it rather than leave it detached.
		if (QWidget *pop = QApplication::activePopupWidget())
			pop->close();
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
		// Our own chrome, deterministically: theme-class
		// propagation quirks must not decide whether the footer
		// scales.
		menuBar()->setFont(QApplication::font("QMenuBar"));
		statusBar()->setFont(QApplication::font("QStatusBar"));
		m_info.setFont(base);
		m_state.setFont(base);
	}
	if (d == ZoomDom::base || d == ZoomDom::captions)
		m_view.setTypeZoom(zoomFactor(m_zoomCaptions));
	if (d != ZoomDom::captions)
		m_bar.setTypeZoom(zoomFactor(m_zoomBar),
		                  zoomFactor(m_zoomRegex));
	if (d == ZoomDom::base || d == ZoomDom::bar)
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
	applyZoom(d);
}

void MainWin::zoomReset(bool all)
{
	if (!all) {
		ZoomDom const d = zoomDomain();
		int &steps = *zoomOf(d);
		if (!steps)
			return;
		steps = 0;
		applyZoom(d);
		return;
	}
	// Reset everything, applying only where a counter moved; a
	// moved base covers the lot.
	if (m_zoomBase) {
		m_zoomBase = 0;
		m_zoomCaptions = 0;
		m_zoomBar = 0;
		m_zoomRegex = 0;
		applyZoom(ZoomDom::base);
		return;
	}
	if (m_zoomCaptions) {
		m_zoomCaptions = 0;
		applyZoom(ZoomDom::captions);
	}
	if (m_zoomBar || m_zoomRegex) {
		m_zoomBar = 0;
		m_zoomRegex = 0;
		applyZoom(ZoomDom::bar);
	}
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
	// A refused open is a state flip: on the record even without
	// SRTVIEW_DEBUG, and the only trace on a headless platform
	// where the dialog blocks invisibly.
	std::fprintf(stderr, "srtview: %s\n", qPrintable(msg));
	QMessageBox::warning(this, QStringLiteral("srtview"), msg);
	return false;
}

void MainWin::setState(QString const &s)
{
	m_state.setText(s);
}
