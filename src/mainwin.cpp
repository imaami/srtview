#include "mainwin.hpp"

#include "discovery.hpp"
#include "palettefix.hpp"
#include "srt.hpp"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QStatusBar>

MainWin::MainWin()
	: m_view(&m_playback, &m_search, this)
	, m_bar(&m_search, &m_view)
	, m_link(&m_playback)
	, m_playback(m_link, m_view, *statusBar())
	, m_search(m_bar, m_view, *statusBar())
{
	setCentralWidget(&m_view);
	setAcceptDrops(true);
	resize(920, 720);
	setWindowTitle(QStringLiteral("srtview"));

	// --- menus ---
	auto *file = menuBar()->addMenu(QStringLiteral("&File"));
	file->addAction(QStringLiteral("&Open\u2026"), QKeySequence::Open,
	                this, [this] { openDialog(); });
	file->addAction(QStringLiteral("&Close"), QKeySequence::Close,
	                this, [this] { closeFile(); });
	file->addSeparator();
	file->addAction(QStringLiteral("&Quit"), QKeySequence::Quit,
	                this, [this] { close(); });

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

void MainWin::openDialog()
{
	QString const p = QFileDialog::getOpenFileName(this,
		QStringLiteral("Open video or subtitle"), QString(),
		QStringLiteral("Video / SRT (*.mp4 *.mkv *.webm *.avi *.mov "
		               "*.m4v *.mpg *.mpeg *.ts *.wmv *.srt);;"
		               "All files (*)"));
	if (!p.isEmpty())
		openPath(p);
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
