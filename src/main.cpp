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
#include <WebKit/WKTextChecker.h>
#include "common/Platform.h"
#include <tlhelp32.h>
#include "browser/AdBlock.h"
#include "browser/MediaControls.h"
#include "browser/Passwords.h"
#include "browser/WebAuthn.h"
#include "ui/Sidebar.h"
#include "browser/PictureInPicture.h"
#include "browser/Reader.h"
#include "browser/Translate.h"
#include "browser/Permissions.h"
#include "browser/Bookmarks.h"
#include "browser/Session.h"
#include "browser/Downloads.h"
#include "browser/Favicons.h"
#include "browser/SystemProxy.h"
#include "browser/History.h"
#include "browser/StartPage.h"
#include "app/CrashHandler.h"
#include "app/StallWatch.h"
#include "browser/Extensions.h"
#include "app/Environment.h"
#include "app/Log.h"
#include "browser/DevProbe.h"
#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include "browser/TabManager.h"
#include "browser/WebContext.h"
#include "core/Constants.h"
#include "core/Settings.h"
#include "core/StringUtils.h"
#include "ui/Chrome.h"
#include "ui/Commands.h"
#include "ui/MainWindow.h"
#include "browser/TabSleep.h"
#include "ui/Theme.h"
#include "ui/WindowFrame.h"
#include "app/Updater.h"

