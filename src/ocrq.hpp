// ocrq.hpp -- the Qt face of the OCR pipeline, and deliberately
// nothing but machinery: the UI constructs it, wires it once, and
// drains finished readings out of the event loop; the grabber's
// worker feeds it pick frames through pick_sink; the scribe's
// worker performs them.  No current-video mirror, no posted-set
// bookkeeping, no filesystem access on the UI thread --
// coordination state on the UI side is how 236 picks got stranded
// once.  SRTVIEW_OCR=0 constructs nothing at all: no model load,
// no worker thread.  A concrete inline shim like DecoderQ: one
// consumer, no type axis.
#ifndef SRTVIEW_SRC_OCRQ_HPP_
#define SRTVIEW_SRC_OCRQ_HPP_

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QMetaObject>
#include <QObject>
#include <QString>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "archive.hpp"
#include "grabber.hpp"
#include "lector.hpp"
#include "scribe.hpp"

// Told about finished readings, queued in the listener's thread.
struct ocr_listener {
	virtual void ocrReady() = 0;

protected:
	~ocr_listener() = default;
};

// One video's share of the corpus reading plan, Qt-side: cue
// starts in seconds, converted at the boundary.
struct ocr_feed {
	QString       path, id;
	QList<double> times;
};

class OcrQ : public pick_sink
{
public:
	OcrQ()
	{
		if (qEnvironmentVariable("SRTVIEW_OCR")
		    != QStringLiteral("0"))
			m_s.emplace(cacheRoot().toStdString(),
			            &OcrQ::poke, this);
	}

	// Wire before the grabber can feed; ctx's thread receives.
	void setListener(QObject *ctx, ocr_listener *l)
	{
		m_ctx = ctx;
		m_l = l;
	}

	// pick_sink: the grabber's worker announces a pick frame.
	// Conversions plus one short-lock queue push, nothing else.
	void pickReady(QString const &path, QString const &id,
	               qint64 ms, bool rush) override
	{
		if (m_s && ms >= 0)
			m_s->desk.post(req(path, id, ms), rush);
	}

	// The corpus reads itself: handed to the scribe's plan, one
	// reading at a time whenever demand runs dry, the pixels held
	// in memory for exactly the duration of each read -- nothing
	// on this path ever touches the disk but the text slots.
	// Inert when the reader is off, so SRTVIEW_OCR=0 costs
	// nothing at all.
	void feed(QList<ocr_feed> const &feeds)
	{
		if (!m_s)
			return;
		std::vector<ocr::feed> plan;
		plan.reserve(std::size_t(feeds.size()));
		for (ocr_feed const &f : feeds) {
			ocr::feed of;
			of.proto = req(f.path, f.id, 0);
			of.times.reserve(std::size_t(f.times.size()));
			for (double const t : f.times)
				of.times.push_back(
					qint64(t * 1000.0 + 0.5));
			plan.push_back(std::move(of));
		}
		m_s->desk.plan(std::move(plan));
	}

	// The shown video's remainder first.
	void prefer(QString const &id)
	{
		if (m_s)
			m_s->desk.prefer(id.toStdString());
	}

	// True while the corpus reading plan still has moments to
	// perform.  The terms pipeline waits this out: windows cut
	// before the frame story completes must not stage or adopt.
	// With the reader off it is never true, so terms flow at once.
	bool reading()
	{
		return m_s && m_s->desk.planning();
	}

	std::vector<ocr::note> drain()
	{
		return m_s ? m_s->desk.drain()
		           : std::vector<ocr::note>();
	}

	void stop()
	{
		if (m_s) {
			m_s->read.wave();    // abandon a read in flight
			m_s->desk.stop();
		}
	}

private:
	// The opportunistic knobs: auto segmentation proved clean on
	// slides and refuses moving noise in the phase-1 acceptance;
	// 2x is safety margin for small screencast text.  Revisit
	// with corpus evidence via ocrview before ingestion leans on
	// this.  The language is no knob here: the lector walks the
	// reader's default ladder, and the label below records what
	// actually loaded.
	static constexpr ocr::layout  kLay   = ocr::layout::any;
	static constexpr std::uint8_t kScale = 2;

	// The label the archive stamps slots with: the language that
	// actually loaded plus the content identity of its
	// traineddata, so a swapped or upgraded model -- or the
	// ladder resolving differently -- re-earns its slots exactly
	// like a library upgrade does.  An unhashable model marks
	// itself; a downed engine only errs and errors are never
	// cached.  Called lazily by the archive on the scribe's
	// worker at first use: the model loads (lector builds its
	// tess on demand) and the file is read and hashed off the UI
	// thread, keeping this header's no-UI-IO claim true.
	static std::string labelOf(void *ctx)
	{
		auto *const read = static_cast<ocr::lector *>(ctx);
		QString dir = QString::fromStdString(read->datapath());
		if (!dir.isEmpty() && !dir.endsWith(QLatin1Char('/')))
			dir += QLatin1Char('/');
		QString const lang = QString::fromUtf8(
			read->lang().data(),
			qsizetype(read->lang().size()));
		QFile f(dir + lang + QStringLiteral(".traineddata"));
		QCryptographicHash h(QCryptographicHash::Blake2b_256);
		QByteArray tag;
		if (!dir.isEmpty() && !lang.isEmpty()
		    && f.open(QIODevice::ReadOnly) && h.addData(&f))
			tag = h.result().left(8).toHex();
		std::string out = read->lang().empty()
			? std::string("down")
			: std::string(read->lang());
		out += ' ';
		out += tag.isEmpty() ? "unhashed" : tag.constData();
		return out;
	}

	static QString cacheRoot()
	{
		QString base = qEnvironmentVariable("XDG_CACHE_HOME");
		if (base.isEmpty())
			base = QDir::homePath() + QStringLiteral("/.cache");
		return base + QStringLiteral("/srtview/ocr");
	}

	static ocr::request req(QString const &path, QString const &id,
	                        qint64 ms)
	{
		ocr::request r;
		r.video = QFile::encodeName(path).toStdString();
		r.id = id.toStdString();
		r.ms = ms;
		r.opts.lay = kLay;
		r.scale = kScale;
		return r;
	}

	// The scribe's poke, on its worker thread: bounce into the
	// listener's thread as a queued call.
	static void poke(void *self) noexcept
	{
		auto *const q = static_cast<OcrQ *>(self);
		if (q->m_ctx && q->m_l)
			QMetaObject::invokeMethod(q->m_ctx,
				[l = q->m_l] { l->ocrReady(); },
				Qt::QueuedConnection);
	}

	// The whole pipeline or nothing; the scribe last, so it
	// joins first while everything it can touch still lives.
	struct stack {
		stack(std::string root, void (*pk)(void *) noexcept,
		      void *cx)
			: back(std::move(root), &OcrQ::labelOf, &read,
			       read),
			  desk(back, pk, cx) {}

		ocr::lector                            read;
		ocr::archive<ocr::lector>              back;
		ocr::scribe<ocr::archive<ocr::lector>> desk;
	};

	QObject             *m_ctx = nullptr;
	ocr_listener        *m_l = nullptr;
	std::optional<stack> m_s;   // last: destroyed first
};

#endif // SRTVIEW_SRC_OCRQ_HPP_
