#include "selftest.hpp"

#include "mainwin.hpp"

#include <QApplication>
#include <QTimer>

#include <cstdio>

void runSelftest(MainWin *w, QString const &video)
{
	auto const log = [](QString const &s) {
		std::fprintf(stdout, "SELFTEST: %s\n", qPrintable(s));
		std::fflush(stdout);
	};
	if (!w->openPath(video)) {
		log(QStringLiteral("open FAILED"));
		QTimer::singleShot(0, w, [] { QApplication::exit(1); });
		return;
	}
	log(QStringLiteral("cues=%1").arg(w->view().cueCount()));

	QTimer::singleShot(1000, w, [w, log] {
		w->search().showSearch();
		w->search().setSearchText(QStringLiteral("voluptat"));
		int const c = w->view().currentCue();
		log(QStringLiteral("match cue=%1 start=%2")
		    .arg(c).arg(w->view().cueStart(c), 0, 'f', 3));
		w->playback().seekCue(c, false);
		log(QStringLiteral("seek-keep sent"));
	});
	QTimer::singleShot(1500, w, [w, log] {
		QRect const g = w->bar().geometry();
		log(QStringLiteral("bar geom x=%1 w=%2 right=%3 win_w=%4")
		    .arg(g.x()).arg(g.width()).arg(g.right()).arg(w->view().width()));
		QString const shot = QStringLiteral("/tmp/srtview-shot1.png");
		w->grab().save(shot);
		log(QStringLiteral("screenshot %1").arg(shot));
	});
	QTimer::singleShot(2200, w, [w, log] {
		w->search().setSearchText(QStringLiteral("d.lor"));
		log(QStringLiteral("regex-on 'd.lor' matches=%1")
		    .arg(w->search().matchCount()));
		w->search().setRegexEnabled(false);
		log(QStringLiteral("literal 'd.lor' matches=%1")
		    .arg(w->search().matchCount()));
		w->search().setRegexEnabled(true);
		w->search().setSearchText(QStringLiteral("voluptat"));
		w->search().commitSearch();                   // Enter path: bar hides
	});
	QTimer::singleShot(2600, w, [w, log] {   // after the slide-out
		log(QStringLiteral("committed, bar visible=%1")
		    .arg(w->bar().isVisible()));
	});
	QTimer::singleShot(3000, w, [w, log] {
		w->playback().setPause(false);
		log(QStringLiteral("play sent"));
	});
	QTimer::singleShot(4600, w, [w, log] {
		log(QStringLiteral("playcue=%1").arg(w->view().playCue()));
	});
	QTimer::singleShot(5000, w, [w, log] {
		w->playback().setPause(true);
		log(QStringLiteral("pause sent"));
	});
	QTimer::singleShot(7000, w, [w, log] {
		w->search().findAgain(false);                 // search bar is hidden
		int const c = w->view().currentCue();
		log(QStringLiteral("match2 cue=%1 start=%2")
		    .arg(c).arg(w->view().cueStart(c), 0, 'f', 3));
		w->playback().seekCue(c, true);
		log(QStringLiteral("seek-pause sent"));
	});
	QTimer::singleShot(8100, w, [w, log] {
		log(QStringLiteral("playcue2=%1").arg(w->view().playCue()));
	});
	QTimer::singleShot(8300, w, [w, log] {
		w->playback().togglePause();                    // Space path
		log(QStringLiteral("toggle sent"));
	});
	QTimer::singleShot(8600, w, [w, log] {
		QString const shot = QStringLiteral("/tmp/srtview-shot2.png");
		w->grab().save(shot);
		log(QStringLiteral("screenshot %1").arg(shot));
	});
	QTimer::singleShot(10000, w, [w, log] {
		log(QStringLiteral("done"));
		w->close();
		QApplication::exit(0);
	});
}
