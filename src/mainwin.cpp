#include "mainwin.hpp"

#include "discovery.hpp"
#include "palettefix.hpp"
#include "srt.hpp"
#include "timefmt.hpp"

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
	: m_edit(this), m_searchBar(this, this), m_mpv(this)
{
	setCentralWidget(&m_edit);
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
	              this, [this] { togglePause(); });
	pb->addAction(QStringLiteral("Back 5 s\t(\u2190)"),
	              this, [this] { seekRel(-5.0); });
	pb->addAction(QStringLiteral("Forward 5 s\t(\u2192)"),
	              this, [this] { seekRel(5.0); });
	pb->addSeparator();
	pb->addAction(QStringLiteral("Seek to cursor cue\t(Return, "
	                             "double-click)"),
	              this, [this] { seekCue(m_edit.currentCue(), false); });
	pb->addAction(QStringLiteral("Seek + pause\t(T)"),
	              this, [this] { seekCue(m_edit.currentCue(), true); });
	pb->addSeparator();
	m_followAct.setText(QStringLiteral("&Follow playback\t(f)"));
	m_followAct.setCheckable(true);
	m_followAct.setChecked(true);
	connect(&m_followAct, &QAction::toggled,
	        this, [this](bool on) { m_edit.setFollow(on); });
	pb->addAction(&m_followAct);

	auto *search = menuBar()->addMenu(QStringLiteral("&Search"));
	search->addAction(QStringLiteral("&Find\u2026"), QKeySequence::Find,
	                  this, [this] { showSearch(); });
	m_nextAct.setText(QStringLiteral("Find &next"));
	m_nextAct.setShortcut(QKeySequence(Qt::Key_F3));
	connect(&m_nextAct, &QAction::triggered,
	        this, [this] { findAgain(false); });
	m_prevAct.setText(QStringLiteral("Find &previous"));
	m_prevAct.setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F3));
	connect(&m_prevAct, &QAction::triggered,
	        this, [this] { findAgain(true); });
	search->addAction(&m_nextAct);
	search->addAction(&m_prevAct);

	// --- status bar ---
	statusBar()->addPermanentWidget(&m_state);
	setState(QStringLiteral("no file"));

	repairMenuPalette(menuBar());
}

bool MainWin::openPath(const QString &path)
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
	const QByteArray raw = srtFile.readAll();
	std::vector<srt::cue> cues = srt::parse(srt::to_utf8(
		{raw.constData(), size_t(raw.size())}));
	if (cues.empty())
		return fail(QStringLiteral("%1: no cues found (not an SRT "
		                           "file?)").arg(srt));

	if (!m_mpv.openFor(video, srt, &err))
		return fail(err);

	const auto n = cues.size();
	m_edit.setCues(std::move(cues));
	searchChanged();
	setWindowTitle(QStringLiteral("%1 \u2014 srtview")
	               .arg(QFileInfo(video).fileName()));
	setState(QStringLiteral("%1 cues \u00b7 mpv %2")
	         .arg(n)
	         .arg(m_mpv.spawned() ? QStringLiteral("spawned")
	                              : QStringLiteral("reused")));
	m_edit.setFocus();
	return true;
}

void MainWin::showSearch()
{
	m_searchAnchor = m_edit.textCursor();
	// Size first: the target x depends on the bar's final width,
	// which before the first layout pass is garbage.
	m_searchBar.adjustSize();
	m_searchBar.open(searchBarTarget());
}

// Enter in the search field: the incremental jump has already landed
// on the match, so accept and get out of the way -- the next
// keystroke (t, Space, ...) belongs to the view.
void MainWin::commitSearch()
{
	hideSearch();
}

void MainWin::hideSearch()
{
	m_searchBar.dismiss();
	m_edit.setFocus();
}

void MainWin::layoutOverlays()
{
	m_searchBar.reposition(searchBarTarget());
}

