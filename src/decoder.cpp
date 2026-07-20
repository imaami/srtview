// decoder.cpp -- see decoder.hpp.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
}

#include <cstdlib>
#include <ranges>
#include <utility>

#include "decoder.hpp"

namespace media {

namespace {

// Forward targets within this window continue decoding instead of
// seeking: bisection revisits neighborhoods constantly.
constexpr std::int64_t k_near_ms = 3000;

std::int64_t to_ms(std::int64_t pts, AVRational tb)
{
	return pts == AV_NOPTS_VALUE
	       ? -1 : av_rescale_q(pts, tb, AVRational{1, 1000});
}

} // namespace

bool same(thumb const &a, thumb const &b, int mean_limit)
{
	long sum = 0;
	for (auto const [x, y] : std::views::zip(a.px, b.px))
		sum += std::abs(int(x) - int(y));
	return sum < long(mean_limit) * long(a.px.size());
}

decoder::~decoder()
{
	close();
}

bool decoder::open(std::string const &path)
{
	close();
	if (avformat_open_input(&m_fmt, path.c_str(),
	                        nullptr, nullptr) < 0)
		return false;
	if (avformat_find_stream_info(m_fmt, nullptr) < 0) {
		close();
		return false;
	}
	AVCodec const *codec = nullptr;
	m_stream = av_find_best_stream(m_fmt, AVMEDIA_TYPE_VIDEO,
	                               -1, -1, &codec, 0);
	if (m_stream < 0 || !codec) {
		close();
		return false;
	}
	m_dec = avcodec_alloc_context3(codec);
	if (!m_dec
	    || avcodec_parameters_to_context(m_dec,
		m_fmt->streams[m_stream]->codecpar) < 0) {
		close();
		return false;
	}
	m_dec->thread_count = 0;             // auto
	if (avcodec_open2(m_dec, codec, nullptr) < 0) {
		close();
		return false;
	}
	m_have = av_frame_alloc();
	m_ahead = av_frame_alloc();
	m_pkt = av_packet_alloc();
	if (!m_have || !m_ahead || !m_pkt) {
		close();
		return false;
	}
	m_path = path;
	return true;
}

void decoder::close()
{
	sws_freeContext(m_sws_thumb);
	sws_freeContext(m_sws_full);
	m_sws_thumb = nullptr;
	m_sws_full = nullptr;
	av_packet_free(&m_pkt);
	av_frame_free(&m_have);
	av_frame_free(&m_ahead);
	if (m_dec)
		avcodec_free_context(&m_dec);
	if (m_fmt)
		avformat_close_input(&m_fmt);
	m_path.clear();
	m_have_pts = -1;
	m_ahead_pts = -1;
	m_stream = -1;
	m_draining = false;
}

// One more decoded frame: the lookahead becomes the candidate, the
// next frame out of the codec becomes the lookahead.
bool decoder::next_frame()
{
	if (m_ahead_pts >= 0) {
		std::swap(m_have, m_ahead);
		m_have_pts = m_ahead_pts;
		m_ahead_pts = -1;
	}
	while (true) {
		int const rc = avcodec_receive_frame(m_dec, m_ahead);
		if (rc == 0) {
			std::int64_t const ms = to_ms(
				m_ahead->best_effort_timestamp,
				m_fmt->streams[m_stream]->time_base);
			m_ahead_pts = ms >= 0 ? ms : m_have_pts + 1;
			return true;
		}
		if (rc != AVERROR(EAGAIN))
			return false;                // EOF or damage
		int got = av_read_frame(m_fmt, m_pkt);
		while (got >= 0 && m_pkt->stream_index != m_stream) {
			av_packet_unref(m_pkt);
			got = av_read_frame(m_fmt, m_pkt);
		}
		if (got < 0) {
			if (m_draining)
				return false;
			m_draining = true;
			avcodec_send_packet(m_dec, nullptr);
			continue;
		}
		avcodec_send_packet(m_dec, m_pkt);
		av_packet_unref(m_pkt);
	}
}

bool decoder::decode_to(std::int64_t ms)
{
	if (!m_fmt)
		return false;
	if (m_have_pts >= 0 && ms >= m_have_pts
	    && (m_ahead_pts < 0 ? ms == m_have_pts : ms < m_ahead_pts))
		return true;                     // already covered
	bool const near = m_have_pts >= 0 && ms >= m_have_pts
	               && ms - m_have_pts < k_near_ms;
	if (!near) {
		if (av_seek_frame(m_fmt, m_stream,
		                  av_rescale_q(ms, AVRational{1, 1000},
			m_fmt->streams[m_stream]->time_base),
		                  AVSEEK_FLAG_BACKWARD) < 0)
			return false;
		avcodec_flush_buffers(m_dec);
		m_have_pts = -1;
		m_ahead_pts = -1;
		m_draining = false;
	}
	while (!(m_ahead_pts >= 0 && m_ahead_pts > ms))
		if (!next_frame())
			break;                   // EOF: clamp to the last
	if (m_have_pts < 0 && m_ahead_pts >= 0) {
		std::swap(m_have, m_ahead);      // target precedes the
		m_have_pts = m_ahead_pts;        // first decodable frame
		m_ahead_pts = -1;
	}
	return m_have_pts >= 0;
}

bool decoder::thumb_at(std::int64_t ms, thumb &out)
{
	if (!decode_to(ms))
		return false;
	m_sws_thumb = sws_getCachedContext(m_sws_thumb,
		m_have->width, m_have->height,
		AVPixelFormat(m_have->format),
		thumb_w, thumb_h, AV_PIX_FMT_GRAY8, SWS_AREA,
		nullptr, nullptr, nullptr);
	if (!m_sws_thumb)
		return false;
	std::uint8_t *dst[4] = {out.px.data()};
	int const stride[4] = {thumb_w};
	return sws_scale(m_sws_thumb, m_have->data, m_have->linesize,
	                 0, m_have->height, dst, stride) > 0;
}

bool decoder::frame_at(std::int64_t ms, frame &out)
{
	if (!decode_to(ms))
		return false;
	m_sws_full = sws_getCachedContext(m_sws_full,
		m_have->width, m_have->height,
		AVPixelFormat(m_have->format),
		m_have->width, m_have->height, AV_PIX_FMT_RGB24,
		SWS_BILINEAR, nullptr, nullptr, nullptr);
	if (!m_sws_full)
		return false;
	out.width = m_have->width;
	out.height = m_have->height;
	out.rgb.resize(std::size_t(3) * out.width * out.height);
	std::uint8_t *dst[4] = {out.rgb.data()};
	int const stride[4] = {3 * out.width};
	return sws_scale(m_sws_full, m_have->data, m_have->linesize,
	                 0, m_have->height, dst, stride) > 0;
}

} // namespace media
