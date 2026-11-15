// Lyra - what the start page keeps outside itself: its layout and the wallpaper pictures.
//
// The page (resources/start.html) arranges its widgets and picks a wallpaper itself; Lyra only
// keeps that as %LOCALAPPDATA%\Lyra\start.json (lyra://start-save) and hands it back as the
// page's cfg= parameter. Pictures live in %LOCALAPPDATA%\Lyra\Wallpapers:
//   custom.<ext>  the user's own picture (lyra://start-wallpaper-file opens the file dialog)
//   bing.jpg      Bing's picture of the day, fetched once a day while that wallpaper is chosen
#pragma once
#include <string>

std::wstring StartConfigJson();                       // start.json as text ("" if none yet)
void StartConfigSave(const std::string& utf8Json);
std::wstring WallpaperParams();                       // &wpdir=..&custom=..&bing=..&bingInfo=..&wpv=..
void WallpaperBingEnsure();                           // fetch today's picture if it is the wallpaper
bool WallpaperPickFile();                             // false: cancelled
// The wallpaper chosen in start.json ("builtin", "bing", "file", "theme"), and changing it.
std::string StartWallpaperKind();
std::string StartWallpaperId();                       // the theme's id with kind "theme"
void StartWallpaperSet(const std::string& kind, const std::string& id = "");
