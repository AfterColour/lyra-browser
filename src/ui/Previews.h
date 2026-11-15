// Lyra - small copies of big pictures for previews (Settings: themes, wallpaper tiles; the start
// page's wallpaper picker). A theme's 6674 x 4449 photo took ~450 ms to decode every time a 200 px
// tile showed it; its 640 px JPEG copy takes a few. Copies are made in the background and kept next
// to the picture; one older than its picture is made again.
#pragma once

#include "common/Platform.h"

// The file to show for `picture`: `preview` when it is ready; the picture itself when no copy can
// be made (a format GDI+ does not read); "" while the copy is being made (then WM_APP_PREVIEWS).
std::wstring PreviewFor(const std::wstring& picture, const std::wstring& preview);
void PreviewsReady();             // WM_APP_PREVIEWS (main window): pages showing previews are refreshed
