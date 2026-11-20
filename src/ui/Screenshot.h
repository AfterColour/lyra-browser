// Lyra — снимок страницы: видимая часть или вся страница, в буфер обмена или в PNG-файл.
#pragma once

#include "common/Platform.h"

// The "more" menu / Ctrl+Shift+S: a small menu (copy / save, the visible part / the whole page),
// then the picture of the open tab's page (WKPageTakeSnapshotOfPage, painted by the web process).
void ScreenshotMenu();

// A short message near the top of the window that goes away by itself ("Copied").
void ShowToast(const std::wstring& text);
