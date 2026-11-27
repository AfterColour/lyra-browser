// Lyra - the settings window: resources/settings.html showing the pages of SettingsPages.cpp.
#pragma once

#include "common/Platform.h"
#include "ui/settings/SettingsModel.h"

extern HWND g_setHwnd;                // the window while it is open
extern std::vector<SPage> g_pages;    // built by BuildPages() (SettingsPages.cpp)

void ShowSettingsWindow();
// Shows page `index` (also a detail page, see SPage::parent), opening the window if needed.
void SettingsOpenPage(int index);
// The pages must be built again (language changed, a list changed): rebuilt and re-sent shortly,
// never inside the caller (it may be a row of the pages being rebuilt).
void SettingsLanguageChanged();
void SettingsRebuild();                 // the pages changed (a layout made, renamed or deleted)
// A value changed outside the window (updater status...): the page is sent the current values.
void SettingsRefresh();
// Light/dark or the accent colour changed.
void SettingsThemeChanged();