// Pattern edited: refresh highlights and jump to the first match at
// or after where the search began.
void MainWin::searchChanged()
{
	highlightAll();
	if (m_matchStarts.empty() || m_searchBar.pattern().isEmpty())
		return;
	QTextCursor from = m_searchAnchor.isNull()
		? QTextCursor(m_edit.document()) : m_searchAnchor;
	QTextCursor hit = m_edit.document()->find(pattern(), from);
	if (hit.isNull())
		hit = m_edit.document()->find(pattern(),
		                              QTextCursor(m_edit.document()));
	if (!hit.isNull()) {
		m_edit.setTextCursor(hit);
		updateCounter(hit);
	}
}

void MainWin::findAgain(bool backward)
{
	const QRegularExpression re = pattern();
	if (!re.isValid() || re.pattern().isEmpty())
		return;
	QTextDocument::FindFlags fl;
	if (backward)
		fl |= QTextDocument::FindBackward;
	if (!m_edit.find(re, fl)) {
		m_edit.moveCursor(backward ? QTextCursor::End
		                           : QTextCursor::Start);
		if (m_edit.find(re, fl))
			statusBar()->showMessage(QStringLiteral("search wrapped"),
			                         1500);
		else
			statusBar()->showMessage(QStringLiteral("no match"), 1500);
	}
	updateCounter(m_edit.textCursor());
}

void MainWin::seekCue(int cue, bool forcePause)
{
	if (cue < 0 || cue >= m_edit.cueCount())
		return;
	QString err;
	const double t = m_edit.cueStart(cue);
	if (!m_mpv.seek(t, forcePause, &err))
		statusBar()->showMessage(QStringLiteral("mpv: ") + err, 3000);
	else
		statusBar()->showMessage(QStringLiteral("#%1 \u2192 %2%3")
		        .arg(cue + 1).arg(fmtTime(t, true),
		             forcePause ? QStringLiteral("  [paused]")
		                        : QString()), 2000);
}

void MainWin::setPause(bool on)
{
	QString err;
	if (!m_mpv.setPause(on, &err))
		statusBar()->showMessage(QStringLiteral("mpv: ") + err, 3000);
}

void MainWin::togglePause()
{
	QString err;
	if (!m_mpv.cyclePause(&err))
		statusBar()->showMessage(QStringLiteral("mpv: ") + err, 3000);
}

void MainWin::seekRel(double dt)
{
	QString err;
	if (!m_mpv.seekRel(dt, &err))
		statusBar()->showMessage(QStringLiteral("mpv: ") + err, 3000);
}

void MainWin::toggleFollow()
{
	m_followAct.toggle();                // toggled() -> setFollow
	statusBar()->showMessage(m_followAct.isChecked()
		? QStringLiteral("following playback")
		: QStringLiteral("following off"), 1500);
}

void MainWin::onMpvTime(double t)
{
	m_edit.setPlayTime(t);
}

void MainWin::setSearchText(const QString &s)
{
	if (m_searchAnchor.isNull() && m_edit.cueCount() > 0)
		m_searchAnchor = m_edit.textCursor();
	m_searchBar.setPattern(s);
}

void MainWin::setRegexEnabled(bool on)
{
	m_searchBar.setRegexEnabled(on);
}

void MainWin::dragEnterEvent(QDragEnterEvent *ev)
{
	if (droppable(ev->mimeData()))
		ev->acceptProposedAction();
}

void MainWin::dropEvent(QDropEvent *ev)
{
	const auto urls = ev->mimeData()->urls();
	if (!urls.isEmpty())
		openPath(urls.first().toLocalFile());
}

void MainWin::closeEvent(QCloseEvent *ev)
{
	m_mpv.shutdown();
	ev->accept();
}

void MainWin::resizeEvent(QResizeEvent *ev)
{
	QMainWindow::resizeEvent(ev);
	layoutOverlays();
}

