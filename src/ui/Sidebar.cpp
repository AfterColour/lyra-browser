// Lyra - the sidebar (see the header).
//
//   strip    icons drawn with the chrome (ui/Chrome DrawChrome -> SidebarPaint); the built-in
//            panels, a line, the web panels with their site icons, "+";
//   panel    a header drawn with the chrome (title and buttons) over one view:
//              built-in panels: resources/sidebar.html in a view of its own. Its user content
//              controller has the "lyraSidebar" message handler, which no web page can reach;
//              the page asks for its data and gives commands through it, and Lyra tells it when
//              something changed (lyraChanged) or which section to show (lyraShow).
//              web panels: one view per site, kept alive while Lyra runs, with an iPhone
//              user agent by default (a narrow layout), links that open new windows go to tabs.
// Stored in the profile: sidebar.json (web panels, reading list, notes) and [Sidebar] in
// settings.ini (the open panel and its width).
#include "ui/Sidebar.h"
#include "ui/Animation.h"
#include "app/Log.h"
#include "browser/Bookmarks.h"
#include "browser/Downloads.h"
#include "browser/Favicons.h"
#include "browser/History.h"
#include "browser/StartPage.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "browser/WebContext.h"
#include "ui/DownloadsPopup.h"
#include "core/Json.h"
#include "core/Localization.h"
#include "core/Settings.h"
#include "core/StringUtils.h"
#include "core/Url.h"
#include "ui/Chrome.h"
#include "ui/Dialogs.h"
#include "ui/Draw.h"
#include "ui/MainWindow.h"
#include "ui/Menu.h"
#include "ui/Theme.h"

#include <WebKit/WKFrameInfoRef.h>
#include <WebKit/WKPageFullScreenClient.h>      // WKCompletionListenerComplete
#include <WebKit/WKScriptMessageRef.h>
#include <WebKit/WKUserContentControllerRef.h>

#include <fstream>
#include <iterator>

namespace {

constexpr float kStripW = 44, kItem = 36, kHeaderH = 40, kDefaultW = 340, kMinW = 260, kMaxW = 760;
constexpr int kBuiltins = 5;                     // bookmarks, history, reading list, downloads, notes
constexpr int kAddItem = 999, kWebBase = 100;
const char* const kSections[kBuiltins] = { "bookmarks", "history", "reading", "downloads", "notes" };
const wchar_t* const kGlyphs[kBuiltins] = { L"", L"", L"", L"", L"" };
const wchar_t* const kMobileUA =
    L"Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Mobile/15E148 Safari/604.1";

struct WebPanel {
    long long id = 0;
    std::wstring url, title;
    bool mobile = true, named = false;           // named: renamed by the user (the page title no longer used)
    WKViewRef view = nullptr;
    WKPageRef page = nullptr;
    HWND hwnd = nullptr;
};
struct ReadingItem { long long id = 0; std::wstring url, title; long long added = 0; bool read = false; };
struct Note { long long id = 0; std::wstring text; long long created = 0, modified = 0; };

std::vector<std::unique_ptr<WebPanel>> s_web;
std::vector<ReadingItem> s_reading;
std::vector<Note> s_notes;
long long s_nextId = 1;

int s_open = -1;                                 // the open panel (item code), -1: none
int s_last = 0;                                  // the panel Ctrl+Shift+E opens
float s_width = kDefaultW;                       // panel width, 96-DPI pixels
bool s_left = false;

// Where things are (client coordinates), from the last layout.
RectF s_strip, s_panel, s_header;
std::vector<std::pair<int, RectF>> s_items;      // item code -> rect
std::vector<RectF> s_buttons;                    // header buttons (index = button code)
float s_sepY = -1;
float s_reveal = 1;                              // the panel sliding in (0 -> 1) or out (1 -> 0)
int s_revealDir = 0;                             // +1 opening, -1 closing, 0 still
int s_closing = -1;                              // a panel sliding out: still laid out until it is gone
float s_markY = -1, s_markA = 0;                 // the strip's highlight: its centre and opacity

// The built-in panels' view.
WKViewRef s_view = nullptr;
WKPageRef s_page = nullptr;
HWND s_viewHwnd = nullptr;
bool s_viewLoaded = false;
WKUserContentControllerRef s_controller = nullptr;

bool s_resizing = false;
int s_resizeStartX = 0;
float s_resizeStartW = 0;

void (*s_prevBookmarksChanged)() = nullptr;
void (*s_prevDownloadsChanged)(bool) = nullptr;

long long NowMs() { return NowUnixMs(); }

// --- storage ----------------------------------------------------------------------------------
std::wstring StorePath() { return g_settingsDir + L"\\sidebar.json"; }

void Save() {
    std::string j = "{\"next\":" + std::to_string(s_nextId) + ",\"web\":[";
    for (size_t i = 0; i < s_web.size(); ++i) {
        const WebPanel& w = *s_web[i];
        if (i) j += ",";
        j += "{\"id\":" + std::to_string(w.id) + ",\"url\":" + JsonQuote(ToUtf8(w.url)) + ",\"title\":" + JsonQuote(ToUtf8(w.title)) +
             ",\"mobile\":" + (w.mobile ? "true" : "false") + ",\"named\":" + (w.named ? "true" : "false") + "}";
    }
    j += "],\"reading\":[";
    for (size_t i = 0; i < s_reading.size(); ++i) {
        const ReadingItem& r = s_reading[i];
        if (i) j += ",";
        j += "{\"id\":" + std::to_string(r.id) + ",\"url\":" + JsonQuote(ToUtf8(r.url)) + ",\"title\":" + JsonQuote(ToUtf8(r.title)) +
             ",\"added\":" + std::to_string(r.added) + ",\"read\":" + (r.read ? "true" : "false") + "}";
    }
    j += "],\"notes\":[";
    for (size_t i = 0; i < s_notes.size(); ++i) {
        const Note& n = s_notes[i];
        if (i) j += ",";
        j += "{\"id\":" + std::to_string(n.id) + ",\"text\":" + JsonQuote(ToUtf8(n.text)) + ",\"created\":" +
             std::to_string(n.created) + ",\"modified\":" + std::to_string(n.modified) + "}";
    }
    j += "]}";
    const std::wstring tmp = StorePath() + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { Log("sidebar: cannot write %ls", tmp.c_str()); return; }
        f.write(j.data(), (std::streamsize)j.size());
    }
    MoveFileExW(tmp.c_str(), StorePath().c_str(), MOVEFILE_REPLACE_EXISTING);
}

