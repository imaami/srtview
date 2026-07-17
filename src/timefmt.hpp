// timefmt.hpp -- display formatting for cue times.  UI layer: the
// result feeds Qt paint/tooltip APIs directly, hence QString.
#ifndef SRTVIEW_SRC_TIMEFMT_HPP_
#define SRTVIEW_SRC_TIMEFMT_HPP_

#include <QString>

// "m:ss", "h:mm:ss", optionally with ".mmm".
QString fmtTime(double t, bool withMs);

#endif // SRTVIEW_SRC_TIMEFMT_HPP_
