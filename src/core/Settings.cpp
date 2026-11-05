// Lyra — настройки браузера: значения, загрузка и сохранение settings.ini, пути профиля.
#include "core/Settings.h"

// Settings
std::wstring g_settingsDir, g_settingsFile;

double       g_zoom = 1.0;

std::wstring g_homePage = L"lyra://start";

std::wstring g_searchEngine = L"https://duckduckgo.com/?q=";

Prefs g_prefs;

// ----------------------------------------------------------------------------
//  Settings
// ----------------------------------------------------------------------------
void InitPaths() {
    wchar_t base[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        GetTempPathW(MAX_PATH, base);
    }
    std::wstring dir = base;
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    g_settingsDir = dir + L"\\Lyra";
    g_settingsFile = g_settingsDir + L"\\settings.ini";

    // The browser used to be called MyBrowser: take its profile (settings, cookies, history)
    // along the first time Lyra starts.
    std::wstring oldDir = dir + L"\\MyBrowser";
    if (GetFileAttributesW(g_settingsDir.c_str()) == INVALID_FILE_ATTRIBUTES
        && GetFileAttributesW(oldDir.c_str()) != INVALID_FILE_ATTRIBUTES)
        MoveFileW(oldDir.c_str(), g_settingsDir.c_str());
}

void LoadSettings() {
    CreateDirectoryW(g_settingsDir.c_str(), nullptr);
    wchar_t buf[512] = {};
    GetPrivateProfileStringW(L"Browser", L"Scale", L"1.0", buf, 512, g_settingsFile.c_str());
    g_zoom = _wtof(buf);
    if (g_zoom < 0.25 || g_zoom > 5.0) g_zoom = 1.0;
    GetPrivateProfileStringW(L"Browser", L"HomePage", L"lyra://start", buf, 512, g_settingsFile.c_str());
    g_homePage = buf;
    if (g_homePage == L"https://example.com") g_homePage = L"lyra://start";   // the old placeholder default
    GetPrivateProfileStringW(L"Browser", L"SearchEngine", L"https://duckduckgo.com/?q=", buf, 512, g_settingsFile.c_str());
    g_searchEngine = buf;

    auto geti = [&](const wchar_t* k, int d) {
        return (int)GetPrivateProfileIntW(L"Browser", k, d, g_settingsFile.c_str());
    };
    g_prefs.language    = std::max(0, std::min(2, geti(L"Language", 0)));
    g_prefs.themeMode   = std::max(0, std::min(2, geti(L"Theme", 0)));
    // "AccentColor" replaced "Accent" when the Windows accent became option 0.
    g_prefs.accent      = std::max(0, std::min(kAccentCount - 1, geti(L"AccentColor", 0)));
    g_prefs.density     = std::max(0, std::min(2, geti(L"Density", 0)));
    g_prefs.tabLayout   = std::max(0, std::min(2, geti(L"TabLayout", 0)));
    g_prefs.animations  = geti(L"Animations", 1) != 0;
    g_prefs.animSpeed   = std::max(25, std::min(300, geti(L"AnimationSpeed", 100)));
    g_prefs.animTabs    = geti(L"AnimateTabs", 1) != 0;
    g_prefs.animHover   = geti(L"AnimateHover", 1) != 0;
    g_prefs.tabResizeDelay = std::max(0, std::min(2000, geti(L"TabResizeDelay", 350)));
    g_prefs.showFullUrl = geti(L"FullUrl", 0) != 0;
    g_prefs.closeAlways = geti(L"CloseAlways", 0) != 0;
    // "NewTabPage" replaced "NewTabMode" (0 home page, 1 blank) when the start page arrived.
    g_prefs.newTabMode  = std::max(0, std::min(2, geti(L"NewTabPage", 0)));
    g_prefs.startFavorites = geti(L"StartFavorites", 1) != 0;
    g_prefs.onboarded   = geti(L"Onboarded", 0) != 0;
    g_prefs.js          = geti(L"JavaScript", 1) != 0;
    g_prefs.images      = geti(L"Images", 1) != 0;
    g_prefs.localStorage= geti(L"LocalStorage", 1) != 0;
    g_prefs.databases   = geti(L"Databases", 1) != 0;
    g_prefs.devExtras   = geti(L"DevTools", 1) != 0;
    g_prefs.fullscreen  = geti(L"Fullscreen", 1) != 0;
    g_prefs.webAudio    = geti(L"WebAudio", 1) != 0;
    g_prefs.defaultFont = std::max(9, std::min(32, geti(L"DefaultFont", 16)));
    g_prefs.minFont     = std::max(0, std::min(24, geti(L"MinFont", 0)));
}

void SaveSettings() {
    wchar_t buf[64];
    swprintf(buf, 64, L"%.2f", g_zoom);
    WritePrivateProfileStringW(L"Browser", L"Scale", buf, g_settingsFile.c_str());
    WritePrivateProfileStringW(L"Browser", L"HomePage", g_homePage.c_str(), g_settingsFile.c_str());
    WritePrivateProfileStringW(L"Browser", L"SearchEngine", g_searchEngine.c_str(), g_settingsFile.c_str());

    auto puti = [&](const wchar_t* k, int v) {
        WritePrivateProfileStringW(L"Browser", k, std::to_wstring(v).c_str(), g_settingsFile.c_str());
    };
    puti(L"Language", g_prefs.language);
    puti(L"Theme", g_prefs.themeMode);   puti(L"AccentColor", g_prefs.accent);
    puti(L"Density", g_prefs.density);   puti(L"TabLayout", g_prefs.tabLayout);
    puti(L"Animations", g_prefs.animations); puti(L"AnimationSpeed", g_prefs.animSpeed);
    puti(L"AnimateTabs", g_prefs.animTabs);  puti(L"AnimateHover", g_prefs.animHover);
    puti(L"TabResizeDelay", g_prefs.tabResizeDelay);
    puti(L"FullUrl", g_prefs.showFullUrl);
    puti(L"CloseAlways", g_prefs.closeAlways); puti(L"NewTabPage", g_prefs.newTabMode);
    puti(L"StartFavorites", g_prefs.startFavorites); puti(L"Onboarded", g_prefs.onboarded);
    puti(L"JavaScript", g_prefs.js);     puti(L"Images", g_prefs.images);
    puti(L"LocalStorage", g_prefs.localStorage); puti(L"Databases", g_prefs.databases);
    puti(L"DevTools", g_prefs.devExtras); puti(L"Fullscreen", g_prefs.fullscreen);
    puti(L"WebAudio", g_prefs.webAudio);
    puti(L"DefaultFont", g_prefs.defaultFont); puti(L"MinFont", g_prefs.minFont);
}
