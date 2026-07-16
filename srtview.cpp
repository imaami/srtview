// srtview -- Qt6 subtitle-transcript viewer that remote-controls mpv.
//
// Opens a video's .srt (VIDEO.EXT.srt, falling back to VIDEO.srt) as a
// large, uncluttered reading view: caption text only, one paragraph
// per cue, SRT inline tags (<i> <b> <u> <font color>) rendered.  Cue
// start times sit in a quiet gutter where line numbers would normally
// live; hovering a caption shows the full cue timing in a tooltip.
//
// mpv runs on the same deterministic per-video socket as the srtjump
// script ($XDG_RUNTIME_DIR/srtjump/<sha256[:16]>.sock), so srtview,
// srtjump, Kate external tools and ad-hoc scripts can all drive one
// player; a running instance is reused and left running on exit.
//
// Controls (reading view):
//   Ctrl+F, /       open the search overlay (regexp; Aa toggles case)
//   Esc             dismiss the overlay, focus returns to the view
//   Enter (in box)  next match          F3 / Shift+F3, n / N   next /
//                                       previous, overlay may be hidden
//   Return, double-click, gutter click  seek mpv to the cue, keep
//                                       play/pause state
//   T               seek + force pause
//   Space           play/pause toggle
//   Left / Right    seek 5 s back / forward
//   c / P           play / pause (srtjump muscle memory)
//
// All custom colors derive from the active QPalette, so dark themes
// (qt6ct etc.) keep their contrast.
//
// Env: SRTVIEW_MPV_ARGS -- extra mpv arguments (split on whitespace)
//
// Build:  cmake -B build && cmake --build build
// Deps :  qt6-base-dev (Widgets, Network), a C++20 compiler

#include <QtWidgets>
#include <QLocalSocket>
#include <QCryptographicHash>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

// --------------------------------------------------------------- cues --

struct Cue {
	double  start = 0.0;
	double  end   = 0.0;
	QString text;          // internal line breaks as QChar::LineSeparator
};

// Result feeds Qt paint/tooltip APIs directly, hence QString rather
// than std::format: a std round-trip would only add a conversion.
QString fmtTime(double t, bool withMs)
{
	int ms = int(std::lround(t * 1000.0));
	int h = ms / 3600000; ms %= 3600000;
	int m = ms / 60000;   ms %= 60000;
	int s = ms / 1000;    ms %= 1000;
	QString out = (h > 0)
		? QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0'))
		                            .arg(s, 2, 10, QLatin1Char('0'))
		: QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
	if (withMs)
		out += QStringLiteral(".%1").arg(ms, 3, 10, QLatin1Char('0'));
	return out;
}

// "HH:MM:SS,mmm --> HH:MM:SS,mmm" (also tolerates '.' separators)
const QRegularExpression &timestampRe()
{
	static const QRegularExpression re(QStringLiteral(
		R"(^\s*(\d+):(\d\d):(\d\d)[,.](\d{1,3})\s*-->\s*(\d+):(\d\d):(\d\d)[,.](\d{1,3}))"));
	return re;
}

double capTime(const QRegularExpressionMatch &m, int base)
{
	return m.captured(base).toInt()     * 3600.0
	     + m.captured(base + 1).toInt() * 60.0
	     + m.captured(base + 2).toInt()
	     + m.captured(base + 3).toInt() / 1000.0;
}

std::vector<Cue> parseSrt(const QString &path, QString *err)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
		*err = f.errorString();
		return {};
	}
	QTextStream ts(&f);            // UTF-8 with BOM auto-detection
	QStringList lines;
	while (!ts.atEnd())
		lines << ts.readLine();

	std::vector<Cue> cues;
	static const QRegularExpression numOnly(QStringLiteral(R"(^\s*\d+\s*$)"));
	for (qsizetype i = 0; i < lines.size(); ++i) {
		const auto m = timestampRe().match(lines[i]);
		if (!m.hasMatch())
			continue;
		Cue c;
		c.start = capTime(m, 1);
		c.end   = capTime(m, 5);
		QStringList text;
		qsizetype j = i + 1;
		for (; j < lines.size(); ++j) {
			const QString &l = lines[j];
			if (l.trimmed().isEmpty())
				break;
			if (timestampRe().match(l).hasMatch())
				break;                          // unseparated next cue
			if (numOnly.match(l).hasMatch()     // cue index line just
			    && j + 1 < lines.size()         // before a timestamp
			    && timestampRe().match(lines[j + 1]).hasMatch())
				break;
			text << l.trimmed();
		}
		c.text = text.join(QChar::LineSeparator);
		cues.push_back(std::move(c));
		i = j - 1;
	}
	if (cues.empty())
		*err = QStringLiteral("no cues found (not an SRT file?)");
	return cues;
}

