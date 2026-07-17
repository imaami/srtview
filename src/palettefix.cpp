#include "palettefix.hpp"

#include <QPalette>
#include <QWidget>

#include <cmath>
#include <utility>

namespace {

double relLuminance(QColor const &c)
{
	auto const lin = [](double v) {
		v /= 255.0;
		return v <= 0.04045 ? v / 12.92
		                    : std::pow((v + 0.055) / 1.055, 2.4);
	};
	return 0.2126 * lin(c.red()) + 0.7152 * lin(c.green())
	     + 0.0722 * lin(c.blue());
}

double contrastRatio(QColor const &a, QColor const &b)
{
	double l1 = relLuminance(a), l2 = relLuminance(b);
	if (l1 < l2)
		std::swap(l1, l2);
	return (l1 + 0.05) / (l2 + 0.05);
}

} // namespace

void repairMenuPalette(QWidget *w)
{
	QPalette p = w->palette();
	constexpr double kMinContrast = 2.5;
	constexpr QPalette::ColorGroup groups[]{QPalette::Active,
	                                        QPalette::Inactive};
	// (background, foreground) pairs the menu bar and its popups
	// actually render with
	constexpr std::pair<QPalette::ColorRole, QPalette::ColorRole> pairs[]{
		{QPalette::Window, QPalette::ButtonText},
		{QPalette::Window, QPalette::WindowText},
		{QPalette::Base,   QPalette::Text},
	};
	bool changed = false;
	for (auto const g : groups) {
		for (auto const &[bgRole, fgRole] : pairs) {
			QColor const bg = p.color(g, bgRole);
			if (contrastRatio(bg, p.color(g, fgRole)) >= kMinContrast)
				continue;
			QColor fix = p.color(g, QPalette::WindowText);
			if (contrastRatio(bg, fix) < kMinContrast)
				fix = relLuminance(bg) < 0.5 ? QColor(0xf0, 0xf0, 0xf0)
				                             : QColor(0x10, 0x10, 0x10);
			p.setColor(g, fgRole, fix);
			changed = true;
		}
	}
	if (changed)
		w->setPalette(p);
}
