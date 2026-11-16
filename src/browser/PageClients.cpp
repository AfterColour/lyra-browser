// Lyra — колбэки WebKit для страницы: навигация, сбои, alert/confirm/prompt, выбор файла.
#include "browser/ContextMenu.h"
#include "browser/PageClients.h"
#include "browser/CustomThemes.h"
#include "browser/ExtensionStore.h"
#include "browser/Extensions.h"
#include "browser/DevProbe.h"
#include "browser/Translate.h"
#include "browser/Reader.h"
#include "browser/AdBlock.h"
#include "browser/MediaControls.h"
#include "browser/Permissions.h"
#include <WebKit/WKPagePrivate.h>
#include "ui/FindBar.h"
#include "browser/History.h"
#include "core/Settings.h"
#include <WebKit/WKWebsitePolicies.h>
#include "core/Url.h"
#include "core/Localization.h"
#include "app/Log.h"
#include "browser/Downloads.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "core/Constants.h"
#include "core/StringUtils.h"
#include "ui/TabTransition.h"
#include "ui/Chrome.h"
#include "ui/Dialogs.h"
#include "ui/MainWindow.h"

static void OnDidStart(WKPageRef, WKNavigationRef, WKTypeRef, const void* ci);
static void OnDidCommit(WKPageRef, WKNavigationRef, WKTypeRef, const void* ci);
static void OnDidFinish(WKPageRef, WKNavigationRef, WKTypeRef, const void* ci);
static void OnDidFail(WKPageRef, WKNavigationRef, WKErrorRef, WKTypeRef, const void* ci);
static void OnDidFailProvisional(WKPageRef, WKNavigationRef, WKErrorRef error, WKTypeRef, const void* ci);
static void OnWebProcessDidTerminate(WKPageRef, WKProcessTerminationReason reason, const void* ci);
static void OnDecidePolicyForResponse(WKPageRef, WKNavigationResponseRef nr, WKFramePolicyListenerRef listener, WKTypeRef, const void*);
static void OnDecidePolicyForAction(WKPageRef, WKNavigationActionRef action, WKFramePolicyListenerRef listener, WKTypeRef, const void* ci);
static void OnStateChanged(const void* ci);
static WKPageRef OnCreateNewPage(WKPageRef, WKPageConfigurationRef conf, WKNavigationActionRef, WKWindowFeaturesRef, const void* ci);
static void OnPageClose(WKPageRef, const void* ci);
static std::wstring OriginTitle(WKSecurityOriginRef origin);
static void OnJsAlert(WKPageRef, WKStringRef text, WKFrameRef, WKSecurityOriginRef origin, WKPageRunJavaScriptAlertResultListenerRef listener, const void* ci);
static void OnJsConfirm(WKPageRef, WKStringRef text, WKFrameRef, WKSecurityOriginRef origin, WKPageRunJavaScriptConfirmResultListenerRef listener, const void* ci);
static void OnJsPrompt(WKPageRef, WKStringRef text, WKStringRef defaultValue, WKFrameRef, WKSecurityOriginRef origin, WKPageRunJavaScriptPromptResultListenerRef listener, const void* ci);
static void OnRunOpenPanel(WKPageRef, WKFrameRef, WKOpenPanelParametersRef params, WKOpenPanelResultListenerRef listener, const void* ci);

