// Lyra — список вкладок: создание, закрытие, переключение, навигация, восстановление.
#include "browser/TabManager.h"
#include "browser/Touchpad.h"
#include "ui/Sidebar.h"
#include "ui/TabHoverCard.h"
#include "browser/SystemProxy.h"
#include "browser/Extensions.h"
#include "browser/PictureInPicture.h"
#include "browser/AdBlock.h"
#include "browser/History.h"
#include "browser/StartPage.h"
#include "browser/TabGroups.h"
#include "browser/Workspaces.h"
#include "ui/LinkStatus.h"
#include "core/Localization.h"
#include "app/Log.h"
#include "browser/PageClients.h"
#include "browser/Session.h"
#include "browser/SplitView.h"
#include "browser/Tab.h"
#include "browser/WebContext.h"
#include "core/Constants.h"
#include "ui/Commands.h"
#include "core/Settings.h"
#include "core/StringUtils.h"
#include "core/Url.h"
#include "ui/AddressBar.h"
#include "ui/Chrome.h"
#include "ui/Dialogs.h"
#include "ui/FullScreen.h"
#include "ui/MainWindow.h"
#include "ui/TabTransition.h"
#include "ui/Theme.h"

static void UpdateWindowTitle();
static void Navigate(WKPageRef page, const std::string& utf8Url);
static WKPageConfigurationRef CreatePageConfiguration();
static void DetachTab(Tab& t);
static void DestroyTab(Tab& t);
static void RememberClosedTab(const Tab& t, int index);

std::vector<ClosedTab> g_closedTabs;           // for Ctrl+Shift+T (most recent last)

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
// A tab just left stays parked (rendered, ready to come back at once) only for a few seconds: a
// parked view counts as visible, so its page keeps animating, and every picture it presents waits
// for the display's refresh on the GPU process's only thread - taking frames from the tab in view.
constexpr DWORD kReadyForMs = 3000;
// The spare new tab is kept on screen (parked) only while it draws its first pictures: a parked
// view counts as visible, so its page keeps animating and the GPU process keeps compositing and
// presenting it (the start page's night sky, its frosted widgets) - that slowed down the whole
// browser. Hidden afterwards; a new tab covers the page area until it has drawn again.
constexpr DWORD kSpareShownMs = 2500;
static DWORD s_spareAt = 0;

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
    if (s_spare && s_spare->hwnd) {
        if (GetTickCount() - s_spareAt < kSpareShownMs) ParkView(s_spare->hwnd, w, h);
        else if (IsWindowVisible(s_spare->hwnd)) ShowWindow(s_spare->hwnd, SW_HIDE);
    }
}

