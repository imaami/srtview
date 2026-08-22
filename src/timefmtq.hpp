// timefmtq.hpp -- fmt_time() for Qt paint and tooltip APIs.
#ifndef SRTVIEW_SRC_TIMEFMTQ_HPP_
#define SRTVIEW_SRC_TIMEFMTQ_HPP_

#include <QString>

#include "timefmt.hpp"

inline QString fmtTime(double t, bool withMs)
{
	return QString::fromStdString(fmt_time(t, withMs));
}

#endif // SRTVIEW_SRC_TIMEFMTQ_HPP_
