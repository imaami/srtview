#include "timefmt.hpp"

#include <cmath>

QString fmtTime(double t, bool withMs)
{
	int ms = int(std::lround(t * 1000.0));
	int h = ms / 3600000; ms %= 3600000;
	int m = ms / 60000;   ms %= 60000;
	int s = ms / 1000;    ms %= 1000;
	QString out = (h > 0)
		? QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0'))
		                            .arg(s, 2, 10, QLatin1Char('0'))
		: QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
	if (withMs)
		out += QStringLiteral(".%1").arg(ms, 3, 10, QLatin1Char('0'));
	return out;
}