// Shows a small built-in page (load error, crashed tab) in place of `failedUrl`.
// The address bar keeps showing `failedUrl` and "Try again" reloads it.
void LoadInfoPage(Tab& t, const std::wstring& heading, const std::wstring& text,
                         const std::wstring& details, const std::wstring& failedUrl) {
    if (!t.page) return;
    std::wstring html =
        L"<!doctype html><html><head><meta charset='utf-8'>"
        L"<meta name='color-scheme' content='light dark'><title>" + HtmlEscape(heading) + L"</title>"
        L"<style>"
        L"body{font:15px/1.5 'Segoe UI',system-ui,sans-serif;margin:0;display:flex;min-height:100vh;"
        L"align-items:center;justify-content:center;background:Canvas;color:CanvasText}"
        L"main{max-width:560px;padding:32px}"
        L"h1{font-size:22px;font-weight:600;margin:0 0 8px}"
        L"p{margin:0 0 12px;opacity:.85}"
        L"code{font:12px Consolas,monospace;opacity:.6;word-break:break-all}"
        L"button{margin-top:12px;font:inherit;padding:6px 18px;border-radius:6px;border:1px solid #8886;"
        L"background:#0a64d8;color:#fff;cursor:pointer}"
        L"</style></head><body><main>"
        L"<h1>" + HtmlEscape(heading) + L"</h1>"
        L"<p>" + HtmlEscape(text) + L"</p>";
    if (!details.empty()) html += L"<p><code>" + HtmlEscape(details) + L"</code></p>";
    if (!failedUrl.empty())
        html += L"<button onclick='location.replace(" + HtmlEscape(JsString(failedUrl)) + L")'>" + std::wstring(Tr(L"Try again", L"Повторить")) + L"</button>";
    html += L"</main></body></html>";

    WKStringRef body = WKStringCreateWithUTF8CString(ToUtf8(html).c_str());
    WKURLRef unreachable = failedUrl.empty() ? nullptr : WKURLCreateWithUTF8CString(ToUtf8(failedUrl).c_str());
    WKPageLoadAlternateHTMLString(t.page, body, nullptr, unreachable);
    if (unreachable) WKRelease(unreachable);
    WKRelease(body);
}

// --- navigation client --------------------------------------------------------
static void OnDidStart(WKPageRef, WKNavigationRef, WKTypeRef, const void* ci) {
    Tab* t = ResolveTab(ci, "didStart");
    if (!t) return;
    t->navStart = GetTickCount();
    Log("tab#%llu didStart", t->id);
    t->loading = true;
    t->progress = 0.08f;
    t->fade = 1.f;
    SyncTab(*t);
    ExtensionsOnTabUpdated(t);
    InvalidateChrome();
}

static void OnDidCommit(WKPageRef, WKNavigationRef, WKTypeRef, const void* ci) {
    Tab* t = ResolveTab(ci, "didCommit");
    if (!t) return;
    t->readerable = false;               // the new page is checked when it finishes
    TranslatePageCommitted(*t);
    ExtensionsOnTabCommitted(t);
    SyncTab(*t);
    ExtensionsOnTabUpdated(t);
    Log("tab#%llu didCommit +%lums %ls", t->id, t->navStart ? GetTickCount() - t->navStart : 0UL, HostOf(t->url).c_str());
    InvalidateChrome();
}

static void OnDidFinish(WKPageRef, WKNavigationRef, WKTypeRef, const void* ci) {
    Tab* t = ResolveTab(ci, "didFinish");
    if (!t) return;
    Log("tab#%llu didFinish +%lums", t->id, t->navStart ? GetTickCount() - t->navStart : 0UL);
    FindBarPageLoaded(t->id);
    t->loading = false;
    t->progress = std::max(t->progress, 0.9f);
    SyncTab(*t);
    ExtensionsOnTabUpdated(t);
    // The finished page's title names its history entry (the title seen when the address
    // changed may still have been the previous page's).
    if (WKStringRef ts = WKPageCopyTitle(t->page)) { HistorySetTitle(t->url, WKStr(ts)); WKRelease(ts); }
    TransitionLoaded(t->id);             // a new tab with a Lyra page: its cover can go
    ReaderPageLoaded(*t);                // an article? (the book in the address field)
    TranslatePageLoaded(*t);             // another language? (or keep translating)
    DevProbePageLoaded(*t);              // LYRA_PROBE measurements (developer)
    InvalidateChrome();
}

void StopLoadingIndicator(Tab& t) {
    t.loading = false;
    t.progress = 0.f;
    t.fade = 0.f;
    SyncTab(t);
    InvalidateChrome();
}

// Failure after the page was committed: keep whatever was rendered.
static void OnDidFail(WKPageRef, WKNavigationRef, WKErrorRef, WKTypeRef, const void* ci) {
    Tab* t = ResolveTab(ci, "didFail");
    if (!t) return;
    Log("tab#%llu didFail", t->id);
    TransitionLoaded(t->id);
    StopLoadingIndicator(*t);
}

