// Lyra — значки сайтов: загрузка /favicon.ico, кэш в памяти и на диске.
#pragma once

#include "common/Platform.h"

// The icon of the site of `pageUrl` (32x32), or nullptr while it is unknown. Asking starts a
// download of https://<site>/favicon.ico in the background; when it arrives the main window
// gets WM_APP_FAVICON and repaints.
Bitmap* FaviconFor(const std::wstring& pageUrl);

// Folder with the cached icons as file:/// URL (<host>.png), for Lyra's own pages.
std::wstring FaviconFolderUrl();
// Name of the cached icon of `pageUrl` inside that folder ("" if not a web page).
std::wstring FaviconKey(const std::wstring& pageUrl);

// Main window: a download finished (lParam from WM_APP_FAVICON).
void FaviconArrived(LPARAM lParam);
void FaviconsShutdown();