long long Num(const JsonValue& v, const char* key) {
    const JsonValue* x = v.get(key);
    return x && x->type == JsonValue::Number ? (long long)x->n : 0;
}

void Load() {
    std::ifstream f(StorePath(), std::ios::binary);
    if (!f) return;
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    JsonValue root;
    if (!JsonParse(text, root) || !root.isObject()) { Log("sidebar: sidebar.json unreadable"); return; }
    s_nextId = std::max(1LL, Num(root, "next"));
    if (const JsonValue* a = root.get("web"); a && a->isArray())
        for (const auto& v : a->items) {
            auto w = std::make_unique<WebPanel>();
            w->id = Num(v, "id");
            w->url = ToWide(v.str("url"));
            w->title = ToWide(v.str("title"));
            w->mobile = v.flag("mobile", true);
            w->named = v.flag("named");
            if (w->id && !w->url.empty()) s_web.push_back(std::move(w));
        }
    if (const JsonValue* a = root.get("reading"); a && a->isArray())
        for (const auto& v : a->items) {
            ReadingItem r;
            r.id = Num(v, "id"); r.url = ToWide(v.str("url")); r.title = ToWide(v.str("title"));
            r.added = Num(v, "added"); r.read = v.flag("read");
            if (r.id && !r.url.empty()) s_reading.push_back(r);
        }
    if (const JsonValue* a = root.get("notes"); a && a->isArray())
        for (const auto& v : a->items) {
            Note n;
            n.id = Num(v, "id"); n.text = ToWide(v.str("text")); n.created = Num(v, "created"); n.modified = Num(v, "modified");
            if (n.id) s_notes.push_back(n);
        }
    for (auto& w : s_web) s_nextId = std::max(s_nextId, w->id + 1);
    for (auto& r : s_reading) s_nextId = std::max(s_nextId, r.id + 1);
    for (auto& n : s_notes) s_nextId = std::max(s_nextId, n.id + 1);
}

void SaveState() {
    WritePrivateProfileStringW(L"Sidebar", L"Open", std::to_wstring(s_open).c_str(), g_settingsFile.c_str());
    WritePrivateProfileStringW(L"Sidebar", L"Last", std::to_wstring(s_last).c_str(), g_settingsFile.c_str());
    WritePrivateProfileStringW(L"Sidebar", L"Width", std::to_wstring((int)s_width).c_str(), g_settingsFile.c_str());
}

WebPanel* WebAt(int item) {
    const int i = item - kWebBase;
    return i >= 0 && i < (int)s_web.size() ? s_web[i].get() : nullptr;
}

bool ValidItem(int item) { return (item >= 0 && item < kBuiltins) || WebAt(item); }

std::wstring ItemTitle(int item) {
    static const wchar_t* en[kBuiltins] = { L"Bookmarks", L"History", L"Reading list", L"Downloads", L"Notes" };
    static const wchar_t* ru[kBuiltins] = { L"Закладки", L"История", L"Список для чтения", L"Загрузки", L"Заметки" };
    if (item >= 0 && item < kBuiltins) return UiRussian() ? ru[item] : en[item];
    if (WebPanel* w = WebAt(item)) return w->title.empty() ? HostOf(w->url) : w->title;
    return std::wstring();
}

// --- data for the built-in page ---------------------------------------------------------------
std::string J(const std::wstring& s) { return JsonQuote(ToUtf8(s)); }

std::string BookmarksData() {
    std::string j = "[";
    for (const Bookmark& b : BookmarksAll()) {
        if (j.size() > 1) j += ",";
        j += "{\"i\":" + std::to_string(b.id) + ",\"p\":" + std::to_string(b.parent);
        if (b.folder) j += ",\"f\":1";
        j += ",\"t\":" + J(b.folder && b.title.empty() ? BookmarkFolderName(b.id) : b.title);   // (Favorites... are named by Lyra)
        if (!b.folder) j += ",\"u\":" + J(b.url);
        j += "}";
    }
    return j + "]";
}

std::string HistoryData(const std::wstring& query) {
    const std::vector<HistoryEntry> list = query.empty() ? HistoryNewest(1500) : HistoryMatch(query, 400);
    std::string j = "[";
    for (const HistoryEntry& e : list) {
        if (j.size() > 1) j += ",";
        j += "{\"u\":" + J(e.url) + ",\"t\":" + J(e.title) + ",\"l\":" + std::to_string(e.lastVisit) + "}";
    }
    return j + "]";
}

std::string ReadingData() {
    std::string j = "[";
    for (const ReadingItem& r : s_reading) {
        if (j.size() > 1) j += ",";
        j += "{\"i\":" + std::to_string(r.id) + ",\"u\":" + J(r.url) + ",\"t\":" + J(r.title) + ",\"a\":" +
             std::to_string(r.added) + ",\"r\":" + (r.read ? "1" : "0") + "}";
    }
    return j + "]";
}

std::string DownloadsData() {
    std::string j = "[";
    for (const DownloadItem& d : DownloadsList()) {
        if (j.size() > 1) j += ",";
        j += "{\"i\":" + std::to_string(d.id) + ",\"s\":" + std::to_string((int)d.state) + ",\"n\":" + J(d.name) +
             ",\"u\":" + J(d.url) + ",\"r\":" + std::to_string(d.received) + ",\"t\":" + std::to_string(d.total) +
             ",\"x\":" + (DownloadFileExists(d) ? "1" : "0") + ",\"e\":" + J(DownloadStatusText(d.id)) + "}";
    }
    return j + "]";
}

std::string NotesData() {
    std::string j = "[";
    for (const Note& n : s_notes) {
        if (j.size() > 1) j += ",";
        j += "{\"i\":" + std::to_string(n.id) + ",\"x\":" + J(n.text) + ",\"c\":" + std::to_string(n.created) +
             ",\"m\":" + std::to_string(n.modified) + "}";
    }
    return j + "]";
}

void IgnoreResult(WKTypeRef, WKErrorRef, void*) {}

void RunInPage(const std::string& script) {
    if (!s_page || !s_viewLoaded) return;
    if (WKStringRef s = WKStringCreateWithUTF8CString(script.c_str())) {
        WKPageEvaluateJavaScriptInMainFrame(s_page, s, nullptr, IgnoreResult);
        WKRelease(s);
    }
}