// Failure before anything was committed (DNS error, refused connection, TLS error...):
// show an error page instead of leaving the previous page / a blank view.
// WebKit explicitly allows loading alternate HTML from inside this callback.
static void OnDidFailProvisional(WKPageRef, WKNavigationRef, WKErrorRef error, WKTypeRef, const void* ci) {
    Tab* t = ResolveTab(ci, "didFailProvisional");
    if (!t) return;
    int code = error ? WKErrorGetErrorCode(error) : 0;
    std::wstring domain, text, url;
    if (error) {
        if (WKStringRef s = WKErrorCopyDomain(error)) { domain = WKStr(s); WKRelease(s); }
        if (WKStringRef s = WKErrorCopyLocalizedDescription(error)) { text = WKStr(s); WKRelease(s); }
        if (WKURLRef u = WKErrorCopyFailingURL(error)) {
            if (WKStringRef s = WKURLCopyString(u)) { url = WKStr(s); WKRelease(s); }
            WKRelease(u);
        }
    }
    Log("tab#%llu didFailProvisional code=%d domain=%ls", t->id, code, domain.c_str());
    StopLoadingIndicator(*t);

    // Not real failures: the load was cancelled (user started another navigation,
    // pressed Stop) or turned into a download.
    const bool webKitDomain = domain == L"WebKitNetworkError" || domain == L"WebKitErrorDomain";
    if (webKitDomain && (code == 302 /* Cancelled */ || code == 400 /* CancelledByUser */ ||
                         code == 102 /* FrameLoadInterruptedByPolicyChange */))
        return;
    if (domain == L"WebKitInternal" || url.empty() || url == L"about:blank") return;

    LoadInfoPage(*t, Tr(L"This page can\x2019t be opened", L"Не удаётся открыть страницу"),
                 text.empty() ? std::wstring(Tr(L"The server could not be reached.", L"Сервер недоступен.")) : text,
                 domain + L" " + std::to_wstring(code) + L" \x2014 " + url, url);
}

static void OnWebProcessDidTerminate(WKPageRef, WKProcessTerminationReason reason, const void* ci) {
    Tab* t = ResolveTab(ci, "webProcessDidTerminate");
    if (!t) return;
    Log("tab#%llu web process terminated, reason=%d", t->id, (int)reason);
    if (reason == kWKProcessTerminationReasonRequestedByClient) return;
    // Do not restart the process from inside the termination notification.
    PostMessageW(g_hWnd, WM_APP_TAB_CRASHED, (WPARAM)t->id, (LPARAM)reason);
}

// lyra://... addresses are commands from Lyra's own pages (start page, welcome screen): they are
// never loaded, the main window carries them out (browser/StartPage.cpp).
static void OnDecidePolicyForAction(WKPageRef, WKNavigationActionRef action, WKFramePolicyListenerRef listener,
                                    WKTypeRef, const void* ci) {
    std::wstring url;
    if (WKURLRequestRef req = WKNavigationActionCopyRequest(action)) {
        if (WKURLRef u = WKURLRequestCopyURL(req)) {
            if (WKStringRef s = WKURLCopyString(u)) { url = WKStr(s); WKRelease(s); }
            WKRelease(u);
        }
        WKRelease(req);
    }
    if (_wcsnicmp(url.c_str(), L"lyra:", 5) == 0) {
        WKFramePolicyListenerIgnore(listener);
        TabCtx* c = (TabCtx*)ci;
        if (c && g_hWnd && !g_closing)
            PostMessageW(g_hWnd, WM_APP_LYRA_URL, (WPARAM)c->id, (LPARAM)new std::wstring(url));
        return;
    }
    // "Download theme" on themes.vivaldi.net: Lyra installs the theme instead of saving the .zip.
    if (CustomThemeIsInstallUrl(url)) {
        WKFramePolicyListenerIgnore(listener);
        CustomThemeDownload(url);
        return;
    }
    // A .crx / .xpi file opened in Lyra: offered for installing, not shown.
    if (_wcsnicmp(url.c_str(), L"file:", 5) == 0 && ExtensionIsPackage(url)) {
        WKFramePolicyListenerIgnore(listener);
        wchar_t path[MAX_PATH] = {};
        DWORD len = MAX_PATH;
        if (SUCCEEDED(PathCreateFromUrlW(url.c_str(), path, &len, 0))) ExtensionInstallFromPath(path);
        return;
    }
    // Sites the user turned ad blocking off for: the page loads without the content blockers.
    if (g_prefs.adBlock && !AdBlockAllowedOn(url)) {
        bool mainFrame = false;
        if (WKFrameInfoRef fi = WKNavigationActionCopyTargetFrameInfo(action)) {
            mainFrame = WKFrameInfoGetIsMainFrame(fi);
            WKRelease(fi);
        }
        if (mainFrame) {
            WKWebsitePoliciesRef policies = WKWebsitePoliciesCreate();
            WKWebsitePoliciesSetContentBlockersEnabled(policies, false);
            WKFramePolicyListenerUseWithPolicies(listener, policies);
            WKRelease(policies);
            return;
        }
    }
    WKFramePolicyListenerUse(listener);
}

