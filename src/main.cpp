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
//   Ctrl+Z          undo one step: climbs down the fundo tree of
//                   search / jump / seek breadcrumbs (pause excluded)
//   Ctrl+Shift+Z    redo: climbs back up the branch last grown or
//                   re-entered; side branches persist, and repeating
//                   an identical action re-enters its old branch
//   Up / Down (in the search field)  cycle the persistent search
//                   history
//   c / P           play / pause (srtjump muscle memory)
//
// File > Export search hits… exports every match of one regex
// across the whole playlist.  A picker offers the live search
// (preselected), every corpus topic by name, and the search
// history; topic patterns match exactly as written, while the
// live search and history entries compile under the bar's regex
// and case toggles -- exactly what searching them would match.
// The suggested filename
// follows the chosen regex -- the topic's name, or a slug of the
// pattern -- never the corpus, which only lends the entries their
// playlist order.  The file is UTF-8 text, Unix newlines: line one
// is the effective regex ((?i)-prefixed when the live search's
// case toggle is off), then one entry line per match of five
// tab-separated fields -- playlist index (0-based; a directly
// opened video is the sole entry of its implicit playlist), cue
// index within the video (0-based), cue start time, the matched
// cue's block line in the .srt file (its counter line, or the
// timecode line when the counter is absent; 1-based and monotonic
// per video -- a video whose transcript has no file to cite would
// carry 0 throughout), and the match with up to three context
// words to the left and six to the right, ellipses marking a cue
// that continues past the span, whitespace runs squeezed to one
// space.  Every field but the last is right-justified to its
// column's widest member: integers space-padded, timestamps
// zero-extended to the latest stamp's colon count and then
// space-padded (" 0:00:09.250" under "12:30:01.000" -- the
// leftmost number never grows an octal-looking leading zero).
//
// Env: SRTVIEW_MPV_ARGS -- extra mpv arguments (split on whitespace)
//      SRTVIEW_LLM      -- llama-server endpoint for the background
//                          facts pipeline as [host][:port]; default
//                          127.0.0.1:8080
//      SRTVIEW_LLM_MODEL_ID -- stable model/quant fingerprint mixed
//                          into cache recipes; set this when the
//                          model behind one endpoint can change
//      SRTVIEW_LLM_PACE -- quiet seconds between summarization
//                          tasks (default 3, 0 disables): thermal
//                          breathing room for the GPU
//      SRTVIEW_OCR      -- 0 disables the frame-text reader; on by
//                          default: every grabbed pick (inspected
//                          frames first) and every cue moment of
//                          the loaded corpus is OCR'd once and
//                          cached under $XDG_CACHE_HOME/srtview/ocr
//                          (~/.cache when unset)
//      SRTVIEW_DEBUG    -- log IPC traffic, player health and facts
//                          pipeline tasks to stderr; state flips are
//                          logged always
//
// Session state (last browsed directory, recent files) persists via
// QSettings.  On WSLg (detected by its PulseAudio bridge socket),
// mpv is spawned with --audio-stream-silence=yes to sidestep known
// audio-ack wedges; override through SRTVIEW_MPV_ARGS.
//
// A player spawned by srtview that stops responding (seen on WSLg,
// where flaky PulseAudio acks can block mpv's core inside the audio
// output with no timeout) is killed and respawned at the last
// observed position and pause state.  WSLg audio mitigation knobs to
// try via SRTVIEW_MPV_ARGS: --audio-stream-silence=yes,
// --pulse-latency-hacks=yes; --ao=null confirms the diagnosis.
//
// Build:  cmake -B build && cmake --build build
// Deps :  qt6-base-dev (Widgets, Network), a C++23 compiler

#include "mainwin.hpp"
#include "selftest.hpp"

#include <QApplication>
#include <QFontInfo>

#include <algorithm>

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	QApplication::setApplicationName(QStringLiteral("srtview"));
	// A pixel-sized platform font would read pointSize() == -1 in
	// every derived font; normalize before any widget constructs so
	// integer points hold everywhere (MainWin's members derive
	// their fonts in their constructors).
	if (QFont f = QApplication::font(); f.pointSize() <= 0) {
		f.setPointSize(std::max(1, QFontInfo(f).pointSize()));
		QApplication::setFont(f);
	}
	MainWin w;
	w.show();

	QStringList const args = app.arguments();
	if (args.size() >= 3 && args[1] == QStringLiteral("--selftest"))
		runSelftest(&w, args[2]);
	else if (args.size() >= 2)
		w.openAny(args[1]);

	return app.exec();
}
