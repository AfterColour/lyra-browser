// Lyra - the sidebar (see the header).
//
//   strip    icons drawn with the chrome (ui/Chrome DrawChrome -> SidebarPaint); the built-in
//            panels, a line, the web panels with their site icons, "+";
//   panel    a child window of the main window (the "host": header with the title and buttons,
//            the view below it), beside the page or over it - see the behaviour modes below:
//              built-in panels: resources/sidebar.html in a view of its own. Its user content
//              controller has the "lyraSidebar" message handler, which no web page can reach;
//              the page asks for its data and gives commands through it, and Lyra tells it when
//              something changed (lyraChanged) or which section to show (lyraShow).
//              web panels: one view per site, kept alive while Lyra runs, with an iPhone
//              user agent by default (a narrow layout), links that open new windows go to tabs.
// Behaviour (Settings > Tabs > Sidebar behaviour):
//   beside the page   the panel takes room from the page (the page gets narrower);
//   over the page     the panel opens over the page, a click elsewhere closes it;
//   auto-hide         nothing beside the page: the strip (with the open panel) slides out over
//                     the page while the pointer is at the window's edge, and back when it leaves;
//   on hover          the strip stays; pointing at an icon opens its panel over the page, leaving
//                     closes it; a click on the icon keeps it open (until a click elsewhere).
//   A thread mouse hook (only in the last three) sees the pointer over any of Lyra's windows,
//   pages included; a 50 ms poll runs only while something waits to open or to hide.
// Stored in the profile: sidebar.json (web panels, reading list, notes) and [Sidebar] in
// settings.ini (the open panel and its width).
#include "ui/Sidebar.h"
#include "ui/Animation.h"
#include "app/Log.h"
#include "browser/Bookmarks.h"
#include "browser/Downloads.h"
#include "browser/Favicons.h"
#include "browser/History.h"
#include "browser/SplitView.h"
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

#include <windowsx.h>
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

// The host: the panel (and in auto-hide mode the strip) in a child window over the page's views.
HWND s_host = nullptr;
RectF s_hostRect;                                // where it goes (client coordinates), before any slide
int s_hKind = 0, s_hIdx = -1, s_dKind = 0, s_dIdx = -1;   // its hover / press: 1 item, 2 header button
float s_auto = 0;                                // auto-hide: the strip slid out (1) or away (0)
int s_autoDir = 0;
bool s_autoShown = false;                        // auto-hide: out or on its way
bool s_kbd = false;                              // auto-hide: brought out from the keyboard (stays)
bool s_pinned = false;                           // on hover: opened by a click (stays)
int s_hoverItem = -1;                            // on hover: the icon pointed at, opening soon
ULONGLONG s_hoverSince = 0, s_leaveSince = 0, s_zoneSince = 0;
HHOOK s_hook = nullptr;
UINT_PTR s_poll = 0;
int s_menuDepth = 0;                             // a sidebar menu or dialog is open: nothing hides
void EnsureHost();

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

int s_pendingShow = -1;                          // a section asked for before the page had loaded

void OnInternalFinish(WKPageRef, WKNavigationRef, WKTypeRef, const void*) {
    s_viewLoaded = true;
    if (s_pendingShow >= 0 && s_pendingShow < kBuiltins) RunInPage(std::string("window.lyraShow && lyraShow('") + kSections[s_pendingShow] + "')");
    s_pendingShow = -1;
}

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
    EnsureHost();
    if (!s_host) { WKRelease(prefs); WKRelease(conf); return; }
    RECT r = { 0, (LONG)S(kHeaderH), (LONG)std::max(1.f, s_panel.Width), (LONG)std::max(S(kHeaderH) + 1, s_panel.Height) };
    s_view = WKViewCreate(r, conf, s_host);
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
    EnsureHost();
    if (!s_host) { WKRelease(conf); return; }
    RECT r = { 0, (LONG)S(kHeaderH), (LONG)std::max(1.f, s_panel.Width), (LONG)std::max(S(kHeaderH) + 1, s_panel.Height) };
    w.view = WKViewCreate(r, conf, s_host);
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
        else if (!s_viewLoaded) s_pendingShow = item;      // (made ahead, still loading)
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
        ++s_menuDepth;
        const bool ok = PromptDialog(g_hWnd, Tr(L"Add a web panel", L"Добавить веб-панель"), st) && st.ok;
        --s_menuDepth;
        if (!ok) return;
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