// Only the main frame decides whether a response is shown or saved.
static void OnDecidePolicyForResponse(WKPageRef page, WKNavigationResponseRef nr, WKFramePolicyListenerRef listener,
                                      WKTypeRef, const void*) {
    bool mainFrame = true;
    if (WKFrameInfoRef fi = WKNavigationResponseCopyFrameInfo(nr)) {
        mainFrame = WKFrameInfoGetIsMainFrame(fi);
        if (!mainFrame) ExtensionsFrameSeen(page, fi);   // extensions may run scripts in it
        WKRelease(fi);
    }
    bool attachment = false;
    std::wstring mime, responseUrl;
    if (WKURLResponseRef resp = WKNavigationResponseCopyResponse(nr)) {
        if (WKURLRef u = WKURLResponseCopyURL(resp)) {
            if (WKStringRef s = WKURLCopyString(u)) { responseUrl = WKStr(s); WKRelease(s); }
            WKRelease(u);
        }
        attachment = WKURLResponseIsAttachment(resp);
        if (WKStringRef m = WKURLResponseCopyMIMEType(resp)) { mime = WKStr(m); WKRelease(m); }
        WKRelease(resp);
    }
    // An extension package from a site: installed (after asking) rather than saved.
    if (mainFrame && _wcsnicmp(responseUrl.c_str(), L"https:", 6) == 0 && ExtensionIsPackage(responseUrl, mime)) {
        WKFramePolicyListenerIgnore(listener);
        ExtensionInstallFromUrl(responseUrl);
        return;
    }
    if (mainFrame && (attachment || !WKNavigationResponseCanShowMIMEType(nr))) {
        Log("response -> download: mime='%ls' attachment=%d canShow=%d", mime.c_str(), (int)attachment,
            (int)WKNavigationResponseCanShowMIMEType(nr));
        WKFramePolicyListenerDownload(listener);
        return;
    }
    WKFramePolicyListenerUse(listener);
}

// --- state client: title / URL / history buttons change -> repaint immediately ---
// --- UI client ----------------------------------------------------------------
static void OnStateChanged(const void* ci) {
    Tab* t = ResolveTab(ci, "stateChanged");
    if (!t) return;
    if (SyncTab(*t)) InvalidateChrome();
    ExtensionsOnTabUpdated(t);
}

// window.open() / target=_blank: WebKit hands us a configuration that already links
// the new page to its opener; we must create the view from it and return the page +1.
static WKPageRef OnCreateNewPage(WKPageRef, WKPageConfigurationRef conf, WKNavigationActionRef,
                                 WKWindowFeaturesRef, const void* ci) {
    Tab* opener = ResolveTab(ci, "createNewPage");
    if (!opener) return nullptr;
    const bool priv = opener->privateTab;     // the configuration carries the opener's data store
    Tab* t = CreateTabWithConfiguration(conf, true);
    if (!t) return nullptr;
    t->privateTab = priv;
    return (WKPageRef)WKRetain(t->page);
}

static void OnPageClose(WKPageRef, const void* ci) {   // window.close()
    Tab* t = ResolveTab(ci, "close");
    if (t) PostMessageW(g_hWnd, WM_APP_CLOSE_TAB, (WPARAM)t->id, 0);
}

static std::wstring OriginTitle(WKSecurityOriginRef origin) {
    std::wstring s;
    if (origin) if (WKStringRef o = WKSecurityOriginCopyToString(origin)) { s = WKStr(o); WKRelease(o); }
    return s.empty() || s == L"null" ? std::wstring(kAppName) : s + L" says";
}

