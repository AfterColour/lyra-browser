// Lyra — стартовая страница и экран первого запуска; команды lyra://… от этих страниц.
#include "browser/StartPage.h"
#include "browser/Favicons.h"
#include "browser/History.h"
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

// Parameters every internal page reads: language, theme, accent.
static std::wstring LookParams() {
    wchar_t accent[8];
    const Color& a = g_th.accent;
    swprintf(accent, 8, L"%02X%02X%02X", a.GetR(), a.GetG(), a.GetB());
    std::wstring p = L"language=" + std::to_wstring(g_prefs.language);
    p += L"&themeMode=" + std::to_wstring(g_prefs.themeMode);
    p += L"&sysru=" + std::wstring(SystemRussian() ? L"1" : L"0");
    p += L"&sysdark=" + std::wstring(SystemDark() ? L"1" : L"0");
    p += L"&accent=" + std::wstring(accent);
    p += L"&icons=" + ToWide(UrlEncode(ToUtf8(FaviconFolderUrl())));   // cached site icons
    return p;
}

// [{"u":...,"t":...}, ...] as JSON (JsString escapes are valid JSON)
static std::wstring EntriesJson(const std::vector<HistoryEntry>& list, bool withTimes) {
    std::wstring json = L"[";
    for (size_t i = 0; i < list.size(); ++i) {
        const HistoryEntry& e = list[i];
        if (i) json += L",";
        json += L"{\"u\":" + JsString(e.url) + L",\"t\":" + JsString(e.title);
        if (withTimes) json += L",\"l\":" + std::to_wstring(e.lastVisit) + L",\"v\":" + std::to_wstring(e.visits);
        json += L"}";
    }
    return json + L"]";
}

std::wstring StartPageUrl(bool welcome) {
    std::wstring url = StartPageFileUrl();
    url += L"?" + LookParams();
    url += L"&layout=" + std::to_wstring(g_prefs.tabLayout);
    url += L"&density=" + std::to_wstring(g_prefs.density);
    url += L"&fav=" + std::wstring(g_prefs.startFavorites ? L"1" : L"0");
    url += L"&freq=" + std::wstring(g_prefs.startFrequent ? L"1" : L"0");
    if (g_prefs.startFrequent && !welcome)
        url += L"&top=" + ToWide(UrlEncode(ToUtf8(EntriesJson(HistoryTopSites(12), false))));
    if (welcome) url += L"&welcome=1";
    return url;
}

static std::string ReadFileUtf8(const std::wstring& path) {
    std::string data;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return data;
    LARGE_INTEGER size = {};
    GetFileSizeEx(f, &size);
    data.resize((size_t)std::min<long long>(size.QuadPart, 8LL << 20));
    DWORD read = 0;
    if (!data.empty()) ReadFile(f, &data[0], (DWORD)data.size(), &read, nullptr);
    CloseHandle(f);
    data.resize(read);
    return data;
}

std::wstring HistoryPageUrl() {
    // resources\history.html sits next to start.html
    std::wstring tpl = StartPagePath();
    tpl.replace(tpl.size() - wcslen(L"start.html"), wcslen(L"start.html"), L"history.html");
    std::string page = ReadFileUtf8(tpl);
    const std::string marker = "/*LYRA_DATA*/[]";
    const size_t at = page.find(marker);
    if (at != std::string::npos) page.replace(at, marker.size(), ToUtf8(EntriesJson(HistoryNewest(5000), true)));
    const std::wstring out = HistoryPagePath();
    HANDLE f = CreateFileW(out.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        if (!page.empty()) WriteFile(f, page.data(), (DWORD)page.size(), &written, nullptr);
        CloseHandle(f);
    } else {
        Log("history page: cannot write %ls", out.c_str());
    }
    wchar_t url[2084] = {};
    DWORD len = ARRAYSIZE(url);
    if (FAILED(UrlCreateFromPathW(out.c_str(), url, &len, 0))) return std::wstring();
    return std::wstring(url) + L"?" + LookParams();
}