void Changed(const char* section) {
    if (s_open >= 0 && s_open < kBuiltins && strcmp(kSections[s_open], section) == 0)
        RunInPage(std::string("window.lyraChanged && lyraChanged('") + section + "')");
}

void OnBookmarksChanged() {
    if (s_prevBookmarksChanged) s_prevBookmarksChanged();
    Changed("bookmarks");
}

void OnDownloadsChanged(bool listChanged) {
    if (s_prevDownloadsChanged) s_prevDownloadsChanged(listChanged);
    Changed("downloads");
}

// Opens a link from a panel: in the open tab, a new tab, or a new tab behind.
void OpenLink(const std::wstring& url, const std::string& how) {
    if (url.empty()) return;
    if (how == "new") OpenInNewTab(url, false, false);
    else if (how == "background") OpenInNewTab(url, true, false);
    else { NavigateActive(url); FocusWebView(); }
}

std::string HandleMessage(const JsonValue& m) {
    const std::string op = m.str("op");
    if (op == "data") {
        const std::string s = m.str("section");
        if (s == "bookmarks") return BookmarksData();
        if (s == "history") return HistoryData(ToWide(m.str("q")));
        if (s == "reading") return ReadingData();
        if (s == "downloads") return DownloadsData();
        if (s == "notes") return NotesData();
        return "null";
    }
    if (op == "open") { OpenLink(ToWide(m.str("url")), m.str("how")); return "true"; }
    if (op == "page") {                                    // the whole page in a tab
        const std::string s = m.str("section");
        if (s == "bookmarks") OpenBookmarks();
        else if (s == "history") OpenHistory();
        else if (s == "downloads") OpenDownloads();
        return "true";
    }
    if (op == "favicons") {
        if (const JsonValue* a = m.get("urls"); a && a->isArray())
            for (size_t i = 0; i < a->items.size() && i < 60; ++i) FaviconFor(ToWide(a->items[i].s));
        return "true";
    }
    if (op == "history-remove") { HistoryRemove(ToWide(m.str("url"))); HistorySave(); return "true"; }
    if (op == "bookmark-remove") { BookmarkRemove(Num(m, "id")); return "true"; }
    if (op == "reading-add") {
        SidebarAddCurrentToReadingList();
        return ReadingData();
    }
    if (op == "reading-remove" || op == "reading-toggle") {
        const long long id = Num(m, "id");
        for (size_t i = 0; i < s_reading.size(); ++i) {
            if (s_reading[i].id != id) continue;
            if (op == "reading-remove") s_reading.erase(s_reading.begin() + (ptrdiff_t)i);
            else s_reading[i].read = !s_reading[i].read;
            break;
        }
        Save();
        return ReadingData();
    }
    if (op == "reading-open") {                            // opening marks it read
        const long long id = Num(m, "id");
        for (auto& r : s_reading)
            if (r.id == id) { r.read = true; OpenLink(r.url, m.str("how")); break; }
        Save();
        return ReadingData();
    }
    if (op == "note-save") {
        const long long id = Num(m, "id");
        const std::wstring text = ToWide(m.str("text"));
        Note* n = nullptr;
        for (auto& x : s_notes) if (x.id == id) n = &x;
        if (!n) {
            Note fresh;
            fresh.id = s_nextId++;
            fresh.created = NowMs();
            s_notes.insert(s_notes.begin(), fresh);
            n = &s_notes.front();
        }
        n->text = text;
        n->modified = NowMs();
        Save();
        return std::to_string(n->id);
    }
    if (op == "note-delete") {
        const long long id = Num(m, "id");
        s_notes.erase(std::remove_if(s_notes.begin(), s_notes.end(), [id](const Note& n) { return n.id == id; }), s_notes.end());
        Save();
        return NotesData();
    }
    if (op == "note-page") {                              // the open tab's title and address
        Tab* t = ActiveTab();
        if (!t || t->url.empty() || IsInternalPageUrl(t->url)) return "null";
        return "{\"t\":" + J(t->title) + ",\"u\":" + J(t->url) + "}";
    }
    if (op.rfind("dl-", 0) == 0) {
        const long long id = Num(m, "id");
        if (op == "dl-open") DownloadOpen(id);
        else if (op == "dl-show") DownloadShowInFolder(id);
        else if (op == "dl-cancel") DownloadCancel(id);
        else if (op == "dl-retry") DownloadRetry(id);
        else if (op == "dl-remove") DownloadRemove(id);
        else if (op == "dl-clear") DownloadsClear();
        else if (op == "dl-folder") OpenDownloadsFolder();
        return "true";
    }
    return "null";
}

void OnMessage(WKScriptMessageRef message, WKCompletionListenerRef reply, const void*) {
    std::string body;
    if (WKTypeRef b = WKScriptMessageGetBody(message); b && WKGetTypeID(b) == WKStringGetTypeID()) body = WKStrUtf8((WKStringRef)b);
    JsonValue m;
    std::string answer = "null";
    if (JsonParse(body, m) && m.isObject()) answer = HandleMessage(m);
    WKStringRef r = WKStringCreateWithUTF8CString(answer.c_str());
    WKCompletionListenerComplete(reply, r);
    WKRelease(r);
}

// --- views --------------------------------------------------------------------------------------
std::wstring PageUrl(int item) {
    std::wstring url = StartPageFileUrl();
    const size_t slash = url.rfind(L'/');
    if (slash != std::wstring::npos) url = url.substr(0, slash + 1) + L"sidebar.html";
    wchar_t accent[8];
    swprintf(accent, 8, L"%02X%02X%02X", g_th.accent.GetR(), g_th.accent.GetG(), g_th.accent.GetB());
    url += L"?dark=" + std::wstring(ResolveDark() ? L"1" : L"0");
    url += L"&ru=" + std::wstring(UiRussian() ? L"1" : L"0");
    url += L"&accent=" + std::wstring(accent);
    url += L"&icons=" + ToWide(UrlEncode(ToUtf8(FaviconFolderUrl())));
    url += L"#" + ToWide(kSections[std::max(0, std::min(kBuiltins - 1, item))]);
    return url;
}

void LoadUrl(WKPageRef page, const std::wstring& url) {
    if (WKURLRef u = WKURLCreateWithUTF8CString(ToUtf8(url).c_str())) {
        WKPageLoadURL(page, u);
        WKRelease(u);
    }
}

