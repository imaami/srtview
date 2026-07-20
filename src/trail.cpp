#include "trail.hpp"

#include <QByteArray>
#include <QDataStream>
#include <QIODevice>

#include <cmath>
#include <cstdio>
#include <cstring>

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

// Byte identity of a node's payload and an encoded state: the ring is
// self-authenticating -- traveling to a neighbor is valid exactly
// when the neighbor already is the state being stepped to.
bool sameState(struct fundo_node const *n, QByteArray const &b)
{
	std::size_t size = 0;
	void const *d = fundo_data(n, &size);
	return size == std::size_t(b.size())
	    && (!size || !std::memcmp(d, b.constData(), size));
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

void Trail::anchorCycle()
{
	m_cycle = fundo_at(&m_f);
	m_ringAt = nullptr;
}

Trail::hop Trail::probeHop(trail_step const &s, int dir)
{
	m_ringAt = nullptr;
	QByteArray const b = encode(s);
	struct fundo_node const *at = fundo_at(&m_f);
	if (sameState(at, b))
		return hop::stay;
	struct fundo_node const *to = dir > 0 ? fundo_next(at)
	                                      : fundo_prev(at);
	if (to && sameState(to, b))
		return hop::travel;
	if (m_cycle && (at == m_cycle || fundo_next(at)))
		m_ringAt = at;               // growth splices after this node
	return hop::grow;
}

void Trail::travelHop(int dir)
{
	fundo_travel(&m_f, dir, nullptr);
	m_ringAt = nullptr;
}

void Trail::growHop(trail_step const &s, int dir)
{
	act(s);
	if (!m_ringAt || !(s.flags & trail_step::video))
		return;
	// EINVAL from an adopted member is fine: it already sits in the
	// ring of the episode being retraced.
	(void)fundo_join(&m_f, m_ringAt, dir);
	m_ringAt = nullptr;
}

// Net outstanding travel around the current node's ring; 0 off-ring.
// While nonzero, undo unwinds travel instead of walking the tree.
int Trail::travelSlack() const
{
	struct fundo_node const *at = fundo_at(&m_f);
	struct fundo_node const *n = fundo_next(at);
	if (!n)
		return 0;
	int sum = fundo_net(at);
	for (; n != at; n = fundo_next(n))
		sum += fundo_net(n);
	return sum;
}

bool Trail::canUndo() const
{
	return travelSlack() || fundo_can_undo(&m_f);
}

bool Trail::canRedo() const
{
	struct fundo_node const *at = fundo_at(&m_f);
	return (fundo_next(at) && fundo_heading(at) < 0)
	    || fundo_can_redo(&m_f);
}

std::optional<trail_step> Trail::undo()
{
	int const slack = travelSlack();
	if (slack) {
		std::size_t n = 0;
		void const *p = fundo_travel(&m_f, slack < 0 ? 1 : -1, &n);
		return decode(p, n);
	}

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
	// A backward heading on a ring means the future lies forward:
	// replay by traveling (forward search and redo coincide here).
	struct fundo_node const *at = fundo_at(&m_f);
	if (fundo_next(at) && fundo_heading(at) < 0) {
		void const *p = fundo_travel(&m_f, 1, &n);
		return decode(p, n);
	}

	void const *p = fundo_redo(&m_f, &n);
	if (!p)
		return std::nullopt;
	return decode(p, n);
}