static void LoadInTab(Tab& t, const std::wstring& url) {
    if (WKURLRef u = WKURLCreateWithUTF8CString(ToUtf8(url).c_str())) {
        WKPageLoadURL(t.page, u);
        WKRelease(u);
    }
}

static void RefreshHistoryPages() {
    std::wstring fresh;
    for (auto& tp : g_tabs) {
        Tab& t = *tp;
        if (t.state != TabState::Live || !t.page || !IsHistoryPageUrl(t.url)) continue;
        if (fresh.empty()) fresh = HistoryPageUrl();       // rewrites the file once
        if (fresh == t.url) WKPageReload(t.page);
        else LoadInTab(t, fresh);
    }
}

void OpenHistory() {
    for (int i = 0; i < (int)g_tabs.size(); ++i) {
        if (g_tabs[i]->state == TabState::Live && IsHistoryPageUrl(g_tabs[i]->url)) {
            SelectTab(i);
            RefreshHistoryPages();
            return;
        }
    }
    CreateTab(kHistoryAlias);
}

std::string ResolveNavigation(const std::wstring& input) {
    std::wstring s = TrimWs(input);
    if (_wcsicmp(s.c_str(), kStartAlias) == 0 || _wcsicmp(s.c_str(), L"lyra://start/") == 0)
        return ToUtf8(StartPageUrl());
    if (_wcsicmp(s.c_str(), kHistoryAlias) == 0 || _wcsicmp(s.c_str(), L"lyra://history/") == 0)
        return ToUtf8(HistoryPageUrl());
    return ResolveInput(s);
}

void RefreshStartPages() {
    for (auto& tp : g_tabs) {
        Tab& t = *tp;
        if (t.state != TabState::Live || !t.page || !IsStartPageUrl(t.url)) continue;
        if (t.url.find(L"welcome=1") != std::wstring::npos) continue;   // restyles itself as you choose
        const std::wstring fresh = StartPageUrl();
        if (fresh == t.url) continue;
        LoadInTab(t, fresh);
    }
    RefreshHistoryPages();
}

// Value of `key` in "a=1&b=x" (still URL-encoded)
static std::wstring QueryString(const std::wstring& query, const wchar_t* key) {
    const std::wstring k = std::wstring(key) + L"=";
    size_t p = 0;
    while (p < query.size()) {
        size_t e = query.find(L'&', p);
        if (e == std::wstring::npos) e = query.size();
        if (query.compare(p, k.size(), k) == 0) return query.substr(p + k.size(), e - p - k.size());
        p = e + 1;
    }
    return std::wstring();
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
    if (command == L"lyra://favicons") {             // the start page lists sites without a cached icon
        size_t p = 0;
        while (p < query.size()) {
            size_t e = query.find(L'&', p);
            if (e == std::wstring::npos) e = query.size();
            if (query.compare(p, 2, L"u=") == 0) FaviconFor(UrlDecode(query.substr(p + 2, e - p - 2)));
            p = e + 1;
        }
        return;
    }
    if (command == L"lyra://history") {
        OpenHistory();
        return;
    }
    if (command == L"lyra://history-remove") {
        HistoryRemove(UrlDecode(QueryString(query, L"url")));
        HistorySave();
        return;                                  // the page already hid the row
    }
    if (command == L"lyra://history-clear") {
        const std::wstring range = QueryString(query, L"range");
        long long since = 0;
        if (range == L"hour") since = NowUnixMs() - 60LL * 60 * 1000;
        else if (range == L"day") {
            SYSTEMTIME st;
            GetLocalTime(&st);
            since = NowUnixMs() - ((long long)st.wHour * 3600 + st.wMinute * 60 + st.wSecond) * 1000LL - st.wMilliseconds;
        }
        HistoryClearSince(since);
        RefreshHistoryPages();
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
    const int freq = QueryInt(query, L"freq");
    if (freq >= 0) g_prefs.startFrequent = freq != 0;
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