void OnInternalPolicy(WKPageRef, WKNavigationActionRef action, WKFramePolicyListenerRef listener, WKTypeRef, const void*) {
    std::wstring url;
    if (WKURLRequestRef req = WKNavigationActionCopyRequest(action)) {
        if (WKURLRef u = WKURLRequestCopyURL(req)) {
            if (WKStringRef s = WKURLCopyString(u)) { url = WKStr(s); WKRelease(s); }
            WKRelease(u);
        }
        WKRelease(req);
    }
    if (_wcsnicmp(url.c_str(), L"file:", 5) == 0 || _wcsnicmp(url.c_str(), L"about:", 6) == 0) { WKFramePolicyListenerUse(listener); return; }
    WKFramePolicyListenerIgnore(listener);
    if (!url.empty()) OpenLink(url, "tab");               // (a plain link in the page)
}

void OnInternalFinish(WKPageRef, WKNavigationRef, WKTypeRef, const void*) { s_viewLoaded = true; }

void EnsureInternalView(int item) {
    if (s_view || !g_context || !g_hWnd) return;
    WKPageConfigurationRef conf = WKPageConfigurationCreate();
    WKPageConfigurationSetContext(conf, g_context);
    // The tabs' preferences (rendering settings included): with WebKit's defaults the view showed
    // nothing at all once it was resized after it was made (opened by a click, window maximized).
    // The panel's page needs scripts and images whatever the user turned off for sites.
    WKPreferencesRef prefs = WKPreferencesCreateCopy(g_preferences);
    WKPreferencesSetJavaScriptEnabled(prefs, true);
    WKPreferencesSetLoadsImagesAutomatically(prefs, true);
    WKPageConfigurationSetPreferences(conf, prefs);
    if (g_dataStore) WKPageConfigurationSetWebsiteDataStore(conf, g_dataStore);
    s_controller = WKUserContentControllerCreate();
    WKStringRef name = WKStringCreateWithUTF8CString("lyraSidebar");
    WKUserContentControllerAddScriptMessageHandler(s_controller, name, OnMessage, nullptr);
    WKRelease(name);
    WKPageConfigurationSetUserContentController(conf, s_controller);
    RECT r = { (LONG)s_panel.X, (LONG)(s_panel.Y + S(kHeaderH)), (LONG)s_panel.GetRight(), (LONG)s_panel.GetBottom() };
    if (r.right <= r.left) r.right = r.left + 1;
    if (r.bottom <= r.top) r.bottom = r.top + 1;
    s_view = WKViewCreate(r, conf, g_hWnd);
    WKRelease(prefs);
    WKRelease(conf);
    if (!s_view) { Log("sidebar: WKViewCreate failed"); return; }
    WKViewSetUseDarkAppearance(s_view, g_dark);
    s_page = WKViewGetPage(s_view);
    s_viewHwnd = WKViewGetWindow(s_view);
    WKViewSetIsInWindow(s_view, true);
    WKPageNavigationClientV0 nc = {};
    nc.base.version = 0;
    nc.decidePolicyForNavigationAction = OnInternalPolicy;
    nc.didFinishNavigation = OnInternalFinish;
    WKPageSetPageNavigationClient(s_page, &nc.base);
    LoadUrl(s_page, PageUrl(item));
}

WKPageRef OnWebCreatePage(WKPageRef, WKPageConfigurationRef conf, WKNavigationActionRef, WKWindowFeaturesRef, const void*) {
    Tab* t = CreateTabWithConfiguration(conf, true);       // a link for a new window: a tab
    return t ? (WKPageRef)WKRetain(t->page) : nullptr;
}

void OnWebFinish(WKPageRef page, WKNavigationRef, WKTypeRef, const void*) {
    for (auto& w : s_web) {
        if (w->page != page || w->named) continue;
        if (WKStringRef ts = WKPageCopyTitle(page)) {
            std::wstring title = WKStr(ts);
            WKRelease(ts);
            if (!title.empty() && title != w->title && title.size() < 80) { w->title = title; Save(); InvalidateChrome(); }
        }
    }
}

void EnsureWebView(WebPanel& w) {
    if (w.view || !g_context || !g_hWnd) return;
    WKPageConfigurationRef conf = CreatePageConfigurationForTab();    // the profile's cookies: signed in as in tabs
    RECT r = { (LONG)s_panel.X, (LONG)(s_panel.Y + S(kHeaderH)), (LONG)std::max(s_panel.GetRight(), s_panel.X + 1), (LONG)std::max(s_panel.GetBottom(), s_panel.Y + S(kHeaderH) + 1) };
    w.view = WKViewCreate(r, conf, g_hWnd);
    WKRelease(conf);
    if (!w.view) return;
    WKViewSetUseDarkAppearance(w.view, g_dark);
    w.page = WKViewGetPage(w.view);
    w.hwnd = WKViewGetWindow(w.view);
    WKViewSetIsInWindow(w.view, true);
    if (w.mobile) {
        WKStringRef ua = WKStringCreateWithUTF8CString(ToUtf8(kMobileUA).c_str());
        WKPageSetCustomUserAgent(w.page, ua);
        WKRelease(ua);
    }
    WKPageUIClientV13 ui = {};
    ui.base.version = 13;
    ui.createNewPage = OnWebCreatePage;
    WKPageSetPageUIClient(w.page, &ui.base);
    WKPageNavigationClientV0 nc = {};
    nc.base.version = 0;
    nc.didFinishNavigation = OnWebFinish;
    WKPageSetPageNavigationClient(w.page, &nc.base);
    LoadUrl(w.page, w.url);
    Log("sidebar: web panel %ls started", HostOf(w.url).c_str());
}

void DestroyView(WKViewRef& view, WKPageRef& page, HWND& hwnd) {
    if (page) {
        WKPageNavigationClientV0 nc = {};
        nc.base.version = 0;
        WKPageSetPageNavigationClient(page, &nc.base);
        WKPageUIClientV0 ui = {};
        ui.base.version = 0;
        WKPageSetPageUIClient(page, &ui.base);
    }
    if (hwnd && IsWindow(hwnd)) DestroyWindow(hwnd);
    else if (page) WKPageClose(page);
    if (view) WKRelease(view);
    view = nullptr; page = nullptr; hwnd = nullptr;
}

float RevealEase() {                                     // ease-out cubic
    const float t = std::max(0.f, std::min(1.f, s_reveal));
    return 1.f - (1.f - t) * (1.f - t) * (1.f - t);
}