// Escape everything, then let the SRT inline-tag subset back through.
QString cueHtml(const QString &text)
{
	QString h = text.toHtmlEscaped();
	h.replace(QChar::LineSeparator, QStringLiteral("<br>"));
	static const QRegularExpression ass(QStringLiteral(R"(\{\\[^}]*\})"));
	h.remove(ass);                              // ASS override blocks
	static const QRegularExpression tag(QStringLiteral(
		R"(&lt;(/?)([ibu])&gt;)"),
		QRegularExpression::CaseInsensitiveOption);
	h.replace(tag, QStringLiteral("<\\1\\2>"));
	static const QRegularExpression fontOpen(QStringLiteral(
		R"(&lt;font\s+color=(?:&quot;)?(#?[A-Za-z0-9]+)(?:&quot;)?\s*&gt;)"),
		QRegularExpression::CaseInsensitiveOption);
	h.replace(fontOpen, QStringLiteral("<font color=\"\\1\">"));
	h.replace(QStringLiteral("&lt;/font&gt;"), QStringLiteral("</font>"),
	          Qt::CaseInsensitive);
	return h;
}

// -------------------------------------------- shared socket derivation --
// Byte-for-byte the same scheme as srtjump: first 16 hex chars of the
// sha256 of the video's canonical path, under $XDG_RUNTIME_DIR/srtjump.

QString runDir()
{
	QString base = qEnvironmentVariable("XDG_RUNTIME_DIR");
	if (base.isEmpty())
		base = qEnvironmentVariable("TMPDIR", QStringLiteral("/tmp"));
	QString d = base + QStringLiteral("/srtjump");
	QDir().mkpath(d);
	return d;
}

QString sockForVideo(const QString &video, QString *err)
{
	const QString rp = QFileInfo(video).canonicalFilePath();
	if (rp.isEmpty()) {
		*err = QStringLiteral("cannot resolve path: %1").arg(video);
		return {};
	}
	const QByteArray h = QCryptographicHash::hash(rp.toUtf8(),
	                     QCryptographicHash::Sha256).toHex().left(16);
	return runDir() + QLatin1Char('/') + QString::fromLatin1(h)
	                + QStringLiteral(".sock");
}

bool socketAlive(const QString &sock)
{
	QLocalSocket probe;
	probe.connectToServer(sock);
	const bool ok = probe.waitForConnected(200);
	probe.abort();
	return ok;
}

// VIDEO.EXT.srt first, VIDEO.srt second.
QString srtForVideo(const QString &video, QString *err)
{
	const QString direct = video + QStringLiteral(".srt");
	if (QFileInfo(direct).isReadable())
		return direct;
	const QFileInfo fi(video);
	if (fi.fileName().contains(QLatin1Char('.'))) {
		const QString alt = fi.path() + QLatin1Char('/')
		                  + fi.completeBaseName() + QStringLiteral(".srt");
		if (QFileInfo(alt).isReadable())
			return alt;
		*err = QStringLiteral("subtitle file not found: %1 or %2")
		       .arg(direct, alt);
		return {};
	}
	*err = QStringLiteral("subtitle file not found: %1").arg(direct);
	return {};
}

// Inverse: VIDEO.EXT.srt strips to an existing file; VIDEO.srt needs a
// sibling search, live players disambiguating multiple candidates.
QString videoForSrt(const QString &srt, QString *err)
{
	QString x = srt;
	x.chop(4);                                   // ".srt"
	if (QFileInfo(x).isFile())
		return x;
	const QFileInfo fi(x);
	const QDir dir = fi.dir();
	QStringList cands;
	const auto entries = dir.entryList({fi.fileName() + QStringLiteral(".*")},
	                                   QDir::Files);
	for (const QString &e : entries)
		if (!e.endsWith(QStringLiteral(".srt")))
			cands << dir.filePath(e);
	if (cands.isEmpty()) {
		*err = QStringLiteral("no video found for %1").arg(srt);
		return {};
	}
	if (cands.size() == 1)
		return cands.first();
	QStringList live;
	for (const QString &c : cands) {
		QString e2;
		const QString s = sockForVideo(c, &e2);
		if (!s.isEmpty() && socketAlive(s))
			live << c;
	}
	if (live.size() == 1)
		return live.first();
	*err = live.isEmpty()
		? QStringLiteral("ambiguous, no running player for any of:\n%1")
		  .arg(cands.join(QLatin1Char('\n')))
		: QStringLiteral("ambiguous, multiple running players:\n%1")
		  .arg(live.join(QLatin1Char('\n')));
	return {};
}

