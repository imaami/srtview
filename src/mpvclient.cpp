// mpvclient.cpp -- see mpvclient.hpp.
#include "mpvclient.hpp"

#include <QFile>

#include <cstdio>

namespace {

bool debugEnabled()
{
	static bool const on = qEnvironmentVariableIsSet("SRTVIEW_DEBUG");
	return on;
}

} // namespace

QString mpvQuote(QString s)
{
	s.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
	s.replace(QLatin1Char('"'), QStringLiteral("\\\""));
	return QLatin1Char('"') + s + QLatin1Char('"');
}

mpv_client_base::mpv_client_base()
{
	// Children, so a derived client moved to a worker thread takes
	// its process and socket along.
	m_proc.setParent(this);
	m_conn.setParent(this);
	m_clock.start();
}

bool mpv_client_base::connectedNow() const
{
	return m_conn.state() == QLocalSocket::ConnectedState;
}

qint64 mpv_client_base::msecsSinceRx() const
{
	return m_clock.elapsed() - m_lastRx;
}

void mpv_client_base::note(QString const &msg) const
{
	std::fprintf(stderr, "srtview[%9.3f]: %s\n",
	             m_clock.elapsed() / 1000.0, qPrintable(msg));
	std::fflush(stderr);
}

void mpv_client_base::dbg(QString const &msg) const
{
	if (debugEnabled())
		note(msg);
}

void mpv_client_base::startProcess(QStringList const &args)
{
	m_proc.setProcessChannelMode(QProcess::ForwardedChannels);
	m_proc.setProgram(QStringLiteral("mpv"));
	m_proc.setArguments(args);
	dbg(QStringLiteral("spawning mpv, socket %1").arg(m_sockPath));
	m_proc.start();
}

bool mpv_client_base::connectSock(int waitMs)
{
	if (m_sockPath.isEmpty())
		return false;
	m_conn.abort();
	m_inbuf.clear();
	m_conn.connectToServer(m_sockPath);
	if (waitMs <= 0)
		return connectedNow();
	if (!m_conn.waitForConnected(waitMs))
		return false;
	m_lastRx = m_clock.elapsed();        // fresh connection, fresh clock
	dbg(QStringLiteral("connected to %1").arg(m_sockPath));
	return true;
}

bool mpv_client_base::writeLine(QString const &line)
{
	dbg(QStringLiteral("tx: ") + line);
	m_conn.write(line.toUtf8() + '\n');
	m_conn.flush();
	// flush() usually drains the whole buffer; only wait if the
	// kernel pushed back.
	return m_conn.bytesToWrite() == 0
	    || m_conn.waitForBytesWritten(500);
}

void mpv_client_base::writeRaw(QByteArray const &raw)
{
	m_conn.write(raw);
	m_conn.flush();
}

QByteArray mpv_client_base::nextLine()
{
	QByteArray const chunk = m_conn.readAll();
	if (!chunk.isEmpty()) {
		m_inbuf += chunk;
		m_lastRx = m_clock.elapsed();
		dbg(QStringLiteral("rx %1 bytes: %2").arg(chunk.size())
		    .arg(QString::fromUtf8(chunk.left(120)).trimmed()));
	}
	qsizetype const nl = m_inbuf.indexOf('\n');
	if (nl < 0)
		return {};
	QByteArray line = m_inbuf.left(nl);
	m_inbuf.remove(0, nl + 1);
	return line;
}

void mpv_client_base::teardown(bool quitPlayer)
{
	if (quitPlayer && m_proc.state() == QProcess::Running) {
		if (connectedNow()) {
			m_conn.write("quit\n");
			m_conn.flush();
		}
		if (!m_proc.waitForFinished(1500))
			m_proc.kill();
		QFile::remove(m_sockPath);
	}
	m_conn.abort();
	m_inbuf.clear();
}
