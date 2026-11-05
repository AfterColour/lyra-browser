// Lyra — стартовая страница и экран первого запуска; команды lyra://… от этих страниц.
#pragma once

#include "common/Platform.h"

// Address of the start page with the current language, theme, accent and options
// (`welcome`: the first-run screen instead).
std::wstring StartPageUrl(bool welcome = false);

// What to load for something typed in the address bar / a home page setting:
// "lyra://start" is the start page, everything else goes through ResolveInput.
std::string ResolveNavigation(const std::wstring& input);

// The look or language changed: reload the open start pages so they follow.
void RefreshStartPages();

// A start page asked for "lyra://setup?..." or "lyra://settings" (see resources/start.html).
// Called on the main window's message loop, never inside a WebKit callback.
void HandleLyraUrl(unsigned long long tabId, const std::wstring& url);