bool MainWin::droppable(const QMimeData *md)
{
	if (!md->hasUrls() || md->urls().isEmpty())
		return false;
	const QString p = md->urls().first().toLocalFile();
	static constexpr QLatin1StringView exts[]{
		QLatin1StringView(".srt"),  QLatin1StringView(".mp4"),
		QLatin1StringView(".mkv"),  QLatin1StringView(".webm"),
		QLatin1StringView(".avi"),  QLatin1StringView(".mov"),
		QLatin1StringView(".m4v"),  QLatin1StringView(".mpg"),
		QLatin1StringView(".mpeg"), QLatin1StringView(".ts"),
		QLatin1StringView(".wmv")};
	for (const auto e : exts)
		if (p.endsWith(e, Qt::CaseInsensitive))
			return true;
	return false;
}

QPoint MainWin::searchBarTarget() const
{
	const QRect er = centralWidget()->geometry();
	return {er.right() - m_searchBar.width() - 24, er.top() + 10};
}

QRegularExpression MainWin::pattern() const
{
	const QString raw = m_searchBar.pattern();
	QRegularExpression re(m_searchBar.regexEnabled()
		? raw : QRegularExpression::escape(raw));
	if (!m_searchBar.caseSensitive())
		re.setPatternOptions(QRegularExpression::CaseInsensitiveOption);
	return re;
}

void MainWin::highlightAll()
{
	QList<QTextEdit::ExtraSelection> sels;
	m_matchStarts.clear();
	const QRegularExpression re = pattern();
	const bool empty = re.pattern().isEmpty();
	if (!re.isValid() && !empty) {
		m_searchBar.setCount(0, -1);     // invalid-pattern feedback
		m_edit.setMatchSelections({});
		return;
	}
	if (!empty) {
		QTextDocument *doc = m_edit.document();
		QTextCursor c(doc);
		// Alpha over the theme base keeps the theme's own text color
		// readable on both light and dark palettes.
		QColor bg = m_edit.palette().color(QPalette::Highlight);
		bg.setAlpha(85);
		QTextCharFormat fmt;
		fmt.setBackground(bg);
		while (true) {
			c = doc->find(re, c);
			if (c.isNull())
				break;
			if (!c.hasSelection()) {         // zero-length match
				if (c.atEnd())
					break;
				c.movePosition(QTextCursor::NextCharacter);
				continue;
			}
			QTextEdit::ExtraSelection s;
			s.cursor = c;
			s.format = fmt;
			sels << s;
			m_matchStarts.push_back(c.selectionStart());
		}
	}
	m_edit.setMatchSelections(sels);
	m_searchBar.setCount(0, empty ? 0 : int(m_matchStarts.size()));
}

void MainWin::updateCounter(const QTextCursor &cur)
{
	int idx = 0;
	const int start = cur.selectionStart();
	for (size_t i = 0; i < m_matchStarts.size(); ++i)
		if (m_matchStarts[i] == start) {
			idx = int(i) + 1;
			break;
		}
	m_searchBar.setCount(idx, int(m_matchStarts.size()));
}

void MainWin::openDialog()
{
	const QString p = QFileDialog::getOpenFileName(this,
		QStringLiteral("Open video or subtitle"), QString(),
		QStringLiteral("Video / SRT (*.mp4 *.mkv *.webm *.avi *.mov "
		               "*.m4v *.mpg *.mpeg *.ts *.wmv *.srt);;"
		               "All files (*)"));
	if (!p.isEmpty())
		openPath(p);
}

void MainWin::closeFile()
{
	m_mpv.shutdown();
	m_edit.setCues({});
	m_edit.clear();
	setWindowTitle(QStringLiteral("srtview"));
	setState(QStringLiteral("no file"));
}

bool MainWin::fail(const QString &msg)
{
	QMessageBox::warning(this, QStringLiteral("srtview"), msg);
	return false;
}

void MainWin::setState(const QString &s)
{
	m_state.setText(s);
}
