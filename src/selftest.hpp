// selftest.hpp -- scripted offscreen exercise of the exact code paths
// behind the keys; an external harness inspects mpv through the
// socket between steps.
#pragma once

class MainWin;
class QString;

void runSelftest(MainWin *w, const QString &video);
