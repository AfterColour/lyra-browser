// Lyra — панель закладок под адресной строкой (папка «Избранное»).
#pragma once

#include "common/Platform.h"

// A row under the toolbar with the bookmarks of the Favorites folder (the same ones as on the start
// page): a click opens one (Ctrl or the middle button: in a new tab), a folder opens a menu, the
// ones that do not fit are behind "»". Ctrl+Shift+B, the "more" menu and Settings show or hide it.
bool BookmarksBarShown();
void BookmarksBarToggle();
float BookmarksBarHeight();                // its height when shown (physical pixels)

// Chrome.cpp: the items laid out in `bar` (cached), hit testing and drawing.
int BookmarksBarHitTest(const RectF& bar, float x, float y);   // item index, -2 for "»", -1 none
void BookmarksBarPaint(Graphics& g, const RectF& bar, int hover, int pressed, float hoverAmount);

// MainWindow.cpp: what the mouse did on item `index` (-2: "»"; -1: the empty part of the bar).
void BookmarksBarClick(int index, bool newTab);
void BookmarksBarMiddleClick(int index);
void BookmarksBarContextMenu(int index, POINT screenPt);