// ------------------------------------------------------------ MpvLink --
// Owns the (possibly spawned) mpv process and a persistent connection
// to its IPC socket.  Commands go out as single raw input.conf lines:
// mpv parses non-JSON lines as command lists, and one line per action
// sidesteps mpv dropping buffered lines when a client disconnects.
// QLocalSocket buffers writes internally, so every send() flushes
// explicitly; without it each command reaches mpv one write late.

class MpvLink : public QObject
{
public:
	MpvLink()
	{
		// mpv replies to nothing we care about; keep the buffer empty.
		connect(&m_conn, &QLocalSocket::readyRead,
		        this, [this] { m_conn.readAll(); });
	}

	bool openFor(const QString &video, const QString &srt, QString *err)
	{
		shutdown();
		m_video = video;
		m_srt   = srt;
		m_sock  = sockForVideo(video, err);
		if (m_sock.isEmpty())
			return false;
		return ensureAlive(err);
	}

	// True if a player is (or can be brought) alive on our socket.
	bool ensureAlive(QString *err)
	{
		if (m_conn.state() == QLocalSocket::ConnectedState)
			return true;
		if (tryConnect())
			return true;                     // reuse a running instance
		if (m_video.isEmpty()) {
			*err = QStringLiteral("no video associated");
			return false;
		}
		QFile::remove(m_sock);               // stale leftover
		QStringList args{QStringLiteral("--no-terminal"),
		                 QStringLiteral("--pause"),
		                 QStringLiteral("--keep-open=yes"),
		                 QStringLiteral("--input-ipc-server=") + m_sock,
		                 QStringLiteral("--sub-file=") + m_srt,
		                 QStringLiteral("--sub-auto=no")};
		args += QProcess::splitCommand(
		        qEnvironmentVariable("SRTVIEW_MPV_ARGS"));
		args << QStringLiteral("--") << m_video;
		m_proc.setProgram(QStringLiteral("mpv"));
		m_proc.setArguments(args);
		m_proc.start();
		if (!m_proc.waitForStarted(3000)) {
			*err = QStringLiteral("cannot start mpv: %1")
			       .arg(m_proc.errorString());
			return false;
		}
		m_spawned = true;
		for (int i = 0; i < 50; ++i) {       // wait for the socket
			if (tryConnect())
				return true;
			if (m_proc.state() != QProcess::Running) {
				*err = QStringLiteral("mpv exited before its socket came up");
				return false;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			QCoreApplication::processEvents();
		}
		*err = QStringLiteral("mpv IPC socket never came up: %1").arg(m_sock);
		return false;
	}

	bool send(const QString &line, QString *err)
	{
		if (!ensureAlive(err))
			return false;
		m_conn.write(line.toUtf8() + '\n');
		m_conn.flush();
		// flush() usually drains the whole buffer; only wait if the
		// kernel pushed back.
		return m_conn.bytesToWrite() == 0
		       || m_conn.waitForBytesWritten(500);
	}

	bool seek(double t, bool forcePause, QString *err)
	{
		const QString s = QStringLiteral("no-osd seek %1 absolute+exact")
		                  .arg(t, 0, 'f', 3);
		return send(forcePause
			? QStringLiteral("no-osd set pause yes; ") + s : s, err);
	}
	bool seekRel(double dt, QString *err)
	{
		return send(QStringLiteral("no-osd seek %1").arg(dt, 0, 'f', 1),
		            err);
	}
	bool setPause(bool on, QString *err)
	{
		return send(QStringLiteral("no-osd set pause %1")
		            .arg(on ? QStringLiteral("yes") : QStringLiteral("no")),
		            err);
	}
	bool cyclePause(QString *err)
	{
		return send(QStringLiteral("no-osd cycle pause"), err);
	}

	// Quit mpv only if this instance started it.
	void shutdown()
	{
		if (m_spawned && m_proc.state() == QProcess::Running) {
			QString e;
			send(QStringLiteral("quit"), &e);
			if (!m_proc.waitForFinished(1500))
				m_proc.kill();
			QFile::remove(m_sock);
		}
		m_conn.abort();
		m_spawned = false;
		m_video.clear(); m_srt.clear(); m_sock.clear();
	}

	bool spawned() const { return m_spawned; }

private:
	bool tryConnect()
	{
		if (m_sock.isEmpty())
			return false;
		m_conn.abort();
		m_conn.connectToServer(m_sock);
		return m_conn.waitForConnected(200);
	}

	QProcess     m_proc;
	QLocalSocket m_conn;
	QString      m_video, m_srt, m_sock;
	bool         m_spawned = false;
};

// ---------------------------------------------------------- SearchBar --
// IDE-style overlay: slides in below the top edge of the view on
// Ctrl+F, slides away on Esc.  The pattern keeps working (F3, n/N)
// while the bar is hidden.  Statically bound to its host.

template <class Host>
class SearchBar : public QWidget
{
public:
	explicit SearchBar(Host *host, QWidget *parent)
		: QWidget(parent), m_host(host)
	{
		setAutoFillBackground(true);
		setStyleSheet(QStringLiteral(
			"SearchBar, QWidget { background: palette(window); }"
			"QLineEdit { border: none; background: palette(base);"
			"            padding: 3px 6px; border-radius: 4px; }"));
		auto *frame = new QHBoxLayout(this);
		frame->setContentsMargins(10, 8, 10, 8);
		frame->setSpacing(6);

		m_edit.setPlaceholderText(QStringLiteral("regexp\u2026"));
		m_edit.setMinimumWidth(240);
		m_edit.installEventFilter(this);
		frame->addWidget(&m_edit);

		m_case.setText(QStringLiteral("Aa"));
		m_case.setCheckable(true);
		m_case.setChecked(true);            // case-sensitive
		m_case.setAutoRaise(true);
		m_case.setToolTip(QStringLiteral("Match case"));
		frame->addWidget(&m_case);

		m_prev.setText(QStringLiteral("\u25b2"));
		m_prev.setAutoRaise(true);
		m_prev.setToolTip(QStringLiteral("Previous match (Shift+F3)"));
		m_next.setText(QStringLiteral("\u25bc"));
		m_next.setAutoRaise(true);
		m_next.setToolTip(QStringLiteral("Next match (F3)"));
		frame->addWidget(&m_prev);
		frame->addWidget(&m_next);

		m_count.setMinimumWidth(
			fontMetrics().horizontalAdvance(QStringLiteral("000/000")));
		m_count.setAlignment(Qt::AlignCenter);
		frame->addWidget(&m_count);

		m_close.setText(QStringLiteral("\u2715"));
		m_close.setAutoRaise(true);
		frame->addWidget(&m_close);

		connect(&m_edit, &QLineEdit::textChanged,
		        this, [this] { m_host->searchChanged(); });
		connect(&m_edit, &QLineEdit::returnPressed,
		        this, [this] { m_host->findAgain(false); });
		connect(&m_case, &QToolButton::toggled,
		        this, [this] { m_host->searchChanged(); });
		connect(&m_prev, &QToolButton::clicked,
		        this, [this] { m_host->findAgain(true); });
		connect(&m_next, &QToolButton::clicked,
		        this, [this] { m_host->findAgain(false); });
		connect(&m_close, &QToolButton::clicked,
		        this, [this] { m_host->hideSearch(); });

		m_anim.setTargetObject(this);
		m_anim.setPropertyName("pos");
		m_anim.setDuration(140);
		m_anim.setEasingCurve(QEasingCurve::OutCubic);
		hide();
	}

