// hashq.hpp -- the Qt-side hash-to-id bridge the composition root
// and the refinery share: BLAKE2b-256 truncated into agenda::id.
#ifndef SRTVIEW_SRC_HASHQ_HPP_
#define SRTVIEW_SRC_HASHQ_HPP_

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QString>

#include <cstring>
#include <string_view>

#include "agenda.hpp"

// The leading 8 bytes of a finished BLAKE2b-256 as a pipeline id.
inline agenda::id takeId(QCryptographicHash &h)
{
	agenda::id out;
	static_assert(sizeof out.b <= 32, "id exceeds BLAKE2b-256");
	std::memcpy(out.b.data(), h.result().constData(), sizeof out.b);
	return out;
}

// vault's injected H8 over arbitrary bytes: the same BLAKE2b-256
// head every pipeline id uses.
inline agenda::id hash8(std::string_view s)
{
	QCryptographicHash h(QCryptographicHash::Blake2b_256);
	h.addData(QByteArrayView(s.data(), qsizetype(s.size())));
	return takeId(h);
}

// A semantic source is the (video, subtitle) pair: video-only
// identity collapsed alternate transcripts of one video, subtitle-
// only identity collapsed videos sharing one transcript -- both
// shipped, both wrong in mirror image.  Built from the two content
// identities (Ident's byte hashes), never from paths; an
// unresolvable video falls back to the subtitle alone.
inline agenda::id semanticSourceId(QString const &videoId,
                            agenda::id subtitles)
{
	if (!subtitles || videoId.isEmpty())
		return subtitles;
	return hash8("semantic-source-v1\n" + videoId.toStdString()
	             + '\n' + subtitles.hex());
}

#endif // SRTVIEW_SRC_HASHQ_HPP_
