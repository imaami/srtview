// mpvclient.hpp -- mpv IPC client machinery.
//
// An mpv process is driven over a local socket with commands going
// out as single raw input.conf lines (one user action per line, an
// explicit flush per send), while mpv's JSON event lines come back
// for observation and sequencing.  mpv_client_base carries the
// process, socket, receive buffer and clocks, compiled once in
// mpvclient.cpp; mpv_client<Derived> is the header-only CRTP layer
// that pumps buffered event lines into the derived class's
// onEvent(QJsonObject).  The viewing link (mpvlink) builds on it;
// the frame grabber used to as well, until it moved to in-process
// libav decoding (decoder.hpp).
#ifndef SRTVIEW_SRC_MPVCLIENT_HPP_
#define SRTVIEW_SRC_MPVCLIENT_HPP_

#include "crtp.hpp"

#include <QByteArray>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

// Raw input.conf quoting for paths embedded in command lines.
QString mpvQuote(QString s);

class mpv_client_base : public QObject
{
public:
	bool connectedNow() const;
	qint64 msecsSinceRx() const;

	// Unconditional for state flips; verbose traffic only with
	// SRTVIEW_DEBUG set.
	void note(QString const &msg) const;
	void dbg(QString const &msg) const;

protected:
	mpv_client_base();

	// Start mpv; args must carry the --input-ipc-server flag.
	// Channels are forwarded, not captured: a captured pipe nobody
	// drains wedges mpv once graphics-stack warnings fill it.
	void startProcess(QStringList const &args);

	// One connection attempt to m_sockPath; waitMs 0 initiates
	// without waiting (poll or use the connected signal).
	bool connectSock(int waitMs);

	// Raw line out, explicit flush.
	bool writeLine(QString const &line);
	void writeRaw(QByteArray const &raw);

	// Next complete buffered line, or a null array.
	QByteArray nextLine();

	// Drop the socket and receive buffer; when quitPlayer, also ask
	// the player to quit (kill on timeout) and remove its socket.
	void teardown(bool quitPlayer);

	QProcess &proc() { return m_proc; }
	QLocalSocket &sock() { return m_conn; }
	QElapsedTimer const &clock() const { return m_clock; }

	QString m_sockPath;

private:
	QProcess      m_proc;
	QLocalSocket  m_conn;
	QByteArray    m_inbuf;
	QElapsedTimer m_clock;
	qint64        m_lastRx = 0;
};

template <typename Derived>
class mpv_client : public mpv_client_base,
                   public crtp<Derived, mpv_client>
{
protected:
	// Pump buffered JSON event lines into the derived handler.
	void dispatch()
	{
		for (QByteArray l = nextLine(); !l.isNull(); l = nextLine()) {
			QJsonDocument const d = QJsonDocument::fromJson(l);
			if (d.isObject())
				this->impl().onEvent(d.object());
		}
	}
};

#endif // SRTVIEW_SRC_MPVCLIENT_HPP_