	QString pattern() const { return m_edit.text(); }
	bool caseSensitive() const { return m_case.isChecked(); }
	void setPattern(const QString &s) { m_edit.setText(s); }
	void setCount(int idx, int n)
	{
		m_count.setText(n <= 0 ? QStringLiteral("\u2014")
			: QStringLiteral("%1/%2")
			  .arg(idx > 0 ? QString::number(idx) : QStringLiteral("?"))
			  .arg(n));
		// invalid-pattern feedback: theme text color vs. plain red
		QPalette pal;
		if (n < 0)
			pal.setColor(QPalette::Text, QColor(214, 72, 72));
		m_edit.setPalette(pal);
	}

	void open(const QPoint &target)
	{
		adjustSize();
		m_target = target;
		if (!isVisible()) {
			move(target.x(), -height());
			show();
		}
		raise();
		slideTo(target);
		m_edit.setFocus();
		m_edit.selectAll();
	}

	void dismiss()
	{
		if (!isVisible())
			return;
		slideTo(QPoint(m_target.x(), -height()));
		connect(&m_anim, &QPropertyAnimation::finished,
		        this, [this] { hide(); }, Qt::SingleShotConnection);
	}

	void reposition(const QPoint &target)
	{
		m_target = target;
		if (isVisible())
			move(target);
	}

protected:
	bool eventFilter(QObject *obj, QEvent *ev) override
	{
		if (obj == &m_edit && ev->type() == QEvent::KeyPress) {
			auto *ke = static_cast<QKeyEvent *>(ev);
			if (ke->key() == Qt::Key_Escape) {
				m_host->hideSearch();
				return true;
			}
		}
		return QWidget::eventFilter(obj, ev);
	}

	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		QColor edge = palette().color(QPalette::Mid);
		p.setPen(edge);
		p.setBrush(palette().color(QPalette::Window));
		p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
		                  8.0, 8.0);
	}

