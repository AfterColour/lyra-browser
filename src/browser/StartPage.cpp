// Lyra — стартовая страница и экран первого запуска; команды lyra://… от этих страниц.
#include "browser/StartPage.h"
#include "app/Log.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "core/Localization.h"
#include "core/Settings.h"
#include "core/StringUtils.h"
#include "core/Url.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/settings/SettingsWindow.h"

static bool SystemRussian() { return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN; }

static bool SystemDark() {
    DWORD v = 1, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS)
        return v == 0;
    return false;
}

std::wstring StartPageUrl(bool welcome) {
    wchar_t accent[8];
    const Color& a = g_th.accent;
    swprintf(accent, 8, L"%02X%02X%02X", a.GetR(), a.GetG(), a.GetB());
    std::wstring url = StartPageFileUrl();
    url += L"?language=" + std::to_wstring(g_prefs.language);
    url += L"&themeMode=" + std::to_wstring(g_prefs.themeMode);
    url += L"&layout=" + std::to_wstring(g_prefs.tabLayout);
    url += L"&density=" + std::to_wstring(g_prefs.density);
    url += L"&fav=" + std::wstring(g_prefs.startFavorites ? L"1" : L"0");
    url += L"&sysru=" + std::wstring(SystemRussian() ? L"1" : L"0");
    url += L"&sysdark=" + std::wstring(SystemDark() ? L"1" : L"0");
    url += L"&accent=" + std::wstring(accent);
    if (welcome) url += L"&welcome=1";
    return url;
}

std::string ResolveNavigation(const std::wstring& input) {
    std::wstring s = TrimWs(input);
    if (_wcsicmp(s.c_str(), kStartAlias) == 0 || _wcsicmp(s.c_str(), L"lyra://start/") == 0)
        return ToUtf8(StartPageUrl());
    return ResolveInput(s);
}

void RefreshStartPages() {
    for (auto& tp : g_tabs) {
        Tab& t = *tp;
        if (t.state != TabState::Live || !t.page || !IsStartPageUrl(t.url)) continue;
        if (t.url.find(L"welcome=1") != std::wstring::npos) continue;   // restyles itself as you choose
        const std::wstring fresh = StartPageUrl();
        if (fresh == t.url) continue;
        if (WKURLRef u = WKURLCreateWithUTF8CString(ToUtf8(fresh).c_str())) {
            WKPageLoadURL(t.page, u);
            WKRelease(u);
        }
    }
}

// "a=1&b=2" -> value of `key`, or -1
static int QueryInt(const std::wstring& query, const wchar_t* key) {
    const std::wstring k = std::wstring(key) + L"=";
    size_t p = 0;
    while (p < query.size()) {
        size_t e = query.find(L'&', p);
        if (e == std::wstring::npos) e = query.size();
        if (query.compare(p, k.size(), k) == 0) return _wtoi(query.substr(p + k.size(), e - p - k.size()).c_str());
        p = e + 1;
    }
    return -1;
}

void HandleLyraUrl(unsigned long long tabId, const std::wstring& url) {
    Log("lyra url: %ls", url.c_str());
    const size_t q = url.find(L'?');
    std::wstring command = url.substr(0, q), query = q == std::wstring::npos ? L"" : url.substr(q + 1);
    while (!command.empty() && command.back() == L'/') command.pop_back();
    CharLowerBuffW(&command[0], (DWORD)command.size());

    if (command == L"lyra://settings") {
        ShowSettingsWindow();
        return;
    }
    if (command != L"lyra://setup") return;

    auto pick = [&](const wchar_t* key, int& field, int maxValue) {
        const int v = QueryInt(query, key);
        if (v >= 0 && v <= maxValue) field = v;
    };
    const int oldLanguage = g_prefs.language;
    pick(L"language", g_prefs.language, 2);
    pick(L"themeMode", g_prefs.themeMode, 2);
    pick(L"layout", g_prefs.tabLayout, 2);
    pick(L"density", g_prefs.density, 2);
    const int fav = QueryInt(query, L"fav");
    if (fav >= 0) g_prefs.startFavorites = fav != 0;
    const bool done = QueryInt(query, L"done") == 1;
    if (done) g_prefs.onboarded = true;
    SaveSettings();

    ApplyTheme(ResolveDark());
    ApplyChromeSettings();
    if (oldLanguage != g_prefs.language) {
        SettingsLanguageChanged();
        if (g_hWnd) InvalidateRect(g_hWnd, nullptr, FALSE);
    }
    if (done) {
        int index = -1;
        if (Tab* t = FindTab(tabId, &index)) {
            if (WKURLRef u = WKURLCreateWithUTF8CString(ToUtf8(StartPageUrl()).c_str())) {
                WKPageLoadURL(t->page, u);
                WKRelease(u);
            }
        }
    }
}
