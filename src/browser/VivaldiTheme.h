// Lyra — темы Vivaldi (themes.vivaldi.net): цвета панелей и обои стартовой страницы.
//
// A Vivaldi theme is a .zip with settings.json (a name, five colours, a few switches) and usually a
// background picture. Lyra takes the colours for its tab strip, toolbar, address field, menus and
// accent, and the picture as a start page wallpaper ("Theme" among the wallpapers). The theme is
// kept in %LOCALAPPDATA%\Lyra\Theme; Settings > Appearance turns it on and off.
//
// It is installed from a file (Settings) or straight from the site: "Download theme" on
// themes.vivaldi.net/themes/... leads to themes.vivaldi.net/install/<id>, which Lyra fetches itself
// instead of saving the .zip as a download.
#pragma once

#include "common/Platform.h"

struct VivaldiTheme {
    bool valid = false;
    std::wstring name;
    Color bg, fg, highlight, accent, window;
    bool hasWindow = false, accentOnWindow = false, dark = true;
    std::wstring picture;            // the background picture in the theme folder ("" if none)
};

const VivaldiTheme& VivaldiThemeGet();          // the installed theme (valid = false: none)
bool VivaldiThemeOn();                           // installed and turned on (Settings > Appearance)
void VivaldiThemeSetOn(bool on);                 // also switches the start page to / from its picture
bool VivaldiThemePickFile();                     // Settings: "Install from a file"; false if cancelled

bool VivaldiThemeIsInstallUrl(const std::wstring& url);
void VivaldiThemeDownload(const std::wstring& url);   // in the background; then WM_APP_THEME
void VivaldiThemeDownloaded(WPARAM ok, LPARAM path);  // main window: WM_APP_THEME