private:
	void slideTo(const QPoint &to)
	{
		m_anim.stop();
		m_anim.setStartValue(pos());
		m_anim.setEndValue(to);
		m_anim.start();
	}

	Host               *m_host;
	QLineEdit           m_edit;
	QToolButton         m_case, m_prev, m_next, m_close;
	QLabel              m_count;
	QPropertyAnimation  m_anim;
	QPoint              m_target;
};

// ------------------------------------------------------------ SrtEdit --
// The reading view.  Statically bound to its host window: actions
// behind the keys resolve to direct Host member calls at compile
// time; the host-touching bodies are instantiated at the end of this
// TU, where Host is complete.

template <class Host>
class SrtEdit : public QTextEdit
{
public:
	explicit SrtEdit(Host *host)
		: QTextEdit(host), m_host(host), m_gutter(this)
	{
		setReadOnly(true);
		setFrameShape(QFrame::NoFrame);
		setAcceptDrops(false);               // let the host take the drop
		viewport()->setAcceptDrops(false);
		QFont f = font();
		f.setPointSizeF(std::max(f.pointSizeF(), 10.0) * 1.5);
		setFont(f);
		m_gutterFont = f;
		m_gutterFont.setPointSizeF(f.pointSizeF() * 0.68);
		document()->setDocumentMargin(28);
		m_gutter.installEventFilter(this);
		m_gutter.setCursor(Qt::PointingHandCursor);
		connect(verticalScrollBar(), &QScrollBar::valueChanged,
		        &m_gutter, QOverload<>::of(&QWidget::update));
		connect(this, &QTextEdit::textChanged,
		        &m_gutter, QOverload<>::of(&QWidget::update));
		connect(this, &QTextEdit::cursorPositionChanged,
		        this, [this] { updateCurrentCueHighlight(); });
	}

	void setCues(std::vector<Cue> cues)
	{
		m_cues = std::move(cues);
		QTextDocument *doc = document();
		doc->clear();
		doc->setDefaultFont(font());

		QTextBlockFormat bf;
		bf.setLineHeight(126, QTextBlockFormat::ProportionalHeight);
		QTextBlockFormat bfGap = bf;
		bfGap.setTopMargin(kCueGap);

		QTextCursor cur(doc);
		cur.setBlockFormat(bf);
		bool first = true;
		for (const Cue &c : m_cues) {
			if (!first)
				cur.insertBlock(bfGap, QTextCharFormat());
			first = false;
			cur.insertHtml(cueHtml(c.text));
		}

		// gutter width from the widest start time in this file
		const QString widest = m_cues.empty()
			? QString() : fmtTime(m_cues.back().start, false);
		m_gutterW = QFontMetrics(m_gutterFont)
		            .horizontalAdvance(widest) + 26;
		setViewportMargins(m_gutterW, 0, 0, 0);
		layoutGutter();
		moveCursor(QTextCursor::Start);
		updateCurrentCueHighlight();
	}

	int cueCount() const { return int(m_cues.size()); }
	double cueStart(int i) const
	{
		return (i >= 0 && size_t(i) < m_cues.size()) ? m_cues[i].start
		                                             : 0.0;
	}
	int currentCue() const { return textCursor().blockNumber(); }

