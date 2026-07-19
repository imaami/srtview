#include "mainwin.hpp"

#include "discovery.hpp"
#include "palettefix.hpp"
#include "srt.hpp"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QContextMenuEvent>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QStatusBar>

MainWin::MainWin()
	: m_view(&m_playback, &m_search, this)
	, m_bar(&m_search, &m_view)
	, m_link(&m_playback)
	, m_playback(m_link, m_view, *statusBar(), m_trail)
	, m_search(m_bar, m_view, *statusBar(), m_prefs, m_trail)
{
	setCentralWidget(&m_view);
	setAcceptDrops(true);
	resize(920, 720);
	setWindowTitle(QStringLiteral("srtview"));

	// --- menus ---
	auto *file = menuBar()->addMenu(QStringLiteral("&File"));
	file->addAction(QStringLiteral("&Open\u2026"), QKeySequence::Open,
	                this, [this] { openDialog(m_prefs.lastDir()); });
	m_recentMenu = file->addMenu(QStringLiteral("Open &recent"));
	m_recentMenu->installEventFilter(this);
	connect(m_recentMenu, &QMenu::aboutToShow,
	        this, [this] { rebuildRecentMenu(); });
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

	auto *search = menuBar()->addMenu(QStringLiteral("&Search"));
	search->addAction(QStringLiteral("&Find\u2026"), QKeySequence::Find,
	                  this, [this] { m_search.showSearch(); });
	search->addAction(&m_search.nextAction());
	search->addAction(&m_search.prevAction());

	// --- status bar ---
	statusBar()->addPermanentWidget(&m_state);
	setState(QStringLiteral("no file"));

	repairMenuPalette(menuBar());
}

bool MainWin::openPath(QString const &path)
{
	QString err, video = path, srt;
	if (path.endsWith(QStringLiteral(".srt"), Qt::CaseInsensitive)) {
		srt   = path;
		video = videoForSrt(path, &err);
		if (video.isEmpty())
			return fail(err);
	} else {
		srt = srtForVideo(video, &err);
		if (srt.isEmpty())
			return fail(err);
	}
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

	if (!m_link.openFor(video, srt, &err))
		return fail(err);

	m_prefs.addRecentFile(video);
	m_prefs.setLastDir(QFileInfo(video).absolutePath());

	auto const n = cues.size();
	m_view.setCues(std::move(cues));
	m_search.searchChanged();
	setWindowTitle(QStringLiteral("%1 \u2014 srtview")
	               .arg(QFileInfo(video).fileName()));
	setState(QStringLiteral("%1 cues \u00b7 mpv %2")
	         .arg(n)
	         .arg(m_link.spawned() ? QStringLiteral("spawned")
	                               : QStringLiteral("reused")));
	m_view.setFocus();
	return true;
}

// Fundo: walk the undo tree.  Undo climbs down toward the past
// applying each step's before-state; redo climbs back up the branch
// last grown or adopted, applying after-states.  Side branches
// persist; retracing an identical action adopts its old branch.
// Application runs with recording suppressed.
void MainWin::undoStep()
{
	std::optional<trail_step> const s = m_trail.undo();
	if (!s) {
		statusBar()->showMessage(QStringLiteral("nothing to undo"),
		                         1500);
		return;
	}
	m_trail.setApplying(true);
	switch (s->k) {
	case trail_step::search_text:
		m_search.applyPattern(s->textBefore);
		statusBar()->showMessage(QStringLiteral("undo \u2192 search "
			"\"%1\"").arg(s->textBefore), 2000);
		break;
	case trail_step::search_jump:
		m_search.applyCursor(s->curBefore);
		break;
	case trail_step::video_jump:
	case trail_step::side_seek:
		if (!m_playback.applyTime(s->timeBefore)) {
			m_trail.redo();  // playback never moved: stay put
			break;
		}
		statusBar()->showMessage(QStringLiteral("undo \u2192 %1")
			.arg(fmtTime(s->timeBefore, true)), 2000);
		break;
	}
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
	switch (s->k) {
	case trail_step::search_text:
		m_search.applyPattern(s->textAfter);
		statusBar()->showMessage(QStringLiteral("redo \u2192 search "
			"\"%1\"").arg(s->textAfter), 2000);
		break;
	case trail_step::search_jump:
		m_search.applyCursor(s->curAfter);
		break;
	case trail_step::video_jump:
	case trail_step::side_seek:
		if (!m_playback.applyTime(s->timeAfter)) {
			m_trail.undo();  // playback never moved: stay put
			break;
		}
		statusBar()->showMessage(QStringLiteral("redo \u2192 %1")
			.arg(fmtTime(s->timeAfter, true)), 2000);
		break;
	}
	m_trail.setApplying(false);
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
	QString const p = md->urls().first().toLocalFile();
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
		QAction *act = m_recentMenu->addAction(
			QFileInfo(path).fileName());
		act->setToolTip(path);
		act->setData(path);
		connect(act, &QAction::triggered,
		        this, [this, path] { openPath(path); });
	}
}

// Right-click on a recent entry: offer to open the file dialog in
// that entry's directory, for revisiting previously used places.
bool MainWin::eventFilter(QObject *obj, QEvent *ev)
{
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