void ExpireReadyTabs() {
    const DWORD now = GetTickCount();
    if (s_spare && s_spare->hwnd && now - s_spareAt >= kSpareShownMs && IsWindowVisible(s_spare->hwnd)) {
        ShowWindow(s_spare->hwnd, SW_HIDE);
        Log("spare tab#%llu hidden", s_spare->id);
    }
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
    i = WorkspaceActivateFor(i);                         // another workspace's tab: that workspace opens
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
    TabGroupsTabSelected(*g_tabs[i]);   // a collapsed group opens
    LinkStatusHide();
    TouchMru(g_tabs[i]->id);
    // The compact layout depends on the active tab; LayoutWebViews puts the new view in place
    // (still parked views are already rendered) and parks or hides the others.
    RefreshLayout(true);
    if (HWND h = g_tabs[i]->hwnd) {
        SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ShowWindow(h, SW_SHOW);
        SidebarRaise();                  // (a panel over the page stays over it)
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
    Tab* t = ActiveTab();
    if (!t) return;
    const std::string target = ResolveNavigation(input);
    if (_strnicmp(target.c_str(), "lyra:", 5) == 0) { HandleLyraUrl(t->id, ToWide(target)); return; }   // lyra://settings ...
    Navigate(t->page, target);
}

static WKPageConfigurationRef CreatePageConfiguration() {
    WKPageConfigurationRef conf = WKPageConfigurationCreate();
    WKPageConfigurationSetContext(conf, g_context);
    WKPageConfigurationSetPreferences(conf, g_preferences);
    if (g_dataStore) WKPageConfigurationSetWebsiteDataStore(conf, g_dataStore);
    if (WKUserContentControllerRef content = AdBlockUserContentController())
        WKPageConfigurationSetUserContentController(conf, content);
    ExtensionsSetupPageConfiguration(conf);       // lyra-extension:// files for extensions' scripts and pages
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
    int top = (int)g_L.chromeH, left = (int)g_L.pageL;
    RECT r = { left, top, std::max<LONG>((LONG)g_L.pageR, left + 1), std::max<LONG>(cr.bottom, top + 1) };
    WKViewRef view = WKViewCreate(r, conf, g_hWnd);
    if (!view) { Log("CreateTab: WKViewCreate failed"); return nullptr; }
    WKViewSetUseDarkAppearance(view, g_dark);

    WKPageRef page = WKViewGetPage(view);
    ExtensionsSetupPage(page);
    HWND viewWnd = WKViewGetWindow(view);
    if (!page || !viewWnd) {            // nothing usable was created: do not keep a half-initialised tab
        Log("CreateTab: view has no page/hwnd -> releasing view");
        if (viewWnd && IsWindow(viewWnd)) DestroyWindow(viewWnd);
        WKRelease(view);
        return nullptr;
    }

    WKViewSetIsInWindow(view, true);
    TouchpadAttach(viewWnd);

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
    raw->workspace = ActiveWorkspace().id;
    {   // after the active workspace's tabs (the open tab keeps being the open one)
        const unsigned long long activeId = ActiveTab() ? ActiveTab()->id : 0;
        g_tabs.insert(g_tabs.begin() + WsTabEnd(), std::move(tab));
        if (activeId) FindTab(activeId, &g_active);
    }
    Log("tab#%llu CREATED view=%p page=%p hwnd=%p popup=%d (tabs=%d)", raw->id, (void*)raw->view,
        (void*)raw->page, (void*)raw->hwnd, (int)openedByPage, (int)g_tabs.size());
    if (openedByPage) TabGroupsTabOpenedFrom(*raw, ActiveTab());   // into its opener's group
    ExtensionsOnTabCreated(raw);
    RefreshLayout(true);                 // the new tab grows in
    int at = -1;
    if (select && FindTab(raw->id, &at)) SelectTab(at);
    else ShowWindow(raw->hwnd, SW_HIDE);
    return raw;
}

bool UnloadTab(Tab& t, const std::string& state) {
    if (g_closing || g_tabOp || g_modalDepth > 0 || t.state != TabState::Live || t.privateTab || !t.view || !t.page) return false;
    if (&t == ActiveTab() || t.pipHwnd || (t.hwnd && t.hwnd == g_fullScreenView)) return false;
    TabOpGuard guard;
    RECT cr;
    GetClientRect(g_hWnd, &cr);
    const int top = (int)g_L.chromeH, left = (int)g_L.pageL;
    RECT r = { left, top, std::max<LONG>((LONG)g_L.pageR, left + 1), std::max<LONG>(cr.bottom, top + 1) };
    WKPageConfigurationRef conf = CreatePageConfiguration();
    WKViewRef view = WKViewCreate(r, conf, g_hWnd);
    WKRelease(conf);
    if (!view) return false;
    WKViewSetUseDarkAppearance(view, g_dark);
    WKPageRef page = WKViewGetPage(view);
    ExtensionsSetupPage(page);
    HWND hwnd = WKViewGetWindow(view);
    if (!page || !hwnd) {
        if (hwnd && IsWindow(hwnd)) DestroyWindow(hwnd);
        WKRelease(view);
        return false;
    }
    WKViewSetIsInWindow(view, true);
    TouchpadAttach(hwnd);
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
    TabHoverCardForget(t.id);
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

// Pages see the browser theme as prefers-color-scheme (sites' own dark styles, the PDF viewer,
// dark form controls and scrollbars). Called by ApplyTheme.
void TabsApplyAppearance() {
    for (auto& t : g_tabs) if (t->view) WKViewSetUseDarkAppearance(t->view, g_dark);
    if (s_spare && s_spare->view) WKViewSetUseDarkAppearance(s_spare->view, g_dark);
    SidebarThemeChanged();
}

void PrepareSpareTab() {
    if (!g_prefs.spareTab || s_spare || g_closing || g_tabOp || !g_hWnd || !g_context) return;
    TabOpGuard guard;
    RECT cr;
    GetClientRect(g_hWnd, &cr);
    const int top = (int)g_L.chromeH, left = (int)g_L.pageL;
    const int w = std::max<int>(1, (int)g_L.pageR - left), h = std::max<int>(1, cr.bottom - top);
    RECT r = { -(w + 400), 0, -400, h };
    WKPageConfigurationRef conf = CreatePageConfiguration();
    WKViewRef view = WKViewCreate(r, conf, g_hWnd);
    WKRelease(conf);
    if (!view) return;
    WKViewSetUseDarkAppearance(view, g_dark);
    WKPageRef page = WKViewGetPage(view);
    ExtensionsSetupPage(page);
    HWND viewWnd = WKViewGetWindow(view);
    if (!page || !viewWnd) {
        if (viewWnd && IsWindow(viewWnd)) DestroyWindow(viewWnd);
        WKRelease(view);
        return;
    }
    WKViewSetIsInWindow(view, true);
    TouchpadAttach(viewWnd);
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
    s_spareAt = GetTickCount();
    s_spareUrl = StartPageUrl();
    Navigate(page, ToUtf8(s_spareUrl));
    Log("spare tab#%llu prepared", s_spare->id);
}

void TabsListPages(std::vector<std::pair<std::wstring, WKPageRef>>& out) {
    for (auto& t : g_tabs) if (t->page) out.push_back({ L"tab#" + std::to_wstring(t->id) + L" " + HostOf(t->url), t->page });
    if (s_spare && s_spare->page) out.push_back({ L"spare new tab", s_spare->page });
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
    raw->workspace = ActiveWorkspace().id;
    const unsigned long long activeId = ActiveTab() ? ActiveTab()->id : 0;
    const int at = WsTabEnd();
    g_tabs.insert(g_tabs.begin() + at, std::move(tab));  // after the active workspace's tabs
    if (activeId) FindTab(activeId, &g_active);
    Log("tab#%llu CREATED from the spare start page (tabs=%d)", raw->id, (int)g_tabs.size());
    SyncTab(*raw);
    RefreshLayout(true);
    s_adopting = true;
    SelectTab(at);
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
    SystemProxyApply(s_privateStore);
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
    if (privateTab) SystemProxyApply(s_privateStore);
    WKPageConfigurationRef conf = CreatePageConfiguration();
    if (privateTab && s_privateStore) WKPageConfigurationSetWebsiteDataStore(conf, s_privateStore);
    Tab* opener = ActiveTab();
    Tab* t = CreateTabWithConfiguration(conf, false, !background);
    WKRelease(conf);
    if (!t) return nullptr;
    t->privateTab = privateTab;
    if (opener && opener != t && !privateTab && !opener->privateTab) TabGroupsTabOpenedFrom(*t, opener);
    t->url = input;                                // shown until the page commits
    InvalidateChrome();
    Navigate(t->page, ResolveNavigation(input));
    return t;
}

static void ReleasePrivateStoreIfUnused() {
    if (!s_privateStore) return;
    for (auto& t : g_tabs) if (t->privateTab) return;
    SystemProxyForget(s_privateStore);
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

static void RememberClosedTab(const Tab& t, int index) {
    if (t.privateTab) return;
    const std::wstring url = t.lazyPending ? t.lazyUrl : t.url;
    if (url.empty() || url == L"about:blank") return;
    ClosedTab c;
    c.url = url;
    c.title = t.title;
    c.index = index;
    c.group = t.group;
    if (const TabGroup* g = FindTabGroup(t.group)) { c.groupName = g->name; c.groupColor = g->color; }
    c.pinned = t.pinned;
    if (t.lazyPending) c.state = t.lazyState;            // (not loaded: the saved one)
    else if (t.page && !IsInternalPageUrl(t.url)) c.state = SessionPageState(t.page);
    g_closedTabs.push_back(std::move(c));
    if (g_closedTabs.size() > 25) g_closedTabs.erase(g_closedTabs.begin());
}

void ReopenClosedTab(int which) {
    if (g_closedTabs.empty()) return;
    if (which < 0 || which >= (int)g_closedTabs.size()) which = (int)g_closedTabs.size() - 1;
    const ClosedTab c = g_closedTabs[which];
    g_closedTabs.erase(g_closedTabs.begin() + which);
    // Like a restored tab: created hidden, it loads its back/forward list when it is shown.
    WKPageConfigurationRef conf = CreatePageConfigurationForTab();
    Tab* t = CreateTabWithConfiguration(conf, false, false);
    WKRelease(conf);
    if (!t) { g_closedTabs.insert(g_closedTabs.begin() + which, c); return; }
    const unsigned long long id = t->id;
    t->lazyPending = true;
    t->lazyUrl = c.url;
    t->lazyState = c.state;
    t->url = c.url;
    t->title = c.title.empty() ? HostOf(c.url) : c.title;
    int at = -1;
    if (c.pinned && FindTab(id, &at)) SetTabPinned(at, true);
    // Back to its place (pinned ones among the pinned, the others after them).
    if (FindTab(id, &at)) {
        const int pinnedOthers = PinnedTabCount() - (t->pinned ? 1 : 0);
        const int last = std::max(0, WsTabEnd() - 1);
        int to = std::clamp(c.index, 0, last);
        to = t->pinned ? std::min(to, pinnedOthers) : std::max(to, pinnedOthers);
        MoveTab(at, std::clamp(to, 0, last));
    }
    // Into its group, if the group is still there (at the group's end when its place is not next to it).
    // A group made again for a tab reopened before stands in for the closed one.
    static std::map<unsigned, unsigned> remade;          // closed group's id -> its new one
    unsigned group = c.group;
    if (group && !FindTabGroup(group)) {
        auto r = remade.find(group);
        if (r != remade.end() && FindTabGroup(r->second)) group = r->second;
    }
    if (group && !t->pinned && FindTabGroup(group) && FindTab(id, &at)) {
        t->group = group;
        TabGroupsNormalize();
        if (!t->group && FindTab(id, &at)) TabGroupAddTab(group, at);
    } else if (group && !t->pinned && FindTab(id, &at)) {     // its group closed with it: made again
        if (const unsigned g = TabGroupCreate(at)) {
            if (TabGroup* grp = FindTabGroup(g)) { grp->name = c.groupName; grp->color = c.groupColor; }
            remade[c.group] = g;
        }
    }
    Log("tab#%llu reopened (%ls)", id, HostOf(c.url).c_str());
    if (FindTab(id, &at)) SelectTab(at);
    RefreshLayout(true);
    InvalidateChrome();
}

void DuplicateTab(int index) {
    if (index < 0 || index >= (int)g_tabs.size()) return;
    const Tab& src = *g_tabs[index];
    const unsigned long long srcId = src.id;
    const std::wstring url = src.lazyPending ? src.lazyUrl : src.url;
    if (url.empty()) return;
    if (src.privateTab) {                                // (a private tab: its page again, no history)
        if (Tab* t = OpenInNewTab(url, false, true)) {
            int at = -1, from = -1;
            if (FindTab(srcId, &at) && FindTab(t->id, &from))
                MoveTab(from, std::clamp(std::max(at + 1, PinnedTabCount()), 0, std::max(0, WsTabEnd() - 1)));
        }
        return;
    }
    // Like a restored tab: it loads the source's back/forward list when it is shown (right away).
    const std::string state = src.lazyPending ? src.lazyState
                            : (src.page && !IsInternalPageUrl(src.url) ? SessionPageState(src.page) : std::string());
    const std::wstring title = src.title;
    const unsigned group = src.group;
    WKPageConfigurationRef conf = CreatePageConfigurationForTab();
    Tab* t = CreateTabWithConfiguration(conf, false, false);
    WKRelease(conf);
    if (!t) return;
    const unsigned long long id = t->id;
    t->lazyPending = true;
    t->lazyUrl = url;
    t->lazyState = state;
    t->url = url;
    t->title = title;
    t->title = title.empty() ? HostOf(url) : title;
    int at = -1, from = -1;
    if (FindTab(srcId, &at) && FindTab(id, &from)) {      // right after it (after the pinned ones)
        const int to = std::clamp(std::max(at + 1, PinnedTabCount()), 0, std::max(0, WsTabEnd() - 1));
        MoveTab(from, to);
        if (group && FindTabGroup(group) && FindTab(id, &at)) { g_tabs[at]->group = group; TabGroupsNormalize(); }
    }
    Log("tab#%llu duplicated as tab#%llu", srcId, id);
    if (FindTab(id, &at)) SelectTab(at);
    RefreshLayout(true);
    InvalidateChrome();
}

static void CloseTabIds(const std::vector<unsigned long long>& ids) {
    for (unsigned long long id : ids) {
        int i = -1;
        if (FindTab(id, &i)) CloseTab(i);
    }
}

void CloseOtherTabs(int index) {
    if (index < 0 || index >= (int)g_tabs.size()) return;
    const unsigned long long keep = g_tabs[index]->id;
    std::vector<unsigned long long> ids;
    for (int i = 0; i < WsTabEnd(); ++i) if (g_tabs[i]->id != keep && !g_tabs[i]->pinned) ids.push_back(g_tabs[i]->id);
    { int i = -1; if (FindTab(keep, &i) && i != g_active) SelectTab(i); }
    CloseTabIds(ids);
}

void CloseTabsToRight(int index) {
    if (index < 0 || index >= (int)g_tabs.size()) return;
    std::vector<unsigned long long> ids;
    for (int i = index + 1; i < WsTabEnd(); ++i) if (!g_tabs[i]->pinned) ids.push_back(g_tabs[i]->id);
    CloseTabIds(ids);
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

int PinnedTabCount() {                                  // (in the active workspace)
    int n = 0;
    for (int i = 0, end = WsTabEnd(); i < end; ++i) if (g_tabs[i]->pinned) ++n;
    return n;
}

void SetTabPinned(int index, bool pinned) {
    if (index < 0 || index >= (int)g_tabs.size() || g_tabs[index]->pinned == pinned) return;
    Tab& t = *g_tabs[index];
    if (pinned && SplitHas(t)) SplitClose();            // a pinned tab is not a half of a split view
    const int count = PinnedTabCount();
    t.pinned = pinned;
    Log("tab#%llu %s", t.id, pinned ? "pinned" : "unpinned");
    if (pinned) t.group = 0;                              // (pinned tabs are not grouped)
    if (FindTab(t.id, &index)) MoveTab(index, pinned ? count : count - 1);
    RefreshLayout(true);
    InvalidateChrome();
}

void DropTabAt(unsigned long long id, int insertAt) {
    int from = -1;
    Tab* t = FindTab(id, &from);
    if (!t) return;
    const int n = WsTabEnd();
    insertAt = std::clamp(insertAt, 0, n);
    const int to = insertAt > from ? insertAt - 1 : insertAt;          // its index once moved
    const int pinnedOthers = PinnedTabCount() - (t->pinned ? 1 : 0);
    // Right after the last pinned tab: a pinned tab stays pinned, an ordinary one stays ordinary.
    const bool pinned = to < pinnedOthers || (t->pinned && to == pinnedOthers);
    if (to == from && pinned == t->pinned) {             // not moved; the group may still change
        const unsigned group = t->group;
        TabGroupsAfterDrop(from);
        if (group != t->group) { RefreshLayout(true); InvalidateChrome(); }
        return;
    }
    if (pinned && SplitHas(*t)) SplitClose();
    Log("tab#%llu moved %d -> %d%s", id, from, to, pinned != t->pinned ? (pinned ? " (pinned)" : " (unpinned)") : "");
    t->pinned = pinned;
    if (FindTab(id, &from)) { MoveTab(from, to); TabGroupsAfterDrop(to); }
    RefreshLayout(true);
    InvalidateChrome();
}

void CloseTab(int i) {
    if (g_closing) return;
    if (g_tabOp) { Log("CloseTab(%d) re-entered during a tab operation -> ignored", i); return; }
    if (g_modalDepth > 0) { Log("CloseTab(%d) while a page dialog is open -> ignored", i); return; }
    if (i < 0 || i >= (int)g_tabs.size()) return;
    if (i < WsTabEnd() && WsTabEnd() == 1 && g_tabs.size() > 1) {
        // The last tab of a workspace (others have tabs): a new tab takes its place.
        const unsigned long long id = g_tabs[i]->id;
        RunCommand(CMD_NEWTAB);
        if (!FindTab(id, &i) || WsTabEnd() < 2) return;
    }
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
    RememberClosedTab(*g_tabs[i], i);
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
        else g_active = std::min(i, std::max(0, WsTabEnd() - 1));
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