	void setMatchSelections(const QList<ExtraSelection> &sel)
	{
		m_matchSel = sel;
		applySelections();
	}

protected:
	void keyPressEvent(QKeyEvent *ev) override
	{
		if (!(ev->modifiers() & (Qt::ControlModifier | Qt::AltModifier
		                         | Qt::MetaModifier))) {
			switch (ev->key()) {
			case Qt::Key_Return:
			case Qt::Key_Enter:
			case Qt::Key_T:
				if (!m_cues.empty())
					m_host->seekCue(currentCue(),
					                ev->text() == QStringLiteral("T"));
				return;
			case Qt::Key_Space:
				m_host->togglePause();
				return;
			case Qt::Key_Left:
				m_host->seekRel(-5.0);
				return;
			case Qt::Key_Right:
				m_host->seekRel(5.0);
				return;
			case Qt::Key_Escape:
				m_host->hideSearch();
				return;
			case Qt::Key_C:
				if (ev->text() == QStringLiteral("c")) {
					m_host->setPause(false);
					return;
				}
				break;
			case Qt::Key_P:
				if (ev->text() == QStringLiteral("P")) {
					m_host->setPause(true);
					return;
				}
				break;
			case Qt::Key_Slash:
				m_host->showSearch();
				return;
			case Qt::Key_N:
				m_host->findAgain(ev->text() == QStringLiteral("N"));
				return;
			default:
				break;
			}
		}
		QTextEdit::keyPressEvent(ev);
	}

	void mouseDoubleClickEvent(QMouseEvent *ev) override
	{
		QTextEdit::mouseDoubleClickEvent(ev);
		if (!m_cues.empty())
			m_host->seekCue(currentCue(), false);
	}

	void resizeEvent(QResizeEvent *ev) override
	{
		QTextEdit::resizeEvent(ev);
		layoutGutter();
		m_host->layoutOverlays();
	}

	bool event(QEvent *ev) override
	{
		if (ev->type() == QEvent::ToolTip && !m_cues.empty()) {
			auto *he = static_cast<QHelpEvent *>(ev);
			const QPoint vp = viewport()->mapFrom(this, he->pos());
			const int cue = cursorForPosition(vp).blockNumber();
			if (cue >= 0 && size_t(cue) < m_cues.size()) {
				const Cue &c = m_cues[size_t(cue)];
				QToolTip::showText(he->globalPos(),
					QStringLiteral("#%1   %2 \u2192 %3")
					.arg(cue + 1)
					.arg(fmtTime(c.start, true), fmtTime(c.end, true)),
					this);
				return true;
			}
		}
		return QTextEdit::event(ev);
	}

	bool eventFilter(QObject *obj, QEvent *ev) override
	{
		if (obj == &m_gutter) {
			if (ev->type() == QEvent::Paint) {
				paintGutter();
				return true;
			}
			if (ev->type() == QEvent::MouseButtonPress) {
				auto *me = static_cast<QMouseEvent *>(ev);
				const int cue = cueAtGutterY(int(me->position().y()));
				if (cue >= 0) {
					setTextCursor(QTextCursor(
						document()->findBlockByNumber(cue)));
					m_host->seekCue(cue, false);
				}
				return true;
			}
		}
		return QTextEdit::eventFilter(obj, ev);
	}

private:
	static constexpr qreal kCueGap = 14.0;

	void layoutGutter()
	{
		const QRect cr = contentsRect();
		m_gutter.setGeometry(cr.left(), cr.top(), m_gutterW, cr.height());
	}

	// Visible blocks under the current scroll offset, gutter-space
	// rects; the visitor binds statically.
	template <typename F> void visitVisibleBlocks(F f)
	{
		auto *lay = document()->documentLayout();
		const int yoff = verticalScrollBar()->value();
		for (QTextBlock b = document()->firstBlock(); b.isValid();
		     b = b.next()) {
			const QRectF r = lay->blockBoundingRect(b)
			                 .translated(0, -yoff);
			if (r.top() > m_gutter.height())
				break;
			if (r.bottom() < 0)
				continue;
			if (!f(b, r))
				break;
		}
	}

	void paintGutter()
	{
		QPainter p(&m_gutter);
		// Same background as the text, only quieter ink: the gutter
		// should read as part of the page, not as a panel.
		p.fillRect(m_gutter.rect(), palette().color(QPalette::Base));
		QColor ink = palette().color(QPalette::Text);
		ink.setAlpha(110);
		p.setPen(ink);
		p.setFont(m_gutterFont);
		const int w = m_gutterW - 12;
		const int lineH = fontMetrics().height();
		visitVisibleBlocks([&](const QTextBlock &b, const QRectF &r) {
			const int cue = b.blockNumber();
			if (size_t(cue) < m_cues.size()) {
				const qreal y = r.top() + b.blockFormat().topMargin();
				p.drawText(QRectF(0, y, w, lineH),
				           Qt::AlignRight | Qt::AlignVCenter,
				           fmtTime(m_cues[size_t(cue)].start, false));
			}
			return true;
		});
	}

