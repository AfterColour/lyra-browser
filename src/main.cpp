// ============================================================================
//  Lyra - Windows + WebKit (WebKit2 C API) with a Safari-style chrome.
//
//  Build notes
//   - Link: gdiplus, dwmapi, comdlg32, shlwapi, uuid (pragmas below), plus your WebKit libs.
//   - Needs Windows 10 1703+ (per-monitor DPI v2). Dark title bar / caption
//     colour matching needs Windows 11 (silently ignored on Windows 10).
//   - C++17 or newer (the project builds as C++20).
//
//  Data: settings, tabs.log and the browser profile (cookies, storage, cache) live
//  in %LOCALAPPDATA%\Lyra (moved there from %LOCALAPPDATA%\MyBrowser on first start).
//
//  Debug switches are environment variables named LYRA_* (LYRA_MEDIA_TRACE,
//  LYRA_DISABLE_GPU_RASTER, ...). WebKit reads them under their old MYBROWSER_* names;
//  ForwardLyraEnvironment() passes them on to the WebKit processes.
// ============================================================================
#include "common/Platform.h"
#include "browser/Bookmarks.h"
#include "browser/Downloads.h"
#include "browser/Favicons.h"
#include "browser/History.h"
#include "browser/StartPage.h"
#include "app/CrashHandler.h"
#include "app/Environment.h"
#include "app/Log.h"
#include "browser/TabManager.h"
#include "browser/WebContext.h"
#include "core/Constants.h"
#include "core/Settings.h"
#include "core/StringUtils.h"
#include "ui/Chrome.h"
#include "ui/Commands.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/WindowFrame.h"

// ----------------------------------------------------------------------------
//  Entry point
// ----------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int nCmdShow) {
    // Must happen before any window is created.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // COM/OLE on the UI thread: needed by the file chooser, drag & drop and the
    // clipboard (MiniBrowser does the same).
    OleInitialize(nullptr);

    GdiplusStartupInput gsi;
    ULONG_PTR gdipToken = 0;
    GdiplusStartup(&gdipToken, &gsi, nullptr);

    ForwardLyraEnvironment();
    InitPaths();
    LogOpen();
    SetUnhandledExceptionFilter(CrashFilter);
    LoadSettings();
    HistoryLoad();
    BookmarksLoad();
    DownloadsLoad();
    EnableDarkMenus();

    ConfigureGStreamerEnvironment();

    const wchar_t CLASS_NAME[] = L"LyraWindowClass";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;                      // we paint everything ourselves
    if (!RegisterClassExW(&wc)) {
        MessageBoxW(nullptr, L"Failed to register window class", L"Error", MB_ICONERROR);
        return -1;
    }

    UINT sysDpi = GetDpiForSystem();
    HWND hwnd = CreateWindowExW(
        0, CLASS_NAME, kAppName,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        CW_USEDEFAULT, CW_USEDEFAULT,
        MulDiv(1180, (int)sysDpi, 96), MulDiv(780, (int)sysDpi, 96),
        nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) {
        MessageBoxW(nullptr, L"Failed to create window", L"Error", MB_ICONERROR);
        return -1;
    }
    // Apply the custom frame (WM_NCCALCSIZE drops the system title bar) before the window shows.
    FrameApply(hwnd);

    // WebKit
    WKContextConfigurationRef contextConf = WKContextConfigurationCreate();
    g_context = WKContextCreateWithConfiguration(contextConf);
    WKRelease(contextConf);
    // Tuned for a browser with back/forward navigation (bigger page/memory caches)
    // instead of the conservative "document viewer" default.
    WKContextSetCacheModel(g_context, kWKCacheModelPrimaryWebBrowser);

    g_dataStore = CreateWebsiteDataStore();

    g_preferences = WKPreferencesCreate();
    ApplyWebPrefs();                                   // JS, images, storage, fonts... from settings
    WKPreferencesSetSuppressesIncrementalRendering(g_preferences, false);
    // (WKPreferencesSetAVFoundationEnabled was removed: AVFoundation is Apple-only and
    //  does nothing on the Windows port, which plays media through Media Foundation.)

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // First start: the welcome screen (language, theme, tab layout...) instead of the home page.
    std::wstring startUrl = g_prefs.onboarded ? g_homePage : StartPageUrl(true);
    if (lpCmdLine && *lpCmdLine) {
        std::wstring arg = TrimWs(lpCmdLine);
        if (arg.size() >= 2 && arg.front() == L'"' && arg.back() == L'"') arg = arg.substr(1, arg.size() - 2);
        if (!arg.empty()) startUrl = arg;
    }
    CreateTab(startUrl);
    ScheduleSpareTab(3000);              // the next new tab, once the first page is up

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (HandleShortcut(msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Log("message loop ended; releasing WebKit preferences/context");
    if (g_preferences) { WKRelease(g_preferences); g_preferences = nullptr; }
    if (g_dataStore)   { WKRelease(g_dataStore);   g_dataStore = nullptr; }
    if (g_context)     { WKRelease(g_context);     g_context = nullptr; }
    Log("WebKit context released");
    g_ctxs.clear();                      // only now can no WebKit callback refer to a TabCtx any more
    ReleaseResources();
    HistorySave();
    DownloadsSave();
    FaviconsShutdown();
    GdiplusShutdown(gdipToken);
    OleUninitialize();
    // Everything is saved. End here instead of running the C++ static destructors: WebKit's
    // own threads can still be finishing, and a crash in the destructors at exit (one was seen
    // in a dump) would only lose the user a "Lyra stopped working" box.
    Log("exit");
    TerminateProcess(GetCurrentProcess(), (UINT)msg.wParam);
    return (int)msg.wParam;
}
