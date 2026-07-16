// discovery.hpp -- shared player-socket derivation and file pairing.
//
// Byte-for-byte the same scheme as the srtjump script: the mpv IPC
// socket lives at $XDG_RUNTIME_DIR/srtjump/<sha256(realpath)[:16]>.sock
// so srtview, srtjump, Kate external tools and ad-hoc scripts all
// find the same player instance for a given video.
#pragma once

#include <QString>

QString sockForVideo(const QString &video, QString *err);
bool socketAlive(const QString &sock);

// VIDEO.EXT.srt first, VIDEO.srt second.
QString srtForVideo(const QString &video, QString *err);

// Inverse: VIDEO.EXT.srt strips to an existing file; VIDEO.srt needs
// a sibling search, live players disambiguating multiple candidates.
QString videoForSrt(const QString &srt, QString *err);