	int cueAtGutterY(int y)
	{
		int hit = -1;
		visitVisibleBlocks([&](const QTextBlock &b, const QRectF &r) {
			if (y >= r.top() && y < r.bottom()) {
				hit = b.blockNumber();
				return false;
			}
			return true;
		});
		return (hit >= 0 && size_t(hit) < m_cues.size()) ? hit : -1;
	}

	void updateCurrentCueHighlight()
	{
		ExtraSelection sel;
		sel.cursor = textCursor();
		sel.cursor.clearSelection();
		QColor bg = palette().color(QPalette::Highlight);
		bg.setAlpha(34);
		sel.format.setBackground(bg);
		sel.format.setProperty(QTextFormat::FullWidthSelection, true);
		m_lineSel = {sel};
		applySelections();
		m_gutter.update();
	}

	void applySelections()
	{
		setExtraSelections(m_lineSel + m_matchSel);
	}

	Host                   *m_host;
	QWidget                 m_gutter;
	QFont                   m_gutterFont;
	int                     m_gutterW = 0;
	std::vector<Cue>        m_cues;
	QList<ExtraSelection>   m_lineSel, m_matchSel;
};

// ------------------------------------------------------------ MainWin --

class MainWin : public QMainWindow
{
public:
	MainWin()
		: m_edit(this), m_searchBar(this, this)
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
	}

	bool openPath(const QString &path)
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
		std::vector<Cue> cues = parseSrt(srt, &err);
		if (cues.empty())
			return fail(QStringLiteral("%1: %2").arg(srt, err));

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

	// ---- host interface for SrtEdit and SearchBar; --selftest too ----

	void showSearch()
	{
		m_searchAnchor = m_edit.textCursor();
		m_searchBar.open(searchBarTarget());
	}
	void hideSearch()
	{
		m_searchBar.dismiss();
		m_edit.setFocus();
	}
	void layoutOverlays()
	{
		m_searchBar.reposition(searchBarTarget());
	}

	// Pattern edited: refresh highlights and jump to the first match
	// at or after where the search began.
	void searchChanged()
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

	void findAgain(bool backward)
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

	void seekCue(int cue, bool forcePause)
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
	void setPause(bool on)
	{
		QString err;
		if (!m_mpv.setPause(on, &err))
			statusBar()->showMessage(QStringLiteral("mpv: ") + err, 3000);
	}
	void togglePause()
	{
		QString err;
		if (!m_mpv.cyclePause(&err))
			statusBar()->showMessage(QStringLiteral("mpv: ") + err, 3000);
	}
	void seekRel(double dt)
	{
		QString err;
		if (!m_mpv.seekRel(dt, &err))
			statusBar()->showMessage(QStringLiteral("mpv: ") + err, 3000);
	}

	void setSearchText(const QString &s)
	{
		if (m_searchAnchor.isNull() && m_edit.cueCount() > 0)
			m_searchAnchor = m_edit.textCursor();
		m_searchBar.setPattern(s);
	}
	auto &edit() { return m_edit; }

protected:
	void dragEnterEvent(QDragEnterEvent *ev) override
	{
		if (droppable(ev->mimeData()))
			ev->acceptProposedAction();
	}
	void dropEvent(QDropEvent *ev) override
	{
		const auto urls = ev->mimeData()->urls();
		if (!urls.isEmpty())
			openPath(urls.first().toLocalFile());
	}
	void closeEvent(QCloseEvent *ev) override
	{
		m_mpv.shutdown();
		ev->accept();
	}
	void resizeEvent(QResizeEvent *ev) override
	{
		QMainWindow::resizeEvent(ev);
		layoutOverlays();
	}