// --- behaviour modes ------------------------------------------------------------------------------
enum { kDocked = 0, kOverlay = 1, kAutoHide = 2, kHover = 3 };
int Mode() { return std::max(0, std::min(3, g_prefs.sidebarMode)); }

Color PageColor() { return ResolveDark() ? Color(255, 0x1F, 0x20, 0x24) : Color(255, 0xF7, 0xF8, 0xFA); }

// The panel's slide: how far its contents are pushed toward the strip.
int PanelOff() {
    if (s_panel.Width <= 0) return 0;
    return std::min((int)s_panel.Width, (int)std::lround((1.f - RevealEase()) * s_panel.Width * 0.35f));
}

float AutoEase() {
    const float t = std::max(0.f, std::min(1.f, s_auto));
    return 1.f - (1.f - t) * (1.f - t) * (1.f - t);
}

void InvalHost() { if (s_host) InvalidateRect(s_host, nullptr, FALSE); }
void InvalStrip() { if (Mode() == kAutoHide) InvalHost(); else InvalRect(s_strip); }

// What is at (x, y), client coordinates of the main window; the strip, the panel's page-side edge
// (resize), the header and its buttons.
SideHit HitAll(float x, float y, int& index) {
    index = -1;
    if (s_strip.Width <= 0) return SideHit::None;
    if (s_panel.Width > 0 && s_open >= 0) {
        const float band = S(5);
        const bool edge = y >= s_panel.Y && (s_left ? (x >= s_panel.GetRight() - band && x <= s_panel.GetRight())
                                                    : (x >= s_panel.X && x <= s_panel.X + band));
        if (edge) return SideHit::Resize;
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

// The strip: icons, the open one's highlight, the line on its page side.
void PaintStrip(Graphics& g, bool fill, int hKind, int hIdx, int dKind, int dIdx) {
    const Theme& T = g_th;
    if (fill) {
        SolidBrush b(MicaActive() ? PageColor() : T.frame);
        g.FillRectangle(&b, s_strip);
    }
    Pen sep(T.sep, 1.f);
    const float x2 = s_left ? s_strip.GetRight() - 0.5f : s_strip.X + 0.5f;
    g.DrawLine(&sep, x2, s_strip.Y, x2, s_strip.GetBottom());
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
        const bool hov = hKind == 1 && hIdx == code;
        const bool pressed = dKind == 1 && dIdx == code && hov;
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
}

// The panel: the page's color, the header (title, buttons) and the line on its page side.
void PaintPanel(Graphics& g, int hKind, int hIdx, int dKind, int dIdx) {
    const Theme& T = g_th;
    SolidBrush page(PageColor());
    g.FillRectangle(&page, s_panel);
    if (!MicaActive()) { SolidBrush frame(T.frame); g.FillRectangle(&frame, s_header); }
    Pen sep(T.sep, 1.f);
    const float ex = s_left ? s_panel.GetRight() - 0.5f : s_panel.X + 0.5f;
    g.DrawLine(&sep, ex, s_panel.Y, ex, s_panel.GetBottom());
    g.DrawLine(&sep, s_header.X, s_header.GetBottom() - 0.5f, s_header.GetRight(), s_header.GetBottom() - 0.5f);
    StringFormat sf(StringFormat::GenericTypographic());
    sf.SetFormatFlags(sf.GetFormatFlags() | StringFormatFlagsNoWrap);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    sf.SetLineAlignment(StringAlignmentCenter);
    const float ha = RevealEase();                        // the header's contents fade with the slide
    SolidBrush tb(Faded(T.text, ha));
    const float left = s_header.X + S(14);
    const float right = s_buttons.empty() ? s_header.GetRight() - S(8) : s_buttons.back().X - S(6);
    Font* f = g_fTabBold ? g_fTabBold.get() : g_fText.get();
    g.DrawString(ItemTitle(s_open).c_str(), -1, f, RectF(left, s_header.Y, std::max(0.f, right - left), s_header.Height), &sf, &tb);
    static const wchar_t* glyphs[] = { L"", L"", L"", L"" };   // close, open in tab, reload, back
    for (size_t i = 0; i < s_buttons.size(); ++i) {
        const bool hov = hKind == 2 && hIdx == (int)i;
        const bool pressed = dKind == 2 && dIdx == (int)i && hov;
        if (pressed) FillRound(g, T.pressed, s_buttons[i], S(6));
        else if (hov) FillRound(g, T.hover, s_buttons[i], S(6));
        DrawGlyph(g, glyphs[i], s_buttons[i], Faded(T.icon, ha), g_fIconSm.get());
    }
}

void PaintHost(Graphics& g) {
    g.TranslateTransform(-s_hostRect.X, -s_hostRect.Y);
    if (s_panel.Width > 0 && s_open >= 0) {
        const GraphicsState st = g.Save();
        g.SetClip(s_panel);
        const int off = PanelOff();
        g.TranslateTransform((float)(s_left ? -off : off), 0);
        PaintPanel(g, s_hKind, s_hIdx, s_dKind, s_dIdx);
        g.Restore(st);
    }
    if (Mode() == kAutoHide) PaintStrip(g, true, s_hKind, s_hIdx, s_dKind, s_dIdx);
}

void SetHostHover(SideHit k, int idx) {
    const int kind = k == SideHit::Item ? 1 : k == SideHit::Button ? 2 : 0;
    if (!kind) idx = -1;
    if (kind == s_hKind && idx == s_hIdx) return;
    s_hKind = kind;
    s_hIdx = idx;
    InvalHost();
}

POINT HostToMain(HWND h, LPARAM lp) {
    POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    MapWindowPoints(h, g_hWnd, &p, 1);
    return p;
}

LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        PaintBuffered(dc, ps.rcPaint, true, [](Graphics& g) { PaintHost(g); });
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(g_hWnd, &p);
            int idx;
            if (s_resizing || HitAll((float)p.x, (float)p.y, idx) == SideHit::Resize) { SetCursor(LoadCursor(nullptr, IDC_SIZEWE)); return TRUE; }
        }
        break;
    case WM_MOUSEMOVE: {
        const POINT p = HostToMain(h, lp);
        if (s_resizing) { SidebarResizeMove(p.x); return 0; }
        int idx;
        SetHostHover(HitAll((float)p.x, (float)p.y, idx), idx);
        TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
        TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE:
        SetHostHover(SideHit::None, -1);
        return 0;
    case WM_LBUTTONDOWN: {
        const POINT p = HostToMain(h, lp);
        int idx;
        const SideHit k = HitAll((float)p.x, (float)p.y, idx);
        SetCapture(h);
        if (k == SideHit::Resize) { SidebarResizeBegin(p.x); return 0; }
        s_dKind = k == SideHit::Item ? 1 : k == SideHit::Button ? 2 : 0;
        s_dIdx = s_dKind ? idx : -1;
        InvalHost();
        return 0;
    }
    case WM_LBUTTONUP: {
        if (s_resizing) { ReleaseCapture(); SidebarResizeEnd(); return 0; }
        const int dk = s_dKind, di = s_dIdx;
        s_dKind = 0;
        s_dIdx = -1;
        if (GetCapture() == h) ReleaseCapture();
        const POINT p = HostToMain(h, lp);
        int idx;
        const SideHit k = HitAll((float)p.x, (float)p.y, idx);
        InvalHost();
        if (dk == 1 && k == SideHit::Item && idx == di && idx >= 0) SidebarItemClick(idx);
        else if (dk == 2 && k == SideHit::Button && idx == di) SidebarButtonClick(idx);
        return 0;
    }
    case WM_RBUTTONUP: {
        const POINT p = HostToMain(h, lp);
        int idx;
        if (HitAll((float)p.x, (float)p.y, idx) == SideHit::Item) {
            POINT sp = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ClientToScreen(h, &sp);
            SidebarItemMenu(idx, sp);
        }
        return 0;
    }
    case WM_MBUTTONUP: {
        const POINT p = HostToMain(h, lp);
        int idx;
        if (HitAll((float)p.x, (float)p.y, idx) == SideHit::Item) SidebarItemMiddleClick(idx);
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (s_resizing && (HWND)lp != h) SidebarResizeEnd();
        if ((HWND)lp != h && s_dKind) { s_dKind = 0; s_dIdx = -1; InvalHost(); }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, m, wp, lp);
}

void EnsureHost() {
    if (s_host || !g_hWnd) return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = HostProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"LyraSidebarHost";
        RegisterClassExW(&wc);
        registered = true;
    }
    s_host = CreateWindowExW(0, L"LyraSidebarHost", L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                             0, 0, 1, 1, g_hWnd, nullptr, GetModuleHandleW(nullptr), nullptr);
}

