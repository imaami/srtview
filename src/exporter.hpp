// exporter.hpp -- corpus export: one directory per topic holding a
// Markdown digest and the frame picks of every matching cue.
//
// The export is a deterministic build artifact of (topic file,
// videos, srts, frame cache): re-running only adds what is missing.
// Hits whose frames are not yet grabbed are marked pending in the
// digest and enqueued on the grabber; the caller re-runs on
// grabsIdle() until nothing is queued, and the user's own prose
// lives outside the generated files, which stay regenerable.
#ifndef SRTVIEW_SRC_EXPORTER_HPP_
#define SRTVIEW_SRC_EXPORTER_HPP_

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <vector>

#include "srt.hpp"
#include "topics.hpp"

class Grabber;

namespace exporter {

struct source {
	QString     video, srt, id;  // resolved paths + discovery id
	QStringList topics;          // topic names scoped to this video
};

// A parsed srt with its cue texts already rendered (tags consumed,
// like the reading view).  One per srt file per session: the corpus
// search and every export pass share the same copy, loaded on first
// touch.
struct transcript {
	std::vector<srt::cue> cues;
	QStringList           lines;
};

using transcripts = QHash<QString, transcript>;

transcript const &load(transcripts &cache, QString const &srtPath);

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
