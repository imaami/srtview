#include "trail.hpp"

#include <QByteArray>
#include <QDataStream>
#include <QIODevice>

#include <cmath>
#include <cstdio>

namespace {

// Ignore playback drift smaller than this when deciding whether a
// jump needs a departure breadcrumb: below it the position is the
// recorded one plus observation jitter, not somewhere new.
constexpr double kDriftEpsilon = 0.5;

// Deterministic wire form: equal states must be byte-equal so the
// tree's adoption rule can recognize a retraced action.  Only the
// flagged facets are encoded -- steps not touching the (live,
// irreproducible) video position stay adoptable.
QByteArray encode(trail_step const &s)
{
	QByteArray out;
	QDataStream w(&out, QIODevice::WriteOnly);
	w.setVersion(QDataStream::Qt_6_0);
	w << quint32(s.flags);
	if (s.flags & trail_step::text)
		w << s.pattern;
	if (s.flags & trail_step::cursor)
		w << qint32(s.cur);
	if (s.flags & trail_step::video)
		w << s.time;
	return out;
}

trail_step decode(void const *data, std::size_t size)
{
	trail_step s;
	if (!size)
		return s;                    // the root: no facets recorded
	QByteArray const raw = QByteArray::fromRawData(
		static_cast<char const *>(data), qsizetype(size));
	QDataStream r(raw);
	r.setVersion(QDataStream::Qt_6_0);
	quint32 fl = 0;
	r >> fl;
	s.flags = fl;
	if (fl & trail_step::text)
		r >> s.pattern;
	if (fl & trail_step::cursor) {
		qint32 c = 0;
		r >> c;
		s.cur = c;
	}
	if (fl & trail_step::video)
		r >> s.time;
	return s;
}

} // namespace

void Trail::act(trail_step const &s)
{
	if (m_applying || !s.flags)
		return;
	if (s.flags & trail_step::video)
		m_lastVideo = s.time;
	QByteArray const b = encode(s);
	int const rc = fundo_act(&m_f, b.constData(), std::size_t(b.size()));
	if (rc)  // breadcrumb lost (OOM): playback state is unaffected,
	         // but silent loss would make the trail lie later
		std::fprintf(stderr, "srtview: undo step not recorded: %d\n",
		             rc);
}

void Trail::driftTo(double t)
{
	if (m_lastVideo && std::abs(*m_lastVideo - t) < kDriftEpsilon)
		return;
	trail_step s;
	s.flags = trail_step::video;
	s.time = t;
	act(s);
}

std::optional<trail_step> Trail::undo()
{
	std::size_t n = 0;
	void const *dep = fundo_data(fundo_at(&m_f), &n);
	if (!fundo_undo(&m_f, nullptr))
		return std::nullopt;

	trail_step out;
	out.flags = decode(dep, n).flags;

	// Resolve each departed facet to its nearest recorded ancestor
	// value; the zero-initialized defaults stand in below the root.
	unsigned need = out.flags;
	for (struct fundo_node const *a = fundo_at(&m_f); a && need;
	     a = fundo_up(a)) {
		void const *d = fundo_data(a, &n);
		trail_step const at = decode(d, n);
		unsigned const got = need & at.flags;
		if (got & trail_step::text)
			out.pattern = at.pattern;
		if (got & trail_step::cursor)
			out.cur = at.cur;
		if (got & trail_step::video)
			out.time = at.time;
		need &= ~got;
	}
	if (need & trail_step::video)
		out.flags &= ~trail_step::video; // no position ever recorded
	return out;
}

std::optional<trail_step> Trail::redo()
{
	std::size_t n = 0;
	void const *p = fundo_redo(&m_f, &n);
	if (!p)
		return std::nullopt;
	return decode(p, n);
}