// --- pointer: auto-hide and hover -----------------------------------------------------------------
void CALLBACK PollProc(HWND, UINT, UINT_PTR, DWORD);

void NeedPoll() {
    const int mode = Mode();
    const bool need = g_prefs.sidebar && mode != kDocked &&
        (s_hoverItem >= 0 || s_leaveSince || s_zoneSince || (mode == kAutoHide && s_autoShown) ||
         (mode == kHover && s_open >= 0 && !s_pinned && s_closing < 0));
    if (need && !s_poll) s_poll = SetTimer(nullptr, 0, 50, PollProc);
    else if (!need && s_poll) { KillTimer(nullptr, s_poll); s_poll = 0; }
}

void ShowAuto(bool show) {
    const bool anim = g_prefs.animations;
    if (show) {
        s_leaveSince = 0;
        if (!s_autoShown || s_autoDir < 0) {
            s_autoShown = true;
            if (anim) { s_autoDir = 1; AnimRequest(); }
            else { s_auto = 1; s_autoDir = 0; }
            SidebarPositionViews();
        }
    } else {
        s_kbd = false;
        s_leaveSince = 0;
        if (s_autoShown && s_autoDir >= 0) {
            if (anim) { s_autoDir = -1; AnimRequest(); }
            else {
                s_auto = 0; s_autoDir = 0; s_autoShown = false;
                HWND f = GetFocus();
                if (f && s_host && IsChild(s_host, f)) FocusWebView();
                SidebarPositionViews();
            }
        }
    }
    NeedPoll();
}

