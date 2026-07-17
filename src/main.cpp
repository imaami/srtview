// srtview -- Qt6 subtitle-transcript viewer that remote-controls mpv.
//
// A large, uncluttered reading view of a video's .srt (VIDEO.EXT.srt,
// falling back to VIDEO.srt): caption text only, one paragraph per
// cue, SRT inline tags rendered, cue times in a quiet gutter, full
// timing in hover tooltips.  mpv runs on the same deterministic
// per-video socket as the srtjump script, so srtview, srtjump, Kate
// external tools and ad-hoc scripts can all drive one player; a
// running instance is reused and left running on exit.
//
// Controls (reading view):
//   Ctrl+F, /       open the search overlay (".*" toggles regexp
//                   mode, on by default; Aa toggles case)
//   Enter (in box)  accept: the overlay hides, focus returns to the
//                   view (the incremental search has already landed
//                   on the first match, so 't' etc. work immediately)
//   Esc             dismiss the overlay, focus returns to the view
//   F3 / Shift+F3, n / N   next / previous match, overlay hidden or not
//   Return, double-click, gutter click  seek mpv to the cue, keep
//                                       play/pause state
//   T               seek + force pause
//   Space           play/pause toggle
//   Left / Right    seek 5 s back / forward
//   f               toggle follow: the cue being spoken is tinted in
//                   sync, and the view glides along while following
//   c / P           play / pause (srtjump muscle memory)
//
// Env: SRTVIEW_MPV_ARGS -- extra mpv arguments (split on whitespace)
//
// Build:  cmake -B build && cmake --build build
// Deps :  qt6-base-dev (Widgets, Network), a C++20 compiler

#include "mainwin.hpp"
#include "selftest.hpp"

#include <QApplication>

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