static void OnJsAlert(WKPageRef, WKStringRef text, WKFrameRef, WKSecurityOriginRef origin,
                      WKPageRunJavaScriptAlertResultListenerRef listener, const void* ci) {
    if (ResolveTab(ci, "alert")) {
        ModalScope modal;
        MessageBoxW(g_hWnd, WKStr(text).c_str(), OriginTitle(origin).c_str(), MB_OK);
    }
    WKPageRunJavaScriptAlertResultListenerCall(listener);
}

static void OnJsConfirm(WKPageRef, WKStringRef text, WKFrameRef, WKSecurityOriginRef origin,
                        WKPageRunJavaScriptConfirmResultListenerRef listener, const void* ci) {
    bool ok = false;
    if (ResolveTab(ci, "confirm")) {
        ModalScope modal;
        ok = MessageBoxW(g_hWnd, WKStr(text).c_str(), OriginTitle(origin).c_str(), MB_OKCANCEL) == IDOK;
    }
    WKPageRunJavaScriptConfirmResultListenerCall(listener, ok);
}

static void OnJsPrompt(WKPageRef, WKStringRef text, WKStringRef defaultValue, WKFrameRef, WKSecurityOriginRef origin,
                       WKPageRunJavaScriptPromptResultListenerRef listener, const void* ci) {
    PromptState st;
    st.message = WKStr(text);
    st.value = WKStr(defaultValue);
    bool ok = false;
    if (ResolveTab(ci, "prompt")) {
        ModalScope modal;
        ok = PromptDialog(g_hWnd, OriginTitle(origin), st);
    }
    if (ok) {
        WKStringRef r = WKStringCreateWithUTF8CString(ToUtf8(st.value).c_str());
        WKPageRunJavaScriptPromptResultListenerCall(listener, r);
        WKRelease(r);
    } else {
        WKPageRunJavaScriptPromptResultListenerCall(listener, nullptr);   // prompt() returns null
    }
}

// <input type=file>
static void OnRunOpenPanel(WKPageRef, WKFrameRef, WKOpenPanelParametersRef params,
                           WKOpenPanelResultListenerRef listener, const void* ci) {
    if (!ResolveTab(ci, "openPanel")) { WKOpenPanelResultListenerCancel(listener); return; }
    const bool multiple = WKOpenPanelParametersGetAllowsMultipleFiles(params);

    std::vector<wchar_t> buf(multiple ? 64 * 1024 : MAX_PATH * 2, L'\0');
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hWnd;
    ofn.lpstrFilter = L"All files (*.*)\0*.*\0";
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = (DWORD)buf.size();
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                (multiple ? OFN_ALLOWMULTISELECT : 0);
    BOOL picked;
    { ModalScope modal; picked = GetOpenFileNameW(&ofn); }
    if (!picked) { WKOpenPanelResultListenerCancel(listener); return; }

    // Single file: "C:\dir\a.txt\0". Multiple: "C:\dir\0a.txt\0b.txt\0\0".
    std::vector<std::wstring> paths;
    std::wstring first = buf.data();
    const wchar_t* p = buf.data() + first.size() + 1;
    if (!multiple || !*p) {
        paths.push_back(first);
    } else {
        for (; *p; p += wcslen(p) + 1) paths.push_back(first + L"\\" + p);
    }

    WKMutableArrayRef urls = WKMutableArrayCreate();
    for (const auto& path : paths) {
        wchar_t url[2084] = {};          // INTERNET_MAX_URL_LENGTH (wininet.h)
        DWORD len = ARRAYSIZE(url);
        if (FAILED(UrlCreateFromPathW(path.c_str(), url, &len, 0))) continue;
        if (WKURLRef u = WKURLCreateWithUTF8CString(ToUtf8(url).c_str())) {
            WKArrayAppendItem(urls, u);
            WKRelease(u);
        }
    }
    WKOpenPanelResultListenerChooseFiles(listener, (WKArrayRef)urls, nullptr);
    WKRelease(urls);
}

// --- find on page / window.print() ------------------------------------------------
static void OnDidFindString(WKPageRef, WKStringRef, unsigned matchCount, const void* ci) {
    if (Tab* t = ResolveTab(ci, "didFindString")) FindBarDidFind(t->id, matchCount);
}

static void OnDidFailToFindString(WKPageRef, WKStringRef, const void* ci) {
    if (Tab* t = ResolveTab(ci, "didFailToFindString")) FindBarDidFail(t->id);
}

