// discovery.hpp -- shared player-socket derivation and file pairing.
//
// Byte-for-byte the same scheme as the srtjump script: the mpv IPC
// socket lives at $XDG_RUNTIME_DIR/srtjump/<sha256(realpath)[:16]>.sock
// so srtview, srtjump, Kate external tools and ad-hoc scripts all
// find the same player instance for a given video.
#ifndef SRTVIEW_SRC_DISCOVERY_HPP_
#define SRTVIEW_SRC_DISCOVERY_HPP_

#include <QString>

QString sockForVideo(QString const &video, QString *err);
bool socketAlive(QString const &sock);

// Stable per-video identity: sha256(realpath)[:16], the same bytes
// that name the player socket.  Empty when the path cannot resolve.
QString idForVideo(QString const &video);

// VIDEO.EXT.srt first, VIDEO.srt second.
QString srtForVideo(QString const &video, QString *err);

// Inverse: VIDEO.EXT.srt strips to an existing file; VIDEO.srt needs
// a sibling search, live players disambiguating multiple candidates.
QString videoForSrt(QString const &srt, QString *err);

#endif // SRTVIEW_SRC_DISCOVERY_HPP_
