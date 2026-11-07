// Lyra — настройки браузера: значения, загрузка и сохранение settings.ini, пути профиля.
#pragma once

#include "common/Platform.h"

extern std::wstring g_settingsDir, g_settingsFile;

extern double g_zoom;
extern std::wstring g_homePage;
extern std::wstring g_searchEngine;
extern std::wstring g_downloadDir;      // Settings > Downloads; empty = the Windows Downloads folder

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
    bool recordHistory = true;
    bool mica = true;            // Windows 11 Mica behind the tab strip and toolbar   // remember visited pages (Settings > Privacy)
    bool onboarded = false;      // the first-run welcome screen was completed
    int  newTabMode = 0;         // 0 start page, 1 blank page, 2 home page
    bool askDownload = false;    // Save As dialog for every download (Settings > Downloads)
    bool downloadsPopup = true;  // show the downloads list when a download starts
    int  loadIndicator = 0;      // page loading in the address field: 0 glowing line, 1 fill (Safari)
    int  warmTabs = 2;           // recently used tabs kept rendered for instant switching (0..4)
    bool spareTab = true;        // a start page is prepared in the background for the next new tab
    bool restoreSession = true;  // reopen the last session's tabs at startup (always after a crash)
    // Reader view: text size (px), 0 sans / 1 serif, 0 auto / 1 light / 2 sepia / 3 dark, 0 narrow / 1 normal / 2 wide
    int  readerSize = 19, readerFont = 1, readerTheme = 0, readerWidth = 1;
    int  translateService = 0;   // 0 Google (no key), 1 DeepL (own API key)
    int  translateTarget = 0;    // index into TranslateLanguages(); 0 = Lyra's language
    bool savePasswords = true;   // offer to save passwords and fill them in (browser/Passwords.cpp)
    bool adBlock = true;         // block ads and trackers (Settings > Privacy, browser/AdBlock.cpp)
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