Color Faded(const Color& c, float a) {
    return Color((BYTE)std::lround(c.GetA() * std::max(0.f, std::min(1.f, a))), c.GetR(), c.GetG(), c.GetB());
}

void InvalRect(const RectF& r) {
    if (!g_hWnd || r.Width <= 0 || r.Height <= 0) return;
    RECT rc = { (LONG)std::floor(r.X) - 1, (LONG)std::floor(r.Y), (LONG)std::ceil(r.GetRight()) + 1, (LONG)std::ceil(r.GetBottom()) };
    InvalidateRect(g_hWnd, &rc, FALSE);
}

// The open item's centre on the strip, or -1.
float MarkTargetY() {
    if (s_closing >= 0) return -1;
    for (auto& it : s_items) if (it.first == s_open) return it.second.Y + it.second.Height / 2;
    return -1;
}

void FinishClose() {                                     // the panel has slid out: the page gets its room
    s_open = -1;
    s_closing = -1;
    s_revealDir = 0;
    s_reveal = 1;
    SaveState();
    RefreshLayout();
}

void SetOpen(int item, bool focus = true) {
    if (item >= 0 && !ValidItem(item)) item = -1;
    const bool anim = g_prefs.animations;
    AnimRequest();                                         // the strip's highlight moves
    if (item < 0) {                                        // closing: slide out first
        if (s_open < 0 || s_closing >= 0) return;
        Log("sidebar: closed %d", s_open);
        s_closing = s_open;
        if (focus) FocusWebView();
        if (!anim) { FinishClose(); return; }
        s_revealDir = -1;
        return;
    }
    const int shown = s_closing >= 0 ? -1 : s_open;
    if (item != shown) Log("sidebar: panel %d", item);
    const bool wasClosed = s_open < 0, wasClosing = s_closing >= 0;
    s_closing = -1;
    s_open = item;
    s_last = item;
    if (!anim) { s_reveal = 1; s_revealDir = 0; }
    else if (wasClosed || wasClosing) {
        if (wasClosed) s_reveal = 0;
        s_revealDir = 1;
    }
    if (item >= 0 && item < kBuiltins) {
        if (!s_view) EnsureInternalView(item);
        else RunInPage(std::string("window.lyraShow && lyraShow('") + kSections[item] + "')");
    } else if (WebPanel* w = WebAt(item)) {
        EnsureWebView(*w);
    }
    SaveState();
    RefreshLayout();
    if (!focus) return;
    if (item >= 0) {                                       // the panel takes the keyboard
        HWND focus = item < kBuiltins ? s_viewHwnd : (WebAt(item) ? WebAt(item)->hwnd : nullptr);
        if (focus) SetFocus(focus);
    } else {
        FocusWebView();
    }
}

void AddWebPanel() {
    PromptState st;
    st.message = Tr(L"The address of the site to keep in the sidebar (Telegram, Discord, a chat, music\x2026):",
                    L"Адрес сайта, который будет в боковой панели (Telegram, Discord, чат, музыка\x2026):");
    Tab* t = ActiveTab();
    if (t && !t->url.empty() && !IsInternalPageUrl(t->url)) st.value = t->url;
    {
        ModalScope modal;
        if (!PromptDialog(g_hWnd, Tr(L"Add a web panel", L"Добавить веб-панель"), st) || !st.ok) return;
    }
    std::wstring url = TrimWs(st.value);
    if (url.empty()) return;
    if (url.find(L"://") == std::wstring::npos) url = L"https://" + url;
    auto w = std::make_unique<WebPanel>();
    w->id = s_nextId++;
    w->url = url;
    w->title = HostOf(url);
    s_web.push_back(std::move(w));
    Save();
    Log("sidebar: web panel added (%ls)", HostOf(url).c_str());
    SetOpen(kWebBase + (int)s_web.size() - 1);
}

float PanelPx() { return std::floor(S(s_width)); }

}  // namespace

// --- public -------------------------------------------------------------------------------------
void SidebarInit() {
    Load();
    s_open = (int)GetPrivateProfileIntW(L"Sidebar", L"Open", -1, g_settingsFile.c_str());
    s_last = (int)GetPrivateProfileIntW(L"Sidebar", L"Last", 0, g_settingsFile.c_str());
    s_width = (float)GetPrivateProfileIntW(L"Sidebar", L"Width", (int)kDefaultW, g_settingsFile.c_str());
    s_width = std::max(kMinW, std::min(kMaxW, s_width));
    if (!ValidItem(s_open)) s_open = -1;
    if (!ValidItem(s_last)) s_last = 0;
    s_prevBookmarksChanged = g_bookmarksChanged;
    g_bookmarksChanged = &OnBookmarksChanged;
    s_prevDownloadsChanged = g_downloadsChanged;
    g_downloadsChanged = &OnDownloadsChanged;
    if (s_open >= 0 && g_prefs.sidebar) {
        const int item = s_open;
        s_open = -1;
        SetOpen(item, false);                              // as it was when Lyra closed
        s_reveal = 1;                                      // (without sliding in)
        s_revealDir = 0;
    }
}

void SidebarShutdown() {
    for (auto& w : s_web) DestroyView(w->view, w->page, w->hwnd);
    DestroyView(s_view, s_page, s_viewHwnd);
    if (s_controller) { WKRelease(s_controller); s_controller = nullptr; }
}

void SidebarInvalidate() {
    InvalRect(s_strip);
    InvalRect(s_panel);                                    // (the view on it is a child window: clipped)
}

RectF SidebarPanelRect() { return s_panel; }

bool SidebarAnimStep(float dt) {
    bool moving = false;
    if (s_revealDir != 0) {
        const float speed = std::max(10, g_prefs.animSpeed) / 100.f;
        s_reveal += s_revealDir * dt * speed / (s_revealDir > 0 ? 0.22f : 0.15f);
        if (s_reveal >= 1.f) { s_reveal = 1.f; s_revealDir = 0; }
        else if (s_reveal <= 0.f) { s_reveal = 0.f; s_revealDir = 0; }
        if (s_revealDir == 0 && s_closing >= 0) FinishClose();
        else {
            SidebarPositionViews();
            InvalRect(s_panel);
            moving = s_revealDir != 0;
        }
    }
    const float ty = MarkTargetY();
    const float ta = ty >= 0 ? 1.f : 0.f;
    if (ty >= 0 && (s_markY < 0 || s_markA < 0.02f || !g_prefs.animations)) s_markY = ty;   // appears in place
    const float yGoal = ty >= 0 ? ty : s_markY;
    if (std::fabs(yGoal - s_markY) > 0.3f || std::fabs(ta - s_markA) > 0.01f) {
        const float k = g_prefs.animations ? AnimFactor(dt, 45) : 1.f;
        s_markY += (yGoal - s_markY) * k;
        s_markA += (ta - s_markA) * k;
        if (std::fabs(yGoal - s_markY) <= 0.3f && std::fabs(ta - s_markA) <= 0.01f) { s_markY = yGoal; s_markA = ta; }
        else moving = true;
        InvalRect(s_strip);
    }
    return moving;
}

