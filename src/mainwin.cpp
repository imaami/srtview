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
#include <utility>

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
	static_assert(sizeof out.b <= 32, "id exceeds BLAKE2b-256");
	std::memcpy(out.b.data(), h.result().constData(), sizeof out.b);
	return out;
}

// vault's injected H8 over arbitrary bytes: the same BLAKE2b-256
// head every pipeline id uses.
agenda::id hash8(std::string_view s)
{
	QCryptographicHash h(QCryptographicHash::Blake2b_256);
	h.addData(QByteArrayView(s.data(), qsizetype(s.size())));
	return takeId(h);
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

// Pair identities from the unordered dive pair, sorted so the same
// two dives name the same artifacts whichever finished first: the
// focus is the written thread, the probe the ask for what to search
// toward it, and the probe's one retry is salted apart.
agenda::id pairId(std::string_view tag, agenda::id a, agenda::id b)
{
	if (b.b < a.b)
		std::swap(a, b);
	QCryptographicHash h(QCryptographicHash::Blake2b_256);
	h.addData(QByteArrayView(tag.data(), qsizetype(tag.size())));
	h.addData(QByteArrayView(
		reinterpret_cast<char const *>(a.b.data()),
		qsizetype(a.b.size())));
	h.addData(QByteArrayView(
		reinterpret_cast<char const *>(b.b.data()),
		qsizetype(b.b.size())));
	return takeId(h);
}

agenda::id focusId(agenda::id a, agenda::id b)
{
	return pairId("focus", a, b);
}

agenda::id probeId(agenda::id a, agenda::id b, bool retry)
{
	return pairId(retry ? "probe!" : "probe", a, b);
}

std::size_t sharedKeys(std::vector<agenda::id> const &a,
                       std::vector<agenda::id> const &b)
{
	std::size_t n = 0;
	for (agenda::id const k : a)
		n += std::ranges::find(b, k) != b.end();
	return n;
}

// Focus fan-out per new dive: the all-pairs plan is quadratic and
// would mostly buy paced NONEs from weakly related pairs.
constexpr std::size_t kFocusFan = 3;

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

// Per-side slice of a dive's excerpts fed to a probe: calibration
// for the variant sweep, not coverage, so the head of the catch is
// enough and two sides fit beside the pair's prose.
constexpr std::size_t kProbeSample = std::size_t{16} * 1024;

// Match display cap per video in the knowledge pane -- display
// only; scans and counts always cover everything.
constexpr int kMatchCap = 500;

// One terms window's worth of numbered transcript, in bytes: cue
// boundaries always, roughly a quarter of the llm clip so prompt
// and reply breathe.
constexpr qsizetype kTermsWindow = qsizetype{10} * 1024;

// Terms-window identity covers the subtitle's discovery id, the
// cue range and the window text itself -- content-honest, so a
// retranscribed subtitle plans fresh windows instead of silently
// reusing stale replies.
agenda::id termsId(QString const &srtId, int first, int last,
                   QByteArray const &text)
{
	QCryptographicHash h(QCryptographicHash::Blake2b_256);
	h.addData(QByteArrayView("terms\n"));
	h.addData(srtId.toUtf8());
	h.addData(QByteArrayView("\n"));
	h.addData(QByteArray::number(first));
	h.addData(QByteArrayView("-"));
	h.addData(QByteArray::number(last));
	h.addData(QByteArrayView("\n"));
	h.addData(text);
	return takeId(h);
}

// The head of an excerpt block, cut at a line boundary; excerpt
// lines are newline-terminated, so a no-fit only happens when the
// first line alone overflows the slice.
std::string_view sampleOf(std::string const &parts)
{
	if (parts.size() <= kProbeSample)
		return parts;
	std::size_t const cut = parts.rfind('\n', kProbeSample);
	return cut == std::string::npos
	       ? std::string_view()
	       : std::string_view(parts.data(), cut + 1);
}

// The payload of a REGEX: line: [from, end) with blanks trimmed off
// the front and trailing controls off the back.
std::string regexPayload(std::string const &text, std::size_t from,
                         std::size_t end)
{
	while (from < end && (text[from] == ' ' || text[from] == '\t'))
		++from;
	while (end > from
	       && static_cast<unsigned char>(text[end - 1]) <= 0x20)
		--end;
	return text.substr(from, end - from);
}

// The last line-anchored REGEX: line of a reply -- the probe prompt
// allows working notes above the answer, and the one-shot focus
// files closed with it.
std::string regexLine(std::string const &text)
{
	std::size_t const at = text.rfind("REGEX:");
	if (at == std::string::npos || (at && text[at - 1] != '\n'))
		return {};
	std::size_t end = text.find('\n', at + 6);
	if (end == std::string::npos)
		end = text.size();
	return regexPayload(text, at + 6, end);
}

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
	, m_know(this)
	, m_link(&m_playback)
	, m_facts(hash8)
	, m_playback(m_link, m_view, *statusBar(), m_trail, m_grab,
	             this)
	, m_search(m_bar, m_view, *statusBar(), m_prefs, m_trail,
	           m_playback, this)
{
	m_grab.setListener(this, this);
	m_exportTick.start();
	// Clicks on the top or bottom chrome focus the footer: neither
	// bar is focusable by itself, and focusing the menu bar would
	// hijack plain keys as mnemonics.  (Zoom no longer cares about
	// focus -- the domain follows the pointer.)
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

	// Zoom keys route by hover (zoomDomain), so they must fire from
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

	// The knowledge pane: hidden until summoned, then keyboard all
	// the way -- Ctrl+K filters, Enter lands in the list, Enter
	// again activates.  A topic or focus applies its exact pattern
	// to the live search (tally and heat follow on their own); a
	// video row switches playback.
	addDockWidget(Qt::RightDockWidgetArea, &m_know);
	m_know.hide();
	auto *view = menuBar()->addMenu(QStringLiteral("Vie&w"));
	QAction *ka = m_know.toggleViewAction();
	ka->setText(QStringLiteral("&Knowledge\tCtrl+K"));
	view->addAction(ka);
	auto *ks = new QShortcut(QKeySequence(
		QStringLiteral("Ctrl+K")), this);
	ks->setContext(Qt::ApplicationShortcut);
	connect(ks, &QShortcut::activated,
	        this, [this] { m_know.summon(); });
	connect(&m_know.tree(), &QTreeWidget::itemActivated, this,
	        [this](QTreeWidgetItem *it, int) {
		QString const pat =
			it->data(0, KnowledgePane::kPattern).toString();
		QString const video =
			it->data(0, KnowledgePane::kVideo).toString();
		if (!pat.isEmpty())
			m_search.applyPattern(pat);
		else if (!video.isEmpty())
			openPath(video,
			         it->data(0, KnowledgePane::kSrt)
			           .toString());
	});
	connect(&m_know.tree(), &QTreeWidget::currentItemChanged, this,
	        [this](QTreeWidgetItem *cur, QTreeWidgetItem *) {
		knowledgeSelected(cur);
	});
	// A match line is an exact cue in an exact video: switch
	// there if needed, park the cursor on the cue and seek -- the
	// same trail-recorded jump a gutter click makes.
	connect(&m_know.hits(), &QTreeWidget::itemActivated, this,
	        [this](QTreeWidgetItem *it, int) {
		QString const video =
			it->data(0, KnowledgePane::kVideo).toString();
		if (video.isEmpty())
			return;
		int const cue =
			it->data(0, KnowledgePane::kCue).toInt();
		if (videoId(video) != m_trail.videoId()
		    && !openPath(video,
		                 it->data(0, KnowledgePane::kSrt)
		                   .toString()))
			return;
		m_view.showCue(cue);
		m_playback.seekCue(cue, false);
	});
	auto *search = menuBar()->addMenu(QStringLiteral("&Search"));
	search->addAction(QStringLiteral("&Find\u2026"), QKeySequence::Find,
	                  this, [this] { m_search.showSearch(); });
	search->addAction(&m_search.nextAction());
	search->addAction(&m_search.prevAction());
	search->addSeparator();
	search->addAction(&m_search.nextTextAction());
	search->addAction(&m_search.prevTextAction());

	// --- status bar ---
	// The pattern owns the left of the footer and stretches with
	// the window; everything else -- video/time/match and the
	// (rare) state text -- keeps to the right edge.
	statusBar()->addWidget(&m_pattern, 1);
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
	// Nonzero, so the scan leaves real idle between cells: a zero
	// timer re-fires as fast as the loop drains, and semantic
	// background work has no latency constraint worth a warm
	// chassis.
	m_diveTick.setInterval(1);
	connect(&m_diveTick, &QTimer::timeout,
	        this, [this] { diveStep(); });
	// The probe and harvest pumps share a slow tick: answered
	// probes advance their focus chains (search, retry, write),
	// and completed focuses fold their REGEX hypotheses back into
	// the corpus as generated topics.
	m_focusTick.setInterval(10000);
	connect(&m_focusTick, &QTimer::timeout, this, [this] {
		pumpProbes();
		harvestTerms();
		stageMerge();
		harvestMerge();
		harvestFocus();
		refreshKnowledge();
	});
	m_focusTick.start();

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
	if (!m_link.setPlaylist(list, sock, index, &err)) {
		errState(QStringLiteral("mpv: %1").arg(err));
		return fail(err);
	}
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
// One transcript truth per session: the view reads the same parse
// the pipeline, tally, evidence and export consume, so none of them
// can disagree about what the document says.
bool MainWin::showDoc(QString const &video, QString const &srt)
{
	exporter::transcript const &tx =
		exporter::load(m_transcripts, srt);
	if (tx.cues.empty()) {
		QFile f(srt);
		return f.open(QIODevice::ReadOnly)
		       ? fail(QStringLiteral("%1: no cues found (not an "
		                             "SRT file?)").arg(srt))
		       : fail(QStringLiteral("%1: %2")
		              .arg(srt, f.errorString()));
	}

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

	m_view.setCues(tx.cues);
	m_search.refresh();
	setWindowTitle(QStringLiteral("%1 \u2014 srtview")
	               .arg(QFileInfo(video).fileName()));
	// No murmur on success: cue counts and mpv lifecycle are
	// implementation trivia, and the state line stays reserved for
	// the rare red error.
	setState({});
	// A search-driven hop must not pull focus out of the bar; only
	// a switch made from elsewhere hands the keyboard to the view.
	if (!barFocused())
		m_view.setFocus();
	return true;
}

// The facts cache is keyed by the srt file's own discovery identity
// (its hex socket hash, rehydrated to bytes at this boundary): one
// summary per unique srt however many entries share it.  The
// rendered transcript (tags consumed) is what the model reads and
// what the vault witnesses: offer() hashes it, marks resolvable
// cache hits done -- new dives depend on cached leaves, so the plan
// must know them -- and asks only when the chain misses.  The
// returned id feeds the pyramid and the heat map.
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

// Reloaded machine topics keep their roles by name stem: term* stay
// index-only (no dives, no pending badge) and focus* stay generated
// (supportive, never pairing), before the harvest re-attaches titles
// from the cached replies.  A hand topic borrowing the shape is
// treated as machine -- deterministic, and adopt() never mints a
// colliding name.  Name-keyed and idempotent, so extension rebuilds
// re-seed for free.
void MainWin::seedGenerated()
{
	for (topics::topic const &t : m_corpus.topics) {
		bool const term = topics::stem_name(t.name, "term");
		if (term)
			m_termTopics.insert(t.name);
		if (term || topics::stem_name(t.name, "focus"))
			m_generated.insert(t.name);
	}
}

// Re-derive everything the corpus defines: the playlist and the
// id registry, then the facts pipeline -- leaf offers, the
// abstraction pyramid, the topic dive scans.  Runs after any corpus
// mutation, a loaded file or an on-the-spot adoption; the transcript
// cache and the facts plan dedupe, so re-derivation is idempotent
// and pending work from an older shape simply finishes into the
// cache.  fresh means the corpus was replaced, not extended: dive
// scans restart instead of merging.  (The Videos menu rebuilds
// itself on show.)
void MainWin::rebuildCorpus(bool fresh)
{
	if (fresh) {
		m_dives.clear();
		m_focusWork.clear();
		m_focusPending.clear();
		m_generated.clear();
		m_harvested.clear();
		m_termsWork.clear();
		m_termsSeen.clear();
		m_diveRetired.clear();
		m_mergeId = {};
		m_mergeSet.clear();
		m_mergeSeen.clear();
		m_termTopics.clear();
		m_termInfo.clear();
		m_termIndex.clear();
	}
	seedGenerated();
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
	// Harvest before staging, so last session's focus regexes sit
	// in the corpus when the dive scans are drawn from it.
	// Terms before focus, here and on the tick: whatever terms
	// have answered adopt before any focus does, so a COMPLETE
	// cache replays the same topics, the same dive ids, and zero
	// asks; a partial band adopts what exists and converges as
	// the rest answers.
	queueTerms();
	harvestTerms();
	stageMerge();
	harvestMerge();
	harvestFocus();
	queueDives(fresh);
	updateInfo();
	refreshKnowledge();
}

// A video seen outside the playlist joins the corpus in memory: a
// bare open founds an implicit playlist, later opens and drops
// extend whatever is loaded.  The topic file on disk is never
// touched -- export writes versions.
void MainWin::adoptVideo(QString const &video, QString const &srt)
{
	if (playlistIndex(video) >= 0)
		return;
	// Anchored before storing: a relative command-line path would
	// otherwise re-resolve against the topic file's directory
	// instead of the directory the caller meant.
	m_corpus.videos.push_back(
		{QFileInfo(video).absoluteFilePath().toStdString(),
		 srt.isEmpty() ? std::string()
		               : QFileInfo(srt).absoluteFilePath()
		                               .toStdString(),
		 {}});
	// An extension, not a replacement: in-progress dive scans keep
	// their cursors -- adoption only appends playlist entries.
	rebuildCorpus(false);
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
	if (!topics::adopt(m_corpus, pat))
		return;
	// Only the new pattern needs staging; a full restage would
	// throw away every in-progress scan on each committed search.
	stageDive(pat, true, false);
	if (m_diveAt < m_diveScans.size())
		m_diveTick.start();
	refreshKnowledge();
}

// Stage the corpus topic dives: one scan per exported grouping,
// chewed a video per tick.  Reopening a summarized corpus still
// scans (a few ms per cell, spread out); Facts drops the finished
// scan against its cache.
void MainWin::queueDives(bool fresh)
{
	if (fresh) {
		m_diveScans.clear();
		m_diveAt = 0;
	}
	for (topics::export_item const &e :
	     topics::export_plan(m_corpus)) {
		// Term topics dive too: their essays are what feed the
		// pairing layer, and pairs are where single spellings
		// crystallize into grouped regexes -- on a corpus with
		// no hand topics they are the only road there.  The
		// generated flag means "born from a focus" (those must
		// not re-pair into probe-of-probe loops); terms are
		// first-generation and pair like hand topics.
		bool const gen = m_generated.contains(e.name);
		stageDive(e.pattern, !gen,
		          gen && !m_termTopics.contains(e.name));
	}
	// The supportive layer: referenced topics dive too, a band
	// lower and unexported -- the nested regexes reveal semantic
	// structure inside the tops, and the queue can lean on it.
	for (topics::topic const *t : topics::components(m_corpus))
		stageDive(topics::expand(m_corpus, *t), false, false);
	if (m_diveAt < m_diveScans.size())
		m_diveTick.start();
	else
		m_diveTick.stop();
}

// Already-staged ids are left alone, finished or in flight: a merge
// restage must not reset a scan's progress.
void MainWin::stageDive(std::string const &pattern, bool exported,
                        bool generated)
{
	DiveScan s;
	s.re = QRegularExpression(QString::fromStdString(pattern));
	if (!s.re.isValid())
		return;
	s.id = diveId(pattern);
	for (DiveScan const &d : m_diveScans)
		if (d.id == s.id)
			return;
	s.pattern = pattern;
	s.exported = exported;
	s.generated = generated;
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
// section and the video's leaf a dependency.  The budget binds per
// append: an oversized catch is cut at a line boundary, and a video
// none of whose lines fit is dropped whole, section and dependency
// both -- the dive never cites a video it did not quote.
void MainWin::scanDiveVideo(DiveScan &s, PlayItem const &it)
{
	if (s.parts.size() >= kDiveBudget)
		return;
	std::size_t const room = kDiveBudget - s.parts.size();
	QString const srt = srtOf(it);
	std::string hits;
	for (QString const &line :
	     exporter::load(m_transcripts, srt).lines) {
		if (!s.re.match(line).hasMatch())
			continue;
		hits += line.toStdString();
		hits += '\n';
		// Enough for any final cut: past the room, the trim
		// below only ever shrinks -- no point holding more.
		if (hits.size() >= room)
			break;
	}
	if (hits.empty())
		return;
	agenda::id const key = agenda::id::from_hex(
		m_disc.id_for_video(srt.toStdString()));
	if (!key || std::ranges::find(s.deps, key) != s.deps.end())
		return;
	std::string head = "== ";
	head += QFileInfo(it.video).fileName().toStdString();
	head += '\n';
	if (head.size() >= room)
		return;
	if (hits.size() > room - head.size()) {
		std::size_t const cut =
			hits.rfind('\n', room - head.size() - 1);
		if (cut == std::string::npos)
			return;
		hits.resize(cut + 1);
	}
	s.deps.push_back(key);
	s.parts += head;
	s.parts += hits;
}

// A finished scan becomes a dive task: deps gate on the hit videos'
// leaf summaries, heat follows the same videos, and the pyramid
// root rides along as optional overview context.  The scan entry
// stays staged (its id blocks re-staging) but sheds its excerpts.
// A scan a pending focus record claims is that record's search, not
// a dive: it routes to finishProbe() instead.
void MainWin::finishDive(DiveScan &s)
{
	// A topic extended mid-scan supersedes this dive: asking or
	// pairing the pre-extension pattern would burn budget a warm
	// replay (which only ever sees the final pattern) never burns.
	if (m_diveRetired.contains(s.id.hex())) {
		s.parts.clear();
		s.parts.shrink_to_fit();
		return;
	}
	std::size_t const at = focusWorkOf(s.id);
	if (at < m_focusWork.size()) {
		if (!finishProbe(s, m_focusWork[at]))
			m_focusWork.erase(m_focusWork.begin()
			                  + std::ptrdiff_t(at));
	} else if (!s.deps.empty()) {
		agenda::task t;
		t.id = s.id;
		t.deps = s.deps;
		t.keys = s.deps;
		t.note = s.pattern;
		t.what = agenda::kind::dive;
		t.exported = s.exported;
		if (m_rootId)
			t.refs.push_back(m_rootId);
		m_facts.offer(std::move(t), s.parts);
		pairFocus(s);
	}
	s.parts.clear();
	s.parts.shrink_to_fit();
}

// The focus trigger: a finished first-generation dive pairs with
// every earlier one sharing a hit video.  A pair no longer asks for
// its focus outright: it opens with a probe -- what would you
// search? -- and the pump chains the search and the write behind
// it.  Generated dives never pair: recursion stops one hop past the
// hypothesis.
void MainWin::pairFocus(DiveScan const &s)
{
	if (s.generated)
		return;
	// Overlap-ranked, capped: at most kFocusFan partners per new
	// dive, the shared-hit-video count as the relatedness prior
	// and recency breaking ties.  Old dives may still accumulate
	// pairs as later ones pick them, so the total stays linear.
	struct pick {
		std::size_t at;
		std::size_t overlap;
	};
	std::vector<pick> best;
	for (std::size_t i = 0; i < m_dives.size(); ++i) {
		std::size_t const n = sharedKeys(m_dives[i].keys, s.deps);
		if (n)
			best.push_back({i, n});
	}
	std::ranges::sort(best, [](pick const &a, pick const &b) {
		return a.overlap != b.overlap ? a.overlap > b.overlap
		                              : a.at > b.at;
	});
	if (best.size() > kFocusFan)
		best.resize(kFocusFan);
	for (pick const &p : best)
		stageProbe(m_dives[p.at], s);
	// Copied before finishDive() sheds the scan's excerpts: the
	// record grounds the probes of future partners.
	m_dives.push_back({s.id, s.deps, s.pattern, s.parts});
}

// One pair's opening move.  An existing focus file ends the pair's
// story -- the one-shot era's artifacts included -- and a pending
// record means the story is already moving; otherwise the probe is
// staged (a cached reply asks nothing) and a record starts tracking
// the chain.  The ask carries a TRANSCRIPT sample of both sides'
// matched lines -- raw speech-to-text to ground the variant sweep.
// The probe depends on the two dive files, so "at least two dives"
// still falls out of dependency gating.
void MainWin::stageProbe(FinishedDive const &a, DiveScan const &b)
{
	agenda::id const fid = focusId(a.id, b.id);
	for (PendingFocus const &w : m_focusWork)
		if (w.focus == fid)
			return;
	agenda::task written;
	written.id = fid;
	written.deps = {a.id, b.id};
	written.what = agenda::kind::focus;
	// A written thread re-enters the corpus only through its own
	// pair -- this door -- so another corpus's focus files can
	// never bleed in.  cached() adopts a stale name en passant
	// when the dive chain is computable; on a cold start the dive
	// prose may still be pending, so bare existence via locate()
	// must also count -- otherwise a cached thread gets re-probed.
	if (m_facts.cached(written)
	    || !m_facts.locate(fid, agenda::kind::focus).empty()) {
		// An extension restage can revisit the pair: one pending
		// entry per file is plenty.
		if (std::ranges::find(m_focusPending, fid)
		    == m_focusPending.end())
			m_focusPending.push_back(fid);
		return;
	}
	PendingFocus w;
	w.probe = probeId(a.id, b.id, false);
	w.focus = fid;
	w.deps = {a.id, b.id};
	w.keys = a.keys;
	for (agenda::id const k : b.deps)
		if (std::ranges::find(w.keys, k) == w.keys.end())
			w.keys.push_back(k);
	w.note = a.pattern + " ~ " + b.pattern;
	w.raw = "TRANSCRIPT\n";
	w.raw += sampleOf(a.parts);
	w.raw += sampleOf(b.parts);
	m_facts.offer(probeTask(w, w.probe), w.raw);
	m_focusWork.push_back(std::move(w));
}

// The ask itself: probe and retry share everything but the id.
agenda::task MainWin::probeTask(PendingFocus const &w,
                                agenda::id ask) const
{
	agenda::task t;
	t.id = ask;
	t.deps = w.deps;
	t.keys = w.keys;
	t.note = w.note;
	t.what = agenda::kind::probe;
	t.exported = false;
	return t;
}

// The probe pump, the interactive half of a focus.  Each record
// waits on its ask's cache file: NONE retires the pair, a missing
// or broken REGEX line earns one corrected attempt with the failure
// as FEEDBACK, and a valid regex becomes a corpus search routed
// back to the record when it completes.  Cache files gate every
// step, so a chain interrupted by shutdown resumes where it stood.
void MainWin::pumpProbes()
{
	for (std::size_t i = 0; i < m_focusWork.size();) {
		if (pumpProbe(m_focusWork[i]))
			++i;
		else
			m_focusWork.erase(m_focusWork.begin()
			                  + std::ptrdiff_t(i));
	}
}

// False retires the record.
bool MainWin::pumpProbe(PendingFocus &w)
{
	if (w.scanning)
		return true;
	std::string const text = m_facts.fetch(
		probeTask(w, w.retry ? w.retry : w.probe));
	if (text.empty())
		return true;   // unanswered; waiting is free
	if (text.starts_with("NONE"))
		return false;
	// Tidied mechanically before anything consumes it: small models
	// repeat branches freely, and the searched, stored and
	// journaled pattern must not carry that.
	std::string const pat = topics::tidy(regexLine(text));
	if (pat.empty())
		return retryProbe(w,
			"FEEDBACK\nYour reply did not end with a REGEX: "
			"line. Reply with exactly one line of the form "
			"REGEX: <pattern>, or NONE.");
	QRegularExpression const re(QString::fromStdString(pat));
	if (!re.isValid())
		return retryProbe(w,
			"FEEDBACK\nYour regex\n  " + pat
			+ "\nis not valid PCRE2: "
			+ re.errorString().toStdString()
			+ ". Reply with a corrected REGEX: line, or NONE.");
	w.scanning = true;
	stageFocusScan(w.focus, pat, re);
	return true;
}

// The one corrected attempt: at temperature zero a bare re-ask is a
// re-run, so the retry exists only because FEEDBACK changes the
// prompt; the TRANSCRIPT sample rides along again so the evidence
// stays in view.  A second failure retires the pair -- false, like
// the pump's.
bool MainWin::retryProbe(PendingFocus &w, std::string const &feedback)
{
	if (w.retry)
		return false;
	w.retry = probeId(w.deps[0], w.deps[1], true);
	m_facts.offer(probeTask(w, w.retry), w.raw + "\n---\n" + feedback);
	return true;
}

// The probe's validated hypothesis becomes a corpus search staged
// under the write task's id; finishDive() routes it back through
// the pending record.  No m_diveScans dedupe here: the record's
// scanning latch is the guard, and a zero-match retry legitimately
// stages the same id again with a broader pattern.
void MainWin::stageFocusScan(agenda::id id, std::string const &pattern,
                             QRegularExpression const &re)
{
	DiveScan s;
	s.re = re;
	s.id = id;
	s.pattern = pattern;
	s.exported = false;
	s.generated = true;
	m_diveScans.push_back(std::move(s));
	m_diveTick.start();
}

// The searched evidence stages the write: the pair's dives ride as
// FIRST/SECOND deps, the excerpts as the snapshot, and the regex as
// the note the REGEX head and the journal carry.  An empty search
// is the probe's failure to answer for -- one broadening retry,
// then the pair retires.  False retires the record.
bool MainWin::finishProbe(DiveScan const &s, PendingFocus &w)
{
	if (s.parts.empty()) {
		w.scanning = false;
		return retryProbe(w,
			"FEEDBACK\nYour regex\n  " + s.pattern
			+ "\nis valid but matched nothing in the "
			"collection's subtitles. Broaden the variants: "
			"loosen separators and word joints, allow "
			"sound-alike respellings and optional inflections "
			"-- or reply NONE.");
	}
	agenda::task t;
	t.id = w.focus;
	t.deps = w.deps;
	t.keys = w.keys;
	t.note = s.pattern;
	t.what = agenda::kind::focus;
	t.exported = false;
	m_facts.offer(std::move(t), s.parts);
	// The write lands asynchronously; the pending list lets the
	// harvest tick pick it up once the file exists.
	m_focusPending.push_back(w.focus);
	return false;
}

std::size_t MainWin::focusWorkOf(agenda::id id) const
{
	for (std::size_t i = 0; i < m_focusWork.size(); ++i)
		if (m_focusWork[i].focus == id)
			return i;
	return m_focusWork.size();
}

// Harvest completed focuses: NONE verdicts and malformed regex
// lines are final; a valid REGEX line joins the corpus as a
// generated topic (focusN) and dives like any other, supportive.
// Runs on a slow tick and at every corpus rebuild.  Candidates come
// from the pending list the pair flow feeds -- never from a scan of
// the shared cache directory, whose files belong to every corpus
// ever studied: a thread re-enters exactly the corpus that
// re-derives its pair.
void MainWin::harvestFocus()
{
	// Terms before focus is the CALL order, per tick and at load:
	// every answered window has adopted by the time this runs.  A
	// complete cache thus still replays terms-then-focus exactly;
	// only a partial band lets a focus adopt against not-yet-
	// complete term subtractions, which beats adopting nothing.
	for (std::size_t i = 0; i < m_focusPending.size();) {
		agenda::id const id = m_focusPending[i];
		// resolve-by-id: adopts a stale name the moment the
		// pair's prose makes the chain computable, so the
		// journaled adoption lands in the session that owns it.
		std::string const p = m_facts.artifact(id);
		if (p.empty()) {             // not yet landed or ripe
			++i;
			continue;
		}
		if (m_harvested.insert(id.hex()).second)
			harvestOne(QString::fromStdString(p));
		m_focusPending.erase(m_focusPending.begin()
		                     + std::ptrdiff_t(i));
	}
	if (m_diveAt < m_diveScans.size())
		m_diveTick.start();
}

void MainWin::harvestOne(QString const &file)
{
	QFile f(file);
	if (!f.open(QIODevice::ReadOnly))
		return;
	std::string const text = f.readAll().toStdString();
	if (text.starts_with("NONE"))
		return;
	std::string pat;
	if (text.starts_with("REGEX:")) {
		// The interactive shape: a machine-written head names the
		// searched regex and prose follows -- unless the model saw
		// the evidence and still judged the thread hollow, which
		// buries the hypothesis with it.
		std::size_t nl = text.find('\n');
		if (nl == std::string::npos)
			nl = text.size();
		pat = regexPayload(text, 6, nl);
		std::size_t body = nl;
		while (body < text.size() && text[body] == '\n')
			++body;
		if (text.compare(body, 4, "NONE") == 0)
			return;
	} else {
		// The one-shot shape closed with the line instead.
		pat = regexLine(text);
	}
	if (pat.empty() || !QRegularExpression(
	                    	QString::fromStdString(pat)).isValid())
		return;
	// adopt_novel() is the gate: branches the corpus already covers
	// (user-authored topics included) are subtracted, and a regex
	// with nothing novel left adopts nothing -- the focus file's
	// prose remains; only the redundant topic and its dive are
	// declined.  What survives is the pattern from here on.
	std::string const kept = topics::adopt_novel(m_corpus, pat,
	                                             "focus");
	if (kept.empty())
		return;
	m_generated.insert(m_corpus.topics.back().name);
	stageDive(kept, false, true);
}

// The knowledge rows, rebuilt whole from where the state already
// lives: the corpus (topics with their dive artifacts), the focus
// cache (harvested threads), and the playlist (per-video
// summaries).  Cheap at session scale, so refresh is rebuild;
// called after corpus mutations and on the harvest tick.
void MainWin::refreshKnowledge()
{
	QVector<KnowledgeRow> rows;
	std::set<std::string> comp;
	for (topics::topic const *t : topics::components(m_corpus))
		comp.insert(t->name);
	QVector<KnowledgeRow> directory;
	for (topics::topic const &t : m_corpus.topics) {
		// Building blocks stay under the hood: a referenced topic
		// is machinery for the exported combinations, not a thing
		// to read -- listing a bare word-suffix regex as a
		// "topic" is noise, and searching it finds no coherent
		// subject.
		if (comp.contains(t.name))
			continue;
		// Adopted focus topics are machinery too: they exist so
		// subtraction and pairing see them, but their face is the
		// Focuses group -- a raw "focusN" label in Topics is
		// noise twice over.
		if (m_generated.contains(t.name)
		    && topics::stem_name(t.name, "focus"))
			continue;
		std::string const pat = topics::expand(m_corpus, t);
		agenda::id const did = diveId(pat);
		QString const path = QString::fromStdString(
			m_facts.locate(did, agenda::kind::dive));
		bool const cached = !path.isEmpty();
		QString const name = QString::fromStdString(t.name);
		TermInfo const info = m_termInfo.value(name);
		// Bar phases: the corpus scan (a unit per video), then the
		// essay ask.  Scans re-run each session; a scan behind the
		// cursor is complete, at it mid-flight, past it unstarted,
		// and a cleared list means they all finished.
		int const vids = int(m_playlist.size());
		int scanned = vids;
		for (std::size_t k = 0; k < m_diveScans.size(); ++k) {
			if (m_diveScans[k].id != did)
				continue;
			scanned = k < m_diveAt ? vids
			        : k == m_diveAt
			          ? int(m_diveScans[k].video) : 0;
			break;
		}
		QStringList words;
		if (!info.kind.isEmpty())
			words << info.kind;
		if (m_generated.contains(t.name))
			words << QStringLiteral("generated");
		words << (cached ? QStringLiteral("summary cached")
		                 : QStringLiteral("summary pending"));
		if (scanned < vids)
			words << QStringLiteral("searched %1/%2 videos")
			         .arg(scanned).arg(vids);
		QString gloss = info.gloss;
		// Sidecar entries key on the display title -- the human-
		// readable term for term topics, the name otherwise --
		// so "- etcd" survives any termN renumbering.
		std::string const key = info.term.isEmpty()
			? t.name : info.term.toStdString();
		for (topics::gloss_entry const &e : m_gloss)
			if (e.name == key) {
				if (!e.lines.empty())
					gloss = QString::fromStdString(
						e.lines.front());
				break;
			}
		directory.push_back({QStringLiteral("Topics"),
		                     info.term.isEmpty() ? name
		                                         : info.term,
		                     QString::fromStdString(pat),
		                     cached ? path : QString(),
		                     {}, {},
		                     words.join(QStringLiteral(" · ")),
		                     gloss.left(120), name,
		                     {scanned, cached ? 1 : 0},
		                     {vids, 1}});
	}
	// Alphabetical within the directory, the corpus name breaking
	// title ties into a total order (an unstable sort must not
	// churn duplicate-titled rows); other groups keep their
	// natural orders (files, playlist).
	std::ranges::sort(directory,
		[](KnowledgeRow const &a, KnowledgeRow const &b) {
			int const c = QString::compare(a.title, b.title,
			                               Qt::CaseInsensitive);
			return c ? c < 0 : a.name < b.name;
		});
	rows += directory;
	// Only threads harvested into THIS corpus list: the shared
	// cache directory holds every corpus's essays, and strangers
	// stay invisible.  Distinct probes can converge on one regex:
	// both essays stay visible, numbered apart past the first.
	QHash<QString, int> seen;
	for (std::string const &hex : m_harvested) {
		QString const path = QString::fromStdString(
			m_facts.locate(agenda::id::from_hex(hex),
			               agenda::kind::focus));
		if (path.isEmpty())
			continue;
		QFile f(path);
		if (!f.open(QIODevice::ReadOnly))
			continue;
		std::string const text = f.readAll().toStdString();
		if (text.starts_with("NONE"))
			continue;
		std::string pat;
		if (text.starts_with("REGEX:")) {
			std::size_t nl = text.find('\n');
			if (nl == std::string::npos)
				nl = text.size();
			pat = regexPayload(text, 6, nl);
		} else {
			pat = regexLine(text);
		}
		if (pat.empty())
			continue;
		QString const qpat = QString::fromStdString(pat);
		QString title = qpat;
		if (int const n = ++seen[qpat]; n > 1)
			title += QStringLiteral(" (%1)").arg(n);
		rows.push_back({QStringLiteral("Focuses"), title, qpat,
		                path, {}, {}, {}, {}, path, {}, {}});
	}
	// The same basename twice in the playlist: the parent
	// directory tells the rows apart.
	QHash<QString, int> bases;
	for (PlayItem const &it : m_playlist)
		++bases[QFileInfo(it.video).fileName()];
	// Per-video terms progress in one pass: staged windows against
	// the ones the harvest has actually seen answered.
	QHash<QString, QPair<int, int>> tw;
	for (TermsWork const &w : m_termsWork) {
		auto &[d, n] = tw[w.video];
		++n;
		d += m_termsSeen.contains(w.id.hex());
	}
	for (PlayItem const &it : m_playlist) {
		QString const srt = srtOf(it);
		QString path;
		if (!srt.isEmpty()) {
			agenda::id const leaf = agenda::id::from_hex(
				m_disc.id_for_video(srt.toStdString()));
			if (leaf)
				path = QString::fromStdString(
					m_facts.locate(leaf,
					               agenda::kind::leaf));
		}
		bool const cached = !path.isEmpty();
		QFileInfo const fi(it.video);
		QString title = fi.fileName();
		if (bases.value(title) > 1)
			title += QStringLiteral(" — ") + fi.dir().dirName();
		auto const [tdone, ttotal] = tw.value(it.video);
		QStringList words;
		words << (cached ? QStringLiteral("summary cached")
		                 : QStringLiteral("summary pending"));
		if (ttotal)
			words << QStringLiteral("glossary %1/%2")
			         .arg(tdone).arg(ttotal);
		rows.push_back({QStringLiteral("Videos"), title, {},
		                cached ? path : QString(),
		                it.video, it.srt,
		                words.join(QStringLiteral(" · ")),
		                {}, it.video,
		                {cached ? 1 : 0, tdone},
		                {1, ttotal}});
	}
	m_know.setRows(std::move(rows));
}

// Stage the terms windows: cue-boundary slices of each transcript,
// numbered with absolute cue indices and timestamps.  Offers dedupe
// against the plan and the cache; records re-derive every session
// so cached replies stay mappable to their cue ranges.
void MainWin::queueTerms()
{
	// Staged ids as a set, built once: a per-flush linear scan of
	// m_termsWork would go quadratic as the corpus grows.
	std::set<agenda::id> staged;
	for (TermsWork const &w : m_termsWork)
		staged.insert(w.id);
	for (PlayItem const &it : m_playlist) {
		QString const srt = srtOf(it);
		if (srt.isEmpty())
			continue;
		QString const sid = QString::fromStdString(
			m_disc.id_for_video(srt.toStdString()));
		if (sid.isEmpty())
			continue;
		exporter::transcript const &tx =
			exporter::load(m_transcripts, srt);
		dbgHop(QStringLiteral("terms: window %1 sid=%2 lines=%3")
		       .arg(srt, sid).arg(tx.lines.size()));
		QByteArray text;
		int first = 0;
		auto const flush = [&](int last) {
			if (text.isEmpty())
				return;
			agenda::id const id = termsId(sid, first, last,
			                              text);
			if (staged.insert(id).second) {
				agenda::task t;
				t.id = id;
				t.keys = {agenda::id::from_hex(
					sid.toStdString())};
				t.note = QFileInfo(it.video).fileName()
				         	.toStdString()
				       + " #" + std::to_string(first)
				       + "-" + std::to_string(last);
				t.what = agenda::kind::terms;
				t.exported = false;
				m_facts.offer(std::move(t),
				              text.toStdString());
				m_termsWork.push_back({id, it.video, srt,
				                       first, last});
			}
			text.clear();
		};
		for (qsizetype i = 0; i < tx.lines.size(); ++i) {
			if (text.isEmpty())
				first = int(i);
			text += '#';
			text += QByteArray::number(i);
			text += " [";
			text += fmtTime(tx.cues[std::size_t(i)].start,
			                true).toUtf8();
			text += "] ";
			text += tx.lines[i].toUtf8();
			text += '\n';
			if (text.size() >= kTermsWindow)
				flush(int(i));
		}
		flush(int(tx.lines.size()) - 1);
	}
}

// Adoption in staging order, strictly: the walk stops at the first
// window whose reply is still missing, so every session's corpus is
// a prefix-replay of the same order however the answers arrive.  A
// parked window starves later adoption until the next session
// re-asks it -- determinism bought with latency.
void MainWin::harvestTerms()
{
	// Staging order, gaps skipped: the agenda answers windows in
	// heat order, so waiting for a strict prefix starves adoption
	// behind whichever window the scheduler felt like deferring --
	// with a large corpus that meant a full band of finished
	// replies and zero visible knowledge.  A skipped window adopts
	// on a later tick or session; until the band completes,
	// machine topic names may shift between sessions, and settle
	// once it has.
	for (TermsWork const &w : m_termsWork) {
		if (m_termsSeen.contains(w.id.hex()))
			continue;
		if (harvestTermsOne(w))
			m_termsSeen.insert(w.id.hex());
	}
}

// Parse, validate and adopt one terms reply.  The gate is
// mechanical: every cited cue must lie inside the window and every
// SEEN spelling must occur on a cited line -- an entry failing
// either drops whole, never kept diluted.  Survivors' spellings
// are escaped literals joined into a case-folded union (the model
// never writes regex here), then pass the same tidy/subtract gate
// as every machine pattern -- one term, one topic: a known term
// grows its owner's alternation, a fully covered one re-attaches
// its directory entry to the covering term topic, and only a novel
// term adopts a new termN.  The gloss waits as a proposal until a
// human copies it into the sidecar.
bool MainWin::harvestTermsOne(TermsWork const &w)
{
	std::string const path = m_facts.locate(w.id,
	                                        agenda::kind::terms);
	if (path.empty())
		return false;   // unanswered: QFile("") would gripe
		                // ("No file name specified") every tick
	QFile f(QString::fromStdString(path));
	if (!f.open(QIODevice::ReadOnly))
		return false;
	QString const text = QString::fromUtf8(f.readAll());
	if (text.startsWith(QStringLiteral("NONE")))
		return true;
	exporter::transcript const &tx =
		exporter::load(m_transcripts, w.srt);
	auto const line = [&](int cue) -> QString const * {
		return cue >= w.first && cue <= w.last
		       && cue < int(tx.lines.size())
		       ? &tx.lines[cue] : nullptr;
	};
	for (QString const &block :
	     text.split(QStringLiteral("\n\n"), Qt::SkipEmptyParts)) {
		QString term, kind, gloss, means;
		QStringList seen;
		QList<int> cues;
		for (QString const &l : block.split(QLatin1Char('\n'))) {
			if (l.startsWith(QStringLiteral("TERM:")))
				term = l.mid(5).trimmed();
			else if (l.startsWith(QStringLiteral("KIND:")))
				kind = l.mid(5).trimmed().toLower();
			else if (l.startsWith(QStringLiteral("MEANS:")))
				means = l.mid(6).trimmed();
			else if (l.startsWith(QStringLiteral("GLOSS:")))
				gloss = l.mid(6).trimmed();
			else if (l.startsWith(QStringLiteral("SEEN:"))) {
				for (QString const &v : l.mid(5)
				     .split(QLatin1Char('|')))
					if (QString const t = v.trimmed();
					    !t.isEmpty())
						seen << t;
			} else if (l.startsWith(QStringLiteral("CUES:"))) {
				for (QString const &c : l.mid(5)
				     .split(QLatin1Char(' '),
				            Qt::SkipEmptyParts)) {
					bool ok = false;
					int const n = QStringView(c)
						.sliced(c.startsWith(
							QLatin1Char('#')))
						.toInt(&ok);
					if (ok)
						cues << n;
				}
			}
		}
		if (term.isEmpty() || seen.isEmpty() || gloss.isEmpty()
		    || cues.isEmpty())
			continue;
		if (!std::ranges::all_of(cues, [&](int c) {
				return line(c) != nullptr;
			}))
			continue;
		QStringList kept;
		for (QString const &v : seen) {
			bool const found = std::ranges::any_of(cues,
				[&](int c) {
					return line(c)->contains(v,
						Qt::CaseInsensitive);
				});
			if (found)
				kept << v;
		}
		if (kept.isEmpty())
			continue;
		QString pat = QStringLiteral("(?i:");
		for (qsizetype i = 0; i < kept.size(); ++i) {
			if (i)
				pat += QLatin1Char('|');
			pat += QRegularExpression::escape(kept[i]);
		}
		pat += QLatin1Char(')');
		std::string const tidied =
			topics::tidy(pat.toStdString());
		// The row's title already IS the term: the gloss text
		// never repeats it, an acronym's expansion just leads.
		// Small models parrot MEANS: <term> verbatim -- an
		// expansion that only restates the term is no expansion.
		QString const shown = means.isEmpty()
		    || !QString::compare(means, term, Qt::CaseInsensitive)
			? gloss
			: means + QStringLiteral(". ") + gloss;
		QString const folded = term.toCaseFolded();
		// One term, one topic: an entry whose term is known, or
		// whose branches overlap an existing term topic at all
		// (cover_of), grows that owner instead of founding a
		// twin from the leftover.  Kind, casing and gloss keep
		// the first non-empty word; a bare re-cover of a fresh
		// owner is the reload re-attach.
		QString own = m_termIndex.value(folded);
		if (own.isEmpty())
			own = QString::fromStdString(
				topics::cover_of(m_corpus, tidied,
				                 "term"));
		if (!own.isEmpty()) {
			bool const fresh = !m_termInfo.contains(own);
			std::string const before =
				expandOf(own.toStdString());
			std::string const grown = topics::extend(
				m_corpus, own.toStdString(), tidied);
			TermInfo &info = m_termInfo[own];
			if (info.term.isEmpty())
				info.term = term;
			if (info.kind.isEmpty())
				info.kind = kind;
			if (info.gloss.isEmpty())
				info.gloss = shown;
			if (!m_termIndex.contains(folded))
				m_termIndex.insert(folded, own);
			indexSpellings(kept, own);
			if (!grown.empty()) {
				dbgHop(QStringLiteral(
					"terms: extended %1 [%2]")
				       .arg(own,
				            QString::fromStdString(grown)));
				retireDive(diveId(before));
				stageTopic(own.toStdString());
			} else if (fresh) {
				dbgHop(QStringLiteral(
					"terms: attached %1 [%2]")
				       .arg(own, term));
			}
			continue;
		}
		std::string const adopted = topics::adopt_novel(
			m_corpus, tidied, "term");
		if (adopted.empty())
			continue;    // hand-covered whole: no twin, no title
		QString const name = QString::fromStdString(
			m_corpus.topics.back().name);
		m_generated.insert(name.toStdString());
		m_termTopics.insert(name.toStdString());
		m_termInfo.insert(name, {term, kind, shown});
		m_termIndex.insert(folded, name);
		indexSpellings(kept, name);
		dbgHop(QStringLiteral("terms: adopted %1 [%2]")
		       .arg(name, QString::fromStdString(adopted)));
		stageTopic(m_corpus.topics.back().name);
	}
	return true;
}

// The expanded pattern of a named topic; empty when the name is
// not currently a topic.
std::string MainWin::expandOf(std::string const &name) const
{
	for (topics::topic const &tp : m_corpus.topics)
		if (tp.name == name)
			return topics::expand(m_corpus, tp);
	return {};
}

// A superseded dive neither records nor pairs nor asks -- and any
// probe chain already staged on it stops before concluding a focus
// from the stale pattern: budget a warm replay never burns.
void MainWin::retireDive(agenda::id id)
{
	m_diveRetired.insert(id.hex());
	std::erase_if(m_dives, [&id](FinishedDive const &d) {
		return d.id == id;
	});
	std::erase_if(m_focusWork, [&id](PendingFocus const &w) {
		return std::ranges::find(w.deps, id) != w.deps.end();
	});
}

// Mid-session adoptions dive immediately: queueDives runs only at
// load, and an unstaged topic would idle a session.
void MainWin::stageTopic(std::string const &name)
{
	if (std::string const pat = expandOf(name); !pat.empty())
		stageDive(pat, false, false);
}

// Every validated spelling joins the index, first owner wins: a
// later window proposing a known VARIANT as its term ("TERM:
// gidger" after gidger was seen under ghidra) must grow the owner,
// not mint a titled twin.
void MainWin::indexSpellings(QStringList const &seen,
                             QString const &owner)
{
	for (QString const &v : seen)
		if (QString const k = v.toCaseFolded();
		    !m_termIndex.contains(k))
			m_termIndex.insert(k, owner);
}

// The directory fold ask: the id keys on the folded, sorted term
// list, so a changed directory stages a fresh judgment and a
// stable one re-resolves its cached reply.
void MainWin::stageMerge()
{
	QStringList terms;
	for (TermInfo const &i : m_termInfo)
		if (!i.term.isEmpty())
			terms << i.term;
	if (terms.size() < 2)
		return;
	std::ranges::sort(terms,
		[](QString const &a, QString const &b) {
			return a.toCaseFolded() < b.toCaseFolded();
		});
	QByteArray text;
	for (QString const &t : terms) {
		text += t.toUtf8();
		text += '\n';
	}
	QCryptographicHash h(QCryptographicHash::Blake2b_256);
	h.addData(QByteArrayView("merge\n"));
	h.addData(text);
	agenda::id const id = takeId(h);
	if (id == m_mergeId)
		return;
	m_mergeId = id;
	m_mergeSet.clear();
	for (QString const &t : terms)
		m_mergeSet.insert(t.toCaseFolded(), t);
	agenda::task t;
	t.id = id;
	t.note = std::to_string(terms.size()) + " terms";
	t.what = agenda::kind::merge;
	t.exported = false;
	m_facts.offer(std::move(t), text.toStdString());
}

// Fold judgments arrive as MERGE lines over the current directory.
// Folding shrinks the directory, which re-keys the next stageMerge
// -- the cascade converges on a NONE and stops.  Replays are
// idempotent: a folded twin no longer resolves.
void MainWin::harvestMerge()
{
	if (!m_mergeId || m_mergeSeen.contains(m_mergeId.hex()))
		return;
	std::string const path = m_facts.locate(m_mergeId,
	                                        agenda::kind::merge);
	if (path.empty())
		return;
	QFile f(QString::fromStdString(path));
	if (!f.open(QIODevice::ReadOnly))
		return;
	m_mergeSeen.insert(m_mergeId.hex());
	QString const text = QString::fromUtf8(f.readAll());
	if (text.startsWith(QStringLiteral("NONE")))
		return;
	for (QString const &l : text.split(QLatin1Char('\n')))
		foldLine(l);
}

// One judgment line.  MERGE: the first listed name is the
// corrected spelling and takes the title; any member already in
// the index anchors the group it folds into.  DROP: the named
// everyday-vocabulary term leaves the directory wholesale.
void MainWin::foldLine(QString const &line)
{
	// The prompt demands names copied exactly from the staged
	// list, so enforcement is exact membership: a hallucinated
	// name -- which m_termIndex might still resolve through a
	// SEEN alias -- rejects the whole line.
	auto const staged = [this](QString const &t) {
		return m_mergeSet.contains(t.toCaseFolded());
	};
	if (line.startsWith(QStringLiteral("DROP:"))) {
		QString const t = line.mid(5).trimmed();
		if (!staged(t)) {
			dbgHop(QStringLiteral(
				"terms: judgment rejected [%1]").arg(t));
			return;
		}
		QString const name = m_termIndex.value(t.toCaseFolded());
		if (name.isEmpty())
			return;
		dropTopic(name);
		dbgHop(QStringLiteral("terms: dropped %1 [%2]")
		       .arg(name, t));
		return;
	}
	if (!line.startsWith(QStringLiteral("MERGE:")))
		return;
	QStringList parts;
	for (QString const &p : line.mid(6).split(QLatin1Char('|')))
		if (QString const t = p.trimmed(); !t.isEmpty())
			parts << t;
	if (parts.size() < 2)
		return;
	for (QString const &p : parts)
		if (!staged(p)) {
			dbgHop(QStringLiteral(
				"terms: judgment rejected [%1]").arg(p));
			return;
		}
	QString owner;
	for (QString const &p : parts) {
		owner = m_termIndex.value(p.toCaseFolded());
		if (!owner.isEmpty())
			break;
	}
	if (owner.isEmpty())
		return;
	for (QString const &p : parts)
		mergeSpelling(owner, p);
	// The model picks WHICH staged term leads; the staged spelling
	// itself titles the group -- a small model's lowercased echo
	// must not degrade the extraction's casing.
	m_termInfo[owner].term =
		m_mergeSet.value(parts.front().toCaseFolded());
}

// Remove one machine topic wholesale: corpus entry, directory
// entry, every index spelling, its dive.  Cached extraction
// replies re-mint it next session and the cached judgment drops it
// again -- deterministic and invisible.
void MainWin::dropTopic(QString const &name)
{
	std::string const victim = name.toStdString();
	std::string const pat = expandOf(victim);
	if (pat.empty())
		return;
	// A topic other topics reference is load-bearing structure:
	// erasing it would dangle their fragments.  Never a victim.
	for (topics::topic const *r : topics::components(m_corpus))
		if (r->name == victim)
			return;
	std::erase_if(m_corpus.topics,
		[&victim](topics::topic const &tp) {
			return tp.name == victim;
		});
	m_termTopics.erase(victim);
	m_generated.erase(victim);
	m_termInfo.remove(name);
	m_termIndex.removeIf([&name](auto it) {
		return it.value() == name;
	});
	retireDive(diveId(pat));
}

// Fold the topic owning one spelling into the group owner: its
// branches join the owner's alternation, the twin topic leaves the
// corpus, and every index entry follows.
void MainWin::mergeSpelling(QString const &owner,
                            QString const &spell)
{
	QString const k = spell.toCaseFolded();
	QString const name = m_termIndex.value(k);
	if (name.isEmpty()) {
		m_termIndex.insert(k, owner);
		return;
	}
	if (name == owner)
		return;
	std::string const victim = name.toStdString();
	std::string const vpat = expandOf(victim);
	std::string const opat = expandOf(owner.toStdString());
	if (vpat.empty() || opat.empty())
		return;
	// Referenced topics are structure, not spellings: folding one
	// away would dangle the fragments that name it.
	for (topics::topic const *r : topics::components(m_corpus))
		if (r->name == victim)
			return;
	// The victim leaves the corpus BEFORE the extend subtracts,
	// or it would cover its own branches and refuse the fold.
	std::erase_if(m_corpus.topics,
		[&victim](topics::topic const &tp) {
			return tp.name == victim;
		});
	std::string const grown = topics::extend(
		m_corpus, owner.toStdString(), vpat);
	m_termTopics.erase(victim);
	m_generated.erase(victim);
	TermInfo const gone = m_termInfo.take(name);
	TermInfo &info = m_termInfo[owner];
	if (info.kind.isEmpty())
		info.kind = gone.kind;
	if (info.gloss.isEmpty())
		info.gloss = gone.gloss;
	for (QString &v : m_termIndex)
		if (v == name)
			v = owner;
	retireDive(diveId(vpat));
	if (!grown.empty()) {
		retireDive(diveId(opat));
		stageTopic(owner.toStdString());
	}
	dbgHop(QStringLiteral("terms: merged %1 <- %2 [%3]")
	       .arg(owner, name, spell));
}

// The gloss sidecar sits beside the corpus file; an implicit corpus
// has none.
QString MainWin::glossPath() const
{
	if (m_corpusPath.isEmpty())
		return {};
	QFileInfo const fi(m_corpusPath);
	return fi.absolutePath() + QLatin1Char('/')
	     + fi.completeBaseName() + QStringLiteral(".gloss");
}

void MainWin::loadGloss()
{
	m_gloss.clear();
	QFile f(glossPath());
	if (!glossPath().isEmpty() && f.open(QIODevice::ReadOnly))
		m_gloss = topics::parse_gloss(
			f.readAll().toStdString());
}

// Show the selected topic's gloss.  Only topic rows carry glosses.
// Two keys per row: the display title keys the human sidecar
// ("- etcd", renumber-proof), the corpus name keys m_termInfo (a
// term topic's title is the term, never its termN name).
void MainWin::showGloss(QTreeWidgetItem const *item)
{
	QString name, topic;
	if (item && item->parent()
	    && item->parent()->text(0) == QStringLiteral("Topics")) {
		name = item->text(0);
		topic = item->data(0, KnowledgePane::kName).toString();
	}
	QString text;
	std::string const key = name.toStdString();
	for (topics::gloss_entry const &e : m_gloss) {
		if (e.name != key)
			continue;
		for (std::string const &l : e.lines) {
			if (!text.isEmpty())
				text += QLatin1Char('\n');
			text += QString::fromStdString(l);
		}
		break;
	}
	// The machine's gloss fills the void; the sidecar -- external,
	// human-owned -- always wins once an entry exists.
	if (text.isEmpty())
		text = m_termInfo.value(topic).gloss;
	m_know.setGloss(text);
}

// Occurrences of the selected pattern, computed over the shared
// transcript cache exactly like the tally: the scan reads every cue
// of every video, so the index is never truncated.  The per-video
// display cap trims presentation only.
void MainWin::knowledgeSelected(QTreeWidgetItem const *item)
{
	showGloss(item);
	QVector<KnowledgeHit> hits;
	QHash<QString, int> counts;
	QString const pat = item
		? item->data(0, KnowledgePane::kPattern).toString()
		: QString();
	QRegularExpression const re(pat);
	if (!pat.isEmpty() && re.isValid()) {
		for (PlayItem const &it : m_playlist) {
			QString const srt = srtOf(it);
			if (srt.isEmpty())
				continue;
			exporter::transcript const &tx =
				exporter::load(m_transcripts, srt);
			int kept = 0, total = 0;
			for (qsizetype i = 0; i < tx.lines.size();
			     ++i) {
				if (!re.match(tx.lines[i]).hasMatch())
					continue;
				++total;
				if (kept >= kMatchCap)
					continue;
				hits.push_back({it.video, srt,
				                tx.lines[i],
				                tx.cues[std::size_t(i)]
				                	.start,
				                int(i)});
				++kept;
			}
			if (total)
				counts.insert(it.video, total);
		}
	}
	m_know.setMatches(std::move(hits), counts);
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
	// A corpus swap orphans any export continuation: grabsIdle()
	// must not re-run the writer against a corpus the export never
	// saw.
	m_exportPending = false;
	m_exportQueued = -1;
	loadGloss();
	rebuildCorpus(true);
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
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		fail(QStringLiteral("%1: %2").arg(target,
		                                  f.errorString()));
		return;
	}
	std::string const text = topics::write(m_corpus);
	if (f.write(text.data(), qint64(text.size()))
	    != qint64(text.size()))
		fail(QStringLiteral("%1: %2").arg(target,
		                                  f.errorString()));
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
		// Options lead and a separator follows: appended to the
		// pattern they could alias ("a"+12 vs "a1"+2).
		QString const key = QString::number(int(re.patternOptions()))
		                  + QLatin1Char('\n') + re.pattern();
		if (key != m_tallyKey) {
			m_tallyKey = key;
			m_tallyTotal = -1;
			m_tallyLag.start();
		}
	}

	QStringList parts;
	qsizetype const at = indexOfId(m_trail.videoId());
	if (at >= 0)
		parts << QStringLiteral("video %1/%2%3")
			.arg(at + 1).arg(m_playlist.size())
			.arg(m_view.cueCount() > 0 && m_link.lastPause()
			     ? QStringLiteral(" (paused)") : QString());
	if (double const t = m_link.lastTime(); t >= 0.0)
		parts << fmtTime(t, false);
	if (QString const m = matchInfo(at); !m.isEmpty())
		parts << m;
	QString const text = parts.join(QStringLiteral("  ·  "));
	if (text != m_info.text())
		m_info.setText(text);
	// The regex reads from the left and elides into its own label,
	// which the stretch hands exactly the width the fixed parts
	// leave free; floored so a crowded footer still shows a useful
	// head.
	QString const pat = m_search.patternText();
	QString const shown = pat.isEmpty() ? QString()
		: m_pattern.fontMetrics().elidedText(
			pat, Qt::ElideRight,
			std::max(160, m_pattern.width()));
	if (shown != m_pattern.text())
		m_pattern.setText(shown);
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
	// The domain is whatever the pointer hovers -- zoom acts on
	// what the eye is on, never on which widget last took focus.
	// Anywhere else (knowledge pane, chrome, outside) is the base
	// domain, which scales everything at once.
	if (m_bar.isVisible() && m_bar.editHovered())
		return ZoomDom::regex;
	if (m_bar.isVisible() && m_bar.underMouse())
		return ZoomDom::bar;
	if (m_view.underMouse())
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
		m_pattern.setFont(base);
		m_info.setFont(base);
		m_state.setFont(base);
		// The knowledge pane belongs to the base domain like the
		// rest of the chrome, and scales the same way -- child by
		// child: a font set on the dock alone loses to the theme's
		// per-class fonts before it reaches any child.
		m_know.setUiFont(base);
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
	m_state.setStyleSheet(QString());
	m_state.setText(s);
}

// Errors wear red so they cannot pass for the ordinary murmur; any
// later plain state clears the paint.
void MainWin::errState(QString const &s)
{
	m_state.setStyleSheet(QStringLiteral("color:#c22222;"));
	m_state.setText(s);
}
