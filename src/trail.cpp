#include "trail.hpp"

#include <QByteArray>
#include <QDataStream>
#include <QIODevice>

namespace {

// Deterministic wire form: equal transitions must be byte-equal so
// the tree's adoption rule can recognize a retraced action.
QByteArray encode(trail_step const &s)
{
	QByteArray out;
	QDataStream w(&out, QIODevice::WriteOnly);
	w.setVersion(QDataStream::Qt_6_0);
	w << qint32(s.k);
	switch (s.k) {
	case trail_step::search_text:
		w << s.textBefore << s.textAfter;
		break;
	case trail_step::search_jump:
		w << qint32(s.curBefore) << qint32(s.curAfter);
		break;
	case trail_step::video_jump:
	case trail_step::side_seek:
		w << s.timeBefore << s.timeAfter;
		break;
	}
	return out;
}

trail_step decode(void const *data, std::size_t size)
{
	QByteArray const raw = QByteArray::fromRawData(
		static_cast<char const *>(data), qsizetype(size));
	QDataStream r(raw);
	r.setVersion(QDataStream::Qt_6_0);
	qint32 k = 0;
	r >> k;
	trail_step s;
	s.k = trail_step::kind(k);
	switch (s.k) {
	case trail_step::search_text:
		r >> s.textBefore >> s.textAfter;
		break;
	case trail_step::search_jump: {
		qint32 a = 0, b = 0;
		r >> a >> b;
		s.curBefore = a;
		s.curAfter = b;
		break;
	}
	case trail_step::video_jump:
	case trail_step::side_seek:
		r >> s.timeBefore >> s.timeAfter;
		break;
	}
	return s;
}

} // namespace

void Trail::act(trail_step const &s)
{
	if (m_applying)
		return;
	QByteArray const b = encode(s);
	fundo_act(&m_f, b.constData(), std::size_t(b.size()));
}

std::optional<trail_step> Trail::undo()
{
	std::size_t n = 0;
	void const *p = fundo_undo(&m_f, &n);
	if (!p)
		return std::nullopt;
	return decode(p, n);
}

std::optional<trail_step> Trail::redo()
{
	std::size_t n = 0;
	void const *p = fundo_redo(&m_f, &n);
	if (!p)
		return std::nullopt;
	return decode(p, n);
}