bool In(const RectF& r, POINT p, float margin = 0) {
    return r.Width > 0 && p.x >= r.X - margin && p.x < r.GetRight() + margin && p.y >= r.Y - margin && p.y < r.GetBottom() + margin;
}

// The pointer is at `p` (main window client coordinates).
void UpdateHover(POINT p) {
    if (!g_prefs.sidebar || g_fullScreenView || s_strip.Width <= 0) return;
    const ULONGLONG now = GetTickCount64();
    const bool busy = s_menuDepth > 0 || s_resizing || (s_host && GetCapture() == s_host);
    const int mode = Mode();
    if (mode == kAutoHide) {
        const float zone = std::max(2.f, std::floor(S(3)));
        const bool inZone = p.y >= s_strip.Y && p.y < s_strip.GetBottom() &&
            (s_left ? (p.x >= s_strip.X - 1 && p.x < s_strip.X + zone) : (p.x >= s_strip.GetRight() - zone && p.x <= s_strip.GetRight()));
        if (inZone && !s_autoShown) { if (!s_zoneSince) s_zoneSince = now; }
        else s_zoneSince = 0;
        const bool over = s_autoShown && In(s_hostRect, p, S(10));
        if (inZone || over || busy || s_kbd) s_leaveSince = 0;
        else if (s_autoShown && s_autoDir >= 0 && !s_leaveSince) s_leaveSince = now;
    } else if (mode == kHover) {
        int item = -1;
        if (In(s_strip, p)) for (auto& it : s_items) if (it.second.Contains((float)p.x, (float)p.y)) item = it.first;
        if (!s_pinned && item >= 0 && item != kAddItem && (item != s_open || s_closing >= 0)) {
            if (s_hoverItem != item) { s_hoverItem = item; s_hoverSince = now; }
        } else s_hoverItem = -1;
        const bool open = s_open >= 0 && s_closing < 0;
        const bool over = In(s_strip, p) || (open && In(s_panel, p, S(10)));
        if (over || busy || s_pinned || !open) s_leaveSince = 0;
        else if (!s_leaveSince) s_leaveSince = now;
    }
    NeedPoll();
}

// Tests (LYRA_TEST_SIDEBAR=1): a pretend pointer moved by SidebarTestPointer, never the real one.
bool TestPointer() {
    static const int on = [] { wchar_t v[4] = {}; return GetEnvironmentVariableW(L"LYRA_TEST_SIDEBAR", v, 4) && v[0] == L'1' ? 1 : 0; }();
    return on != 0;
}
POINT s_testPt = { -10000, -10000 };

