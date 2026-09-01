// exporter.hpp -- corpus export: one directory per topic holding a
// Markdown digest and the frame picks of every matching cue.
//
// The export is a deterministic build artifact of (topic file,
// videos, srts, frame cache): re-running only adds what is missing.
// Hits whose frames are not yet grabbed are marked pending in the
// digest and enqueued on the grabber; the caller re-runs on the
// grabber's progress and idle signals until nothing is queued, and
// the user's own prose lives outside the generated files, which
// stay regenerable.
#ifndef SRTVIEW_SRC_EXPORTER_HPP_
#define SRTVIEW_SRC_EXPORTER_HPP_

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <vector>

#include "srt.hpp"
#include "topics.hpp"

class Grabber;

namespace exporter {

struct source {
	QString     video, srt, id;  // fully resolved (srt derivation
	                             // included) + content id
	QStringList topics;          // topic names scoped to this video
};

// A parsed srt with its cue texts already rendered (tags consumed,
// like the reading view).  One per srt file per session: the corpus
// search and every export pass share the same copy, loaded on first
// touch.  cueLine carries each cue block's first visible line in
// the srt file (counter, or timecode when the counter is absent),
// 1-based and strictly increasing -- the hits export cites it.
struct transcript {
	std::vector<srt::cue> cues;
	QStringList           lines;
	std::vector<int>      cueLine;
};

using transcripts = QHash<QString, transcript>;

transcript const &load(transcripts &cache, QString const &srtPath);

// The regex-locations export: line one carries the effective
// pattern text; every match of @a re across every transcript makes
// one entry line of five tab-separated fields -- playlist index,
// cue index (both 0-based), cue start time, the matched cue's
// 1-based block line in the srt file (monotonic per video; a video
// whose transcript has no file to cite would carry 0 throughout),
// and the match with up to three context words on the left and six
// on the right, ellipses marking a cue that continues past the
// span, whitespace runs squeezed to one space.  Every field but
// the last is right-justified to its column's widest member:
// integers space-padded, stamps zero-extended to the latest
// stamp's colon count (" 0:00:09.250" under "12:30:01.000" --
// the leftmost number stays unpadded, so no octal-looking zero)
// then space-padded.  srts holds one transcript path per playlist
// entry, in playlist order, empty for entries without one.
// UTF-8, Unix newlines, trailing newline included.
QByteArray hits(QString const &pattern, QRegularExpression const &re,
                QStringList const &srts, transcripts &cache);

struct stats {
	int topics = 0;              // topic digests written
	int hits   = 0;              // matching cues found
	int framed = 0;              // hits with picks on disk
	int queued = 0;              // hits pending on the grabber
};

stats run(topics::doc const &corpus, QList<source> const &videos,
          Grabber &grab, QString const &outDir, transcripts &cache);

} // namespace exporter

#endif // SRTVIEW_SRC_EXPORTER_HPP_
