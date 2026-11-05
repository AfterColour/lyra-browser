// Lyra — список вкладок: создание, закрытие, переключение, навигация, восстановление.
#include "browser/TabManager.h"
#include "app/Log.h"
#include "browser/PageClients.h"
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
    if (!t.page) return false;
    bool changed = false, titleChanged = false;

    std::wstring url;
    if (WKURLRef u = WKPageCopyActiveURL(t.page)) {
        if (WKStringRef s = WKURLCopyString(u)) { url = WKStr(s); WKRelease(s); }
        WKRelease(u);
    }
    if (url != t.url) { t.url = url; changed = true; }

    std::wstring title;
    if (WKStringRef ts = WKPageCopyTitle(t.page)) { title = WKStr(ts); WKRelease(ts); }
    if (title.empty()) {
        title = HostOf(t.url);
        if (title.empty()) title = L"New Tab";
    }
    if (title != t.title) { t.title = title; changed = titleChanged = true; }

    bool b = WKPageCanGoBack(t.page), f = WKPageCanGoForward(t.page);
    if (b != t.canBack) { t.canBack = b; changed = true; }
    if (f != t.canFwd)  { t.canFwd = f;  changed = true; }

    if (titleChanged && &t == ActiveTab()) UpdateWindowTitle();
    return changed;
}

void SelectTab(int i) {
    if (i < 0 || i >= (int)g_tabs.size()) return;
    if (g_fullScreenView && (!g_tabs[i]->hwnd || g_tabs[i]->hwnd != g_fullScreenView))
        ExitFullScreenForTabSwitch();
    g_active = i;
    LayoutWebViews();                    // size the newly active view first, then show it
    for (int k = 0; k < (int)g_tabs.size(); ++k)
        if (g_tabs[k]->hwnd) ShowWindow(g_tabs[k]->hwnd, k == i ? SW_SHOW : SW_HIDE);
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
    if (Tab* t = ActiveTab()) Navigate(t->page, ResolveInput(input));
}

static WKPageConfigurationRef CreatePageConfiguration() {
    WKPageConfigurationRef conf = WKPageConfigurationCreate();
    WKPageConfigurationSetContext(conf, g_context);
    WKPageConfigurationSetPreferences(conf, g_preferences);
    if (g_dataStore) WKPageConfigurationSetWebsiteDataStore(conf, g_dataStore);
    return conf;
}

// Creates a view from `conf` (not consumed) and makes it the active tab.
Tab* CreateTabWithConfiguration(WKPageConfigurationRef conf, bool openedByPage) {
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
    int top = (int)g_L.chromeH;
    RECT r = { 0, top, cr.right, std::max<LONG>(cr.bottom, top + 1) };
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
    RefreshLayout();
    SelectTab((int)g_tabs.size() - 1);
    return raw;
}

void CreateTab(const std::wstring& input) {
    WKPageConfigurationRef conf = CreatePageConfiguration();
    Tab* t = CreateTabWithConfiguration(conf, false);
    WKRelease(conf);
    if (t) Navigate(t->page, ResolveInput(input));
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

    Log("CloseTab: begin index=%d tab#%llu loading=%d active=%d tabs=%d", i, g_tabs[i]->id,
        (int)g_tabs[i]->loading, g_active, (int)g_tabs.size());
    RememberClosedTab(*g_tabs[i]);

    // 1) Take the tab out of our own state and cut off callbacks, so nothing that
    //    runs while WebKit objects are being destroyed can reach it.
    std::unique_ptr<Tab> dead = std::move(g_tabs[i]);
    g_tabs.erase(g_tabs.begin() + i);
    DetachTab(*dead);
    if (g_active > i) --g_active;
    else if (g_active == i) g_active = std::min(i, (int)g_tabs.size() - 1);
    g_hover = Hit();
    g_down  = Hit();
    RefreshLayout();                     // g_L.tabs must match g_tabs before any repaint

    // 2) Hide the dead view and give focus to the surviving tab BEFORE the focused
    //    child window is destroyed.
    if (dead->hwnd) ShowWindow(dead->hwnd, SW_HIDE);
    SelectTab(g_active);
    Log("CloseTab: new active=%d tab#%llu", g_active, ActiveTab() ? ActiveTab()->id : 0ULL);

    // 3) Only now release the WebKit objects.
    DestroyTab(*dead);
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
        LoadInfoPage(*t, L"This page used too much memory",
                     L"The page was stopped to keep the rest of the browser responsive.", L"", url);
    else
        LoadInfoPage(*t, L"This page crashed",
                     L"Something went wrong while displaying this page. Other tabs are not affected.", L"", url);
}
