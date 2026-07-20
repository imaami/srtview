// decoder.hpp -- persistent libav decode context for one video:
// exact frame extraction in-process, no player round-trips.
// Standard C++23 + FFmpeg only, no Qt; the grabber bridges Qt types
// at its own boundary (decoderq.hpp).
//
// The demuxer and decoder stay open across extractions, seeks land
// on the preceding keyframe and decode forward, and nearby forward
// targets skip the seek entirely (bisection revisits neighborhoods).
// Probes only ever become 64x36 grayscale compare thumbs straight
// out of swscale; a full RGB frame is produced solely for a pick
// that is about to be encoded.  Blocking by design: lives on the
// grabber's worker thread.
#ifndef SRTVIEW_SRC_DECODER_HPP_
#define SRTVIEW_SRC_DECODER_HPP_

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace media {

inline constexpr int thumb_w = 64;
inline constexpr int thumb_h = 36;

// The comparison currency: a tiny grayscale rendering of a frame.
struct thumb {
	std::array<std::uint8_t, std::size_t(thumb_w) * thumb_h> px{};
};

// Same content?  Mean absolute difference against a per-pixel
// limit; downscaling has already averaged compression noise away.
bool same(thumb const &a, thumb const &b, int mean_limit);

// One full frame as tightly packed RGB24.
struct frame {
	std::vector<std::uint8_t> rgb;
	int width = 0;
	int height = 0;
};

class decoder
{
public:
	decoder() = default;
	~decoder();

	decoder(decoder const &) = delete;
	decoder &operator=(decoder const &) = delete;

	bool open(std::string const &path);
	void close();
	bool is_open() const { return m_fmt != nullptr; }
	std::string const &path() const { return m_path; }

	// The frame shown at ms (clamped to the last frame past EOF).
	bool thumb_at(std::int64_t ms, thumb &out);
	bool frame_at(std::int64_t ms, frame &out);

private:
	bool decode_to(std::int64_t ms);     // target frame into m_have
	bool next_frame();                   // shift m_ahead into m_have

	std::string      m_path;
	AVFormatContext *m_fmt = nullptr;
	AVCodecContext  *m_dec = nullptr;
	AVFrame         *m_have = nullptr;   // covers the target
	AVFrame         *m_ahead = nullptr;  // first frame beyond it
	AVPacket        *m_pkt = nullptr;
	SwsContext      *m_sws_thumb = nullptr;
	SwsContext      *m_sws_full = nullptr;
	std::int64_t     m_have_pts = -1;    // ms
	std::int64_t     m_ahead_pts = -1;   // ms
	int              m_stream = -1;
	bool             m_draining = false;
};

} // namespace media

#endif // SRTVIEW_SRC_DECODER_HPP_
