// timefmt.hpp -- display formatting for cue times.  UI layer: the
// result feeds Qt paint/tooltip APIs directly, hence QString.
#pragma once

#include <QString>

// "m:ss", "h:mm:ss", optionally with ".mmm".
QString fmtTime(double t, bool withMs);
