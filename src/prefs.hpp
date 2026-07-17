// prefs.hpp -- persistent user state (QSettings-backed): last browsed
// directory and the recent-files list.
#ifndef SRTVIEW_SRC_PREFS_HPP_
#define SRTVIEW_SRC_PREFS_HPP_

#include <QSettings>
#include <QString>
#include <QStringList>

class Prefs
{
public:
	QString lastDir() const;
	void setLastDir(QString const &dir);

	QStringList recentFiles() const;
	void addRecentFile(QString const &path);

private:
	static constexpr int kMaxRecent = 20;

	mutable QSettings m_s{QStringLiteral("srtview"),
	                      QStringLiteral("srtview")};
};

#endif // SRTVIEW_SRC_PREFS_HPP_
