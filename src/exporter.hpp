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

#include "topics.hpp"

#include <QList>
#include <QString>
#include <QStringList>

class Grabber;

namespace exporter {

struct source {
	QString     video, srt, id;  // resolved paths + discovery id
	QStringList topics;          // topic names scoped to this video
};

struct stats {
	int topics = 0;              // topic digests written
	int hits   = 0;              // matching cues found
	int framed = 0;              // hits with picks on disk
	int queued = 0;              // hits pending on the grabber
};

stats run(topics::doc const &corpus, QList<source> const &videos,
          Grabber &grab, QString const &outDir);

} // namespace exporter

#endif // SRTVIEW_SRC_EXPORTER_HPP_
