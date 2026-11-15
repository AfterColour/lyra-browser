// Lyra — список вкладок: создание, закрытие, переключение, навигация, восстановление.
#include "browser/TabManager.h"
#include "browser/Extensions.h"
#include "browser/PictureInPicture.h"
#include "browser/AdBlock.h"
#include "browser/History.h"
#include "browser/StartPage.h"
#include "core/Localization.h"
#include "app/Log.h"
#include "browser/PageClients.h"
#include "browser/Session.h"
#include "browser/SplitView.h"
#include "browser/Tab.h"
#include "browser/WebContext.h"
#include "core/Constants.h"
#include "core/Settings.h"
#include "core/StringUtils.h"
#include "core/Url.h"
#include "ui/AddressBar.h"
#include "ui/Chrome.h"
#include "ui/Dialogs.h"
#include "ui/FullScreen.h"
#include "ui/MainWindow.h"
#include "ui/TabTransition.h"

static void UpdateWindowTitle();
static void Navigate(WKPageRef page, const std::string& utf8Url);
static WKPageConfigurationRef CreatePageConfiguration();
static void DetachTab(Tab& t);
static void DestroyTab(Tab& t);
static void RememberClosedTab(const Tab& t);

std::vector<std::wstring> g_closedUrls;        // for Ctrl+Shift+T (most recent last)

std::vector<std::unique_ptr<Tab>> g_tabs;

int g_active = -1;

static unsigned long long g_nextTabId = 1;

std::vector<std::unique_ptr<TabCtx>> g_ctxs;   // owns every TabCtx until shutdown (see TabCtx)

bool g_closing = false;        // main window is shutting down

bool g_tabOp = false;          // a create/close operation is in progress (re-entrancy guard)

bool g_closeRequested = false; // WM_CLOSE already posted for "last tab closed"

static std::vector<unsigned long long> s_mru;  // tab ids, most recently active first
static std::unique_ptr<Tab> s_spare;           // see PrepareSpareTab
static TabCtx* s_spareCtx = nullptr;
static std::wstring s_spareUrl;
static bool s_adopting = false;                // SelectTab for the spare start page (it is drawn already)
// A parked tab still runs at full speed (WebKit sees it as visible): after a while it is hidden
// like the others, so a busy page (video, animations) does not keep taking CPU and GPU time.
static std::map<unsigned long long, DWORD> s_parkedAt;
constexpr DWORD kReadyForMs = 20000;

Tab* ActiveTab() {
    return (g_active >= 0 && g_active < (int)g_tabs.size()) ? g_tabs[g_active].get() : nullptr;
}

// ----------------------------------------------------------------------------
//  Tabs / WebKit
// ----------------------------------------------------------------------------
static void UpdateWindowTitle() {
    if (!g_hWnd) return;
    Tab* t = ActiveTab();
    std::wstring title = (t && !t->title.empty()) ? t->title : std::wstring(kAppName);
    SetWindowTextW(g_hWnd, title.c_str());
}

// Pulls URL / title / history state from WebKit. Returns true if anything changed.
bool SyncTab(Tab& t) {
    if (!t.page || t.lazyPending) return false;   // a restored tab not loaded yet keeps its saved address
    bool changed = false, titleChanged = false;

    std::wstring url;
    if (WKURLRef u = WKPageCopyActiveURL(t.page)) {
        if (WKStringRef s = WKURLCopyString(u)) { url = WKStr(s); WKRelease(s); }
        WKRelease(u);
    }
    const bool urlChanged = url != t.url;
    if (urlChanged) { t.url = url; changed = true; }

    std::wstring title;
    if (WKStringRef ts = WKPageCopyTitle(t.page)) { title = WKStr(ts); WKRelease(ts); }
    // History: a new address is a visit; a page title arriving later names it.
    // (At the moment the address changes the title can still be the previous page's.)
    if (t.privateTab) { }                         // private tabs leave no history
    else if (urlChanged) HistoryVisit(url, L"");
    else if (!title.empty() && title != t.title) HistorySetTitle(url, title);
    if (title.empty()) {
        title = HostOf(t.url);
        if (title.empty()) title = Tr(L"New Tab", L"Новая вкладка");
    }
    if (title != t.title) { t.title = title; changed = titleChanged = true; }

    bool b = WKPageCanGoBack(t.page), f = WKPageCanGoForward(t.page);
    if (b != t.canBack) { t.canBack = b; changed = true; }
    if (f != t.canFwd)  { t.canFwd = f;  changed = true; }

    if (titleChanged && &t == ActiveTab()) UpdateWindowTitle();
    return changed;
}

