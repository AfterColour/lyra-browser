// Lyra — масштаб для каждого сайта: Ctrl + / − / 0 меняют масштаб сайта открытой вкладки.
#pragma once

#include "common/Platform.h"

struct Tab;

// Every site keeps its own zoom (like Chrome): Ctrl+= / Ctrl+- step through 25 % .. 500 % for the
// open tab's site, and every tab of that site follows; Ctrl+0 goes back to the default zoom
// (Settings > Appearance, g_zoom). Kept in %LOCALAPPDATA%\Lyra\zoom.tsv; private tabs' changes
// are kept in memory only.
double SiteZoomFor(const Tab& t);                    // the zoom the tab's page should have
bool   SiteZoomIsCustom(const Tab& t);               // its site has its own zoom (the badge shows)
void   SiteZoomApply(Tab& t);                        // the page committed: its site's zoom
void   SiteZoomStep(int direction);                  // +1 / -1: the open tab's site
void   SiteZoomReset();                              // the open tab's site back to the default
void   SiteZoomDefaultChanged();                     // g_zoom changed: tabs without their own follow
