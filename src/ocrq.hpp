// ocrq.hpp -- the Qt face of the OCR pipeline: QString video paths
// and qint64 milliseconds in, drained ocr::note batches out, the
// scribe's worker-thread poke bounced into the listener's thread
// as a queued call (grab_listener's pattern).  A concrete inline
// shim like DecoderQ: one consumer, no type axis.  Owns the whole
// stack -- lector reads, archive remembers under
// $XDG_CACHE_HOME/srtview/ocr/<video id>/, scribe queues -- and
// SRTVIEW_OCR=0 leaves the desk inert.  setListener before the
// first post; setVideo, post, sweep and drain belong to the UI
// thread -- the scribe is the only thread boundary.
#ifndef SRTVIEW_SRC_OCRQ_HPP_
#define SRTVIEW_SRC_OCRQ_HPP_

#include <QDir>
#include <QFile>
#include <QHash>
#include <QMetaObject>
#include <QObject>
#include <QSet>
#include <QString>

#include <cstdint>
#include <vector>

#include "archive.hpp"
#include "lector.hpp"
#include "scribe.hpp"

// Told about finished readings, queued in the listener's thread.
struct ocr_listener {
	virtual void ocrReady() = 0;

protected:
	~ocr_listener() = default;
};

class OcrQ
{
public:
	OcrQ()
		: m_back(cacheRoot().toStdString(), "eng", m_lector),
		  m_desk(m_back, &OcrQ::poke, this)
	{
	}

	void setListener(QObject *ctx, ocr_listener *l)
	{
		m_ctx = ctx;
		m_l = l;
	}

	// The video whose jumps are being followed (set on every open).
	void setVideo(QString const &path, QString const &id)
	{
		if (path.isEmpty() || id.isEmpty())
			return;
		m_path = path;
		m_id = id;
	}

	// A jump landed at ms: read the inspected frame first.
	ocr::ticket post(qint64 ms, bool rush)
	{
		if (!m_on || m_path.isEmpty() || ms < 0)
			return 0;
		m_posted[m_id].insert(ms);
		return m_desk.post(req(m_path, m_id, ms), rush);
	}

	// Every grabbed pick of one video: the picks on disk are the
	// input queue, one post per frame per session -- the scribe
	// coalesces in-flight twins, the archive absorbs the
	// already-read.  Any video, not just the shown one: boundary
	// picks finish encoding after the user has hopped away, so
	// the caller re-offers every playlist video each time the
	// grabber drains.
	void sweep(QString const &path, QString const &id,
	           QString const &framesDir)
	{
		if (!m_on || path.isEmpty() || id.isEmpty())
			return;
		QSet<qint64> &posted = m_posted[id];
		auto const names = QDir(framesDir).entryList(
			{QStringLiteral("*.png")}, QDir::Files);
		for (QString const &name : names) {
			bool ok = false;
			qint64 const ms = name.chopped(4).toLongLong(&ok);
			if (!ok || posted.contains(ms))
				continue;
			posted.insert(ms);
			m_desk.post(req(path, id, ms), false);
		}
	}

	std::vector<ocr::note> drain() { return m_desk.drain(); }
	void stop() { m_desk.stop(); }

private:
	// The opportunistic knobs: auto segmentation proved clean on
	// slides and refuses moving noise in the phase-1 acceptance;
	// 2x is safety margin for small screencast text.  Revisit
	// with corpus evidence via ocrview before ingestion leans on
	// this.
	static constexpr ocr::layout  kLay   = ocr::layout::any;
	static constexpr std::uint8_t kScale = 2;

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

	// The scribe's poke, on the worker thread: bounce into the
	// listener's thread as a queued call.
	static void poke(void *self)
	{
		auto *const q = static_cast<OcrQ *>(self);
		if (q->m_ctx && q->m_l)
			QMetaObject::invokeMethod(q->m_ctx,
				[l = q->m_l] { l->ocrReady(); },
				Qt::QueuedConnection);
	}

	ocr::lector                            m_lector;
	ocr::archive<ocr::lector>              m_back;
	ocr::scribe<ocr::archive<ocr::lector>> m_desk;
	QString                                m_path, m_id;
	QHash<QString, QSet<qint64>>           m_posted;
	QObject                               *m_ctx = nullptr;
	ocr_listener                          *m_l = nullptr;
	bool m_on = qEnvironmentVariable("SRTVIEW_OCR")
	            != QStringLiteral("0");
};

#endif // SRTVIEW_SRC_OCRQ_HPP_