// ----------------------------------------------------------------------------
//  Ready (parked) views and the spare new tab
// ----------------------------------------------------------------------------
static void ParkView(HWND h, int w, int hgt) {
    if (!h) return;
    // Left of the window's client area: clipped away completely, but still a visible window,
    // so WebKit keeps the page rendered.
    SetWindowPos(h, nullptr, -(w + 400), 0, std::max(1, w), std::max(1, hgt),
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
}

static bool IsReady(const Tab& t) {
    auto parked = s_parkedAt.find(t.id);
    if (parked != s_parkedAt.end() && GetTickCount() - parked->second > kReadyForMs) return false;
    int n = 0;
    for (unsigned long long id : s_mru) {
        if (n >= g_prefs.warmTabs) break;
        const Tab* a = ActiveTab();
        if (a && id == a->id) continue;
        if (id == t.id) return true;
        if (FindTab(id)) ++n;
    }
    return false;
}

void ParkBackgroundViews(int w, int h) {
    const Tab* active = ActiveTab();
    for (auto& tp : g_tabs) {
        Tab& t = *tp;
        if (&t == active || !t.hwnd || t.state != TabState::Live) continue;
        if (t.hwnd == g_fullScreenView || SplitVisible(t)) continue;   // (the other half is placed by SplitLayout)
        if (IsReady(t)) {
            if (!s_parkedAt.count(t.id)) s_parkedAt[t.id] = GetTickCount();
            ParkView(t.hwnd, w, h);
        } else if (IsWindowVisible(t.hwnd)) {
            ShowWindow(t.hwnd, SW_HIDE);
        }
    }
    if (s_spare && s_spare->hwnd) ParkView(s_spare->hwnd, w, h);
}

void ExpireReadyTabs() {
    const DWORD now = GetTickCount();
    for (auto& [id, at] : s_parkedAt) {
        if (now - at <= kReadyForMs) continue;
        Tab* t = FindTab(id);
        if (t && t->hwnd && t != ActiveTab() && !SplitVisible(*t) && IsWindowVisible(t->hwnd)) {
            ShowWindow(t->hwnd, SW_HIDE);
            Log("tab#%llu no longer kept ready", id);
        }
    }
}

static void TouchMru(unsigned long long id) {
    s_parkedAt.erase(id);
    s_mru.erase(std::remove(s_mru.begin(), s_mru.end(), id), s_mru.end());
    s_mru.insert(s_mru.begin(), id);
}

void SelectTab(int i) {
    if (i < 0 || i >= (int)g_tabs.size()) return;
    if (g_fullScreenView && (!g_tabs[i]->hwnd || g_tabs[i]->hwnd != g_fullScreenView))
        ExitFullScreenForTabSwitch();
    // A tab shown before (or the prepared start page) is covered by its last picture until
    // WebKit has drawn it again; a brand-new tab is not (it has nothing to show yet).
    Tab* from = ActiveTab();
    Tab* to = g_tabs[i].get();
    // The other half of the split view is already on screen: no cover, nothing goes hidden.
    const bool withinSplit = SplitPaired(from, to);
    const bool seen = std::find(s_mru.begin(), s_mru.end(), to->id) != s_mru.end();
    if (from != to && !withinSplit && (seen || s_adopting)) TransitionBegin(from, to);
    if (from && from != to && !withinSplit) {
        from->hiddenSince = GetTickCount64();
        if (Tab* half = SplitPartnerOf(*from)) if (half != to) half->hiddenSince = GetTickCount64();
    }
    g_tabs[i]->hiddenSince = 0;
    g_active = i;
    TouchMru(g_tabs[i]->id);
    // The compact layout depends on the active tab; LayoutWebViews puts the new view in place
    // (still parked views are already rendered) and parks or hides the others.
    RefreshLayout(true);
    if (HWND h = g_tabs[i]->hwnd) {
        SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ShowWindow(h, SW_SHOW);
    }
    SessionLoadLazyTab(*g_tabs[i]);
    if (from != to) ExtensionsOnTabActivated(to);
    if (IsEditing()) ShowWindow(g_urlEdit, SW_HIDE);
    UpdateWindowTitle();
    InvalidateChrome();
    FocusWebView();
}

// --- WebKit client callbacks (one set of clients per tab, clientInfo = TabCtx*) ----
// A callback is honoured only if its context is still attached to a Live tab.
Tab* ResolveTab(const void* clientInfo, const char* what) {
    TabCtx* c = (TabCtx*)clientInfo;
    if (!c) { Log("callback %s: NULL context -> ignored", what); return nullptr; }
    Tab* t = c->tab;
    if (g_closing || !t || t->state != TabState::Live) {
        Log("callback %s: tab#%llu is closed/closing -> ignored", what, c->id);
        return nullptr;
    }
    return t;
}

Tab* FindTab(unsigned long long id, int* index) {
    for (int i = 0; i < (int)g_tabs.size(); ++i) {
        Tab* t = g_tabs[i].get();
        if (t->id == id && t->state == TabState::Live) {
            if (index) *index = i;
            return t;
        }
    }
    return nullptr;
}

// --- tab lifetime -----------------------------------------------------------------
static void Navigate(WKPageRef page, const std::string& utf8Url) {
    if (!page || utf8Url.empty()) return;
    WKURLRef url = WKURLCreateWithUTF8CString(utf8Url.c_str());
    if (url) {
        WKPageLoadURL(page, url);
        WKRelease(url);
    }
}

void NavigateActive(const std::wstring& input) {
    if (Tab* t = ActiveTab()) Navigate(t->page, ResolveNavigation(input));
}

static WKPageConfigurationRef CreatePageConfiguration() {
    WKPageConfigurationRef conf = WKPageConfigurationCreate();
    WKPageConfigurationSetContext(conf, g_context);
    WKPageConfigurationSetPreferences(conf, g_preferences);
    if (g_dataStore) WKPageConfigurationSetWebsiteDataStore(conf, g_dataStore);
    if (WKUserContentControllerRef content = AdBlockUserContentController())
        WKPageConfigurationSetUserContentController(conf, content);
    return conf;
}

// Creates a view from `conf` (not consumed) and makes it the active tab.
WKPageConfigurationRef CreatePageConfigurationForTab() { return CreatePageConfiguration(); }

Tab* CreateTabWithConfiguration(WKPageConfigurationRef conf, bool openedByPage, bool select) {
    if (g_closing) return nullptr;
    if (g_tabOp) { Log("CreateTab re-entered during a tab operation -> ignored"); return nullptr; }
    if ((int)g_tabs.size() >= metrics::kMaxTabs) {
        Log("CreateTab: tab limit (%d) reached", metrics::kMaxTabs);
        MessageBeep(MB_ICONWARNING);
        return nullptr;
    }
    TabOpGuard guard;

    RECT cr;
    GetClientRect(g_hWnd, &cr);
    int top = (int)g_L.chromeH, left = (int)g_L.sideW;
    RECT r = { left, top, std::max<LONG>(cr.right, left + 1), std::max<LONG>(cr.bottom, top + 1) };
    WKViewRef view = WKViewCreate(r, conf, g_hWnd);
    if (!view) { Log("CreateTab: WKViewCreate failed"); return nullptr; }

    WKPageRef page = WKViewGetPage(view);
    HWND viewWnd = WKViewGetWindow(view);
    if (!page || !viewWnd) {            // nothing usable was created: do not keep a half-initialised tab
        Log("CreateTab: view has no page/hwnd -> releasing view");
        if (viewWnd && IsWindow(viewWnd)) DestroyWindow(viewWnd);
        WKRelease(view);
        return nullptr;
    }

    WKViewSetIsInWindow(view, true);

    auto tab = std::make_unique<Tab>();
    auto ctx = std::make_unique<TabCtx>();
    ctx->id  = g_nextTabId++;
    ctx->tab = tab.get();
    tab->id    = ctx->id;
    tab->ctx   = ctx.get();
    tab->state = TabState::Live;
    tab->view  = view;
    tab->page  = page;
    tab->hwnd  = viewWnd;
    tab->openedByPage = openedByPage;
    tab->hiddenSince = GetTickCount64();
    Tab* raw = tab.get();

    SetPageClients(page, ctx.get());
    WKStringRef appName = WKStringCreateWithUTF8CString(kUserAgentAppName);
    WKPageSetApplicationNameForUserAgent(page, appName);
    WKRelease(appName);
    WKPageSetPageZoomFactor(page, g_zoom);

    g_ctxs.push_back(std::move(ctx));
    g_tabs.push_back(std::move(tab));
    Log("tab#%llu CREATED view=%p page=%p hwnd=%p popup=%d (tabs=%d)", raw->id, (void*)raw->view,
        (void*)raw->page, (void*)raw->hwnd, (int)openedByPage, (int)g_tabs.size());
    ExtensionsOnTabCreated(raw);
    RefreshLayout(true);                 // the new tab grows in
    if (select) SelectTab((int)g_tabs.size() - 1);
    else ShowWindow(raw->hwnd, SW_HIDE);
    return raw;
}

bool UnloadTab(Tab& t, const std::string& state) {
    if (g_closing || g_tabOp || g_modalDepth > 0 || t.state != TabState::Live || t.privateTab || !t.view || !t.page) return false;
    if (&t == ActiveTab() || t.pipHwnd || (t.hwnd && t.hwnd == g_fullScreenView)) return false;
    TabOpGuard guard;
    RECT cr;
    GetClientRect(g_hWnd, &cr);
    const int top = (int)g_L.chromeH, left = (int)g_L.sideW;
    RECT r = { left, top, std::max<LONG>(cr.right, left + 1), std::max<LONG>(cr.bottom, top + 1) };
    WKPageConfigurationRef conf = CreatePageConfiguration();
    WKViewRef view = WKViewCreate(r, conf, g_hWnd);
    WKRelease(conf);
    if (!view) return false;
    WKPageRef page = WKViewGetPage(view);
    HWND hwnd = WKViewGetWindow(view);
    if (!page || !hwnd) {
        if (hwnd && IsWindow(hwnd)) DestroyWindow(hwnd);
        WKRelease(view);
        return false;
    }
    WKViewSetIsInWindow(view, true);
    ShowWindow(hwnd, SW_HIDE);

    // The old page: no more callbacks, then closed with its view (as in DestroyTab).
    WKViewRef oldView = t.view;
    WKPageRef oldPage = t.page;
    HWND oldHwnd = t.hwnd;
    ClearPageClients(oldPage);
    t.view = view;
    t.page = page;
    t.hwnd = hwnd;
    SetPageClients(page, t.ctx);
    WKStringRef appName = WKStringCreateWithUTF8CString(kUserAgentAppName);
    WKPageSetApplicationNameForUserAgent(page, appName);
    WKRelease(appName);
    WKPageSetPageZoomFactor(page, g_zoom);
    if (oldHwnd && IsWindow(oldHwnd)) DestroyWindow(oldHwnd);
    else WKPageClose(oldPage);
    WKRelease(oldView);

    s_parkedAt.erase(t.id);
    TransitionForget(t.id);                // its last picture (memory) goes too
    t.lazyPending = true;
    t.lazyUrl = t.url;
    t.lazyState = state;
    t.sleeping = true;
    t.loading = false;
    t.progress = t.fade = 0.f;
    t.audible = false;
    t.translatable = t.translated = t.translateCollecting = false;
    t.readerable = false;
    return true;
}

static bool IsStartInput(const std::wstring& input) {
    const std::wstring s = TrimWs(input);
    return _wcsicmp(s.c_str(), kStartAlias) == 0 || _wcsicmp(s.c_str(), L"lyra://start/") == 0;
}

void ScheduleSpareTab(UINT delayMs) {
    if (g_hWnd && g_prefs.spareTab && !g_closing) SetTimer(g_hWnd, TIMER_SPARE_TAB, delayMs, nullptr);
}

void PrepareSpareTab() {
    if (!g_prefs.spareTab || s_spare || g_closing || g_tabOp || !g_hWnd || !g_context) return;
    TabOpGuard guard;
    RECT cr;
    GetClientRect(g_hWnd, &cr);
    const int top = (int)g_L.chromeH, left = (int)g_L.sideW;
    const int w = std::max<int>(1, cr.right - left), h = std::max<int>(1, cr.bottom - top);
    RECT r = { -(w + 400), 0, -400, h };
    WKPageConfigurationRef conf = CreatePageConfiguration();
    WKViewRef view = WKViewCreate(r, conf, g_hWnd);
    WKRelease(conf);
    if (!view) return;
    WKPageRef page = WKViewGetPage(view);
    HWND viewWnd = WKViewGetWindow(view);
    if (!page || !viewWnd) {
        if (viewWnd && IsWindow(viewWnd)) DestroyWindow(viewWnd);
        WKRelease(view);
        return;
    }
    WKViewSetIsInWindow(view, true);
    auto tab = std::make_unique<Tab>();
    auto ctx = std::make_unique<TabCtx>();
    ctx->id = g_nextTabId++;
    ctx->tab = nullptr;                  // no callbacks reach Lyra until the tab is used
    tab->id = ctx->id;
    tab->ctx = ctx.get();
    tab->view = view;
    tab->page = page;
    tab->hwnd = viewWnd;
    SetPageClients(page, ctx.get());
    WKStringRef appName = WKStringCreateWithUTF8CString(kUserAgentAppName);
    WKPageSetApplicationNameForUserAgent(page, appName);
    WKRelease(appName);
    WKPageSetPageZoomFactor(page, g_zoom);
    s_spareCtx = ctx.get();
    g_ctxs.push_back(std::move(ctx));
    s_spare = std::move(tab);
    ParkView(viewWnd, w, h);
    s_spareUrl = StartPageUrl();
    Navigate(page, ToUtf8(s_spareUrl));
    Log("spare tab#%llu prepared", s_spare->id);
}

void DiscardSpareTab() {
    KillTimer(g_hWnd, TIMER_SPARE_TAB);
    if (!s_spare) return;
    std::unique_ptr<Tab> dead = std::move(s_spare);
    s_spareCtx = nullptr;
    DetachTab(*dead);
    DestroyTab(*dead);
}

// Ctrl+T with the start page: the prepared one becomes the new tab.
static bool AdoptSpareTab() {
    if (!s_spare || g_closing || g_tabOp || (int)g_tabs.size() >= metrics::kMaxTabs) return false;
    if (!s_spare->page || !s_spare->hwnd || !IsWindow(s_spare->hwnd)) { DiscardSpareTab(); return false; }
    std::unique_ptr<Tab> tab = std::move(s_spare);
    Tab* raw = tab.get();
    if (s_spareCtx) s_spareCtx->tab = raw;
    s_spareCtx = nullptr;
    raw->state = TabState::Live;
    g_tabs.push_back(std::move(tab));
    Log("tab#%llu CREATED from the spare start page (tabs=%d)", raw->id, (int)g_tabs.size());
    SyncTab(*raw);
    RefreshLayout(true);
    s_adopting = true;
    SelectTab((int)g_tabs.size() - 1);
    s_adopting = false;
    // The start page changed meanwhile (favourites, theme...): load the current one.
    const std::wstring fresh = StartPageUrl();
    if (fresh != s_spareUrl) Navigate(raw->page, ToUtf8(fresh));
    ScheduleSpareTab(150);               // ready again for the next Ctrl+T
    return true;
}

// Lyra's own pages (start, history, bookmarks, downloads): a new tab with one of them keeps
// showing the tab being left until the page has loaded and drawn.
static bool IsLyraPageInput(const std::wstring& input) {
    const std::wstring s = TrimWs(input);
    return _wcsnicmp(s.c_str(), L"lyra://", 7) == 0 || IsInternalPageUrl(s);
}

void CreateTab(const std::wstring& input) {
    if (IsStartInput(input) && AdoptSpareTab()) return;
    const bool covered = IsLyraPageInput(input) && TransitionCoverFrom(ActiveTab(), IsStartInput(input));
    WKPageConfigurationRef conf = CreatePageConfiguration();
    Tab* t = CreateTabWithConfiguration(conf, false);
    WKRelease(conf);
    if (t) TransitionNewTab(t->id);
    if (covered) TransitionWaitLoad(t);
    if (t) Navigate(t->page, ResolveNavigation(input));
}

// Private tabs share one memory-only website data store (cookies, storage, cache). It is
// dropped with the last private tab, so whatever they kept is gone.
static WKWebsiteDataStoreRef s_privateStore = nullptr;

void CreatePrivateTab() {
    if (!s_privateStore) s_privateStore = WKWebsiteDataStoreCreateNonPersistentDataStore();
    WKPageConfigurationRef conf = CreatePageConfiguration();
    if (s_privateStore) WKPageConfigurationSetWebsiteDataStore(conf, s_privateStore);
    Tab* t = CreateTabWithConfiguration(conf, false);
    WKRelease(conf);
    if (!t) return;
    t->privateTab = true;
    InvalidateChrome();
    Navigate(t->page, ToUtf8(PrivatePageUrl()));
}

Tab* OpenInNewTab(const std::wstring& input, bool background, bool privateTab) {
    if (privateTab && !s_privateStore) s_privateStore = WKWebsiteDataStoreCreateNonPersistentDataStore();
    WKPageConfigurationRef conf = CreatePageConfiguration();
    if (privateTab && s_privateStore) WKPageConfigurationSetWebsiteDataStore(conf, s_privateStore);
    Tab* t = CreateTabWithConfiguration(conf, false, !background);
    WKRelease(conf);
    if (!t) return nullptr;
    t->privateTab = privateTab;
    t->url = input;                                // shown until the page commits
    InvalidateChrome();
    Navigate(t->page, ResolveNavigation(input));
    return t;
}

static void ReleasePrivateStoreIfUnused() {
    if (!s_privateStore) return;
    for (auto& t : g_tabs) if (t->privateTab) return;
    WKRelease(s_privateStore);
    s_privateStore = nullptr;
    Log("private: the last private tab closed, its data is gone");
}

// Stops every path by which the tab can still be reached from our side:
// callbacks (context detached) and anything that looks tabs up (it must already
// be out of g_tabs). Does not touch WebKit.
static void DetachTab(Tab& t) {
    t.state = TabState::Closing;
    if (t.ctx) t.ctx->tab = nullptr;
}

// Releases the WebKit objects of a tab that is already out of g_tabs and detached.
//
// Order matters. WKViewCreate makes a child HWND whose window data points at the C++
// WebView. WKRelease(view) frees that WebView but does NOT destroy the HWND, so the
// window would stay alive with a dangling pointer and the next message it receives
// (WM_PAINT, WM_SIZE, or WM_DESTROY when the main window closes) runs on freed memory.
// That was the crash at WebKit2.dll+0x6C4036 / WebCore.dll+0xD0AF5 in tabs.log.
//   1. replace our clients with empty ones (no more callbacks into this tab),
//   2. DestroyWindow(view hwnd) - WebView handles WM_DESTROY by closing its page
//      (WebView::closeInternal -> WebPageProxy::close), exactly like MiniBrowser,
//   3. WKRelease(view) - drop the reference WKViewCreate returned (create rule).
static void DestroyTab(Tab& t) {
    PipDetach(t);                        // the view must be back in the main window first
    const unsigned long long id = t.id;
    WKViewRef view = t.view;
    WKPageRef page = t.page;
    HWND      hwnd = t.hwnd;
    t.view = nullptr; t.page = nullptr; t.hwnd = nullptr;   // a second call is a no-op
    if (hwnd && hwnd == g_fullScreenView) RestoreFromFullScreen();
    Log("tab#%llu destroy: begin view=%p page=%p hwnd=%p", id, (void*)view, (void*)page, (void*)hwnd);

    if (page) ClearPageClients(page);
    if (hwnd && IsWindow(hwnd)) {
        Log("tab#%llu destroy: DestroyWindow(view)", id);
        DestroyWindow(hwnd);
    } else if (page) {
        Log("tab#%llu destroy: no view window -> WKPageClose", id);
        WKPageClose(page);
    }
    if (view) {
        Log("tab#%llu destroy: WKRelease(view)", id);
        WKRelease(view);
    }
    t.state = TabState::Closed;
    Log("tab#%llu destroy: end", id);
}

static void RememberClosedTab(const Tab& t) {
    if (t.privateTab) return;
    if (t.url.empty() || t.url == L"about:blank") return;
    g_closedUrls.push_back(t.url);
    if (g_closedUrls.size() > 25) g_closedUrls.erase(g_closedUrls.begin());
}

void ReopenClosedTab() {
    if (g_closedUrls.empty()) return;
    std::wstring url = g_closedUrls.back();
    g_closedUrls.pop_back();
    CreateTab(url);
}

void MoveTab(int from, int to) {
    const int n = (int)g_tabs.size();
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
    const unsigned long long activeId = ActiveTab() ? ActiveTab()->id : 0;
    std::unique_ptr<Tab> t = std::move(g_tabs[from]);
    g_tabs.erase(g_tabs.begin() + from);
    g_tabs.insert(g_tabs.begin() + to, std::move(t));
    if (activeId) FindTab(activeId, &g_active);
    g_hover = Hit();
    g_down = Hit();
}

int PinnedTabCount() {
    int n = 0;
    for (auto& t : g_tabs) if (t->pinned) ++n;
    return n;
}

void SetTabPinned(int index, bool pinned) {
    if (index < 0 || index >= (int)g_tabs.size() || g_tabs[index]->pinned == pinned) return;
    Tab& t = *g_tabs[index];
    if (pinned && SplitHas(t)) SplitClose();            // a pinned tab is not a half of a split view
    const int count = PinnedTabCount();
    t.pinned = pinned;
    Log("tab#%llu %s", t.id, pinned ? "pinned" : "unpinned");
    if (FindTab(t.id, &index)) MoveTab(index, pinned ? count : count - 1);
    RefreshLayout(true);
    InvalidateChrome();
}

void DropTabAt(unsigned long long id, int insertAt) {
    int from = -1;
    Tab* t = FindTab(id, &from);
    if (!t) return;
    const int n = (int)g_tabs.size();
    insertAt = std::clamp(insertAt, 0, n);
    const int to = insertAt > from ? insertAt - 1 : insertAt;          // its index once moved
    const int pinnedOthers = PinnedTabCount() - (t->pinned ? 1 : 0);
    // Right after the last pinned tab: a pinned tab stays pinned, an ordinary one stays ordinary.
    const bool pinned = to < pinnedOthers || (t->pinned && to == pinnedOthers);
    if (to == from && pinned == t->pinned) return;
    if (pinned && SplitHas(*t)) SplitClose();
    Log("tab#%llu moved %d -> %d%s", id, from, to, pinned != t->pinned ? (pinned ? " (pinned)" : " (unpinned)") : "");
    t->pinned = pinned;
    if (FindTab(id, &from)) MoveTab(from, to);
    RefreshLayout(true);
    InvalidateChrome();
}

void CloseTab(int i) {
    if (g_closing) return;
    if (g_tabOp) { Log("CloseTab(%d) re-entered during a tab operation -> ignored", i); return; }
    if (g_modalDepth > 0) { Log("CloseTab(%d) while a page dialog is open -> ignored", i); return; }
    if (i < 0 || i >= (int)g_tabs.size()) return;
    if (g_tabs.size() == 1) {            // closing the last tab closes the window (like Safari)
        Log("CloseTab: last tab tab#%llu -> posting WM_CLOSE (already requested=%d)",
            g_tabs[0]->id, (int)g_closeRequested);
        if (!g_closeRequested) { g_closeRequested = true; PostMessageW(g_hWnd, WM_CLOSE, 0, 0); }
        return;
    }
    TabOpGuard guard;
    const bool wasActive = g_active == i;

    Log("CloseTab: begin index=%d tab#%llu loading=%d active=%d tabs=%d", i, g_tabs[i]->id,
        (int)g_tabs[i]->loading, g_active, (int)g_tabs.size());
    RememberClosedTab(*g_tabs[i]);
    ExtensionsOnTabRemoved(g_tabs[i]->id);

    // 1) Take the tab out of our own state and cut off callbacks, so nothing that
    //    runs while WebKit objects are being destroyed can reach it.
    std::unique_ptr<Tab> dead = std::move(g_tabs[i]);
    g_tabs.erase(g_tabs.begin() + i);
    s_mru.erase(std::remove(s_mru.begin(), s_mru.end(), dead->id), s_mru.end());
    s_parkedAt.erase(dead->id);
    const unsigned long long otherHalf = SplitTabClosed(dead->id);
    DetachTab(*dead);
    if (g_active > i) --g_active;
    else if (g_active == i) {
        int half = -1;                                   // a half of the split view closed: the other one
        if (otherHalf && FindTab(otherHalf, &half)) g_active = half;
        else g_active = std::min(i, (int)g_tabs.size() - 1);
    }
    // The tab that comes forward is covered by its last picture until it has drawn (no empty
    // page area); the closing tab's own picture goes with it.
    if (wasActive && g_active >= 0 && g_active < (int)g_tabs.size()) TransitionBegin(dead.get(), g_tabs[g_active].get());
    TransitionForget(dead->id);
    g_hover = Hit();
    g_down  = Hit();
    RefreshLayout(true);                 // g_L.tabs must match g_tabs before any repaint

    // 2) Hide the dead view and give focus to the surviving tab BEFORE the focused
    //    child window is destroyed.
    if (dead->hwnd) ShowWindow(dead->hwnd, SW_HIDE);
    SelectTab(g_active);
    Log("CloseTab: new active=%d tab#%llu", g_active, ActiveTab() ? ActiveTab()->id : 0ULL);

    // 3) Only now release the WebKit objects.
    DestroyTab(*dead);
    ReleasePrivateStoreIfUnused();
    InvalidateChrome();
    Log("CloseTab: done (tabs=%d)", (int)g_tabs.size());
}

void CloseAllTabs() {
    TabOpGuard guard;
    Log("CloseAllTabs: begin (tabs=%d)", (int)g_tabs.size());
    // Detach the whole list first: while views are torn down g_tabs is empty and every
    // context is detached, so timers / paint / late callbacks never see these tabs.
    std::vector<std::unique_ptr<Tab>> dead;
    dead.swap(g_tabs);
    if (s_spare) { dead.push_back(std::move(s_spare)); s_spareCtx = nullptr; }
    for (unsigned long long id : s_mru) TransitionForget(id);
    TransitionCancel();
    s_mru.clear();
    g_active = -1;
    g_hover = Hit();
    g_down  = Hit();
    for (auto& t : dead) DetachTab(*t);
    for (auto& t : dead) DestroyTab(*t);
    Log("CloseAllTabs: done");
}

// Posted from OnWebProcessDidTerminate.
void HandleTabCrashed(unsigned long long id, int reason) {
    Tab* t = FindTab(id);
    if (!t) return;
    StopLoadingIndicator(*t);
    std::wstring url = t->url;
    if (reason == kWKProcessTerminationReasonExceededMemoryLimit)
        LoadInfoPage(*t, Tr(L"This page used too much memory", L"Странице не хватило памяти"),
                     Tr(L"The page was stopped to keep the rest of the browser responsive.",
                        L"Страница остановлена, чтобы остальной браузер продолжал работать."), L"", url);
    else
        LoadInfoPage(*t, Tr(L"This page crashed", L"Страница аварийно завершилась"),
                     Tr(L"Something went wrong while displaying this page. Other tabs are not affected.",
                        L"При показе страницы произошёл сбой. Другие вкладки не пострадали."), L"", url);
}