void CALLBACK PollProc(HWND, UINT, UINT_PTR, DWORD) {
    if (!g_hWnd) return;
    POINT p;
    if (TestPointer()) p = s_testPt;
    else { GetCursorPos(&p); ScreenToClient(g_hWnd, &p); }
    UpdateHover(p);
    const ULONGLONG now = GetTickCount64();
    const int mode = Mode();
    if (mode == kAutoHide && s_zoneSince && now - s_zoneSince >= 90) { s_zoneSince = 0; ShowAuto(true); }
    if (mode == kHover && s_hoverItem >= 0 && now - s_hoverSince >= 220) {
        const int item = s_hoverItem;
        s_hoverItem = -1;
        s_pinned = false;
        SetOpen(item, false);                              // (the keyboard stays where it is)
    }
    if (s_leaveSince && now - s_leaveSince >= (ULONGLONG)(mode == kAutoHide ? 450 : 350)) {
        s_leaveSince = 0;
        if (mode == kAutoHide) ShowAuto(false);
        else if (mode == kHover && !s_pinned) SetOpen(-1, false);
    }
    NeedPoll();
}

// A click at `p` on `hwnd` (one of Lyra's windows): elsewhere than the sidebar closes what opened.
void ClickAt(POINT p, HWND hwnd) {
    const bool inside = (s_host && (hwnd == s_host || IsChild(s_host, hwnd))) || (Mode() != kAutoHide && In(s_strip, p));
    if (inside || s_menuDepth > 0) return;
    switch (Mode()) {
    case kOverlay:
        if (s_open >= 0 && s_closing < 0) SetOpen(-1, false);
        break;
    case kHover:
        if (s_open >= 0 && s_closing < 0) { s_pinned = false; SetOpen(-1, false); }
        break;
    case kAutoHide:
        if (s_autoShown) ShowAuto(false);
        break;
    default:
        break;
    }
}

LRESULT CALLBACK MouseHookProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_hWnd && !TestPointer()) {
        const MOUSEHOOKSTRUCT* m = (const MOUSEHOOKSTRUCT*)lp;
        if (m->hwnd && GetAncestor(m->hwnd, GA_ROOT) == g_hWnd) {
            POINT p = m->pt;
            ScreenToClient(g_hWnd, &p);
            switch (wp) {
            case WM_MOUSEMOVE: case WM_NCMOUSEMOVE:
                UpdateHover(p);
                break;
            case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
            case WM_NCLBUTTONDOWN: case WM_NCRBUTTONDOWN: case WM_NCMBUTTONDOWN:
                ClickAt(p, m->hwnd);
                break;
            default:
                break;
            }
        }
    }
    return CallNextHookEx(s_hook, code, wp, lp);
}

void UpdateHook() {
    const bool want = g_prefs.sidebar && Mode() != kDocked;
    if (want && !s_hook) s_hook = SetWindowsHookExW(WH_MOUSE, MouseHookProc, nullptr, GetCurrentThreadId());
    else if (!want && s_hook) { UnhookWindowsHookEx(s_hook); s_hook = nullptr; }
}

void ResetTransient() {
    s_autoShown = false; s_auto = 0; s_autoDir = 0; s_kbd = false;
    s_hoverItem = -1; s_leaveSince = 0; s_zoneSince = 0;
    s_pinned = Mode() == kHover && s_open >= 0;
}

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
    ResetTransient();
    UpdateHook();
}

void SidebarShutdown() {
    for (auto& w : s_web) DestroyView(w->view, w->page, w->hwnd);
    DestroyView(s_view, s_page, s_viewHwnd);
    if (s_controller) { WKRelease(s_controller); s_controller = nullptr; }
    if (s_hook) { UnhookWindowsHookEx(s_hook); s_hook = nullptr; }
    if (s_poll) { KillTimer(nullptr, s_poll); s_poll = 0; }
    if (s_host && IsWindow(s_host)) DestroyWindow(s_host);
    s_host = nullptr;
}

UINT SidebarTestMessage() {
    static const UINT m = TestPointer() ? RegisterWindowMessageW(L"LyraSidebarTestPointer") : 0;
    return m;
}

