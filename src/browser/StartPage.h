// Lyra — стартовая страница и экран первого запуска; команды lyra://… от этих страниц.
#pragma once

#include "common/Platform.h"

// Address of the start page with the current language, theme, accent and options
// (`welcome`: the first-run screen instead).
std::wstring StartPageUrl(bool welcome = false);

// What to load for something typed in the address bar / a home page setting:
// "lyra://start" is the start page, everything else goes through ResolveInput.
std::string ResolveNavigation(const std::wstring& input);

// The look or language changed: reload the open start (and history) pages so they follow.
void RefreshStartPages();

// The history page: written to %LOCALAPPDATA%\Lyra\history.html from resources\history.html
// with the current visits, and its address returned. Ctrl+H / menu: OpenHistory().
std::wstring HistoryPageUrl();
void OpenHistory();

// The bookmarks page, generated the same way into %LOCALAPPDATA%\Lyra\bookmarks.html
// (with the browsers bookmarks can be imported from). Ctrl+Shift+O / menu: OpenBookmarks().
std::wstring BookmarksPageUrl();
void OpenBookmarks();

// The downloads page, generated the same way into %LOCALAPPDATA%\Lyra\downloads.html.
// Ctrl+J / menu / the downloads list: OpenDownloads(). PushDownloadsToPages() hands open
// downloads pages the current list (progress) without reloading them.
std::wstring DownloadsPageUrl();
void OpenDownloads();
void PushDownloadsToPages();

// A start page asked for "lyra://setup?..." or "lyra://settings" (see resources/start.html).
// Called on the main window's message loop, never inside a WebKit callback.
void HandleLyraUrl(unsigned long long tabId, const std::wstring& url);