bool SidebarShown() { return g_prefs.sidebar; }
bool SidebarPanelOpen() { return g_prefs.sidebar && s_open >= 0 && s_closing < 0; }
bool SidebarResizing() { return s_resizing; }

float SidebarComputeLayout(Layout&, float left0, float top, float w, float h) {
    s_items.clear();
    s_buttons.clear();
    s_strip = s_panel = s_header = RectF();
    s_sepY = -1;
    s_left = !g_prefs.sidebarRight;
    if (!g_prefs.sidebar || g_fullScreenView) return 0;
    const float strip = std::floor(S(kStripW));
    float panel = s_open >= 0 ? PanelPx() : 0;
    const float avail = w - left0 - strip;
    panel = std::min(panel, std::max(0.f, std::floor(avail * 0.6f)));
    const float total = strip + panel;
    const float hh = std::max(0.f, h - top);
    if (s_left) {
        s_strip = RectF(left0, top, strip, hh);
        if (panel > 0) s_panel = RectF(left0 + strip, top, panel, hh);
    } else {
        s_strip = RectF(w - strip, top, strip, hh);
        if (panel > 0) s_panel = RectF(w - strip - panel, top, panel, hh);
    }
    if (panel > 0) s_header = RectF(s_panel.X, s_panel.Y, s_panel.Width, std::floor(S(kHeaderH)));
    // The icons.
    const float item = std::floor(S(kItem)), gap = std::floor(S(4));
    const float x = std::floor(s_strip.X + (strip - item) / 2);
    float y = top + std::floor(S(8));
    for (int i = 0; i < kBuiltins; ++i) { s_items.push_back({ i, RectF(x, y, item, item) }); y += item + gap; }
    s_sepY = y + std::floor(S(3));
    y += std::floor(S(10));
    for (int i = 0; i < (int)s_web.size() && y + item < top + hh; ++i) { s_items.push_back({ kWebBase + i, RectF(x, y, item, item) }); y += item + gap; }
    if (y + item < top + hh) s_items.push_back({ kAddItem, RectF(x, y, item, item) });
    // The header's buttons, from the right: close, open in a tab, (web panels) reload, back.
    if (panel > 0) {
        const float b = std::floor(S(28)), by = std::floor(s_header.Y + (s_header.Height - b) / 2);
        float bx = s_header.GetRight() - std::floor(S(8)) - b;
        const bool web = WebAt(s_open) != nullptr;
        const bool page = s_open == 0 || s_open == 1 || s_open == 3;
        const int count = web ? 4 : page ? 2 : 1;
        for (int i = 0; i < count; ++i) { s_buttons.push_back(RectF(bx, by, b, b)); bx -= b + std::floor(S(2)); }
    }
    { const float ty = MarkTargetY(); if ((ty >= 0) != (s_markA > 0.5f) || (ty >= 0 && std::fabs(ty - s_markY) > 0.3f)) AnimRequest(); }
    return total;
}

SideHit SidebarHitTest(float x, float y, int& index) {
    index = -1;
    if (s_strip.Width <= 0) return SideHit::None;
    if (s_panel.Width > 0) {
        const float edge = s_left ? s_panel.GetRight() : s_panel.X, band = S(4);
        if (y >= s_panel.Y && x >= edge - band && x <= edge + band) return SideHit::Resize;
    }
    if (s_strip.Contains(x, y)) {                          // (index -1: the strip's empty part)
        for (auto& it : s_items) if (it.second.Contains(x, y)) { index = it.first; return SideHit::Item; }
        return SideHit::Item;
    }
    if (s_header.Width > 0 && s_header.Contains(x, y)) {
        for (size_t i = 0; i < s_buttons.size(); ++i) if (s_buttons[i].Contains(x, y)) { index = (int)i; return SideHit::Button; }
        return SideHit::Empty;
    }
    return SideHit::None;
}