// ----------------------------------------------------------------------------
//  Entry point
// ----------------------------------------------------------------------------
// Waits until the processes Lyra started have exited, `timeoutMs` at most.
static void WaitForChildProcesses(DWORD timeoutMs) {
    const DWORD self = GetCurrentProcessId();
    std::vector<HANDLE> children;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = { sizeof(pe) };
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
            if (pe.th32ParentProcessID != self) continue;
            if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pe.th32ProcessID)) children.push_back(h);
        }
        CloseHandle(snap);
    }
    const DWORD start = GetTickCount();
    for (HANDLE h : children) {
        const DWORD spent = GetTickCount() - start;
        if (spent < timeoutMs) WaitForSingleObject(h, timeoutMs - spent);
        CloseHandle(h);
    }
    Log("exit: %d helper process(es), waited %lu ms", (int)children.size(), GetTickCount() - start);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int nCmdShow) {
    // Must happen before any window is created.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // The installer: build the GStreamer plugin cache and exit (app/Environment.cpp).
    if (lpCmdLine && TrimWs(lpCmdLine) == L"--gst-registry") return BuildGStreamerRegistry();

    // One Lyra at a time: a second Lyra.exe (a link opened from another app, a shortcut, the
    // installer's "Launch") hands its URL to the running one, which opens it in a new tab and
    // comes to the front, and exits before touching the profile.
    if (HWND running = FindWindowW(L"LyraWindowClass", nullptr)) {
        std::wstring arg = lpCmdLine ? TrimWs(lpCmdLine) : std::wstring();
        if (arg.size() >= 2 && arg.front() == L'"' && arg.back() == L'"') arg = arg.substr(1, arg.size() - 2);
        COPYDATASTRUCT cds = { kOpenUrlCopyData, (DWORD)((arg.size() + 1) * sizeof(wchar_t)), (void*)arg.c_str() };
        DWORD pid = 0;
        GetWindowThreadProcessId(running, &pid);
        AllowSetForegroundWindow(pid);                // so it may bring itself to the front
        DWORD_PTR handled = 0;
        if (SendMessageTimeoutW(running, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG, 5000, &handled) && handled)
            return 0;
    }
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
    UiLoad();
    HistoryLoad();
    BookmarksLoad();
    DownloadsLoad();
    EnableDarkMenus();

    ConfigureGStreamerEnvironment();
    ConfigureNetworkEnvironment();

    const wchar_t CLASS_NAME[] = L"LyraWindowClass";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;                      // we paint everything ourselves
    wc.hIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,       // Lyra.rc
                                 GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
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
    // The icon at this screen's size (the class icons above are for 100 %); other windows
    // (picture-in-picture, print preview, notifications) take it from here with WM_GETICON.
    {
        auto icon = [&](int metric) {
            const int px = GetSystemMetricsForDpi(metric, sysDpi);
            return (LPARAM)LoadImageW(hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, px, px, 0);
        };
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, icon(SM_CXICON));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, icon(SM_CXSMICON));
    }
    // Apply the custom frame (WM_NCCALCSIZE drops the system title bar) before the window shows.
    FrameApply(hwnd);

    // WebKit
    WKContextConfigurationRef contextConf = WKContextConfigurationCreate();
    // A page that moves to another site gets a new web process (process swap on cross-site
    // navigation); starting one takes a while on Windows. Keep one started in advance.
    WKContextConfigurationSetPrewarmsProcessesAutomatically(contextConf, true);
    g_context = WKContextCreateWithConfiguration(contextConf);
    WKRelease(contextConf);
    // Tuned for a browser with back/forward navigation (bigger page/memory caches)
    // instead of the conservative "document viewer" default.
    WKContextSetCacheModel(g_context, kWKCacheModelPrimaryWebBrowser);

    g_dataStore = CreateWebsiteDataStore();
    SystemProxyInit();                                 // the Windows proxy (VPN clients' "system proxy" mode)

    g_preferences = WKPreferencesCreate();
    ApplyWebPrefs();                                   // JS, images, storage, fonts... from settings
    WKPreferencesSetSuppressesIncrementalRendering(g_preferences, false);
    PermissionsInit();                                 // site permissions: notifications, location
    AdBlockInit();                                     // compiled filter lists; updates them in the background
    ExtensionsInit();                                  // installed extensions: their content scripts
    MediaControlsInit();                               // media keys, Windows' media flyout
    PasswordsInit();
    WebAuthnInit();                                   // passkeys and security keys (Windows WebAuthn)                                   // saved passwords: offer to save, fill in
    PipInit();                                         // picture-in-picture
    TabSleepInit();                                    // sleeping tabs (typed-text check)
    WKTextCheckerSetContinuousSpellCheckingEnabled(g_prefs.spellCheck);   // spelling (Windows dictionaries)
    ReaderInit();                                      // reader view
    TranslateInit();                                   // page translation (the DeepL key)
    SidebarInit();                                     // the sidebar: panels, web panels, reading list, notes
    // (WKPreferencesSetAVFoundationEnabled was removed: AVFoundation is Apple-only and
    //  does nothing on the Windows port, which plays media through Media Foundation.)

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // First start: the welcome screen (language, theme, tab layout...) instead of the home page.
    std::wstring startUrl = g_prefs.onboarded ? g_homePage : StartPageUrl(true);
    bool fromCommandLine = false;
    if (lpCmdLine && *lpCmdLine) {
        std::wstring arg = TrimWs(lpCmdLine);
        if (arg.size() >= 2 && arg.front() == L'"' && arg.back() == L'"') arg = arg.substr(1, arg.size() - 2);
        if (!arg.empty()) { startUrl = arg; fromCommandLine = true; }
    }
    // The tabs of the last session (always after a crash; after a normal exit if Settings say so).
    const bool restored = g_prefs.onboarded && SessionRestore(fromCommandLine);
    if (!restored) CreateTab(startUrl);
    ScheduleSpareTab(3000);              // the next new tab, once the first page is up
    UpdaterInit();                       // installed copies look for updates (app/Updater.cpp)
    StallWatchStart();                   // notes in tabs.log where the window got stuck (app/StallWatch.cpp)
    DevProcessesStart();                 // LYRA_PROCS (developer)

    // LYRA_MSG_PROF=1 (developer): where the main thread's time goes, by window class and
    // message, every 2 s in tabs.log.
    static const bool msgProf = [] { wchar_t v[4] = {}; return GetEnvironmentVariableW(L"LYRA_MSG_PROF", v, 4) && v[0] == L'1'; }();
    struct MsgCost { double ms = 0, max = 0; int n = 0; };
    std::map<std::pair<std::wstring, UINT>, MsgCost> costs;
    LARGE_INTEGER qf, profStart = {};
    QueryPerformanceFrequency(&qf);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (HandleShortcut(msg)) continue;
        TranslateMessage(&msg);
        if (!msgProf) { DispatchMessageW(&msg); continue; }
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        DispatchMessageW(&msg);
        QueryPerformanceCounter(&b);
        wchar_t cls[64] = L"(thread)";
        if (msg.hwnd) GetClassNameW(msg.hwnd, cls, 64);
        MsgCost& c = costs[{ cls, msg.message == WM_TIMER ? 0x10000u + (UINT)(msg.wParam & 0xFFFF) : msg.message }];
        const double ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)qf.QuadPart;
        c.ms += ms; c.max = std::max(c.max, ms); ++c.n;
        if (!profStart.QuadPart) profStart = b;
        if (b.QuadPart - profStart.QuadPart > 2 * qf.QuadPart) {
            std::vector<std::pair<double, std::string>> top;
            for (auto& [k, v] : costs) {
                char line[200];
                snprintf(line, sizeof line, "%ls msg %s%04X: %d x, %.1f ms total, max %.1f", k.first.c_str(),
                         k.second >= 0x10000 ? "timer " : "", k.second & 0xFFFF, v.n, v.ms, v.max);
                top.push_back({ v.ms, line });
            }
            std::sort(top.rbegin(), top.rend());
            for (size_t i = 0; i < top.size() && i < 8; ++i) Log("msgprof %s", top[i].second.c_str());
            costs.clear();
            profStart = b;
        }
    }

    StallWatchStop();
    ExtensionsShutdown();                // background pages before the WebKit context goes
    Log("message loop ended; releasing WebKit preferences/context");
    MediaControlsShutdown();
    WebAuthnShutdown();
    PasswordsShutdown();
    AdBlockShutdown();
    PermissionsShutdown();
    if (g_preferences) { WKRelease(g_preferences); g_preferences = nullptr; }
    SystemProxyShutdown();
    if (g_dataStore)   { WKRelease(g_dataStore);   g_dataStore = nullptr; }
    if (g_context)     { WKRelease(g_context);     g_context = nullptr; }
    Log("WebKit context released");
    g_ctxs.clear();                      // only now can no WebKit callback refer to a TabCtx any more
    ReleaseResources();
    HistorySave();
    BookmarksFlush();
    DownloadsSave();
    FaviconsShutdown();
    GdiplusShutdown(gdipToken);
    OleUninitialize();
    // Everything is saved. End here instead of running the C++ static destructors: WebKit's
    // own threads can still be finishing, and a crash in the destructors at exit (one was seen
    // in a dump) would only lose the user a "Lyra stopped working" box.
    // WebKit's helper processes (network, web, GPU) shut down by themselves once the context is
    // gone; the network process still writes cookies, site storage and the cache. Wait for them
    // (a few seconds at most) so ending Lyra here never cuts that short.
    WaitForChildProcesses(4000);
    UpdaterOnExit();                     // a downloaded update installs now, quietly
    Log("exit");
    TerminateProcess(GetCurrentProcess(), (UINT)msg.wParam);
    return (int)msg.wParam;
}