// The page asked to be printed: the print dialog opens once WebKit's call has returned.
static void OnPrintFrame(WKPageRef, WKFrameRef, const void* ci) {
    Tab* t = ResolveTab(ci, "printFrame");
    if (t && g_hWnd && !g_closing) PostMessageW(g_hWnd, WM_APP_PRINT, (WPARAM)t->id, 0);
}

void SetPageClients(WKPageRef page, TabCtx* ctx) {
    WKPageNavigationClientV3 nc = {};
    nc.base.version = 3;
    nc.base.clientInfo = ctx;           // stable heap object, outlives every WebKit callback (see TabCtx)
    nc.decidePolicyForNavigationAction   = OnDecidePolicyForAction;
    nc.decidePolicyForNavigationResponse = OnDecidePolicyForResponse;
    nc.didStartProvisionalNavigation     = OnDidStart;
    nc.didFailProvisionalNavigation      = OnDidFailProvisional;
    nc.didCommitNavigation               = OnDidCommit;
    nc.didFinishNavigation               = OnDidFinish;
    nc.didFailNavigation                 = OnDidFail;
    nc.webProcessDidTerminate            = OnWebProcessDidTerminate;
    nc.navigationActionDidBecomeDownload   = OnActionBecameDownload;
    nc.navigationResponseDidBecomeDownload = OnResponseBecameDownload;
    nc.contextMenuDidCreateDownload        = OnContextMenuDownload;
    WKPageSetPageNavigationClient(page, &nc.base);

    WKPageUIClientV13 ui = {};
    ui.base.version = 13;
    ui.base.clientInfo = ctx;
    ui.createNewPage        = OnCreateNewPage;
    ui.close                = OnPageClose;
    ui.runJavaScriptAlert   = OnJsAlert;
    ui.runJavaScriptConfirm = OnJsConfirm;
    ui.runJavaScriptPrompt  = OnJsPrompt;
    ui.runOpenPanel         = OnRunOpenPanel;
    ui.printFrame           = OnPrintFrame;
    ui.isPlayingAudioDidChange = [](WKPageRef page, const void* ci) {
        Tab* t = ResolveTab(ci, "audio");
        if (!t) return;
        t->audible = WKPageIsPlayingAudio(page);
        Log("tab#%llu audio %s", t->id, t->audible ? "on" : "off");
        InvalidateChrome();
        MediaControlsAudioChanged();
    };
    ui.decidePolicyForNotificationPermissionRequest = [](WKPageRef page, WKSecurityOriginRef origin,
            WKNotificationPermissionRequestRef request, const void*) { PermissionsDecideNotifications(page, origin, request); };
    ui.decidePolicyForGeolocationPermissionRequest = [](WKPageRef page, WKFrameRef, WKSecurityOriginRef origin,
            WKGeolocationPermissionRequestRef request, const void*) { PermissionsDecideGeolocation(page, origin, request); };
    WKPageSetPageUIClient(page, &ui.base);

    WKPageFindClientV0 fc = {};
    fc.base.version = 0;
    fc.base.clientInfo = ctx;
    fc.didFindString = OnDidFindString;
    fc.didFailToFindString = OnDidFailToFindString;
    WKPageSetPageFindClient(page, &fc.base);

    WKPageStateClientV0 sc = {};
    sc.base.version = 0;
    sc.base.clientInfo = ctx;
    sc.didChangeTitle        = OnStateChanged;
    sc.didChangeActiveURL    = OnStateChanged;
    sc.didChangeCanGoBack    = OnStateChanged;
    sc.didChangeCanGoForward = OnStateChanged;
    WKPageSetPageStateClient(page, &sc.base);

    ContextMenuSetClient(page, ctx);
}

void ClearPageClients(WKPageRef page) {
    WKPageNavigationClientV0 nc = {};
    nc.base.version = 0;
    WKPageSetPageNavigationClient(page, &nc.base);
    WKPageUIClientV0 ui = {};
    ui.base.version = 0;
    WKPageSetPageUIClient(page, &ui.base);
    WKPageStateClientV0 sc = {};
    sc.base.version = 0;
    WKPageSetPageStateClient(page, &sc.base);
    WKPageFindClientV0 fc = {};
    fc.base.version = 0;
    WKPageSetPageFindClient(page, &fc.base);
    ContextMenuClearClient(page);
}
