// dbg.hpp -- dbgHop(), the SRTVIEW_DEBUG-gated diagnostic line the
// Qt layer shares (the mpv clients carry their own dbg()).
#ifndef SRTVIEW_SRC_DBG_HPP_
#define SRTVIEW_SRC_DBG_HPP_

#include <QString>

#include <cstdio>

inline void dbgHop(QString const &msg)
{
	static bool const on =
		qEnvironmentVariableIsSet("SRTVIEW_DEBUG");
	if (on) {
		std::fprintf(stderr, "srtview: %s\n", qPrintable(msg));
		std::fflush(stderr);
	}
}

#endif // SRTVIEW_SRC_DBG_HPP_