private:
	static bool droppable(const QMimeData *md)
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

	QPoint searchBarTarget() const
	{
		const QRect er = centralWidget()->geometry();
		return {er.right() - m_searchBar.width() - 24, er.top() + 10};
	}

	QRegularExpression pattern() const
	{
		QRegularExpression re(m_searchBar.pattern());
		if (!m_searchBar.caseSensitive())
			re.setPatternOptions(QRegularExpression::CaseInsensitiveOption);
		return re;
	}

	void highlightAll()
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
			// Alpha over the theme base keeps the theme's own text
			// color readable on both light and dark palettes.
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

	void updateCounter(const QTextCursor &cur)
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

	void openDialog()
	{
		const QString p = QFileDialog::getOpenFileName(this,
			QStringLiteral("Open video or subtitle"), QString(),
			QStringLiteral("Video / SRT (*.mp4 *.mkv *.webm *.avi *.mov "
			               "*.m4v *.mpg *.mpeg *.ts *.wmv *.srt);;"
			               "All files (*)"));
		if (!p.isEmpty())
			openPath(p);
	}

	void closeFile()
	{
		m_mpv.shutdown();
		m_edit.setCues({});
		m_edit.clear();
		setWindowTitle(QStringLiteral("srtview"));
		setState(QStringLiteral("no file"));
	}

	bool fail(const QString &msg)
	{
		QMessageBox::warning(this, QStringLiteral("srtview"), msg);
		return false;
	}

	void setState(const QString &s) { m_state.setText(s); }

	SrtEdit<MainWin>   m_edit;
	SearchBar<MainWin> m_searchBar;
	MpvLink            m_mpv;
	QLabel             m_state;
	QAction            m_nextAct, m_prevAct;
	QTextCursor        m_searchAnchor;
	std::vector<int>   m_matchStarts;
};

// ----------------------------------------------------------- selftest --
// Scripted offscreen exercise of the exact code paths behind the keys;
// an external harness inspects mpv through the socket between steps.

void runSelftest(MainWin *w, const QString &video)
{
	auto log = [](const QString &s) {
		std::fprintf(stdout, "SELFTEST: %s\n", qPrintable(s));
		std::fflush(stdout);
	};
	if (!w->openPath(video)) {
		log(QStringLiteral("open FAILED"));
		QTimer::singleShot(0, w, [] { QApplication::exit(1); });
		return;
	}
	log(QStringLiteral("cues=%1").arg(w->edit().cueCount()));

	QTimer::singleShot(1000, w, [w, log] {
		w->showSearch();
		w->setSearchText(QStringLiteral("voluptat"));
		const int c = w->edit().currentCue();
		log(QStringLiteral("match cue=%1 start=%2")
		    .arg(c).arg(w->edit().cueStart(c), 0, 'f', 3));
		w->seekCue(c, false);
		log(QStringLiteral("seek-keep sent"));
	});
	QTimer::singleShot(1500, w, [w, log] {
		const QString shot = QStringLiteral("/tmp/srtview-shot1.png");
		w->grab().save(shot);
		log(QStringLiteral("screenshot %1").arg(shot));
	});
	QTimer::singleShot(2200, w, [w, log] {
		w->hideSearch();
		log(QStringLiteral("search hidden"));
	});
	QTimer::singleShot(3000, w, [w, log] {
		w->setPause(false);
		log(QStringLiteral("play sent"));
	});
	QTimer::singleShot(5000, w, [w, log] {
		w->setPause(true);
		log(QStringLiteral("pause sent"));
	});
	QTimer::singleShot(7000, w, [w, log] {
		w->findAgain(false);                 // search bar is hidden
		const int c = w->edit().currentCue();
		log(QStringLiteral("match2 cue=%1 start=%2")
		    .arg(c).arg(w->edit().cueStart(c), 0, 'f', 3));
		w->seekCue(c, true);
		log(QStringLiteral("seek-pause sent"));
	});
	QTimer::singleShot(8300, w, [w, log] {
		w->togglePause();                    // Space path
		log(QStringLiteral("toggle sent"));
	});
	QTimer::singleShot(8600, w, [w, log] {
		const QString shot = QStringLiteral("/tmp/srtview-shot2.png");
		w->grab().save(shot);
		log(QStringLiteral("screenshot %1").arg(shot));
	});
	QTimer::singleShot(10000, w, [w, log] {
		log(QStringLiteral("done"));
		w->close();
		QApplication::exit(0);
	});
}

} // namespace

// --------------------------------------------------------------- main --

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	QApplication::setApplicationName(QStringLiteral("srtview"));
	MainWin w;
	w.show();

	const QStringList args = app.arguments();
	if (args.size() >= 3 && args[1] == QStringLiteral("--selftest"))
		runSelftest(&w, args[2]);
	else if (args.size() >= 2)
		w.openPath(args[1]);

	return app.exec();
}
