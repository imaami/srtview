#include "selftest.hpp"

#include "mainwin.hpp"

#include <QApplication>
#include <QTimer>

#include <cstdio>

void runSelftest(MainWin *w, const QString &video)
{
	const auto log = [](const QString &s) {
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
		const QRect g = w->searchBarGeometry();
		log(QStringLiteral("bar geom x=%1 w=%2 right=%3 win_w=%4")
		    .arg(g.x()).arg(g.width()).arg(g.right()).arg(w->width()));
		const QString shot = QStringLiteral("/tmp/srtview-shot1.png");
		w->grab().save(shot);
		log(QStringLiteral("screenshot %1").arg(shot));
	});
	QTimer::singleShot(2200, w, [w, log] {
		w->setSearchText(QStringLiteral("d.lor"));
		log(QStringLiteral("regex-on 'd.lor' matches=%1")
		    .arg(w->matchCount()));
		w->setRegexEnabled(false);
		log(QStringLiteral("literal 'd.lor' matches=%1")
		    .arg(w->matchCount()));
		w->setRegexEnabled(true);
		w->setSearchText(QStringLiteral("voluptat"));
		w->commitSearch();                   // Enter path: bar hides
	});
	QTimer::singleShot(2600, w, [w, log] {   // after the slide-out
		log(QStringLiteral("committed, bar visible=%1")
		    .arg(w->searchBarVisible()));
	});
	QTimer::singleShot(3000, w, [w, log] {
		w->setPause(false);
		log(QStringLiteral("play sent"));
	});
	QTimer::singleShot(4600, w, [w, log] {
		log(QStringLiteral("playcue=%1").arg(w->playCue()));
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
	QTimer::singleShot(8100, w, [w, log] {
		log(QStringLiteral("playcue2=%1").arg(w->playCue()));
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
