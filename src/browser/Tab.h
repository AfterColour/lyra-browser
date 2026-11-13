// Lyra — описание вкладки и контекста колбэков WebKit.
#pragma once

#include "common/Platform.h"

enum class TabState { Live, Closing, Closed };
struct TabCtx;

struct Tab {
    unsigned long long id = 0;      // for logging / identification only; never given to WebKit
    TabState     state = TabState::Live;
    TabCtx*      ctx  = nullptr;    // callback context registered with WebKit (see TabCtx)
    WKViewRef    view = nullptr;
    WKPageRef    page = nullptr;
    HWND         hwnd = nullptr;
    HWND         pipHwnd = nullptr; // the view while it is in the picture-in-picture window (hwnd is null then)
    std::wstring title = L"New Tab";
    std::wstring url;
    bool  loading = false;
    DWORD navStart = 0;            // GetTickCount at didStart (timings in tabs.log)
    bool  canBack = false;
    bool  canFwd  = false;
    float progress = 0.f;   // animated 0..1
    float fade     = 0.f;   // progress bar opacity 0..1
    // Translation (browser/Translate.cpp): offered, shown translated, keep translating new pages
    bool  translatable = false, translated = false, autoTranslate = false;
    bool  translateCollecting = false, translateErrorShown = false;
    unsigned translateGen = 0;      // results of an older page or state are dropped
    int   translatePending = 0;     // batches at the service
    bool  readerable = false;       // the page looks like an article (reader view, browser/Reader.cpp)
    bool  privateTab = false;       // private (incognito): own memory-only cookies/storage, no history, not in the session
    bool  audible = false;          // the page plays sound (speaker on the tab)
    bool  muted = false;            // muted with the tab's speaker
    bool  openedByPage = false;     // created by window.open / target=_blank
    // Restored from the last session and not shown yet: nothing is loaded until it is selected
    // (Session.cpp). url / title hold the saved ones meanwhile.
    bool  lazyPending = false;
    std::wstring lazyUrl;
    std::string  lazyState;         // WebKit session state (back/forward list), may be empty
    // Put to sleep (browser/TabSleep.cpp): unloaded like a restored tab (lazyPending), drawn faded.
    bool  sleeping = false;
    ULONGLONG hiddenSince = 0;      // GetTickCount64 when the tab stopped being the active one (0: active)
};

// Callback context handed to WebKit as `clientInfo`.
// Lifetime rule: the WebKit C API gives no notification when it stops using a client
// pointer, so a context is NEVER freed while the WebKit context is alive. When a tab
// closes, the context is only detached (tab = nullptr); the owning list g_ctxs is
// cleared in wWinMain after the last WebKit object has been released. A late callback
// therefore always dereferences valid memory and finds `tab == nullptr`.
struct TabCtx {
    unsigned long long id  = 0;
    Tab*               tab = nullptr;   // nullptr once the tab is closing/closed
};
