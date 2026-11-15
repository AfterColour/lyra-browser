// Lyra — свои темы: цвета панелей, акцент и обои из тем с themes.vivaldi.net.
//
// A theme is a .zip with settings.json (a name, five colours, a few switches) and usually a
// background picture - the format of the themes on themes.vivaldi.net. Lyra keeps every theme it
// was given in %LOCALAPPDATA%\Lyra\Themes\<id>; one of them (or none) is on. For each theme the
// user chooses what it brings (Settings > Themes, the gear of a theme): the colours of the tab
// strip, toolbar, address field and menus, the accent colour, the start page wallpaper. Every
// theme's picture is also offered among the start page's wallpapers.
//
// Installed from a file (Settings) or straight from the site: "Download theme" leads to
// themes.vivaldi.net/install/<id>, which Lyra fetches itself instead of saving the .zip.
#pragma once

#include "common/Platform.h"

struct CustomTheme {
    std::wstring id, name, dir;
    Color bg, fg, highlight, accent, window;
    bool hasWindow = false, accentOnWindow = false, dark = true;
    std::wstring picture;            // file name of the background picture in `dir` ("" if none)
    bool useColors = true, useAccent = true, useWallpaper = true;   // what it brings, when on
};

enum CustomThemeOption { kThemeColors, kThemeAccent, kThemeWallpaper };

const std::vector<CustomTheme>& CustomThemes();     // installed, in the order they came
const CustomTheme* CustomThemeActive();              // the theme that is on, or null
bool CustomThemeColorsOn();                          // a theme is on and brings its colours
bool CustomThemeAccentOn();                          // ... and its accent colour
void CustomThemeActivate(const std::wstring& id);    // "" = Lyra's own look
void CustomThemeSetOption(const std::wstring& id, CustomThemeOption what, bool on);
void CustomThemeDelete(const std::wstring& id);
bool CustomThemePickFile();                          // Settings: "Install from a file"; false if cancelled
// &thdir=<folder URL>&themes=<JSON [{id,f,n,v}]>: the themes' pictures for the start page
std::wstring CustomThemeWallpaperParams();

bool CustomThemeIsInstallUrl(const std::wstring& url);
void CustomThemeDownload(const std::wstring& url);       // in the background; then WM_APP_THEME
void CustomThemeDownloaded(WPARAM ok, LPARAM path);      // main window: WM_APP_THEME