void SidebarPaint(Graphics& g, bool transparent) {
    if (s_strip.Width <= 0) return;
    const Theme& T = g_th;
    SolidBrush frame(T.frame);
    if (!transparent) g.FillRectangle(&frame, s_strip);
    if (s_panel.Width > 0) {
        // The panel is opaque, also with Mica (UpdateMainBackdrop keeps it out of the backdrop): the
        // built-in page's color under the view, which shows while the view slides in or out.
        SolidBrush page(ResolveDark() ? Color(255, 0x1F, 0x20, 0x24) : Color(255, 0xF7, 0xF8, 0xFA));
        g.FillRectangle(&page, s_panel);
        if (!transparent) g.FillRectangle(&frame, s_header);
    }
    Pen sep(T.sep, 1.f);
    // The line between the sidebar and the page.
    const float lineX = s_left ? (s_panel.Width > 0 ? s_panel.GetRight() : s_strip.GetRight()) - 0.5f
                               : (s_panel.Width > 0 ? s_panel.X : s_strip.X) + 0.5f;
    g.DrawLine(&sep, lineX, s_strip.Y, lineX, s_strip.GetBottom());
    if (s_panel.Width > 0) {                              // between the strip and the panel
        const float x2 = s_left ? s_strip.GetRight() - 0.5f : s_strip.X + 0.5f;
        g.DrawLine(&sep, x2, s_strip.Y, x2, s_strip.GetBottom());
    }
    const Hit hover = g_hover, down = g_down;
    if (s_markA > 0.01f && !s_items.empty() && s_markY >= 0) {   // the open panel's highlight, gliding between icons
        const RectF& r0 = s_items.front().second;
        FillRound(g, Faded(T.pressed, s_markA), RectF(r0.X, s_markY - r0.Height / 2, r0.Width, r0.Height), S(8));
        const float mx = s_left ? s_strip.X + S(2) : s_strip.GetRight() - S(5);
        const float mh = r0.Height * 0.5f * (0.3f + 0.7f * s_markA);
        FillRound(g, Faded(T.accent, s_markA), RectF(mx, s_markY - mh / 2, S(3), mh), S(1.5f));
    }
    for (auto& it : s_items) {
        const int code = it.first;
        const RectF& r = it.second;
        const bool open = code == s_open && s_closing < 0;
        const bool hov = hover.kind == Hit::SideItem && hover.index == code;
        const bool pressed = down.kind == Hit::SideItem && down.index == code && hov;
        if (pressed) FillRound(g, T.pressed, r, S(8));
        else if (hov && !open) FillRound(g, T.hover, r, S(8));
        const RectF icon(r.X + (r.Width - S(20)) / 2, r.Y + (r.Height - S(20)) / 2, S(20), S(20));
        if (code < kBuiltins) DrawGlyph(g, kGlyphs[code], r, open ? T.accent : T.icon, g_fIcon.get());
        else if (code == kAddItem) DrawGlyph(g, L"", r, T.icon, g_fIconSm.get());
        else if (WebPanel* w = WebAt(code)) {
            if (Bitmap* fav = FaviconFor(w->url)) g.DrawImage(fav, icon);
            else DrawGlyph(g, L"", r, T.icon, g_fIcon.get());
        }
    }
    if (s_sepY > 0) g.DrawLine(&sep, s_strip.X + S(12), s_sepY, s_strip.GetRight() - S(12), s_sepY);
    // The panel's header.
    if (s_header.Width > 0) {
        g.DrawLine(&sep, s_header.X, s_header.GetBottom() - 0.5f, s_header.GetRight(), s_header.GetBottom() - 0.5f);
        StringFormat sf(StringFormat::GenericTypographic());
        sf.SetFormatFlags(sf.GetFormatFlags() | StringFormatFlagsNoWrap);
        sf.SetTrimming(StringTrimmingEllipsisCharacter);
        sf.SetLineAlignment(StringAlignmentCenter);
        const float ha = RevealEase();                    // the header's contents fade with the slide
        SolidBrush tb(Faded(T.text, ha));
        const float left = s_header.X + S(14);
        const float right = s_buttons.empty() ? s_header.GetRight() - S(8) : s_buttons.back().X - S(6);
        Font* f = g_fTabBold ? g_fTabBold.get() : g_fText.get();
        g.DrawString(ItemTitle(s_open).c_str(), -1, f, RectF(left, s_header.Y, std::max(0.f, right - left), s_header.Height), &sf, &tb);
        static const wchar_t* glyphs[] = { L"", L"", L"", L"" };   // close, open in tab, reload, back
        for (size_t i = 0; i < s_buttons.size(); ++i) {
            const bool hov = hover.kind == Hit::SideButton && hover.index == (int)i;
            const bool pressed = down.kind == Hit::SideButton && down.index == (int)i && hov;
            if (pressed) FillRound(g, T.pressed, s_buttons[i], S(6));
            else if (hov) FillRound(g, T.hover, s_buttons[i], S(6));
            DrawGlyph(g, glyphs[i], s_buttons[i], Faded(T.icon, ha), g_fIconSm.get());
        }
    }
}

void SidebarPositionViews() {
    const bool internalShown = s_panel.Width > 0 && s_open >= 0 && s_open < kBuiltins;
    WebPanel* web = s_panel.Width > 0 ? WebAt(s_open) : nullptr;
    const int x = (int)s_panel.X, y = (int)(s_panel.Y + std::floor(S(kHeaderH)));
    const int w = (int)s_panel.Width, h = std::max(0, (int)(s_panel.GetBottom() - (s_panel.Y + std::floor(S(kHeaderH)))));
    // Sliding in / out: the view is pushed toward the strip by part of its width and clipped to
    // the panel (only moved, not resized: the page in it is not laid out again on every frame).
    const int off = std::min(w, (int)std::lround((1.f - RevealEase()) * w * 0.35f));
    auto place = [&](HWND hwnd) {
        SetWindowPos(hwnd, nullptr, s_left ? x - off : x + off, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        HRGN cur = CreateRectRgn(0, 0, 0, 0);
        const bool clipped = GetWindowRgn(hwnd, cur) != ERROR;
        DeleteObject(cur);
        if (off > 0) SetWindowRgn(hwnd, CreateRectRgn(s_left ? off : 0, 0, s_left ? w : w - off, h), TRUE);
        else if (clipped) SetWindowRgn(hwnd, nullptr, TRUE);
        ShowWindow(hwnd, SW_SHOWNA);
    };
    if (s_viewHwnd) {
        if (internalShown) place(s_viewHwnd);
        else ShowWindow(s_viewHwnd, SW_HIDE);
    }
    for (auto& p : s_web) {
        if (!p->hwnd) continue;
        if (p.get() == web) place(p->hwnd);
        else ShowWindow(p->hwnd, SW_HIDE);
    }
}

void SidebarItemClick(int index) {
    if (index == kAddItem) { AddWebPanel(); return; }
    if (!ValidItem(index)) return;
    SetOpen(index == s_open && s_closing < 0 ? -1 : index);
}

void SidebarItemMiddleClick(int index) {
    if (WebPanel* w = WebAt(index)) OpenInNewTab(w->url, false, false);
}

void SidebarItemMenu(int index, POINT pt) {
    enum { kOpen = 1, kNewTab, kReload, kRename, kAddress, kMobile, kRemove, kSide, kHide };
    HMENU m = CreatePopupMenu();
    WebPanel* w = WebAt(index);
    if (w) {
        AppendMenuW(m, MF_STRING, kOpen, index == s_open ? Tr(L"Close panel", L"Закрыть панель") : Tr(L"Open panel", L"Открыть панель"));
        AppendMenuW(m, MF_STRING, kNewTab, Tr(L"Open in new tab", L"Открыть в новой вкладке"));
        AppendMenuW(m, MF_STRING | (w->page ? 0 : MF_GRAYED), kReload, Tr(L"Reload", L"Обновить"));
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, kRename, Tr(L"Rename\x2026", L"Переименовать\x2026"));
        AppendMenuW(m, MF_STRING, kAddress, Tr(L"Change address\x2026", L"Изменить адрес\x2026"));
        AppendMenuW(m, MF_STRING | (w->mobile ? MF_CHECKED : 0), kMobile, Tr(L"Mobile version", L"Мобильная версия"));
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, kRemove, Tr(L"Remove panel", L"Удалить панель"));
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        MenuSetIcon(m, kNewTab, L"");
        MenuSetIcon(m, kReload, L"");
        MenuSetIcon(m, kRename, L"");
        MenuSetIcon(m, kRemove, L"");
    }
    AppendMenuW(m, MF_STRING, kSide, g_prefs.sidebarRight ? Tr(L"Move sidebar to the left", L"Переместить панель влево")
                                                           : Tr(L"Move sidebar to the right", L"Переместить панель вправо"));
    AppendMenuW(m, MF_STRING, kHide, Tr(L"Hide sidebar", L"Скрыть боковую панель"));
    const int cmd = MenuTrack(m, TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, g_hWnd);
    DestroyMenu(m);
    w = WebAt(index);                                     // (the menu ran a message loop)
    switch (cmd) {
    case kOpen: SidebarItemClick(index); break;
    case kNewTab: if (w) OpenInNewTab(w->url, false, false); break;
    case kReload: if (w && w->page) WKPageReload(w->page); break;
    case kRename:
    case kAddress: {
        if (!w) break;
        PromptState st;
        st.message = cmd == kRename ? Tr(L"Name:", L"Название:") : Tr(L"Address:", L"Адрес:");
        st.value = cmd == kRename ? w->title : w->url;
        {
            ModalScope modal;
            if (!PromptDialog(g_hWnd, cmd == kRename ? Tr(L"Rename panel", L"Переименовать панель") : Tr(L"Change address", L"Изменить адрес"), st) || !st.ok) break;
        }
        w = WebAt(index);
        if (!w) break;
        std::wstring v = TrimWs(st.value);
        if (v.empty()) break;
        if (cmd == kRename) { w->title = v; w->named = true; }
        else {
            if (v.find(L"://") == std::wstring::npos) v = L"https://" + v;
            w->url = v;
            if (w->page) LoadUrl(w->page, v);
        }
        Save();
        InvalidateChrome();
        break;
    }
    case kMobile:
        if (!w) break;
        w->mobile = !w->mobile;
        Save();
        if (w->page) {
            WKStringRef ua = WKStringCreateWithUTF8CString(w->mobile ? ToUtf8(kMobileUA).c_str() : "");
            WKPageSetCustomUserAgent(w->page, ua);
            WKRelease(ua);
            WKPageReload(w->page);
        }
        break;
    case kRemove: {
        if (!w) break;
        const int i = index - kWebBase;
        DestroyView(w->view, w->page, w->hwnd);
        s_web.erase(s_web.begin() + i);
        if (s_open == index) s_open = -1;
        else if (s_open > index && s_open >= kWebBase) --s_open;
        if (s_last >= index && s_last >= kWebBase) s_last = 0;
        Save();
        SaveState();
        RefreshLayout();
        break;
    }
    case kSide:
        g_prefs.sidebarRight = !g_prefs.sidebarRight;
        SaveSettings();
        SidebarSettingsChanged();
        break;
    case kHide:
        g_prefs.sidebar = false;
        SaveSettings();
        SidebarSettingsChanged();
        break;
    default: break;
    }
}

