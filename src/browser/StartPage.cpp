// Lyra — стартовая страница и экран первого запуска; команды lyra://… от этих страниц.
#include "browser/StartPage.h"
#include "browser/Reader.h"
#include "browser/Wallpaper.h"
#include "browser/Bookmarks.h"
#include "browser/Downloads.h"
#include "browser/Favicons.h"
#include "browser/History.h"
#include "app/Log.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "core/Localization.h"
#include "core/Settings.h"
#include "core/StringUtils.h"
#include "core/Url.h"
#include "ui/Chrome.h"
#include "ui/DownloadsPopup.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/settings/SettingsPages.h"     // kEngines
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

// Bookmark importers found on this PC as [{"k":key,"n":name}] (the bookmarks page, the welcome).
static std::wstring ImportersJson() {
    std::wstring importers = L"[";
    for (auto& [key, name] : BookmarkImporters()) {
        if (importers.size() > 1) importers += L",";
        importers += L"{\"k\":" + JsString(key) + L",\"n\":" + JsString(name) + L"}";
    }
    return importers + L"]";
}

// Lyra is registered as a browser (by the installer): Windows can make it the default.
static bool RegisteredAsBrowser() {
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", L"Lyra", RRF_RT_REG_SZ,
                        nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

std::wstring StartPageUrl(bool welcome) {
    std::wstring url = welcome ? WelcomePageFileUrl() : StartPageFileUrl();
    url += L"?" + LookParams();
    url += L"&layout=" + std::to_wstring(g_prefs.tabLayout);
    url += L"&density=" + std::to_wstring(g_prefs.density);
    url += L"&fav=" + std::wstring(g_prefs.startFavorites ? L"1" : L"0");
    url += L"&freq=" + std::wstring(g_prefs.startFrequent ? L"1" : L"0");
    if (g_prefs.startFrequent && !welcome)
        url += L"&top=" + ToWide(UrlEncode(ToUtf8(EntriesJson(HistoryTopSites(12), false))));
    if (!welcome) {
        // Favorites = the bookmarks in the Favorites folder (sub-folders are not shown).
        std::wstring favs = L"[";
        for (const Bookmark& b : BookmarkChildren(kFavoritesFolder)) {
            if (b.folder) continue;
            if (favs.size() > 1) favs += L",";
            favs += L"{\"i\":" + std::to_wstring(b.id) + L",\"u\":" + JsString(b.url) + L",\"t\":" + JsString(b.title) + L"}";
        }
        url += L"&favs=" + ToWide(UrlEncode(ToUtf8(favs + L"]")));
        // The widgets' layout and settings, and the wallpaper pictures (browser/Wallpaper.cpp).
        url += L"&cfg=" + ToWide(UrlEncode(ToUtf8(StartConfigJson()))) + WallpaperParams();
        WallpaperBingEnsure();
    }
    if (welcome) {
        url += L"&welcome=1&anim=" + std::wstring(g_prefs.animations ? L"1" : L"0");
        url += L"&canDefault=" + std::wstring(RegisteredAsBrowser() ? L"1" : L"0");
        int engine = -1;                                 // the search engine step: which one is chosen now
        for (int i = 0; i < kEngineCount; ++i) if (g_searchEngine == kEngines[i].url) engine = i;
        url += L"&engine=" + std::to_wstring(engine);
        url += L"&importers=" + ToWide(UrlEncode(ToUtf8(ImportersJson())));
    }
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

// resources\<name> (next to start.html) with `json` in place of the LYRA_DATA marker, written to
// `out`; returns the file URL of `out` (without parameters).
static std::wstring WriteGeneratedPage(const wchar_t* name, const std::wstring& out, const std::wstring& json) {
    std::wstring tpl = StartPagePath();
    tpl.replace(tpl.size() - wcslen(L"start.html"), wcslen(L"start.html"), name);
    std::string page = ReadFileUtf8(tpl);
    const std::string marker = "/*LYRA_DATA*/[]";
    const size_t at = page.find(marker);
    if (at != std::string::npos) page.replace(at, marker.size(), ToUtf8(json));
    HANDLE f = CreateFileW(out.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        if (!page.empty()) WriteFile(f, page.data(), (DWORD)page.size(), &written, nullptr);
        CloseHandle(f);
    } else {
        Log("internal page: cannot write %ls", out.c_str());
    }
    wchar_t url[2084] = {};
    DWORD len = ARRAYSIZE(url);
    if (FAILED(UrlCreateFromPathW(out.c_str(), url, &len, 0))) return std::wstring();
    return url;
}

std::wstring HistoryPageUrl() {
    const std::wstring url = WriteGeneratedPage(L"history.html", HistoryPagePath(), EntriesJson(HistoryNewest(5000), true));
    return url.empty() ? url : url + L"?" + LookParams();
}

// [{"i":id,"p":parent,"f":1,"t":title,"u":url}] in tree order
static std::wstring BookmarksJson() {
    std::wstring json = L"[";
    for (const Bookmark& b : BookmarksAll()) {
        if (json.size() > 1) json += L",";
        json += L"{\"i\":" + std::to_wstring(b.id) + L",\"p\":" + std::to_wstring(b.parent);
        if (b.folder) json += L",\"f\":1";
        json += L",\"t\":" + JsString(b.title);
        if (!b.folder) json += L",\"u\":" + JsString(b.url);
        json += L"}";
    }
    return json + L"]";
}

std::wstring BookmarksPageUrl() {
    const std::wstring url = WriteGeneratedPage(L"bookmarks.html", BookmarksPagePath(), BookmarksJson());
    if (url.empty()) return url;
    return url + L"?" + LookParams() + L"&importers=" + ToWide(UrlEncode(ToUtf8(ImportersJson())));
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

// Rewrites the bookmarks page (a tab restored later must not show old bookmarks) and reloads the
// open ones, except the tab `skip` (it changed itself).
static void RefreshBookmarksPages(unsigned long long skip = 0, const std::wstring& extra = L"") {
    const std::wstring fresh = BookmarksPageUrl();
    for (auto& tp : g_tabs) {
        Tab& t = *tp;
        if (t.state != TabState::Live || !t.page || !IsBookmarksPageUrl(t.url)) continue;
        if (t.id == skip) continue;
        if (fresh == t.url && extra.empty()) WKPageReload(t.page);
        else LoadInTab(t, fresh + extra);
    }
}

void OpenBookmarks() {
    for (int i = 0; i < (int)g_tabs.size(); ++i) {
        if (g_tabs[i]->state == TabState::Live && IsBookmarksPageUrl(g_tabs[i]->url)) {
            SelectTab(i);
            RefreshBookmarksPages();
            return;
        }
    }
    CreateTab(kBookmarksAlias);
}

// [{"i":id,"s":state,"n":name,"u":url,"r":received,"t":total,"a":started,"x":exists,"e":status}]
static std::wstring DownloadsJson() {
    std::wstring json = L"[";
    for (const DownloadItem& d : DownloadsList()) {
        if (json.size() > 1) json += L",";
        json += L"{\"i\":" + std::to_wstring(d.id) + L",\"s\":" + std::to_wstring((int)d.state) +
                L",\"n\":" + JsString(d.name) + L",\"u\":" + JsString(d.url) +
                L",\"r\":" + std::to_wstring(d.received) + L",\"t\":" + std::to_wstring(d.total) +
                L",\"a\":" + std::to_wstring(d.started) + L",\"x\":" + (DownloadFileExists(d) ? L"1" : L"0") +
                L",\"e\":" + JsString(DownloadStatusText(d.id)) + L"}";
    }
    return json + L"]";
}

std::wstring DownloadsPageUrl() {
    const std::wstring url = WriteGeneratedPage(L"downloads.html", DownloadsPagePath(), DownloadsJson());
    return url.empty() ? url : url + L"?" + LookParams();
}

static void IgnoreScriptResult(WKTypeRef, WKErrorRef, void*) {}

void PushDownloadsToPages() {
    std::string script;
    for (auto& tp : g_tabs) {
        Tab& t = *tp;
        if (t.state != TabState::Live || !t.page || !IsDownloadsPageUrl(t.url)) continue;
        if (script.empty()) script = "window.lyraDownloads && lyraDownloads(" + ToUtf8(DownloadsJson()) + ")";
        if (WKStringRef s = WKStringCreateWithUTF8CString(script.c_str())) {
            WKPageEvaluateJavaScriptInMainFrame(t.page, s, nullptr, IgnoreScriptResult);
            WKRelease(s);
        }
    }
}

void OpenDownloads() {
    for (int i = 0; i < (int)g_tabs.size(); ++i) {
        if (g_tabs[i]->state == TabState::Live && IsDownloadsPageUrl(g_tabs[i]->url)) {
            SelectTab(i);
            PushDownloadsToPages();
            return;
        }
    }
    CreateTab(kDownloadsAlias);
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

std::wstring PrivatePageUrl() {
    return StartPageFileUrl() + L"?" + LookParams() + L"&private=1";
}

std::string ResolveNavigation(const std::wstring& input) {
    std::wstring s = TrimWs(input);
    if (_wcsicmp(s.c_str(), kStartAlias) == 0 || _wcsicmp(s.c_str(), L"lyra://start/") == 0)
        return ToUtf8(StartPageUrl());
    if (_wcsicmp(s.c_str(), kHistoryAlias) == 0 || _wcsicmp(s.c_str(), L"lyra://history/") == 0)
        return ToUtf8(HistoryPageUrl());
    if (_wcsicmp(s.c_str(), kBookmarksAlias) == 0 || _wcsicmp(s.c_str(), L"lyra://bookmarks/") == 0)
        return ToUtf8(BookmarksPageUrl());
    if (_wcsicmp(s.c_str(), kDownloadsAlias) == 0 || _wcsicmp(s.c_str(), L"lyra://downloads/") == 0)
        return ToUtf8(DownloadsPageUrl());
    return ResolveInput(s);
}

static void ReloadStartPages(unsigned long long skip) {
    for (auto& tp : g_tabs) {
        Tab& t = *tp;
        if (t.state != TabState::Live || !t.page || !IsStartPageUrl(t.url) || t.id == skip) continue;
        if (t.url.find(L"welcome=1") != std::wstring::npos) continue;
        const std::wstring fresh = t.privateTab ? PrivatePageUrl() : StartPageUrl();
        if (fresh != t.url) LoadInTab(t, fresh);
    }
}

// A page that changed bookmarks itself (removed, renamed, moved one) already shows the change:
// it is not reloaded, so it keeps its scroll position and search.
static unsigned long long s_quietTab = 0;

static void OnBookmarksChanged() {
    ReloadStartPages(s_quietTab);
    RefreshBookmarksPages(s_quietTab);
    InvalidateChrome();                          // the star in the address field
}
static const bool s_hooked = (g_bookmarksChanged = &OnBookmarksChanged, true);

void RefreshStartPages() {
    for (auto& tp : g_tabs) {
        Tab& t = *tp;
        if (t.state != TabState::Live || !t.page || !IsStartPageUrl(t.url)) continue;
        if (t.url.find(L"welcome=1") != std::wstring::npos) continue;   // restyles itself as you choose
        const std::wstring fresh = t.privateTab ? PrivatePageUrl() : StartPageUrl();
        if (fresh == t.url) continue;
        LoadInTab(t, fresh);
    }
    RefreshHistoryPages();
    RefreshBookmarksPages();
    // The downloads page follows the look too.
    std::wstring fresh;
    for (auto& tp : g_tabs) {
        Tab& t = *tp;
        if (t.state != TabState::Live || !t.page || !IsDownloadsPageUrl(t.url)) continue;
        if (fresh.empty()) fresh = DownloadsPageUrl();
        if (fresh != t.url) LoadInTab(t, fresh);
    }
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

static long long QueryId(const std::wstring& query, const wchar_t* key) {
    const std::wstring v = QueryString(query, key);
    return v.empty() ? -1 : _wtoi64(v.c_str());
}

// lyra://bookmarks-<verb>?... from the start page or the bookmarks page.
static void HandleBookmarksCommand(unsigned long long tabId, const std::wstring& verb, const std::wstring& query) {
    const bool quiet = verb == L"remove" || verb == L"rename" || verb == L"move";
    s_quietTab = quiet ? tabId : 0;
    if (verb == L"add") {
        const long long folder = QueryId(query, L"folder");
        const long long index = QueryId(query, L"index");
        std::wstring u = UrlDecode(QueryString(query, L"u"));
        if (!u.empty()) BookmarkAdd(folder > 0 ? folder : kFavoritesFolder, UrlDecode(QueryString(query, L"t")), u, (int)index);
    } else if (verb == L"folder") {
        const long long parent = QueryId(query, L"parent");
        BookmarkAddFolder(parent > 0 ? parent : kBookmarksFolder, UrlDecode(QueryString(query, L"t")));
    } else if (verb == L"remove") {
        BookmarkRemove(QueryId(query, L"id"));
    } else if (verb == L"rename") {
        const long long id = QueryId(query, L"id");
        const std::wstring u = UrlDecode(QueryString(query, L"u"));
        if (!u.empty()) BookmarkSetUrl(id, u);
        BookmarkRename(id, UrlDecode(QueryString(query, L"t")));
    } else if (verb == L"move") {
        BookmarkMove(QueryId(query, L"id"), QueryId(query, L"folder"), (int)QueryId(query, L"index"));
    } else if (verb == L"import") {
        const int n = BookmarkImport(UrlDecode(QueryString(query, L"from")));
        // The page that asked shows how many came in.
        int index = -1;
        if (Tab* t = FindTab(tabId, &index)) LoadInTab(*t, BookmarksPageUrl() + L"&imported=" + std::to_wstring(n));
    } else if (verb == L"favs") {
        // The start page used to keep Favorites itself: it hands them over once (t=..&u=.. pairs).
        std::vector<std::pair<std::wstring, std::wstring>> list;
        std::wstring title;
        size_t p = 0;
        while (p < query.size()) {
            size_t e = query.find(L'&', p);
            if (e == std::wstring::npos) e = query.size();
            if (query.compare(p, 2, L"t=") == 0) title = UrlDecode(query.substr(p + 2, e - p - 2));
            else if (query.compare(p, 2, L"u=") == 0) { list.emplace_back(title, UrlDecode(query.substr(p + 2, e - p - 2))); title.clear(); }
            p = e + 1;
        }
        BookmarkReplaceFavorites(list);
    }
    s_quietTab = 0;
}

void HandleLyraUrl(unsigned long long tabId, const std::wstring& url) {
    Log("lyra url: %ls", url.c_str());
    const size_t q = url.find(L'?');
    std::wstring command = url.substr(0, q), query = q == std::wstring::npos ? L"" : url.substr(q + 1);
    while (!command.empty() && command.back() == L'/') command.pop_back();
    CharLowerBuffW(&command[0], (DWORD)command.size());

    if (command.rfind(L"lyra://reader-", 0) == 0) {    // the reader page's close button and Aa panel
        ReaderLyraUrl(tabId, command, query);
        return;
    }
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
    if (command == L"lyra://bookmarks") {
        OpenBookmarks();
        return;
    }
    if (command == L"lyra://downloads") {
        OpenDownloads();
        return;
    }
    if (command.compare(0, 17, L"lyra://downloads-") == 0) {
        const std::wstring verb = command.substr(17);
        const std::wstring idText = QueryString(query, L"id");
        const long long id = idText.empty() ? 0 : _wtoi64(idText.c_str());
        if (verb == L"open") DownloadOpen(id);
        else if (verb == L"show") DownloadShowInFolder(id);
        else if (verb == L"cancel") DownloadCancel(id);
        else if (verb == L"retry") DownloadRetry(id);
        else if (verb == L"remove") DownloadRemove(id);
        else if (verb == L"clear") DownloadsClear();
        else if (verb == L"folder") OpenDownloadsFolder();
        return;
    }
    if (command.compare(0, 17, L"lyra://bookmarks-") == 0) {
        HandleBookmarksCommand(tabId, command.substr(17), query);
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
    if (command == L"lyra://start-save") {           // the start page's layout changed
        StartConfigSave(ToUtf8(UrlDecode(QueryString(query, L"cfg"))));
        WallpaperBingEnsure();
        return;
    }
    if (command == L"lyra://start-wallpaper-file") { // "Your own picture": the file dialog
        if (WallpaperPickFile()) {
            int index = -1;
            if (Tab* t = FindTab(tabId, &index)) LoadInTab(*t, StartPageUrl() + L"&picked=1");
        }
        return;
    }
    if (command == L"lyra://go") {                   // the search widget: like typing into the address field
        const std::wstring text = UrlDecode(QueryString(query, L"q"));
        int index = -1;
        if (!text.empty())
            if (Tab* t = FindTab(tabId, &index)) LoadInTab(*t, ToWide(ResolveInput(text)));
        return;
    }
    if (command == L"lyra://welcome-import") {        // the welcome's bookmarks step
        const std::wstring from = UrlDecode(QueryString(query, L"from"));
        const int n = BookmarkImport(from);
        int index = -1;
        if (Tab* t = FindTab(tabId, &index))
            LoadInTab(*t, StartPageUrl(true) + L"&step=3&imported=" + std::to_wstring(n) +
                          L"&from=" + ToWide(UrlEncode(ToUtf8(from))));
        return;
    }
    if (command != L"lyra://setup") return;
    if (QueryInt(query, L"defaultBrowser") == 1)    // Windows decides; this opens Lyra's page in Default apps
        ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps?registeredAppUser=Lyra", nullptr, nullptr, SW_SHOWNORMAL);

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
    const int engine = QueryInt(query, L"engine");     // the welcome's search engine step
    if (engine >= 0 && engine < kEngineCount) g_searchEngine = kEngines[engine].url;
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
