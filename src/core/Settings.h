// Lyra — настройки браузера: значения, загрузка и сохранение settings.ini, пути профиля.
#pragma once

#include "common/Platform.h"

extern std::wstring g_settingsDir, g_settingsFile;

extern double g_zoom;
extern std::wstring g_homePage;
extern std::wstring g_searchEngine;

struct Prefs {
    int  language = 0;           // 0 as in Windows, 1 English, 2 Russian
    int  themeMode = 0;          // 0 system, 1 light, 2 dark
    int  accent = 0;             // index into kAccents (0 = the Windows accent colour)
    int  density = 0;            // toolbar size: 0 compact, 1 normal, 2 large
    int  tabLayout = 0;          // 0 compact (tabs and address in one row), 1 separate tab bar, 2 vertical tabs
    // Animations (Settings > Animations)
    bool animations = true;      // master switch
    int  animSpeed = 100;        // percent: 200 = twice as fast
    bool animTabs = true;        // tabs slide / grow instead of jumping
    bool animHover = true;       // hover highlights fade in and out
    int  tabResizeDelay = 350;   // ms after the mouse leaves the tab strip before tabs widen again (0 = never wait)
    bool showFullUrl = false;
    bool closeAlways = false;
    bool startFavorites = true;  // start page shows Favorites
    bool startFrequent = false;  // start page shows frequently visited sites
    bool recordHistory = true;   // remember visited pages (Settings > Privacy)
    bool onboarded = false;      // the first-run welcome screen was completed
    int  newTabMode = 0;         // 0 start page, 1 blank page, 2 home page
    bool js = true, images = true, localStorage = true, databases = true;
    bool devExtras = true, fullscreen = true, webAudio = true;
    int  defaultFont = 16, minFont = 0;
};
extern Prefs g_prefs;

// Entry 0 is a placeholder for the accent colour chosen in Windows (see WindowsAccentColor).
struct AccentDef { const wchar_t* name; BYTE r, g, b; };
static const AccentDef kAccents[] = {
    { L"Windows accent", 0, 103, 192 },
    { L"Blue", 10, 124, 255 },  { L"Purple", 122, 82, 204 }, { L"Pink", 194, 56, 158 },
    { L"Red", 209, 52, 56 },    { L"Orange", 194, 94, 10 },  { L"Yellow", 154, 123, 0 },
    { L"Green", 44, 138, 62 },  { L"Graphite", 105, 112, 122 },
};
constexpr int kAccentCount = (int)(sizeof(kAccents) / sizeof(kAccents[0]));

void InitPaths();
void LoadSettings();
void SaveSettings();
