// Lyra — настройки браузера: значения, загрузка и сохранение settings.ini, пути профиля.
#pragma once

#include "common/Platform.h"

extern std::wstring g_settingsDir, g_settingsFile;

extern double g_zoom;
extern std::wstring g_homePage;
extern std::wstring g_searchEngine;

struct Prefs {
    int  themeMode = 0;          // 0 system, 1 light, 2 dark
    int  accent = 0;             // index into kAccents
    bool compact = false;
    bool showFullUrl = false;
    bool closeAlways = false;
    int  newTabMode = 0;         // 0 home page, 1 blank page
    bool js = true, images = true, localStorage = true, databases = true;
    bool devExtras = true, fullscreen = true, webAudio = true;
    int  defaultFont = 16, minFont = 0;
};
extern Prefs g_prefs;

struct AccentDef { const wchar_t* name; BYTE r, g, b; };
static const AccentDef kAccents[] = {
    { L"Blue", 0, 122, 255 },  { L"Purple", 175, 82, 222 }, { L"Pink", 255, 45, 85 },
    { L"Red", 255, 59, 48 },   { L"Orange", 255, 149, 0 },  { L"Green", 40, 184, 65 },
    { L"Graphite", 120, 120, 128 },
};
constexpr int kAccentCount = (int)(sizeof(kAccents) / sizeof(kAccents[0]));

void InitPaths();
void LoadSettings();
void SaveSettings();
