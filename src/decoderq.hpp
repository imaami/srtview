// decoderq.hpp -- the Qt face of the pure media decoder: QString
// paths in, QImage frames out, cached-PNG thumbs bridged into the
// pure compare type.  A concrete inline shim rather than a template:
// the decoder has one consumer and no type axis to vary over; all
// actual functionality lives in decoder.hpp/cpp (std C++23 + FFmpeg,
// no Qt).
#ifndef SRTVIEW_SRC_DECODERQ_HPP_
#define SRTVIEW_SRC_DECODERQ_HPP_

#include <QFile>
#include <QImage>
#include <QString>

#include <cstring>

#include "decoder.hpp"

class DecoderQ
{
public:
	bool open(QString const &path)
	{
		if (!m_d.open(QFile::encodeName(path).toStdString()))
			return false;
		m_path = path;
		return true;
	}

	void close()
	{
		m_d.close();
		m_path.clear();
	}

	QString const &path() const { return m_path; }

	bool thumbAt(qint64 ms, media::thumb &out)
	{
		return m_d.thumb_at(ms, out);
	}

	bool frameAt(qint64 ms, QImage &out)
	{
		media::frame f;
		if (!m_d.frame_at(ms, f))
			return false;
		out = QImage(f.rgb.data(), f.width, f.height,
		             3 * f.width, QImage::Format_RGB888).copy();
		return true;
	}

	// A cached pick PNG rendered into the pure compare type.
	static media::thumb toThumb(QImage const &img)
	{
		QImage const t = img.scaled(media::thumb_w,
			media::thumb_h, Qt::IgnoreAspectRatio,
			Qt::SmoothTransformation)
			.convertToFormat(QImage::Format_Grayscale8);
		media::thumb out;
		for (int y = 0; y < media::thumb_h; ++y)
			std::memcpy(out.px.data()
			            + std::size_t(y) * media::thumb_w,
			            t.constScanLine(y), media::thumb_w);
		return out;
	}

private:
	media::decoder m_d;
	QString        m_path;
};

#endif // SRTVIEW_SRC_DECODERQ_HPP_