void SidebarTestPointer(WPARAM kind, LPARAM lp) {
    if (!TestPointer()) return;
    s_testPt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    if (kind == 1) {                                       // a click there
        HWND h = ChildWindowFromPointEx(g_hWnd, s_testPt, CWP_SKIPINVISIBLE | CWP_SKIPTRANSPARENT);
        ClickAt(s_testPt, h ? h : g_hWnd);
    }
    UpdateHover(s_testPt);
    Log("sidebar test: pointer %ld,%ld%s shown=%d open=%d pinned=%d", s_testPt.x, s_testPt.y, kind == 1 ? " click" : "", (int)s_autoShown, s_open, (int)s_pinned);
}

void SidebarRaise() {
    if (s_host && IsWindowVisible(s_host)) SetWindowPos(s_host, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void SidebarInvalidate() {
    if (Mode() != kAutoHide) InvalRect(s_strip);
    if (Mode() == kDocked) InvalRect(s_panel);             // (the parent's fill under the panel)
    InvalHost();
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
            SidebarPositionViews();                        // (repaints what moved)
            moving = s_revealDir != 0;
        }
    }
    if (s_autoDir != 0) {                                  // auto-hide: the strip sliding out / back
        const float speed = std::max(10, g_prefs.animSpeed) / 100.f;
        s_auto += s_autoDir * dt * speed / (s_autoDir > 0 ? 0.18f : 0.16f);
        if (s_auto >= 1.f) { s_auto = 1.f; s_autoDir = 0; }
        else if (s_auto <= 0.f) {
            s_auto = 0.f;
            s_autoDir = 0;
            s_autoShown = false;
            HWND f = GetFocus();
            if (f && s_host && IsChild(s_host, f)) FocusWebView();
            NeedPoll();
        }
        SidebarPositionViews();
        moving = moving || s_autoDir != 0;
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
        InvalStrip();
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
    const int mode = Mode();
    const float total = mode == kDocked ? strip + panel : mode == kAutoHide ? 0.f : strip;   // what the page gives up
    const float hh = std::max(0.f, h - top);
    if (s_left) {
        s_strip = RectF(left0, top, strip, hh);
        if (panel > 0) s_panel = RectF(left0 + strip, top, panel, hh);
    } else {
        s_strip = RectF(w - strip, top, strip, hh);
        if (panel > 0) s_panel = RectF(w - strip - panel, top, panel, hh);
    }
    if (panel > 0) s_header = RectF(s_panel.X, s_panel.Y, s_panel.Width, std::floor(S(kHeaderH)));
    s_hostRect = s_panel;
    if (mode == kAutoHide) {                               // the strip goes into the host too
        const float l = panel > 0 ? std::min(s_strip.X, s_panel.X) : s_strip.X;
        const float r = panel > 0 ? std::max(s_strip.GetRight(), s_panel.GetRight()) : s_strip.GetRight();
        s_hostRect = RectF(l, top, r - l, hh);
    }
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

SideHit SidebarHitTest(float x, float y, int& index) {      // the main window: the strip it draws
    index = -1;
    if (Mode() == kAutoHide || !s_strip.Contains(x, y)) return SideHit::None;
    return HitAll(x, y, index);
}

void SidebarPaint(Graphics& g, bool transparent) {
    if (s_strip.Width <= 0) return;
    if (Mode() == kDocked && s_panel.Width > 0) {          // under the panel's window (shows while it slides)
        SolidBrush page(PageColor());
        g.FillRectangle(&page, s_panel);
    }
    if (Mode() == kAutoHide) return;                       // (the strip is in the host)
    const int hk = g_hover.kind == Hit::SideItem ? 1 : 0, dk = g_down.kind == Hit::SideItem ? 1 : 0;
    PaintStrip(g, !transparent, hk, g_hover.index, dk, g_down.index);
}

void SidebarPositionViews() {
    const int mode = Mode();
    // The built-in panels' page is made a moment after startup (hidden), so the first panel opens
    // at once instead of after the page has loaded.
    static bool s_prewarm = false;
    if (!s_view && !s_prewarm && g_prefs.sidebar && g_hWnd && s_strip.Width > 0) {
        s_prewarm = true;
        SetTimer(nullptr, 0, 2500, [](HWND, UINT, UINT_PTR id, DWORD) {
            KillTimer(nullptr, id);
            if (s_view || !g_prefs.sidebar || !g_hWnd) return;
            EnsureInternalView(s_last >= 0 && s_last < kBuiltins ? s_last : 0);
            SidebarPositionViews();
        });
    }
    const bool panelShown = s_panel.Width > 0 && s_open >= 0;
    const bool hostShown = mode == kAutoHide ? (s_strip.Width > 0 && s_autoShown) : panelShown;
    if (!s_host) {
        if (!hostShown) return;
        EnsureHost();
        if (!s_host) return;
    }
    // Over the page: what the host stops covering must be drawn again by the page. The page's view is
    // presented by the GPU process and does not repaint on its own when it is uncovered (the host's
    // old pixels, then black, stayed there): that part of its window is invalidated and the page is
    // asked for a fresh frame (either alone was not enough).
    static RECT s_lastCover = {};
    auto uncovered = [&](const RECT& cover) {
        if (mode != kDocked && !IsRectEmpty(&s_lastCover)) {
            RECT both;
            UnionRect(&both, &cover, &s_lastCover);
            if (!EqualRect(&both, &cover)) {
                const RECT was = s_lastCover;
                auto repaint = [&was](Tab* t) {
                    if (!t || !t->hwnd || !t->page) return;
                    RECT r = was;
                    MapWindowPoints(g_hWnd, t->hwnd, (POINT*)&r, 2);
                    RedrawWindow(t->hwnd, &r, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
                    WKPageForceRepaint(t->page, nullptr, [](WKErrorRef, void*) {});
                };
                Tab* t = ActiveTab();
                repaint(t);
                if (t) repaint(SplitPartnerOf(*t));
            }
        }
        s_lastCover = cover;
    };
    if (!hostShown) {
        // The views are hidden themselves too: a view only learns it is shown again from its own
        // WM_SHOWWINDOW (one made or left visible inside the hidden host stayed blank).
        if (s_viewHwnd && (GetWindowLongW(s_viewHwnd, GWL_STYLE) & WS_VISIBLE)) ShowWindow(s_viewHwnd, SW_HIDE);
        for (auto& p : s_web) if (p->hwnd && (GetWindowLongW(p->hwnd, GWL_STYLE) & WS_VISIBLE)) ShowWindow(p->hwnd, SW_HIDE);
        if (IsWindowVisible(s_host)) ShowWindow(s_host, SW_HIDE);
        uncovered(RECT {});
        return;
    }
    const RectF& hr = s_hostRect;
    const int hw = std::max(1, (int)hr.Width), hh = std::max(1, (int)hr.Height);
    const int autoOff = mode == kAutoHide ? (int)std::lround((1.f - AutoEase()) * (hw + S(2))) : 0;
    const int hx = (int)hr.X + (s_left ? -autoOff : autoOff);
    // (It is painted right away on every frame of a slide, below: copied bits are its last frame.)
    SetWindowPos(s_host, HWND_TOP, hx, (int)hr.Y, hw, hh, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    // While the panel or the auto-hide strip slides, its view stays hidden and the host shows the
    // panel's color: a moved or clipped view lost its picture (black until the page drew again),
    // and keeping it filled on every frame took most of a frame. It appears when the slide ends.
    const int off = panelShown ? PanelOff() : 0;
    const bool sliding = off > 0 || autoOff > 0 || s_revealDir != 0 || s_autoDir != 0;
    const int px = (int)(s_panel.X - hr.X), pw = (int)s_panel.Width;
    const int top = (int)std::floor(S(kHeaderH)), ph = std::max(0, hh - top);
    const bool internalShown = panelShown && !sliding && s_open < kBuiltins;
    WebPanel* web = panelShown && !sliding ? WebAt(s_open) : nullptr;
    auto place = [&](HWND v, WKViewRef view) {
        const bool wasHidden = !(GetWindowLongW(v, GWL_STYLE) & WS_VISIBLE);
        SetWindowPos(v, nullptr, px, top, std::max(1, pw), std::max(1, ph), SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(v, SW_SHOWNA);
        // Shown again: without a nudge a view that was hidden kept showing nothing until it got the
        // keyboard focus, and its old picture is gone: its window is repainted and the page asked
        // for a fresh frame.
        if (wasHidden && view) {
            WKViewSetIsInWindow(view, false);
            WKViewSetIsInWindow(view, true);
            RedrawWindow(v, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
            WKPageForceRepaint(WKViewGetPage(view), nullptr, [](WKErrorRef, void*) {});
        }
    };
    if (s_viewHwnd) {
        if (internalShown) place(s_viewHwnd, s_view);
        else if (GetWindowLongW(s_viewHwnd, GWL_STYLE) & WS_VISIBLE) ShowWindow(s_viewHwnd, SW_HIDE);
    }
    for (auto& p : s_web) {
        if (!p->hwnd) continue;
        if (p.get() == web) place(p->hwnd, p->view);
        else if (GetWindowLongW(p->hwnd, GWL_STYLE) & WS_VISIBLE) ShowWindow(p->hwnd, SW_HIDE);
    }
    // What the slides uncover: the panel's far side, and (auto-hide on the left) the tab list.
    int rl = 0, rr = hw;
    if (off > 0) { if (s_left) rr = std::min(rr, px + pw - off); else rl = std::max(rl, px + off); }
    if (mode == kAutoHide && s_left) rl = std::max(rl, (int)s_strip.X - hx);
    HRGN cur = CreateRectRgn(0, 0, 0, 0);
    const bool clipped = GetWindowRgn(s_host, cur) != ERROR;
    DeleteObject(cur);
    if (rl > 0 || rr < hw) SetWindowRgn(s_host, CreateRectRgn(rl, 0, std::max(rl, rr), hh), TRUE);
    else if (clipped) SetWindowRgn(s_host, nullptr, TRUE);
    uncovered(RECT { hx + rl, (LONG)hr.Y, hx + std::max(rl, rr), (LONG)hr.Y + hh });
    static bool s_wasSliding = false;
    if (sliding || s_wasSliding) {
        // The header moves with the panel's slide; the rest is one color, and what a slide newly
        // shows is invalidated by Windows (a moved host keeps its painted pixels).
        RECT band = { 0, 0, hw, top };
        InvalidateRect(s_host, s_wasSliding && !sliding ? nullptr : &band, FALSE);
        UpdateWindow(s_host);                              // painted now, between the animation's frames
    }
    s_wasSliding = sliding;
}

void SidebarItemClick(int index) {
    if (index == kAddItem) { AddWebPanel(); return; }
    if (!ValidItem(index)) return;
    if (Mode() == kHover) {                                // a click keeps it open; again: closes
        if (index == s_open && s_closing < 0 && s_pinned) { s_pinned = false; SetOpen(-1); }
        else { s_pinned = true; s_hoverItem = -1; SetOpen(index); }
        NeedPoll();
        return;
    }
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
    ++s_menuDepth;
    const int cmd = MenuTrack(m, TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, g_hWnd);
    --s_menuDepth;
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
            ++s_menuDepth;
            const bool ok = PromptDialog(g_hWnd, cmd == kRename ? Tr(L"Rename panel", L"Переименовать панель") : Tr(L"Change address", L"Изменить адрес"), st) && st.ok;
            --s_menuDepth;
            if (!ok) break;
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
    if (!g_prefs.sidebar) { g_prefs.sidebar = true; SaveSettings(); UpdateHook(); }
    const bool close = s_open >= 0 && s_closing < 0 && (Mode() != kAutoHide || s_autoShown);
    if (close) {
        SetOpen(-1);
        if (Mode() == kAutoHide) ShowAuto(false);
        s_pinned = false;
        return;
    }
    if (Mode() == kAutoHide) { s_kbd = true; ShowAuto(true); }
    if (Mode() == kHover) s_pinned = true;
    SetOpen(ValidItem(s_last) ? s_last : 0);
    NeedPoll();
}

void SidebarOpen(int item) {
    if (!g_prefs.sidebar) { g_prefs.sidebar = true; SaveSettings(); UpdateHook(); }
    if (Mode() == kAutoHide) { s_kbd = true; ShowAuto(true); }
    if (Mode() == kHover) s_pinned = true;
    SetOpen(item);
    NeedPoll();
}

void SidebarSettingsChanged() {
    if (!g_prefs.sidebar && s_open >= 0) { s_open = -1; s_closing = -1; s_revealDir = 0; s_reveal = 1; SaveState(); }
    ResetTransient();
    UpdateHook();
    NeedPoll();
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
    InvalHost();
    if (s_view) WKViewSetUseDarkAppearance(s_view, g_dark);
    for (auto& w : s_web) if (w->view) WKViewSetUseDarkAppearance(w->view, g_dark);
    if (s_page) { s_viewLoaded = false; LoadUrl(s_page, PageUrl(s_open >= 0 && s_open < kBuiltins ? s_open : s_last < kBuiltins ? s_last : 0)); }
}
