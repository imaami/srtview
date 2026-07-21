// discovery.hpp -- shared player-socket derivation and file pairing.
//
// One instance for the app: the runtime directory is resolved and
// created once at construction, and per-video identities and derived
// subtitle paths are cached on success (files do not move
// mid-session; misses stay uncached so files may appear later).
// Standard C++23 interface; the .cpp walks with bare POSIX calls
// (realpath, stat, opendir -- byte paths deserve byte tools, and
// std::filesystem's path objects are the very churn this class
// exists to remove) plus one Qt include: QCryptographicHash,
// because the standard library still cannot hash bytes.
//
// The mpv IPC socket scheme is byte-for-byte the srtjump script's:
//   $XDG_RUNTIME_DIR/srtjump/<first 16 hex of blake2b-256(realpath)>.sock
// (srtjump side: cksum -a blake2b -l 256 --untagged), so srtview,
// srtjump, Kate external tools and ad-hoc scripts all drive one
// player per video.  BLAKE2b-256 replaced SHA-256 measured: 155 ns
// against 516 ns per path.
#ifndef SRTVIEW_SRC_DISCOVERY_HPP_
#define SRTVIEW_SRC_DISCOVERY_HPP_

#include <string>
#include <utility>
#include <vector>

class discovery
{
public:
	discovery();

	// Stable per-video identity: the socket-naming hash.  Empty
	// when the path cannot resolve.
	std::string id_for_video(std::string const &video);

	// The shared player socket for a video -- or any file, e.g. a
	// topic file claiming a corpus-wide player.  Empty when the
	// path cannot resolve.
	std::string sock_for_video(std::string const &video);

	// VIDEO.EXT.srt first, VIDEO.srt second; err receives the
	// story on failure.
	std::string srt_for_video(std::string const &video,
	                          std::string &err);

	// Inverse: VIDEO.EXT.srt strips to an existing file; VIDEO.srt
	// needs a sibling search, live players disambiguating multiple
	// candidates.
	std::string video_for_srt(std::string const &srt,
	                          std::string &err);

	// True if something is accepting connections on the socket.
	static bool socket_alive(std::string const &sock);

private:
	// Session-scale caches: a couple dozen videos at most, so a
	// linear scan beats any hashed container on size and speed.
	using cache = std::vector<std::pair<std::string, std::string>>;

	static std::string const *find_in(cache const &c,
	                                  std::string const &key);

	cache m_ids;
	cache m_srts;
	std::string m_sock_prefix;           // "<runtime dir>/srtjump/"
};

#endif // SRTVIEW_SRC_DISCOVERY_HPP_
