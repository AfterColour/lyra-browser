// Lyra — настройки браузера: значения, загрузка и сохранение settings.ini, пути профиля.
#include "core/Settings.h"

// Settings
std::wstring g_settingsDir, g_settingsFile;

double       g_zoom = 1.0;

std::wstring g_homePage = L"lyra://start";
std::wstring g_downloadDir;
std::wstring g_printPrinter;
std::wstring g_activeTheme;

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
    GetPrivateProfileStringW(L"Browser", L"DownloadFolder", L"", buf, 512, g_settingsFile.c_str());
    g_downloadDir = buf;
    GetPrivateProfileStringW(L"Browser", L"Printer", L"", buf, 512, g_settingsFile.c_str());
    g_printPrinter = buf;
    GetPrivateProfileStringW(L"Browser", L"ActiveTheme", L"", buf, 512, g_settingsFile.c_str());
    g_activeTheme = buf;

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
    g_prefs.startFrequent  = geti(L"StartFrequent", 0) != 0;
    g_prefs.recordHistory  = geti(L"RecordHistory", 1) != 0;
    g_prefs.mica           = geti(L"Mica", 1) != 0;
    g_prefs.onboarded   = geti(L"Onboarded", 0) != 0;
    g_prefs.askDownload = geti(L"AskDownloadFolder", 0) != 0;
    g_prefs.downloadsPopup = geti(L"ShowDownloads", 1) != 0;
    g_prefs.loadIndicator = std::max(0, std::min(1, geti(L"LoadIndicator", 0)));
    g_prefs.warmTabs = std::max(0, std::min(4, geti(L"ReadyTabs", 2)));
    g_prefs.spareTab = geti(L"PrepareNewTab", 1) != 0;
    g_prefs.frameRate = std::max(0, std::min(2, geti(L"FrameRate", 2)));
    g_prefs.bookmarksBar = geti(L"BookmarksBar", 0) != 0;
    g_prefs.swipeNav = geti(L"SwipeNavigation", 1) != 0;
    g_prefs.pinchZoom = geti(L"PinchZoom", 1) != 0;
    g_prefs.clearExitHistory = geti(L"ClearOnExitHistory", 0) != 0;
    g_prefs.clearExitCookies = geti(L"ClearOnExitCookies", 0) != 0;
    g_prefs.clearExitCache = geti(L"ClearOnExitCache", 0) != 0;
    g_prefs.clearExitDownloads = geti(L"ClearOnExitDownloads", 0) != 0;
    g_prefs.adBlock  = geti(L"AdBlock", 1) != 0;
    g_prefs.savePasswords = geti(L"SavePasswords", 1) != 0;
    g_prefs.translateService = geti(L"TranslateService", 0) ? 1 : 0;
    g_prefs.translateTarget = std::max(0, geti(L"TranslateTarget", 0));
    g_prefs.readerSize = std::min(30, std::max(14, geti(L"ReaderSize", 19)));
    g_prefs.readerFont = geti(L"ReaderFont", 1) ? 1 : 0;
    g_prefs.readerTheme = std::min(3, std::max(0, geti(L"ReaderTheme", 0)));
    g_prefs.readerWidth = std::min(2, std::max(0, geti(L"ReaderWidth", 1)));
    g_prefs.printMargins = std::min(3, std::max(0, geti(L"PrintMargins", 0)));
    g_prefs.printScale = std::min(200, std::max(25, geti(L"PrintScale", 100)));
    g_prefs.printHeaders = geti(L"PrintHeaders", 0) != 0;
    g_prefs.sleepTabs = geti(L"SleepTabs", 1) != 0;
    g_prefs.spellCheck = geti(L"SpellCheck", 1) != 0;
    g_prefs.autoUpdate = geti(L"AutoUpdate", 1) != 0;
    g_prefs.sleepAfter = std::min(4, std::max(0, geti(L"SleepTabsAfter", 2)));
    g_prefs.printBackground = geti(L"PrintBackground", 0) != 0;
    g_prefs.restoreSession = geti(L"RestoreSession", 1) != 0;
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
    WritePrivateProfileStringW(L"Browser", L"DownloadFolder", g_downloadDir.c_str(), g_settingsFile.c_str());
    WritePrivateProfileStringW(L"Browser", L"Printer", g_printPrinter.c_str(), g_settingsFile.c_str());
    WritePrivateProfileStringW(L"Browser", L"ActiveTheme", g_activeTheme.c_str(), g_settingsFile.c_str());

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
    puti(L"StartFavorites", g_prefs.startFavorites); puti(L"StartFrequent", g_prefs.startFrequent);
    puti(L"RecordHistory", g_prefs.recordHistory); puti(L"Mica", g_prefs.mica); puti(L"Onboarded", g_prefs.onboarded);
    puti(L"JavaScript", g_prefs.js);     puti(L"Images", g_prefs.images);
    puti(L"LocalStorage", g_prefs.localStorage); puti(L"Databases", g_prefs.databases);
    puti(L"DevTools", g_prefs.devExtras); puti(L"Fullscreen", g_prefs.fullscreen);
    puti(L"WebAudio", g_prefs.webAudio);
    puti(L"AskDownloadFolder", g_prefs.askDownload); puti(L"ShowDownloads", g_prefs.downloadsPopup);
    puti(L"LoadIndicator", g_prefs.loadIndicator); puti(L"ReadyTabs", g_prefs.warmTabs);
    puti(L"PrepareNewTab", g_prefs.spareTab); puti(L"FrameRate", g_prefs.frameRate); puti(L"BookmarksBar", g_prefs.bookmarksBar);
    puti(L"SwipeNavigation", g_prefs.swipeNav); puti(L"PinchZoom", g_prefs.pinchZoom);
    puti(L"ClearOnExitHistory", g_prefs.clearExitHistory); puti(L"ClearOnExitCookies", g_prefs.clearExitCookies);
    puti(L"ClearOnExitCache", g_prefs.clearExitCache); puti(L"ClearOnExitDownloads", g_prefs.clearExitDownloads); puti(L"AdBlock", g_prefs.adBlock); puti(L"SavePasswords", g_prefs.savePasswords);
    puti(L"TranslateService", g_prefs.translateService); puti(L"TranslateTarget", g_prefs.translateTarget);
    puti(L"ReaderSize", g_prefs.readerSize); puti(L"ReaderFont", g_prefs.readerFont);
    puti(L"ReaderTheme", g_prefs.readerTheme); puti(L"ReaderWidth", g_prefs.readerWidth); puti(L"RestoreSession", g_prefs.restoreSession);
    puti(L"DefaultFont", g_prefs.defaultFont); puti(L"MinFont", g_prefs.minFont);
    puti(L"PrintMargins", g_prefs.printMargins); puti(L"PrintScale", g_prefs.printScale);
    puti(L"SleepTabs", g_prefs.sleepTabs); puti(L"SleepTabsAfter", g_prefs.sleepAfter);
    puti(L"SpellCheck", g_prefs.spellCheck);
    puti(L"AutoUpdate", g_prefs.autoUpdate);
    puti(L"PrintHeaders", g_prefs.printHeaders); puti(L"PrintBackground", g_prefs.printBackground);
}
