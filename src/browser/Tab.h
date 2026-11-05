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
    std::wstring title = L"New Tab";
    std::wstring url;
    bool  loading = false;
    DWORD navStart = 0;            // GetTickCount at didStart (timings in tabs.log)
    bool  canBack = false;
    bool  canFwd  = false;
    float progress = 0.f;   // animated 0..1
    float fade     = 0.f;   // progress bar opacity 0..1
    bool  openedByPage = false;     // created by window.open / target=_blank
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
