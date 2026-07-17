#include "prefs.hpp"

#include <QFileInfo>

namespace {

auto const kLastDir = QStringLiteral("session/lastDir");
auto const kRecent  = QStringLiteral("session/recentFiles");
auto const kSearch  = QStringLiteral("search/history");

} // namespace

QString Prefs::lastDir() const
{
	return m_s.value(kLastDir).toString();
}

void Prefs::setLastDir(QString const &dir)
{
	if (dir.isEmpty())
		return;
	m_s.setValue(kLastDir, dir);
}

QStringList Prefs::recentFiles() const
{
	return m_s.value(kRecent).toStringList();
}

void Prefs::addRecentFile(QString const &path)
{
	QString const abs = QFileInfo(path).absoluteFilePath();
	QStringList files = recentFiles();
	files.removeAll(abs);
	files.prepend(abs);
	while (files.size() > kMaxRecent)
		files.removeLast();
	m_s.setValue(kRecent, files);
}

QStringList Prefs::searchHistory() const
{
	return m_s.value(kSearch).toStringList();
}

void Prefs::addSearch(QString const &pattern)
{
	if (pattern.isEmpty())
		return;
	QStringList h = searchHistory();
	h.removeAll(pattern);
	h.prepend(pattern);
	m_s.setValue(kSearch, h);
}
