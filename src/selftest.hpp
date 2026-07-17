// selftest.hpp -- scripted offscreen exercise of the exact code paths
// behind the keys; an external harness inspects mpv through the
// socket between steps.
#ifndef SRTVIEW_SRC_SELFTEST_HPP_
#define SRTVIEW_SRC_SELFTEST_HPP_

class MainWin;
class QString;

void runSelftest(MainWin *w, QString const &video);

#endif // SRTVIEW_SRC_SELFTEST_HPP_
