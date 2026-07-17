// palettefix.hpp -- first aid for broken theme palettes.
//
// Some custom color schemes ship a ButtonText that vanishes against
// Window; Qt styles draw menu-bar items with ButtonText, so the menus
// become invisible while everything else looks fine.  Repairs only
// genuinely broken pairs (WCAG-ish contrast < 2.5) so sane themes
// pass through untouched.
#ifndef SRTVIEW_SRC_PALETTEFIX_HPP_
#define SRTVIEW_SRC_PALETTEFIX_HPP_

class QWidget;

void repairMenuPalette(QWidget *w);

#endif // SRTVIEW_SRC_PALETTEFIX_HPP_