void SidebarButtonClick(int index) {
    WebPanel* w = WebAt(s_open);
    switch (index) {
    case 0: SetOpen(-1); break;
    case 1:
        if (w) OpenInNewTab(w->page ? [&] {
            std::wstring u = w->url;
            if (WKURLRef cur = WKPageCopyActiveURL(w->page)) {
                if (WKStringRef s = WKURLCopyString(cur)) { u = WKStr(s); WKRelease(s); }
                WKRelease(cur);
            }
            return u;
        }() : w->url, false, false);
        else if (s_open == 0) OpenBookmarks();
        else if (s_open == 1) OpenHistory();
        else if (s_open == 3) OpenDownloads();
        break;
    case 2: if (w && w->page) WKPageReload(w->page); break;
    case 3: if (w && w->page && WKPageCanGoBack(w->page)) WKPageGoBack(w->page); break;
    default: break;
    }
}

void SidebarResizeBegin(int x) {
    s_resizing = true;
    s_resizeStartX = x;
    s_resizeStartW = s_width;
}

void SidebarResizeMove(int x) {
    if (!s_resizing) return;
    const float scale = S(1.f);
    const float dx = (float)(x - s_resizeStartX) / (scale > 0 ? scale : 1.f);
    const float w = std::max(kMinW, std::min(kMaxW, s_resizeStartW + (s_left ? dx : -dx)));
    if (std::fabs(w - s_width) < 0.5f) return;
    s_width = w;
    RefreshLayout();
}

void SidebarResizeEnd() {
    if (!s_resizing) return;
    s_resizing = false;
    Log("sidebar: panel width %d", (int)s_width);
    SaveState();
}

void SidebarToggle() {
    if (!g_prefs.sidebar) { g_prefs.sidebar = true; SaveSettings(); }
    SetOpen(s_open >= 0 && s_closing < 0 ? -1 : (ValidItem(s_last) ? s_last : 0));
}

void SidebarOpen(int item) {
    if (!g_prefs.sidebar) { g_prefs.sidebar = true; SaveSettings(); }
    SetOpen(item);
}

void SidebarSettingsChanged() {
    if (!g_prefs.sidebar && s_open >= 0) { s_open = -1; s_closing = -1; s_revealDir = 0; s_reveal = 1; SaveState(); }
    RefreshLayout();
    InvalidateRect(g_hWnd, nullptr, FALSE);
}

void SidebarAddCurrentToReadingList() {
    Tab* t = ActiveTab();
    if (!t || t->url.empty() || IsInternalPageUrl(t->url) || t->url == L"about:blank") return;
    for (auto& r : s_reading) if (r.url == t->url) { r.read = false; Save(); Changed("reading"); return; }
    ReadingItem r;
    r.id = s_nextId++;
    r.url = t->url;
    r.title = t->title.empty() ? t->url : t->title;
    r.added = NowMs();
    s_reading.insert(s_reading.begin(), r);
    Save();
    Log("sidebar: reading list + %ls", HostOf(r.url).c_str());
    Changed("reading");
}

void SidebarThemeChanged() {
    if (s_view) WKViewSetUseDarkAppearance(s_view, g_dark);
    for (auto& w : s_web) if (w->view) WKViewSetUseDarkAppearance(w->view, g_dark);
    if (s_page) { s_viewLoaded = false; LoadUrl(s_page, PageUrl(s_open >= 0 && s_open < kBuiltins ? s_open : s_last < kBuiltins ? s_last : 0)); }
}
