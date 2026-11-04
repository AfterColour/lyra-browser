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
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// COM declarations must be visible BEFORE gdiplus.h. windows.h skips them when
// WIN32_LEAN_AND_MEAN is defined (project-wide or by a precompiled header), which
// causes "IUnknown undefined" / "STDMETHOD not found" inside gdiplus.h.
#include <objbase.h>   // STDMETHOD, IUnknown, COM base types
#include <unknwn.h>    // IUnknown
#include <objidl.h>    // IStream (used by GDI+ image/stream APIs)
#include <windowsx.h>
#include <shellapi.h>  // ShellExecuteW (also skipped by WIN32_LEAN_AND_MEAN)
#include <commdlg.h>   // GetOpenFileNameW (file chooser for <input type=file>)
#include <shlobj.h>    // SHGetKnownFolderPath (Downloads folder)
#include <shlwapi.h>   // UrlCreateFromPathW
#include <dwmapi.h>
#include <algorithm>
#include <map>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <functional>
#include <memory>
#include <string>
#include <vector>
using std::min;   // gdiplus.h uses unqualified min/max
using std::max;
#include <gdiplus.h>

#include <WebKit/WebKit2_C.h>
#include <WebKit/WKContextConfigurationRef.h>
#include <WebKit/WKDownloadClient.h>
#include <WebKit/WKDownloadRef.h>
#include <WebKit/WKFrameInfoRef.h>
#include <WebKit/WKInspector.h>
#include <WebKit/WKNavigationResponseRef.h>
#include <WebKit/WKPreferencesRefPrivate.h>
#include <WebKit/WKSecurityOriginRef.h>
#include <WebKit/WKWebsiteDataStoreConfigurationRef.h>
#include <WebKit/WKWebsiteDataStoreRef.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "uuid.lib")

using namespace Gdiplus;

// ----------------------------------------------------------------------------
//  Metrics (in 96-DPI pixels; scaled with S())
// ----------------------------------------------------------------------------
namespace metrics {
constexpr int kToolbarH   = 46;
constexpr int kTabBarH    = 34;
constexpr int kButton     = 32;
constexpr int kEdge       = 12;
constexpr int kPillH      = 30;
constexpr int kPillMaxW   = 720;
constexpr int kPillRadius = 8;
constexpr int kTabInsetY  = 4;
constexpr int kTabRadius  = 7;
constexpr int kTabMaxW    = 360;
constexpr int kTabMinW    = 30;
constexpr int kNewTabW    = 38;
constexpr int kCloseBtn   = 18;
constexpr int kMaxTabs    = 20;
}  // namespace metrics

constexpr int   ID_URL_EDIT = 1001;
constexpr UINT_PTR TIMER_UI = 1;

// Display name used in the window title, dialogs and the About page.
static const wchar_t kAppName[] = L"Lyra";

// Appended to WebKit's standard Windows user agent. Without a "Version/... Safari/..."
// token many sites (Google, YouTube, ...) treat the browser as unknown and serve a
// degraded page. This is the same token GNOME Web / WebKitGTK use.
static const char kUserAgentAppName[] = "Version/26.0 Safari/605.1.15";

// Work that must not run inside a WebKit callback is posted back to the main window.
// wParam = tab id.
constexpr UINT WM_APP_CLOSE_TAB    = WM_APP + 1;   // window.close() from a page
constexpr UINT WM_APP_TAB_CRASHED  = WM_APP + 2;   // lParam = WKProcessTerminationReason

enum Cmd {
    CMD_NEWTAB = 301, CMD_CLOSETAB, CMD_COPYURL,
    CMD_ZOOMIN, CMD_ZOOMOUT, CMD_ZOOMRESET, CMD_SETHOME, CMD_SETTINGS,
    CMD_REOPENTAB, CMD_DEVTOOLS
};

// DWM attributes (own constants so they never clash with SDK enums)
constexpr DWORD kDwmDarkModeOld  = 19;
constexpr DWORD kDwmDarkMode     = 20;
constexpr DWORD kDwmBorderColor  = 34;
constexpr DWORD kDwmCaptionColor = 35;
constexpr DWORD kDwmTextColor    = 36;

// ----------------------------------------------------------------------------
//  Global state
// ----------------------------------------------------------------------------
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

static HWND   g_hWnd = nullptr;

// Element fullscreen (video players). WebKit sends the registered message "WebKitFullScreen" to
// this window (wParam: TRUE enter / FALSE exit, lParam: the WKView's HWND); see OnFullScreenMessage.
static UINT   g_fullScreenMsg = 0;
static HWND   g_fullScreenView = nullptr;      // WKView shown fullscreen, nullptr otherwise
static LONG_PTR g_savedStyle = 0;
static WINDOWPLACEMENT g_savedPlacement = { sizeof(WINDOWPLACEMENT) };
static HWND   g_urlEdit = nullptr;
static WNDPROC g_oldEditProc = nullptr;
static HFONT  g_editFont = nullptr;
static HBRUSH g_editBrush = nullptr;
static int    g_editTextH = 18;
static UINT   g_dpi = 96;
static int    g_clientW = 0;
static int    g_tick = 0;
static bool   g_trackingMouse = false;
static bool   g_dark = false;

static WKContextRef          g_context = nullptr;
static WKPreferencesRef      g_preferences = nullptr;
static WKWebsiteDataStoreRef g_dataStore = nullptr;   // cookies, storage, cache under %LOCALAPPDATA%\Lyra

static std::vector<std::wstring> g_closedUrls;        // for Ctrl+Shift+T (most recent last)
static int g_modalDepth = 0;                          // >0 while a dialog runs inside a WebKit callback
static std::map<WKDownloadRef, std::wstring> g_downloadPaths;   // destination of each running download
struct ModalScope {
    ModalScope()  { ++g_modalDepth; }
    ~ModalScope() { --g_modalDepth; }
};

static std::vector<std::unique_ptr<Tab>> g_tabs;
static int g_active = -1;
static unsigned long long g_nextTabId = 1;
static std::vector<std::unique_ptr<TabCtx>> g_ctxs;   // owns every TabCtx until shutdown (see TabCtx)
static bool g_closing = false;        // main window is shutting down
static bool g_tabOp = false;          // a create/close operation is in progress (re-entrancy guard)
static bool g_closeRequested = false; // WM_CLOSE already posted for "last tab closed"
struct TabOpGuard {
    TabOpGuard()  { g_tabOp = true; }
    ~TabOpGuard() { g_tabOp = false; }
};

// Settings
static std::wstring g_settingsDir, g_settingsFile;

// ----------------------------------------------------------------------------
//  Diagnostic log (tab lifecycle only; never written from paint code).
//  File: %LOCALAPPDATA%\Lyra\tabs.log  - appended across runs, every line is
//  written straight to the file so it survives a crash.
// ----------------------------------------------------------------------------
static HANDLE g_logFile = INVALID_HANDLE_VALUE;

static void Log(const char* fmt, ...) {
    if (g_logFile == INVALID_HANDLE_VALUE) return;
    char buf[768];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%02d:%02d:%02d.%03d [t%lu] ",
                        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentThreadId());
    if (n < 0) n = 0;
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE, fmt, ap);
    va_end(ap);
    size_t len = strlen(buf);
    if (len > sizeof(buf) - 3) len = sizeof(buf) - 3;
    buf[len++] = '\r'; buf[len++] = '\n';
    DWORD wr = 0;
    WriteFile(g_logFile, buf, (DWORD)len, &wr, nullptr);
}

static void LogOpen() {
    if (g_logFile != INVALID_HANDLE_VALUE || g_settingsDir.empty()) return;
    CreateDirectoryW(g_settingsDir.c_str(), nullptr);
    std::wstring path = g_settingsDir + L"\\tabs.log";
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz = {};
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > (1 << 20)) {      // keep the file small: restart it
        CloseHandle(h);
        h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
    }
    g_logFile = h;
    Log("==== session start, pid=%lu ====", GetCurrentProcessId());
}

static void LogAddress(const char* label, const void* addr) {
    HMODULE mod = nullptr;
    wchar_t path[MAX_PATH] = {};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)addr, &mod) && mod && GetModuleFileNameW(mod, path, MAX_PATH)) {
        const wchar_t* base = wcsrchr(path, L'\\');
        Log("%s %p  %ls+0x%llX", label, addr, base ? base + 1 : path,
            (unsigned long long)((const char*)addr - (const char*)mod));
    } else {
        Log("%s %p  (unknown module)", label, addr);
    }
}

// Last-resort diagnostics: records exception code, fault address and a stack trace
// (module + offset, resolve with the matching .pdb) into the log, then lets Windows
// handle the crash as usual.
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep);
static double       g_zoom = 1.0;
static std::wstring g_homePage = L"https://example.com";
static std::wstring g_searchEngine = L"https://duckduckgo.com/?q=";

struct Prefs {
    int  themeMode = 0;          // 0 system, 1 light, 2 dark
    int  accent = 0;             // index into kAccents
    bool compact = false;
    bool showFullUrl = false;
    bool closeAlways = false;
    int  newTabMode = 0;         // 0 home page, 1 blank page
    bool js = true, images = true, localStorage = true, databases = true;
    bool devExtras = true, fullscreen = true, webAudio = true;
    int  defaultFont = 16, minFont = 0;
};
static Prefs g_prefs;

struct AccentDef { const wchar_t* name; BYTE r, g, b; };
static const AccentDef kAccents[] = {
    { L"Blue", 0, 122, 255 },  { L"Purple", 175, 82, 222 }, { L"Pink", 255, 45, 85 },
    { L"Red", 255, 59, 48 },   { L"Orange", 255, 149, 0 },  { L"Green", 40, 184, 65 },
    { L"Graphite", 120, 120, 128 },
};
constexpr int kAccentCount = (int)(sizeof(kAccents) / sizeof(kAccents[0]));

static HWND g_setHwnd = nullptr;           // settings window
static void ShowSettingsWindow();
static void SettingsThemeChanged();
static void ApplyWebPrefs();

// ----------------------------------------------------------------------------
//  Theme
// ----------------------------------------------------------------------------
struct Theme {
    Color toolbar, tabbar, tabActive, tabHover, tabBorder, sep;
    Color pill, pillHover, pillFocus;
    Color text, text2, icon, iconDis, accent, hover, pressed;
    int   progressAlpha = 70;
};
static Theme g_th;

static Theme MakeTheme(bool dark) {
    Theme t;
    if (!dark) {
        t.toolbar   = Color(247, 247, 248);
        t.tabbar    = Color(233, 233, 235);
        t.tabActive = Color(255, 255, 255);
        t.tabHover  = Color(222, 222, 225);
        t.tabBorder = Color(30, 0, 0, 0);
        t.sep       = Color(40, 0, 0, 0);
        t.pill      = Color(20, 0, 0, 0);
        t.pillHover = Color(30, 0, 0, 0);
        t.pillFocus = Color(255, 255, 255);
        t.text      = Color(30, 30, 32);
        t.text2     = Color(112, 112, 118);
        t.icon      = Color(70, 70, 76);
        t.iconDis   = Color(176, 176, 182);
        t.accent    = Color(0, 122, 255);
        t.hover     = Color(20, 0, 0, 0);
        t.pressed   = Color(38, 0, 0, 0);
        t.progressAlpha = 70;
    } else {
        t.toolbar   = Color(50, 50, 52);
        t.tabbar    = Color(38, 38, 40);
        t.tabActive = Color(72, 72, 75);
        t.tabHover  = Color(56, 56, 59);
        t.tabBorder = Color(24, 255, 255, 255);
        t.sep       = Color(110, 0, 0, 0);
        t.pill      = Color(70, 0, 0, 0);
        t.pillHover = Color(95, 0, 0, 0);
        t.pillFocus = Color(28, 28, 30);
        t.text      = Color(240, 240, 243);
        t.text2     = Color(152, 152, 160);
        t.icon      = Color(206, 206, 212);
        t.iconDis   = Color(96, 96, 102);
        t.accent    = Color(10, 132, 255);
        t.hover     = Color(22, 255, 255, 255);
        t.pressed   = Color(42, 255, 255, 255);
        t.progressAlpha = 85;
    }
    return t;
}

static COLORREF ToColorRef(const Color& c) { return RGB(c.GetR(), c.GetG(), c.GetB()); }

// ----------------------------------------------------------------------------
//  Small helpers
// ----------------------------------------------------------------------------
static float S(float v) { return v * (float)g_dpi / 96.0f; }

static std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::wstring WKStr(WKStringRef s) {
    if (!s) return std::wstring();
    size_t cap = WKStringGetMaximumUTF8CStringSize(s);
    std::vector<char> buf(cap + 1, 0);
    size_t n = WKStringGetUTF8CString(s, buf.data(), cap);
    if (n == 0) return std::wstring();
    return ToWide(std::string(buf.data()));
}

static std::wstring TrimWs(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

static std::string UrlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else if (c == ' ') o += '+';
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

// "https://www.example.com/a?b" -> "example.com"
static std::wstring HostOf(const std::wstring& url) {
    if (url.empty() || url == L"about:blank") return std::wstring();
    size_t p = url.find(L"://");
    if (p == std::wstring::npos) return url;
    std::wstring scheme = url.substr(0, p);
    if (scheme != L"http" && scheme != L"https") return url;
    std::wstring rest = url.substr(p + 3);
    size_t e = rest.find_first_of(L"/?#");
    if (e != std::wstring::npos) rest.resize(e);
    size_t at = rest.rfind(L'@');
    if (at != std::wstring::npos) rest = rest.substr(at + 1);
    if (rest.compare(0, 4, L"www.") == 0) rest = rest.substr(4);
    return rest;
}

// "https://example.com/path" -> "example.com/path"
static std::wstring DisplayUrl(const std::wstring& url) {
    if (url.empty() || url == L"about:blank") return std::wstring();
    std::wstring s = url;
    if (s.compare(0, 8, L"https://") == 0) s = s.substr(8);
    else if (s.compare(0, 7, L"http://") == 0) s = s.substr(7);
    if (!s.empty() && s.back() == L'/' && s.find(L'/') == s.size() - 1) s.pop_back();
    return s;
}

// "https://...", "file:///...", "ftp://..." -> true. Only a real scheme counts, so a search
// such as "what does :// mean" is not mistaken for an address.
static bool HasUrlScheme(const std::wstring& s) {
    size_t p = s.find(L"://");
    if (p == std::wstring::npos || p == 0) return false;
    if (!iswalpha(s[0])) return false;
    for (size_t i = 1; i < p; ++i)
        if (!(iswalnum(s[i]) || s[i] == L'+' || s[i] == L'-' || s[i] == L'.')) return false;
    return true;
}

// Turn whatever the user typed into a URL (or a search query).
static std::string ResolveInput(const std::wstring& input) {
    std::wstring s = TrimWs(input);
    if (s.empty()) return std::string();
    std::string u = ToUtf8(s);
    const bool hasSpace = s.find(L' ') != std::wstring::npos;

    if (!hasSpace && HasUrlScheme(s)) return u;
    if (s.compare(0, 6, L"about:") == 0 || s.compare(0, 5, L"data:") == 0) return u;
    // "javascript:" typed into the address bar is a classic self-XSS trick: search instead.

    // A local file path: C:\folder\page.html or \\server\share\page.html
    if ((s.size() > 2 && iswalpha(s[0]) && s[1] == L':' && (s[2] == L'\\' || s[2] == L'/')) ||
        s.compare(0, 2, L"\\\\") == 0) {
        wchar_t url[2084] = {};
        DWORD len = ARRAYSIZE(url);
        if (SUCCEEDED(UrlCreateFromPathW(s.c_str(), url, &len, 0))) return ToUtf8(url);
    }

    if (!hasSpace) {
        bool local = s.compare(0, 9, L"localhost") == 0;
        bool ipLike = s.find_first_not_of(L"0123456789.:/") == std::wstring::npos &&
                      s.find(L'.') != std::wstring::npos;
        if (local || ipLike) return "http://" + u;
        if (s.find(L'.') != std::wstring::npos && s[0] != L'.' && s.back() != L'.') return "https://" + u;
    }
    return ToUtf8(g_searchEngine) + UrlEncode(u);
}

// ----------------------------------------------------------------------------
//  Settings
// ----------------------------------------------------------------------------
static void InitPaths() {
    wchar_t base[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        GetTempPathW(MAX_PATH, base);
    }
    std::wstring dir = base;
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    g_settingsDir = dir + L"\\Lyra";
    g_settingsFile = g_settingsDir + L"\\settings.ini";

    // The browser used to be called MyBrowser: take its profile (settings, cookies, history)
    // along the first time Lyra starts.
    std::wstring oldDir = dir + L"\\MyBrowser";
    if (GetFileAttributesW(g_settingsDir.c_str()) == INVALID_FILE_ATTRIBUTES
        && GetFileAttributesW(oldDir.c_str()) != INVALID_FILE_ATTRIBUTES)
        MoveFileW(oldDir.c_str(), g_settingsDir.c_str());
}

// Debug switches are documented as LYRA_*; the WebKit side (GPU and web processes) still reads
// them as MYBROWSER_*. Copy every LYRA_X to MYBROWSER_X (unless that is set already) before
// WebKit starts its processes, which inherit this environment.
static void ForwardLyraEnvironment() {
    LPWCH block = GetEnvironmentStringsW();
    if (!block) return;
    std::vector<std::pair<std::wstring, std::wstring>> forward;
    for (LPWCH entry = block; *entry; entry += wcslen(entry) + 1) {
        std::wstring line = entry;
        if (_wcsnicmp(line.c_str(), L"LYRA_", 5) != 0) continue;
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        forward.emplace_back(L"MYBROWSER_" + line.substr(5, eq - 5), line.substr(eq + 1));
    }
    FreeEnvironmentStringsW(block);
    for (auto& [name, value] : forward) {
        if (!GetEnvironmentVariableW(name.c_str(), nullptr, 0))
            SetEnvironmentVariableW(name.c_str(), value.c_str());
    }
}

static void LoadSettings() {
    CreateDirectoryW(g_settingsDir.c_str(), nullptr);
    wchar_t buf[512] = {};
    GetPrivateProfileStringW(L"Browser", L"Scale", L"1.0", buf, 512, g_settingsFile.c_str());
    g_zoom = _wtof(buf);
    if (g_zoom < 0.25 || g_zoom > 5.0) g_zoom = 1.0;
    GetPrivateProfileStringW(L"Browser", L"HomePage", L"https://example.com", buf, 512, g_settingsFile.c_str());
    g_homePage = buf;
    GetPrivateProfileStringW(L"Browser", L"SearchEngine", L"https://duckduckgo.com/?q=", buf, 512, g_settingsFile.c_str());
    g_searchEngine = buf;

    auto geti = [&](const wchar_t* k, int d) {
        return (int)GetPrivateProfileIntW(L"Browser", k, d, g_settingsFile.c_str());
    };
    g_prefs.themeMode   = std::max(0, std::min(2, geti(L"Theme", 0)));
    g_prefs.accent      = std::max(0, std::min(kAccentCount - 1, geti(L"Accent", 0)));
    g_prefs.compact     = geti(L"Compact", 0) != 0;
    g_prefs.showFullUrl = geti(L"FullUrl", 0) != 0;
    g_prefs.closeAlways = geti(L"CloseAlways", 0) != 0;
    g_prefs.newTabMode  = std::max(0, std::min(1, geti(L"NewTabMode", 0)));
    g_prefs.js          = geti(L"JavaScript", 1) != 0;
    g_prefs.images      = geti(L"Images", 1) != 0;
    g_prefs.localStorage= geti(L"LocalStorage", 1) != 0;
    g_prefs.databases   = geti(L"Databases", 1) != 0;
    g_prefs.devExtras   = geti(L"DevTools", 1) != 0;
    g_prefs.fullscreen  = geti(L"Fullscreen", 1) != 0;
    g_prefs.webAudio    = geti(L"WebAudio", 1) != 0;
    g_prefs.defaultFont = std::max(9, std::min(32, geti(L"DefaultFont", 16)));
    g_prefs.minFont     = std::max(0, std::min(24, geti(L"MinFont", 0)));
}

static void SaveSettings() {
    wchar_t buf[64];
    swprintf(buf, 64, L"%.2f", g_zoom);
    WritePrivateProfileStringW(L"Browser", L"Scale", buf, g_settingsFile.c_str());
    WritePrivateProfileStringW(L"Browser", L"HomePage", g_homePage.c_str(), g_settingsFile.c_str());
    WritePrivateProfileStringW(L"Browser", L"SearchEngine", g_searchEngine.c_str(), g_settingsFile.c_str());

    auto puti = [&](const wchar_t* k, int v) {
        WritePrivateProfileStringW(L"Browser", k, std::to_wstring(v).c_str(), g_settingsFile.c_str());
    };
    puti(L"Theme", g_prefs.themeMode);   puti(L"Accent", g_prefs.accent);
    puti(L"Compact", g_prefs.compact);   puti(L"FullUrl", g_prefs.showFullUrl);
    puti(L"CloseAlways", g_prefs.closeAlways); puti(L"NewTabMode", g_prefs.newTabMode);
    puti(L"JavaScript", g_prefs.js);     puti(L"Images", g_prefs.images);
    puti(L"LocalStorage", g_prefs.localStorage); puti(L"Databases", g_prefs.databases);
    puti(L"DevTools", g_prefs.devExtras); puti(L"Fullscreen", g_prefs.fullscreen);
    puti(L"WebAudio", g_prefs.webAudio);
    puti(L"DefaultFont", g_prefs.defaultFont); puti(L"MinFont", g_prefs.minFont);
}

// ----------------------------------------------------------------------------
//  Layout & hit testing
// ----------------------------------------------------------------------------
struct Layout {
    float toolbarH = 0, tabBarH = 0, chromeH = 0;
    RectF back, fwd, more, pill, reload, newTab;
    std::vector<RectF> tabs;
};
static Layout g_L;

struct Hit {
    enum Kind { None, Back, Forward, Reload, More, NewTab, Pill, TabBody, TabClose };
    Kind kind = None;
    int  index = -1;
    bool operator==(const Hit& o) const { return kind == o.kind && index == o.index; }
};
static Hit MakeHit(Hit::Kind k, int i = -1) { Hit h; h.kind = k; h.index = i; return h; }
static Hit g_hover, g_down;

static Tab* ActiveTab() {
    return (g_active >= 0 && g_active < (int)g_tabs.size()) ? g_tabs[g_active].get() : nullptr;
}

static bool IsEditing() { return g_urlEdit && IsWindowVisible(g_urlEdit); }

static void InvalidateChrome() {
    if (!g_hWnd) return;
    RECT r = { 0, 0, g_clientW, (LONG)std::ceil(g_L.chromeH) };
    InvalidateRect(g_hWnd, &r, FALSE);
}

static void ComputeLayout(int clientW) {
    using namespace metrics;
    g_clientW = clientW;
    const float w = (float)clientW;
    Layout& L = g_L;

    L.toolbarH = std::floor(S((float)(g_prefs.compact ? 40 : kToolbarH)));
    L.tabBarH  = std::floor(S((float)(g_prefs.compact ? 30 : kTabBarH)));
    L.chromeH  = L.toolbarH + L.tabBarH;

    const float btn = S((float)(g_prefs.compact ? 28 : kButton)), edge = S((float)kEdge), gap = S(2);
    const float by = std::floor((L.toolbarH - btn) / 2);
    L.back = RectF(edge, by, btn, btn);
    L.fwd  = RectF(edge + btn + gap, by, btn, btn);
    L.more = RectF(w - edge - btn, by, btn, btn);

    const float avL = L.fwd.GetRight() + S(16);
    const float avR = L.more.X - S(16);
    const float avail = std::max(0.f, avR - avL);
    const float pw = std::min(S((float)kPillMaxW), avail);
    const float ph = S((float)(g_prefs.compact ? 26 : kPillH));
    float px = w / 2 - pw / 2;
    px = std::max(avL, std::min(px, avR - pw));
    L.pill = RectF(std::floor(px), std::floor((L.toolbarH - ph) / 2), std::floor(pw), ph);

    const float rb = S(g_prefs.compact ? 22.f : 26.f);
    L.reload = RectF(L.pill.GetRight() - rb - S(3), L.pill.Y + (ph - rb) / 2, rb, rb);

    // Tab strip
    L.newTab = RectF(w - S((float)kNewTabW), L.toolbarH, S((float)kNewTabW), L.tabBarH);
    L.tabs.clear();
    const size_t n = g_tabs.size();
    const float margin = S(6);
    const float stripW = std::max(0.f, w - S((float)kNewTabW) - margin * 2);
    float tabW = n ? stripW / (float)n : 0.f;
    tabW = std::min(tabW, S((float)kTabMaxW));
    tabW = std::max(tabW, S((float)kTabMinW));
    const float inset = S((float)kTabInsetY);
    for (size_t i = 0; i < n; ++i) {
        L.tabs.push_back(RectF(std::floor(margin + tabW * (float)i), L.toolbarH + inset,
                               std::floor(tabW - S(2)), L.tabBarH - inset * 2));
    }
}

static RectF CloseRect(int i) {
    const RectF& r = g_L.tabs[i];
    const float c = S((float)metrics::kCloseBtn);
    return RectF(r.X + S(6), r.Y + (r.Height - c) / 2, c, c);
}

static bool CloseVisible(int i) {
    if (i < 0 || i >= (int)g_L.tabs.size()) return false;
    bool hov = (g_hover.kind == Hit::TabBody || g_hover.kind == Hit::TabClose) && g_hover.index == i;
    return g_L.tabs[i].Width >= S(44) && (i == g_active || hov || g_prefs.closeAlways);
}

static Hit HitTest(int px, int py) {
    const float x = (float)px, y = (float)py;
    if (y < 0 || y >= g_L.chromeH) return MakeHit(Hit::None);
    if (g_L.back.Contains(x, y))  return MakeHit(Hit::Back);
    if (g_L.fwd.Contains(x, y))   return MakeHit(Hit::Forward);
    if (g_L.more.Contains(x, y))  return MakeHit(Hit::More);
    if (!IsEditing() && g_L.reload.Contains(x, y)) return MakeHit(Hit::Reload);
    if (g_L.pill.Contains(x, y))  return MakeHit(Hit::Pill);
    if (g_L.newTab.Contains(x, y)) return MakeHit(Hit::NewTab);
    if (y >= g_L.toolbarH) {
        for (int i = (int)g_L.tabs.size() - 1; i >= 0; --i) {
            if (CloseVisible(i) && CloseRect(i).Contains(x, y)) return MakeHit(Hit::TabClose, i);
            const RectF& r = g_L.tabs[i];
            if (x >= r.X && x < r.GetRight() + S(2)) return MakeHit(Hit::TabBody, i);
        }
    }
    return MakeHit(Hit::None);
}

// ----------------------------------------------------------------------------
//  GDI+ resources
// ----------------------------------------------------------------------------
static std::unique_ptr<Font> g_fText, g_fTab, g_fIcon, g_fIconSm;

static std::unique_ptr<Font> MakeFont(const wchar_t* const* families, int count, float px) {
    for (int i = 0; i < count; ++i) {
        auto f = std::make_unique<Font>(families[i], px, FontStyleRegular, UnitPixel);
        if (f->IsAvailable()) return f;
    }
    return std::make_unique<Font>(L"Segoe UI", px, FontStyleRegular, UnitPixel);
}

static void PositionEdit() {
    if (!g_urlEdit) return;
    const float padX = S(14);
    int x = (int)(g_L.pill.X + padX);
    int w = (int)std::max(10.f, g_L.pill.Width - padX * 2);
    int h = g_editTextH;
    int y = (int)(g_L.pill.Y + (g_L.pill.Height - (float)h) / 2);
    MoveWindow(g_urlEdit, x, y, w, h, TRUE);
}

static void RebuildResources() {
    static const wchar_t* ui[]   = { L"Segoe UI" };
    static const wchar_t* icons[] = { L"Segoe Fluent Icons", L"Segoe MDL2 Assets" };
    g_fText   = MakeFont(ui, 1, S(14));
    g_fTab    = MakeFont(ui, 1, S(13));
    g_fIcon   = MakeFont(icons, 2, S(16));
    g_fIconSm = MakeFont(icons, 2, S(12));

    HFONT old = g_editFont;
    g_editFont = CreateFontW(-(int)std::round(S(14)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (g_urlEdit) {
        HDC dc = GetDC(g_urlEdit);
        HGDIOBJ prev = SelectObject(dc, g_editFont);
        TEXTMETRICW tm = {};
        GetTextMetricsW(dc, &tm);
        g_editTextH = tm.tmHeight;
        SelectObject(dc, prev);
        ReleaseDC(g_urlEdit, dc);
        SendMessageW(g_urlEdit, WM_SETFONT, (WPARAM)g_editFont, TRUE);
    }
    if (old) DeleteObject(old);
}

// ----------------------------------------------------------------------------
//  Theme / DWM
// ----------------------------------------------------------------------------
static bool SystemUsesDarkMode() {
    DWORD v = 1, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS)
        return v == 0;
    return false;
}

// Lets popup menus follow the system dark mode (undocumented but widely used).
static void EnableDarkMenus() {
    HMODULE ux = LoadLibraryW(L"uxtheme.dll");
    if (!ux) return;
    typedef int  (WINAPI *SetPreferredAppModeFn)(int);
    typedef void (WINAPI *FlushMenuThemesFn)();
    SetPreferredAppModeFn setMode = (SetPreferredAppModeFn)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    FlushMenuThemesFn flush = (FlushMenuThemesFn)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (setMode) setMode(1 /* AllowDark */);
    if (flush) flush();
}

static bool ResolveDark() {
    return g_prefs.themeMode == 2 || (g_prefs.themeMode == 0 && SystemUsesDarkMode());
}

static void ApplyDwm(HWND hwnd) {
    if (!hwnd) return;
    BOOL d = g_dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, kDwmDarkMode, &d, sizeof(d));
    DwmSetWindowAttribute(hwnd, kDwmDarkModeOld, &d, sizeof(d));
    // Windows 11: make the native title bar blend into the toolbar.
    COLORREF cap = ToColorRef(g_th.toolbar);
    COLORREF txt = ToColorRef(g_th.text);
    DwmSetWindowAttribute(hwnd, kDwmCaptionColor, &cap, sizeof(cap));
    DwmSetWindowAttribute(hwnd, kDwmBorderColor, &cap, sizeof(cap));
    DwmSetWindowAttribute(hwnd, kDwmTextColor, &txt, sizeof(txt));
}

static void ApplyTheme(bool dark) {
    g_dark = dark;
    g_th = MakeTheme(dark);
    if (g_prefs.accent > 0 && g_prefs.accent < kAccentCount) {
        const AccentDef& a = kAccents[g_prefs.accent];
        g_th.accent = Color(a.r, a.g, a.b);
    }
    if (g_editBrush) DeleteObject(g_editBrush);
    g_editBrush = CreateSolidBrush(ToColorRef(g_th.pillFocus));

    if (g_hWnd) {
        ApplyDwm(g_hWnd);
        InvalidateRect(g_hWnd, nullptr, FALSE);
        if (g_urlEdit) InvalidateRect(g_urlEdit, nullptr, TRUE);
    }
    SettingsThemeChanged();
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
static bool SyncTab(Tab& t) {
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

static void LayoutWebViews() {
    if (!g_hWnd) return;
    RECT cr;
    GetClientRect(g_hWnd, &cr);
    int top = g_fullScreenView ? 0 : (int)g_L.chromeH;   // fullscreen: the page covers the tab strip and toolbar
    int h = std::max(0, (int)cr.bottom - top);
    // Only the visible tab is resized. Hidden tabs are brought to the right size
    // by SelectTab() (which calls this function) right after they are shown, so
    // a window resize no longer sends resize IPC to every background page.
    if (Tab* t = ActiveTab())
        if (t->hwnd) MoveWindow(t->hwnd, 0, top, cr.right, h, TRUE);
}

static void RefreshLayout() {
    RECT cr;
    GetClientRect(g_hWnd, &cr);
    ComputeLayout(cr.right);
    PositionEdit();
    LayoutWebViews();
    InvalidateChrome();
}

static void FocusWebView() {
    if (Tab* t = ActiveTab()) if (t->hwnd) SetFocus(t->hwnd);
}

static void ShowUrlEdit() {
    if (!g_urlEdit) return;
    Tab* t = ActiveTab();
    std::wstring text = (t && t->url != L"about:blank") ? t->url : L"";
    SetWindowTextW(g_urlEdit, text.c_str());
    PositionEdit();
    ShowWindow(g_urlEdit, SW_SHOW);
    SetFocus(g_urlEdit);
    SendMessageW(g_urlEdit, EM_SETSEL, 0, -1);
    InvalidateChrome();
}

// Leaves element fullscreen on the window side. Called for WebKit's exit message and when the
// fullscreen tab goes away without WebKit telling us (tab closed, page crashed).
static void RestoreFromFullScreen() {
    if (!g_fullScreenView) return;
    g_fullScreenView = nullptr;
    SetWindowLongPtrW(g_hWnd, GWL_STYLE, g_savedStyle);
    SetWindowPlacement(g_hWnd, &g_savedPlacement);
    SetWindowPos(g_hWnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    RefreshLayout();
    InvalidateRect(g_hWnd, nullptr, FALSE);
}

static LRESULT OnFullScreenMessage(bool enter, HWND view) {
    if (!enter) {
        RestoreFromFullScreen();
        return TRUE;
    }
    if (g_fullScreenView) return TRUE;              // already fullscreen
    Tab* t = ActiveTab();
    if (!t || t->hwnd != view) return FALSE;        // a background tab: let WebKit handle it
    if (IsEditing()) ShowWindow(g_urlEdit, SW_HIDE);

    g_savedStyle = GetWindowLongPtrW(g_hWnd, GWL_STYLE);
    g_savedPlacement.length = sizeof(g_savedPlacement);
    GetWindowPlacement(g_hWnd, &g_savedPlacement);
    g_fullScreenView = view;

    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(g_hWnd, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& m = mi.rcMonitor;
    SetWindowLongPtrW(g_hWnd, GWL_STYLE, (g_savedStyle & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
    // WM_SIZE -> RefreshLayout() gives the view the whole client area (see LayoutWebViews).
    SetWindowPos(g_hWnd, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top,
                 SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    RefreshLayout();
    SetFocus(view);
    return TRUE;
}

// The page shown fullscreen must leave fullscreen before another tab is shown.
static void ExitFullScreenForTabSwitch() {
    if (!g_fullScreenView) return;
    for (auto& tp : g_tabs) {
        if (tp->hwnd == g_fullScreenView && tp->page) {
            WKPageRequestExitFullScreen(tp->page);   // WebKit answers with the exit message
            break;
        }
    }
    RestoreFromFullScreen();                          // in case the page does not answer
}

static void SelectTab(int i) {
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
static Tab* ResolveTab(const void* clientInfo, const char* what) {
    TabCtx* c = (TabCtx*)clientInfo;
    if (!c) { Log("callback %s: NULL context -> ignored", what); return nullptr; }
    Tab* t = c->tab;
    if (g_closing || !t || t->state != TabState::Live) {
        Log("callback %s: tab#%llu is closed/closing -> ignored", what, c->id);
        return nullptr;
    }
    return t;
}

static Tab* FindTab(unsigned long long id, int* index = nullptr) {
    for (int i = 0; i < (int)g_tabs.size(); ++i) {
        Tab* t = g_tabs[i].get();
        if (t->id == id && t->state == TabState::Live) {
            if (index) *index = i;
            return t;
        }
    }
    return nullptr;
}

static std::wstring HtmlEscape(const std::wstring& s) {
    std::wstring o;
    o.reserve(s.size());
    for (wchar_t c : s) {
        switch (c) {
        case L'&': o += L"&amp;"; break;
        case L'<': o += L"&lt;"; break;
        case L'>': o += L"&gt;"; break;
        case L'"': o += L"&quot;"; break;
        case L'\'': o += L"&#39;"; break;
        default: o += c;
        }
    }
    return o;
}

// A JavaScript string literal that is also safe inside an HTML <script>/attribute.
static std::wstring JsString(const std::wstring& s) {
    std::wstring o = L"\"";
    for (wchar_t c : s) {
        if (c == L'\\' || c == L'"' || c == L'\'' || c == L'<' || c == L'>' || c == L'&' ||
            c < 0x20 || c == 0x2028 || c == 0x2029) {
            wchar_t buf[8];
            swprintf(buf, 8, L"\\u%04X", (unsigned)c);
            o += buf;
        } else {
            o += c;
        }
    }
    return o + L"\"";
}

// Shows a small built-in page (load error, crashed tab) in place of `failedUrl`.
// The address bar keeps showing `failedUrl` and "Try again" reloads it.
static void LoadInfoPage(Tab& t, const std::wstring& heading, const std::wstring& text,
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
        html += L"<button onclick='location.replace(" + HtmlEscape(JsString(failedUrl)) + L")'>Try again</button>";
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
    Log("tab#%llu didStart", t->id);
    t->loading = true;
    t->progress = 0.08f;
    t->fade = 1.f;
    SyncTab(*t);
    InvalidateChrome();
}
static void OnDidCommit(WKPageRef, WKNavigationRef, WKTypeRef, const void* ci) {
    Tab* t = ResolveTab(ci, "didCommit");
    if (!t) return;
    Log("tab#%llu didCommit", t->id);
    SyncTab(*t);
    InvalidateChrome();
}
static void OnDidFinish(WKPageRef, WKNavigationRef, WKTypeRef, const void* ci) {
    Tab* t = ResolveTab(ci, "didFinish");
    if (!t) return;
    Log("tab#%llu didFinish", t->id);
    t->loading = false;
    t->progress = std::max(t->progress, 0.9f);
    SyncTab(*t);
    InvalidateChrome();
}
static void StopLoadingIndicator(Tab& t) {
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

    LoadInfoPage(*t, L"This page can\x2019t be opened",
                 text.empty() ? L"The server could not be reached." : text,
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

// Only the main frame decides whether a response is shown or saved.
static void OnDecidePolicyForResponse(WKPageRef, WKNavigationResponseRef nr, WKFramePolicyListenerRef listener,
                                      WKTypeRef, const void*) {
    bool mainFrame = true;
    if (WKFrameInfoRef fi = WKNavigationResponseCopyFrameInfo(nr)) {
        mainFrame = WKFrameInfoGetIsMainFrame(fi);
        WKRelease(fi);
    }
    bool attachment = false;
    std::wstring mime;
    if (WKURLResponseRef resp = WKNavigationResponseCopyResponse(nr)) {
        attachment = WKURLResponseIsAttachment(resp);
        if (WKStringRef m = WKURLResponseCopyMIMEType(resp)) { mime = WKStr(m); WKRelease(m); }
        WKRelease(resp);
    }
    if (mainFrame && (attachment || !WKNavigationResponseCanShowMIMEType(nr))) {
        Log("response -> download: mime='%ls' attachment=%d canShow=%d", mime.c_str(), (int)attachment,
            (int)WKNavigationResponseCanShowMIMEType(nr));
        WKFramePolicyListenerDownload(listener);
        return;
    }
    WKFramePolicyListenerUse(listener);
}

// --- downloads (saved straight to the user's Downloads folder) ------------------
static std::wstring DownloadsFolder() {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &p)) && p) dir = p;
    if (p) CoTaskMemFree(p);
    if (dir.empty()) {
        wchar_t buf[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH)) dir = std::wstring(buf) + L"\\Downloads";
    }
    return dir;
}

static std::wstring SanitizeFileName(std::wstring name) {
    for (wchar_t& c : name)
        if (c < 0x20 || wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    while (!name.empty() && (name.back() == L'.' || name.back() == L' ')) name.pop_back();
    while (!name.empty() && name.front() == L' ') name.erase(name.begin());
    if (name.size() > 180) name = name.substr(name.size() - 180);
    return name.empty() ? std::wstring(L"download") : name;
}

// "file.zip" -> "file (1).zip", "file (2).zip", ... until the name is free.
static std::wstring UniquePath(const std::wstring& dir, const std::wstring& name) {
    std::wstring path = dir + L"\\" + name;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return path;
    size_t dot = name.rfind(L'.');
    std::wstring stem = dot == std::wstring::npos || dot == 0 ? name : name.substr(0, dot);
    std::wstring ext  = dot == std::wstring::npos || dot == 0 ? L"" : name.substr(dot);
    for (int i = 1; i < 10000; ++i) {
        path = dir + L"\\" + stem + L" (" + std::to_wstring(i) + L")" + ext;
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return path;
    }
    return dir + L"\\" + stem + L" (" + std::to_wstring(GetTickCount()) + L")" + ext;
}

static WKStringRef OnDownloadDecideDestination(WKDownloadRef download, WKURLResponseRef response,
                                               WKStringRef suggestedFilename, const void*) {
    std::wstring name = WKStr(suggestedFilename);
    if (name.empty() && response) {
        if (WKURLRef u = WKURLResponseCopyURL(response)) {
            if (WKStringRef s = WKURLCopyLastPathComponent(u)) { name = WKStr(s); WKRelease(s); }
            if (name.empty() || name == L"/")
                if (WKStringRef s = WKURLCopyHostName(u)) { name = WKStr(s); WKRelease(s); }
            WKRelease(u);
        }
    }
    std::wstring dir = DownloadsFolder();
    if (dir.empty()) { Log("download: no Downloads folder"); return nullptr; }
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring path = UniquePath(dir, SanitizeFileName(name));
    g_downloadPaths[download] = path;
    Log("download: -> %ls", path.c_str());
    return WKStringCreateWithUTF8CString(ToUtf8(path).c_str());   // +1, owned by WebKit
}

static void OnDownloadFinished(WKDownloadRef download, const void*) {
    std::wstring path;
    auto it = g_downloadPaths.find(download);
    if (it != g_downloadPaths.end()) { path = it->second; g_downloadPaths.erase(it); }
    Log("download finished: %ls", path.c_str());
    if (path.empty() || g_closing || !g_hWnd) return;
    const wchar_t* base = wcsrchr(path.c_str(), L'\\');
    std::wstring msg = std::wstring(L"Downloaded \x201C") + (base ? base + 1 : path.c_str()) +
                       L"\x201D to your Downloads folder.\n\nShow it in the folder?";
    int r;
    { ModalScope modal; r = MessageBoxW(g_hWnd, msg.c_str(), kAppName, MB_YESNO | MB_ICONINFORMATION); }
    if (r == IDYES) {
        std::wstring args = L"/select,\"" + path + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
}

static void OnDownloadFailed(WKDownloadRef download, WKErrorRef error, WKDataRef, const void*) {
    g_downloadPaths.erase(download);
    int code = error ? WKErrorGetErrorCode(error) : 0;
    std::wstring text;
    if (error) if (WKStringRef s = WKErrorCopyLocalizedDescription(error)) { text = WKStr(s); WKRelease(s); }
    Log("download failed: code=%d %ls", code, text.c_str());
    if (code == 302 || code == 400 || g_closing || !g_hWnd) return;   // cancelled
    std::wstring msg = L"The download failed.\n\n" + text;
    ModalScope modal;
    MessageBoxW(g_hWnd, msg.c_str(), kAppName, MB_OK | MB_ICONWARNING);
}

static void AttachDownloadClient(WKDownloadRef download) {
    WKDownloadClientV0 dc = {};
    dc.base.version = 0;
    dc.base.clientInfo = nullptr;
    dc.decideDestinationWithResponse = OnDownloadDecideDestination;
    dc.didFinish = OnDownloadFinished;
    dc.didFailWithError = OnDownloadFailed;
    WKDownloadSetClient(download, &dc.base);
}

// A link opened in a new tab that turns out to be a file leaves an empty tab behind.
static void CloseTabIfEmptyPopup(const void* ci) {
    Tab* t = ResolveTab(ci, "download");
    if (t && t->openedByPage && (t->url.empty() || t->url == L"about:blank"))
        PostMessageW(g_hWnd, WM_APP_CLOSE_TAB, (WPARAM)t->id, 0);
}
static void OnActionBecameDownload(WKPageRef, WKNavigationActionRef, WKDownloadRef d, const void* ci) {
    AttachDownloadClient(d);
    CloseTabIfEmptyPopup(ci);
}
static void OnResponseBecameDownload(WKPageRef, WKNavigationResponseRef, WKDownloadRef d, const void* ci) {
    AttachDownloadClient(d);
    CloseTabIfEmptyPopup(ci);
}
static void OnContextMenuDownload(WKPageRef, WKDownloadRef d, const void*) {
    AttachDownloadClient(d);
}

// --- state client: title / URL / history buttons change -> repaint immediately ---
static void OnStateChanged(const void* ci) {
    Tab* t = ResolveTab(ci, "stateChanged");
    if (!t) return;
    if (SyncTab(*t)) InvalidateChrome();
}

// --- UI client ----------------------------------------------------------------
static Tab* CreateTabWithConfiguration(WKPageConfigurationRef conf, bool openedByPage);

// window.open() / target=_blank: WebKit hands us a configuration that already links
// the new page to its opener; we must create the view from it and return the page +1.
static WKPageRef OnCreateNewPage(WKPageRef, WKPageConfigurationRef conf, WKNavigationActionRef,
                                 WKWindowFeaturesRef, const void* ci) {
    if (!ResolveTab(ci, "createNewPage")) return nullptr;
    Tab* t = CreateTabWithConfiguration(conf, true);
    if (!t) return nullptr;
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

// Minimal modal text-input dialog built from an in-memory template (no .rc needed).
struct PromptState { std::wstring message, value; bool ok = false; };

static INT_PTR CALLBACK PromptDlgProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_INITDIALOG: {
        PromptState* st = (PromptState*)l;
        SetWindowLongPtrW(h, DWLP_USER, (LONG_PTR)st);
        SetDlgItemTextW(h, 100, st->message.c_str());
        SetDlgItemTextW(h, 101, st->value.c_str());
        SendDlgItemMessageW(h, 101, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(h, 101));
        return FALSE;   // focus was set manually
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL) {
            PromptState* st = (PromptState*)GetWindowLongPtrW(h, DWLP_USER);
            if (st && LOWORD(w) == IDOK) {
                int n = GetWindowTextLengthW(GetDlgItem(h, 101));
                std::wstring v(n + 1, L'\0');
                GetDlgItemTextW(h, 101, &v[0], n + 1);
                v.resize(n);
                st->value = v;
                st->ok = true;
            }
            EndDialog(h, LOWORD(w));
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static bool PromptDialog(HWND owner, const std::wstring& title, PromptState& st) {
    std::vector<WORD> t;
    auto align = [&]() { if (t.size() & 1) t.push_back(0); };   // DWORD alignment (2 WORDs)
    auto str = [&](const wchar_t* s) { do { t.push_back((WORD)*s); } while (*s++); };
    auto item = [&](DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls, const wchar_t* text) {
        align();
        DLGITEMTEMPLATE it = {};
        it.style = style | WS_CHILD | WS_VISIBLE;
        it.x = x; it.y = y; it.cx = cx; it.cy = cy; it.id = id;
        const WORD* p = (const WORD*)&it;
        t.insert(t.end(), p, p + sizeof(it) / sizeof(WORD));
        t.push_back(0xFFFF); t.push_back(cls);   // predefined class atom
        str(text);
        t.push_back(0);                          // no creation data
    };
    DLGTEMPLATE d = {};
    d.style = DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    d.cdit = 4; d.cx = 260; d.cy = 92;
    const WORD* p = (const WORD*)&d;
    t.insert(t.end(), p, p + sizeof(d) / sizeof(WORD));
    t.push_back(0); t.push_back(0);              // no menu, default class
    str(title.c_str());
    t.push_back(9); str(L"Segoe UI");            // DS_SETFONT: point size + face
    item(SS_LEFT | SS_NOPREFIX, 8, 7, 244, 34, 100, 0x0082, L"");
    item(ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 8, 44, 244, 14, 101, 0x0081, L"");
    item(BS_DEFPUSHBUTTON | WS_TABSTOP, 148, 70, 50, 14, IDOK, 0x0080, L"OK");
    item(BS_PUSHBUTTON | WS_TABSTOP, 202, 70, 50, 14, IDCANCEL, 0x0080, L"Cancel");
    DialogBoxIndirectParamW(GetModuleHandleW(nullptr), (LPCDLGTEMPLATEW)t.data(), owner, PromptDlgProc, (LPARAM)&st);
    return st.ok;
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

// --- tab lifetime -----------------------------------------------------------------
static void Navigate(WKPageRef page, const std::string& utf8Url) {
    if (!page || utf8Url.empty()) return;
    WKURLRef url = WKURLCreateWithUTF8CString(utf8Url.c_str());
    if (url) {
        WKPageLoadURL(page, url);
        WKRelease(url);
    }
}

static void NavigateActive(const std::wstring& input) {
    if (Tab* t = ActiveTab()) Navigate(t->page, ResolveInput(input));
}

static void SetPageClients(WKPageRef page, TabCtx* ctx) {
    WKPageNavigationClientV3 nc = {};
    nc.base.version = 3;
    nc.base.clientInfo = ctx;           // stable heap object, outlives every WebKit callback (see TabCtx)
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
    WKPageSetPageUIClient(page, &ui.base);

    WKPageStateClientV0 sc = {};
    sc.base.version = 0;
    sc.base.clientInfo = ctx;
    sc.didChangeTitle        = OnStateChanged;
    sc.didChangeActiveURL    = OnStateChanged;
    sc.didChangeCanGoBack    = OnStateChanged;
    sc.didChangeCanGoForward = OnStateChanged;
    WKPageSetPageStateClient(page, &sc.base);
}

static void ClearPageClients(WKPageRef page) {
    WKPageNavigationClientV0 nc = {};
    nc.base.version = 0;
    WKPageSetPageNavigationClient(page, &nc.base);
    WKPageUIClientV0 ui = {};
    ui.base.version = 0;
    WKPageSetPageUIClient(page, &ui.base);
    WKPageStateClientV0 sc = {};
    sc.base.version = 0;
    WKPageSetPageStateClient(page, &sc.base);
}

static WKPageConfigurationRef CreatePageConfiguration() {
    WKPageConfigurationRef conf = WKPageConfigurationCreate();
    WKPageConfigurationSetContext(conf, g_context);
    WKPageConfigurationSetPreferences(conf, g_preferences);
    if (g_dataStore) WKPageConfigurationSetWebsiteDataStore(conf, g_dataStore);
    return conf;
}

// Creates a view from `conf` (not consumed) and makes it the active tab.
static Tab* CreateTabWithConfiguration(WKPageConfigurationRef conf, bool openedByPage) {
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

static void CreateTab(const std::wstring& input) {
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

static void ReopenClosedTab() {
    if (g_closedUrls.empty()) return;
    std::wstring url = g_closedUrls.back();
    g_closedUrls.pop_back();
    CreateTab(url);
}

static void CloseTab(int i) {
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

static void CloseAllTabs() {
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
static void HandleTabCrashed(unsigned long long id, int reason) {
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

// Without an explicit data store WebKit keeps cookies, localStorage, IndexedDB and the
// HTTP cache in %LOCALAPPDATA%\Apple Computer\WebKit - shared with MiniBrowser and any
// other WebKit app. Give the browser its own profile folder instead.
static WKWebsiteDataStoreRef CreateWebsiteDataStore() {
    const std::wstring root = g_settingsDir + L"\\WebsiteData";
    CreateDirectoryW(g_settingsDir.c_str(), nullptr);
    CreateDirectoryW(root.c_str(), nullptr);

    WKWebsiteDataStoreConfigurationRef conf = WKWebsiteDataStoreConfigurationCreate();
    auto set = [&](void (*setter)(WKWebsiteDataStoreConfigurationRef, WKStringRef), const wchar_t* sub) {
        std::wstring path = root + L"\\" + sub;
        WKStringRef s = WKStringCreateWithUTF8CString(ToUtf8(path).c_str());
        setter(conf, s);
        WKRelease(s);
    };
    set(WKWebsiteDataStoreConfigurationSetCookieStorageFile,                L"Cookies.db");
    set(WKWebsiteDataStoreConfigurationSetNetworkCacheDirectory,            L"Cache");
    set(WKWebsiteDataStoreConfigurationSetLocalStorageDirectory,            L"LocalStorage");
    set(WKWebsiteDataStoreConfigurationSetIndexedDBDatabaseDirectory,       L"IndexedDB");
    set(WKWebsiteDataStoreConfigurationSetWebSQLDatabaseDirectory,          L"WebSQL");
    set(WKWebsiteDataStoreConfigurationSetCacheStorageDirectory,            L"CacheStorage");
    set(WKWebsiteDataStoreConfigurationSetGeneralStorageDirectory,          L"Storage");
    set(WKWebsiteDataStoreConfigurationSetServiceWorkerRegistrationDirectory, L"ServiceWorkers");
    set(WKWebsiteDataStoreConfigurationSetMediaKeysStorageDirectory,        L"MediaKeys");
    set(WKWebsiteDataStoreConfigurationSetResourceLoadStatisticsDirectory,  L"ResourceLoadStatistics");
    WKWebsiteDataStoreRef store = WKWebsiteDataStoreCreateWithConfiguration(conf);
    WKRelease(conf);
    Log("website data store: %ls (%p)", root.c_str(), (void*)store);
    return store;
}

static void ShowWebInspector() {
    Tab* t = ActiveTab();
    if (!t || !t->page) return;
    if (!g_prefs.devExtras) {
        MessageBoxW(g_hWnd, L"Developer tools are turned off. Enable them in Settings \x2192 Webpages.",
                    kAppName, MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (WKInspectorRef insp = WKPageGetInspector(t->page)) WKInspectorShow(insp);
}

static void SetZoom(double z) {
    z = std::round(z * 100.0) / 100.0;
    g_zoom = std::max(0.25, std::min(5.0, z));
    for (auto& t : g_tabs) WKPageSetPageZoomFactor(t->page, g_zoom);
    SaveSettings();
}

static void GoBack()    { if (Tab* t = ActiveTab()) if (WKPageCanGoBack(t->page))    WKPageGoBack(t->page); }
static void GoForward() { if (Tab* t = ActiveTab()) if (WKPageCanGoForward(t->page)) WKPageGoForward(t->page); }
static void ReloadActive(bool stopIfLoading) {
    Tab* t = ActiveTab();
    if (!t) return;
    if (stopIfLoading && t->loading) WKPageStopLoading(t->page);
    else WKPageReload(t->page);
}

static void CopyToClipboard(const std::wstring& s) {
    if (!OpenClipboard(g_hWnd)) return;
    EmptyClipboard();
    size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        memcpy(GlobalLock(h), s.c_str(), bytes);
        GlobalUnlock(h);
        if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
    }
    CloseClipboard();
}

static void RunCommand(int cmd) {
    switch (cmd) {
    case CMD_NEWTAB:    CreateTab(g_prefs.newTabMode == 1 ? std::wstring(L"about:blank") : g_homePage); break;
    case CMD_SETTINGS:  ShowSettingsWindow(); break;
    case CMD_CLOSETAB:  CloseTab(g_active); break;
    case CMD_COPYURL:   if (Tab* t = ActiveTab()) CopyToClipboard(t->url); break;
    case CMD_ZOOMIN:    SetZoom(g_zoom + 0.1); break;
    case CMD_ZOOMOUT:   SetZoom(g_zoom - 0.1); break;
    case CMD_ZOOMRESET: SetZoom(1.0); break;
    case CMD_SETHOME:   if (Tab* t = ActiveTab()) { g_homePage = t->url; SaveSettings(); } break;
    case CMD_REOPENTAB: ReopenClosedTab(); break;
    case CMD_DEVTOOLS:  ShowWebInspector(); break;
    }
}

static void ShowMoreMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CMD_NEWTAB,    L"New Tab\tCtrl+T");
    AppendMenuW(m, MF_STRING, CMD_CLOSETAB,  L"Close Tab\tCtrl+W");
    AppendMenuW(m, MF_STRING | (g_closedUrls.empty() ? MF_GRAYED : 0), CMD_REOPENTAB,
                L"Reopen Closed Tab\tCtrl+Shift+T");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_COPYURL,   L"Copy Address");
    AppendMenuW(m, MF_STRING, CMD_SETHOME,   L"Set Current Page as Home Page");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_ZOOMIN,    L"Zoom In\tCtrl++");
    AppendMenuW(m, MF_STRING, CMD_ZOOMOUT,   L"Zoom Out\tCtrl+-");
    AppendMenuW(m, MF_STRING, CMD_ZOOMRESET, L"Actual Size\tCtrl+0");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    if (g_prefs.devExtras) AppendMenuW(m, MF_STRING, CMD_DEVTOOLS, L"Developer Tools\tF12");
    AppendMenuW(m, MF_STRING, CMD_SETTINGS,  L"Settings...\tCtrl+,");
    POINT pt = { (LONG)g_L.more.GetRight(), (LONG)g_L.more.GetBottom() };
    ClientToScreen(g_hWnd, &pt);
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN | TPM_NONOTIFY,
                                  pt.x, pt.y, 0, g_hWnd, nullptr);
    DestroyMenu(m);
    if (cmd) RunCommand(cmd);
}

// ----------------------------------------------------------------------------
//  Keyboard shortcuts (handled before dispatch so they work while the web
//  view or the address field has focus)
// ----------------------------------------------------------------------------
static bool HandleShortcut(const MSG& m) {
    if (m.message != WM_KEYDOWN && m.message != WM_SYSKEYDOWN) return false;
    if (!g_hWnd || !(m.hwnd == g_hWnd || IsChild(g_hWnd, m.hwnd))) return false;

    const bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt   = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const WPARAM k = m.wParam;

    if (ctrl && shift && !alt) {
        if (k == 'T') { ReopenClosedTab(); return true; }
        if (k == 'I') { ShowWebInspector(); return true; }
    }
    if (ctrl && !alt) {
        switch (k) {
        case 'T': RunCommand(CMD_NEWTAB); return true;
        case 'W': RunCommand(CMD_CLOSETAB); return true;
        case 'L': ShowUrlEdit(); return true;
        case VK_OEM_COMMA: RunCommand(CMD_SETTINGS); return true;
        case 'R': ReloadActive(false); return true;
        case VK_OEM_PLUS: case VK_ADD:         RunCommand(CMD_ZOOMIN); return true;
        case VK_OEM_MINUS: case VK_SUBTRACT:   RunCommand(CMD_ZOOMOUT); return true;
        case '0': case VK_NUMPAD0:             RunCommand(CMD_ZOOMRESET); return true;
        case VK_TAB: {
            int n = (int)g_tabs.size();
            if (n > 1) SelectTab(((g_active + (shift ? -1 : 1)) % n + n) % n);
            return true;
        }
        }
        if (k >= '1' && k <= '9') {
            int n = (int)g_tabs.size();
            SelectTab(k == '9' ? n - 1 : (int)(k - '1'));
            return true;
        }
    }
    if (alt && !ctrl) {
        if (k == VK_LEFT)  { GoBack(); return true; }
        if (k == VK_RIGHT) { GoForward(); return true; }
    }
    if (k == VK_F5) { ReloadActive(false); return true; }
    if (k == VK_F12) { ShowWebInspector(); return true; }
    return false;
}

// ----------------------------------------------------------------------------
//  Address field subclass
// ----------------------------------------------------------------------------
static LRESULT CALLBACK UrlEditProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_KEYDOWN:
        if (w == VK_RETURN) {
            wchar_t buf[2048] = {};
            GetWindowTextW(h, buf, ARRAYSIZE(buf));
            NavigateActive(buf);
            FocusWebView();
            return 0;
        }
        if (w == VK_ESCAPE) { FocusWebView(); return 0; }
        if (w == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessageW(h, EM_SETSEL, 0, -1);
            return 0;
        }
        break;
    case WM_CHAR:
        if (w == VK_RETURN || w == VK_ESCAPE || w == 1) return 0;  // no beep
        break;
    }
    return CallWindowProcW(g_oldEditProc, h, msg, w, l);
}

// ----------------------------------------------------------------------------
//  Painting
// ----------------------------------------------------------------------------
static void AddRound(GraphicsPath& p, const RectF& r, float rad) {
    float d = rad * 2;
    if (d > r.Width) d = r.Width;
    if (d > r.Height) d = r.Height;
    p.AddArc(r.X, r.Y, d, d, 180.f, 90.f);
    p.AddArc(r.GetRight() - d, r.Y, d, d, 270.f, 90.f);
    p.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0.f, 90.f);
    p.AddArc(r.X, r.GetBottom() - d, d, d, 90.f, 90.f);
    p.CloseFigure();
}

static void FillRound(Graphics& g, const Color& c, const RectF& r, float rad) {
    GraphicsPath p;
    AddRound(p, r, rad);
    SolidBrush b(c);
    g.FillPath(&b, &p);
}

static void DrawGlyph(Graphics& g, const wchar_t* glyph, const RectF& r, const Color& c, Font* f) {
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsNoClip);
    SolidBrush b(c);
    g.DrawString(glyph, -1, f, r, &sf, &b);
}

static void DrawChrome(Graphics& g, int w) {
    const Theme& T = g_th;
    const float fw = (float)w;
    Tab* cur = ActiveTab();
    const bool editing = IsEditing();

    auto hov = [&](Hit::Kind k, int i) { return g_hover == MakeHit(k, i); };
    auto dwn = [&](Hit::Kind k, int i) { return g_down == MakeHit(k, i) && hov(k, i); };
    auto toolButton = [&](const RectF& r, const wchar_t* glyph, bool enabled, bool h, bool p, Font* f) {
        if (enabled && (h || p)) FillRound(g, p ? T.pressed : T.hover, r, S(6));
        DrawGlyph(g, glyph, r, enabled ? T.icon : T.iconDis, f);
    };

    // Backgrounds + hairlines
    {
        SolidBrush b1(T.toolbar), b2(T.tabbar);
        g.FillRectangle(&b1, 0.f, 0.f, fw, g_L.toolbarH);
        g.FillRectangle(&b2, 0.f, g_L.toolbarH, fw, g_L.tabBarH);
        Pen sep(T.sep, 1.f);
        g.DrawLine(&sep, 0.f, g_L.toolbarH - 0.5f, fw, g_L.toolbarH - 0.5f);
        g.DrawLine(&sep, 0.f, g_L.chromeH - 0.5f, fw, g_L.chromeH - 0.5f);
    }

    // Back / forward / more
    toolButton(g_L.back, L"\uE76B", cur && cur->canBack, hov(Hit::Back, -1), dwn(Hit::Back, -1), g_fIcon.get());
    toolButton(g_L.fwd,  L"\uE76C", cur && cur->canFwd,  hov(Hit::Forward, -1), dwn(Hit::Forward, -1), g_fIcon.get());
    toolButton(g_L.more, L"\uE712", true, hov(Hit::More, -1), dwn(Hit::More, -1), g_fIcon.get());

    // Address pill
    {
        const RectF& pill = g_L.pill;
        const float rad = S((float)metrics::kPillRadius);
        Color base = editing ? T.pillFocus : (hov(Hit::Pill, -1) || hov(Hit::Reload, -1) ? T.pillHover : T.pill);
        FillRound(g, base, pill, rad);

        // Safari-style loading fill
        if (!editing && cur && cur->fade > 0.01f && cur->progress > 0.f) {
            GraphicsPath path;
            AddRound(path, pill, rad);
            g.SetClip(&path);
            Color a((BYTE)(T.progressAlpha * cur->fade), T.accent.GetR(), T.accent.GetG(), T.accent.GetB());
            SolidBrush b(a);
            g.FillRectangle(&b, pill.X, pill.Y, pill.Width * std::min(1.f, cur->progress), pill.Height);
            g.ResetClip();
        }

        if (editing) {
            RectF ring = pill;
            ring.Inflate(S(1.5f), S(1.5f));
            GraphicsPath p;
            AddRound(p, ring, rad + S(1.5f));
            Pen pen(Color(120, T.accent.GetR(), T.accent.GetG(), T.accent.GetB()), S(3));
            g.DrawPath(&pen, &p);
        } else {
            std::wstring label = cur ? (g_prefs.showFullUrl ? DisplayUrl(cur->url) : HostOf(cur->url)) : L"";
            const bool placeholder = label.empty();
            if (placeholder) label = L"Search or enter website name";
            const bool secure = cur && cur->url.compare(0, 8, L"https://") == 0;

            const float sideInset = S(34);
            const float maxText = pill.Width - sideInset * 2;
            const float lockW = secure ? S(14) : 0.f, lockGap = secure ? S(4) : 0.f;

            StringFormat fmt(StringFormat::GenericTypographic());
            fmt.SetFormatFlags(fmt.GetFormatFlags() | StringFormatFlagsNoWrap);
            fmt.SetTrimming(StringTrimmingEllipsisCharacter);
            fmt.SetLineAlignment(StringAlignmentCenter);

            RectF m;
            g.MeasureString(label.c_str(), -1, g_fText.get(), RectF(0, 0, 10000, 1000), &fmt, &m);
            float tw = std::min(m.Width, maxText - lockW - lockGap);
            float total = lockW + lockGap + tw;
            float x0 = pill.X + pill.Width / 2 - total / 2;

            if (secure)
                DrawGlyph(g, L"\uE72E", RectF(x0, pill.Y, lockW, pill.Height), T.text2, g_fIconSm.get());
            SolidBrush tb(placeholder ? T.text2 : T.text);
            g.DrawString(label.c_str(), -1, g_fText.get(),
                         RectF(x0 + lockW + lockGap, pill.Y, tw + 1.f, pill.Height), &fmt, &tb);

            // Reload / stop
            bool loading = cur && cur->loading;
            toolButton(g_L.reload, loading ? L"\uE711" : L"\uE72C", cur != nullptr,
                       hov(Hit::Reload, -1), dwn(Hit::Reload, -1), g_fIconSm.get());
        }
    }

    // Tabs
    {
        StringFormat tf;
        tf.SetAlignment(StringAlignmentCenter);
        tf.SetLineAlignment(StringAlignmentCenter);
        tf.SetTrimming(StringTrimmingEllipsisCharacter);
        tf.SetFormatFlags(StringFormatFlagsNoWrap);

        const int n = (int)std::min(g_L.tabs.size(), g_tabs.size());   // layout may lag behind g_tabs by one frame
        for (int i = 0; i < n; ++i) {
            const RectF& r = g_L.tabs[i];
            const bool act = (i == g_active);
            const bool h = hov(Hit::TabBody, i) || hov(Hit::TabClose, i);
            const float rad = S((float)metrics::kTabRadius);

            if (act) {
                FillRound(g, T.tabActive, r, rad);
                GraphicsPath p;
                AddRound(p, r, rad);
                Pen pen(T.tabBorder, 1.f);
                g.DrawPath(&pen, &p);
            } else if (h) {
                FillRound(g, T.tabHover, r, rad);
            } else if (i + 1 < n && (i + 1) != g_active &&
                       !hov(Hit::TabBody, i + 1) && !hov(Hit::TabClose, i + 1)) {
                Pen sp(T.sep, 1.f);
                float x = r.GetRight() + S(1);
                g.DrawLine(&sp, x, r.Y + S(7), x, r.GetBottom() - S(7));
            }

            const bool showClose = CloseVisible(i);
            if (showClose) {
                RectF c = CloseRect(i);
                if (hov(Hit::TabClose, i))
                    FillRound(g, dwn(Hit::TabClose, i) ? T.pressed : T.hover, c, c.Width / 2);
                DrawGlyph(g, L"\uE711", c, T.text2, g_fIconSm.get());
            }

            if (r.Width >= S(56)) {
                float inset = (r.Width >= S(70)) ? S(28) : S(6);
                RectF tr(r.X + inset, r.Y, r.Width - inset * 2, r.Height);
                SolidBrush tb(act ? T.text : T.text2);
                g.DrawString(g_tabs[i]->title.c_str(), -1, g_fTab.get(), tr, &tf, &tb);
            }
        }

        // New tab button
        const float bw = S(28), bh = S(24);
        RectF nt(g_L.newTab.X + (g_L.newTab.Width - bw) / 2, g_L.newTab.Y + (g_L.newTab.Height - bh) / 2, bw, bh);
        toolButton(nt, L"\uE710", (int)g_tabs.size() < metrics::kMaxTabs,
                   hov(Hit::NewTab, -1), dwn(Hit::NewTab, -1), g_fIconSm.get());
    }
}

static void PaintChrome(HDC hdc, int w) {
    int h = (int)std::ceil(g_L.chromeH);
    if (w <= 0 || h <= 0) return;
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    {
        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeHighQuality);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        DrawChrome(g, w);
    }
    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// ============================================================================
//  Web preferences + Settings window
//  (Vivaldi-style: category sidebar, searchable, scrollable pages with
//   toggles / dropdowns / sliders / text fields; everything is persisted)
// ============================================================================
static void ApplyWebPrefs() {
    if (!g_preferences) return;
    WKPreferencesSetJavaScriptEnabled(g_preferences, g_prefs.js);
    WKPreferencesSetLoadsImagesAutomatically(g_preferences, g_prefs.images);
    WKPreferencesSetLocalStorageEnabled(g_preferences, g_prefs.localStorage);
    WKPreferencesSetDatabasesEnabled(g_preferences, g_prefs.databases);
    WKPreferencesSetDeveloperExtrasEnabled(g_preferences, g_prefs.devExtras);
    WKPreferencesSetFullScreenEnabled(g_preferences, g_prefs.fullscreen);
    WKPreferencesSetWebAudioEnabled(g_preferences, g_prefs.webAudio);
    WKPreferencesSetDefaultFontSize(g_preferences, (uint32_t)g_prefs.defaultFont);
    WKPreferencesSetMinimumFontSize(g_preferences, (uint32_t)g_prefs.minFont);

    // Canvas 2D follows the page tiles: both on the GPU (Skia on ANGLE in the GPU process), or both
    // on the CPU with LYRA_DISABLE_GPU_RASTER=1. A GPU canvas under CPU-painted tiles would be
    // read back from the GPU for every tile it covers, every frame.
    wchar_t noGpuRaster[4] = {};
    bool cpuRaster = GetEnvironmentVariableW(L"MYBROWSER_DISABLE_GPU_RASTER", noGpuRaster, 4) && noGpuRaster[0] == L'1';
    WKPreferencesSetCanvasUsesAcceleratedDrawing(g_preferences, !cpuRaster);
}

static COLORREF SFieldColor() { return g_dark ? RGB(30, 30, 32) : RGB(236, 236, 238); }
static Color WithAlpha(const Color& c, int a) { return Color((BYTE)a, c.GetR(), c.GetG(), c.GetB()); }
static Color SOffColor() { return g_dark ? Color(99, 99, 104) : Color(196, 196, 201); }

// ---- model -----------------------------------------------------------------
struct SRow {
    enum Type { Header, Toggle, Choice, Slider, Text, Button, Info };
    Type type = Info;
    std::wstring label, desc, right;
    bool kbd = false;
    std::vector<std::wstring> options;
    int minV = 0, maxV = 100, step = 1;
    std::function<int()> getInt;
    std::function<void(int)> setInt;
    std::function<std::wstring()> getStr;
    std::function<void(const std::wstring&)> setStr;
    std::function<std::wstring(int)> fmt;
    std::function<void()> action;
};
struct SPage { std::wstring name; const wchar_t* icon = L""; std::vector<SRow> rows; };
struct SView {
    SRow* row = nullptr;          // null => synthetic heading (search results)
    std::wstring heading;
    float y = 0, h = 0;
    int group = -1;
    bool first = false, last = false;
};

static SRow RHeader(const wchar_t* t) { SRow r; r.type = SRow::Header; r.label = t; return r; }
static SRow RToggle(const wchar_t* label, const wchar_t* desc, bool* v, std::function<void()> changed) {
    SRow r; r.type = SRow::Toggle; r.label = label; r.desc = desc;
    r.getInt = [v]() { return *v ? 1 : 0; };
    r.setInt = [v, changed](int x) { *v = (x != 0); if (changed) changed(); };
    return r;
}
static SRow RChoice(const wchar_t* label, const wchar_t* desc, std::vector<std::wstring> opts,
                    std::function<int()> get, std::function<void(int)> set) {
    SRow r; r.type = SRow::Choice; r.label = label; r.desc = desc;
    r.options = std::move(opts); r.getInt = std::move(get); r.setInt = std::move(set);
    return r;
}
static SRow RSlider(const wchar_t* label, const wchar_t* desc, int mn, int mx, int step,
                    std::function<int()> get, std::function<void(int)> set,
                    std::function<std::wstring(int)> fmt) {
    SRow r; r.type = SRow::Slider; r.label = label; r.desc = desc;
    r.minV = mn; r.maxV = mx; r.step = step;
    r.getInt = std::move(get); r.setInt = std::move(set); r.fmt = std::move(fmt);
    return r;
}
static SRow RText(const wchar_t* label, const wchar_t* desc, std::function<std::wstring()> get,
                  std::function<void(const std::wstring&)> set) {
    SRow r; r.type = SRow::Text; r.label = label; r.desc = desc;
    r.getStr = std::move(get); r.setStr = std::move(set);
    return r;
}
static SRow RButton(const wchar_t* label, const wchar_t* desc, const wchar_t* caption,
                    std::function<void()> action) {
    SRow r; r.type = SRow::Button; r.label = label; r.desc = desc; r.right = caption;
    r.action = std::move(action);
    return r;
}
static SRow RInfo(const wchar_t* label, const std::wstring& right, bool kbd = false) {
    SRow r; r.type = SRow::Info; r.label = label; r.right = right; r.kbd = kbd;
    return r;
}

// ---- state -----------------------------------------------------------------
constexpr int SET_ID_SEARCH = 2001;
constexpr int SET_ID_EDIT   = 2002;
static const wchar_t kSetClass[] = L"LyraSettingsWindow";

static HWND   g_setSearch = nullptr, g_setEdit = nullptr;
static HBRUSH g_fieldBrush = nullptr;
static HFONT  g_sEditFont = nullptr;
static int    g_sEditH = 18;
static UINT   g_sDpi = 96;
static WNDPROC g_oldSEditProc = nullptr;
static std::unique_ptr<Font> g_sfBody, g_sfSmall, g_sfTitle, g_sfHead, g_sfIcon, g_sfIconSm;
static std::vector<SPage> g_pages;
static std::vector<SView> g_view;
static std::vector<std::pair<float, float>> g_groups;
static int   g_sPage = 0;
static float g_sScroll = 0.f, g_sContentH = 0.f;
static int   g_sHoverSide = -1, g_sHoverRow = -1, g_sDragRow = -1;
static bool  g_sHoverCtl = false, g_sHoverPill = false, g_sTracking = false;
static SRow* g_sEditRow = nullptr;

static float SS(float v) { return v * (float)g_sDpi / 96.f; }

struct SGeo { float W, H, sideW, headerH, vpY, vpH, colX, colW; };
static SGeo Geo() {
    RECT cr = {};
    GetClientRect(g_setHwnd, &cr);
    SGeo g;
    g.W = (float)cr.right; g.H = (float)cr.bottom;
    g.sideW = SS(232); g.headerH = SS(72);
    g.vpY = g.headerH; g.vpH = std::max(0.f, g.H - g.headerH);
    float vpW = std::max(0.f, g.W - g.sideW);
    g.colW = std::min(SS(780), std::max(0.f, vpW - SS(64)));
    g.colX = g.sideW + (vpW - g.colW) / 2;
    return g;
}
static RectF SSideRect(const SGeo& G, int i) {
    return RectF(SS(12), SS(80) + SS(42) * (float)i, G.sideW - SS(24), SS(38));
}
static RectF SSearchRect(const SGeo& G) {
    return RectF(G.colX + G.colW - SS(280), (G.headerH - SS(34)) / 2, SS(280), SS(34));
}
static RectF SRowRect(const SGeo& G, const SView& v) {
    return RectF(G.colX, G.vpY + v.y - g_sScroll, G.colW, v.h);
}
static RectF SCtlRect(const SRow& r, const RectF& row) {
    const float right = row.GetRight() - SS(20);
    float w = 0, h = SS(32);
    switch (r.type) {
    case SRow::Toggle: w = SS(44); h = SS(24); break;
    case SRow::Choice: w = std::min(SS(190), row.Width * 0.4f); break;
    case SRow::Slider: w = std::min(SS(200), row.Width * 0.38f); h = SS(24); break;
    case SRow::Text:   w = std::min(SS(320), row.Width * 0.5f); break;
    case SRow::Button: w = SS(130); break;
    default: return RectF(0, 0, 0, 0);
    }
    return RectF(right - w, row.Y + (row.Height - h) / 2, w, h);
}

static void SRebuildFonts() {
    const wchar_t* icoFam = L"Segoe MDL2 Assets";
    { FontFamily ff(L"Segoe Fluent Icons"); if (ff.IsAvailable()) icoFam = L"Segoe Fluent Icons"; }
    auto mk = [](const wchar_t* fam, float px, int style) {
        auto f = std::make_unique<Font>(fam, px, style, UnitPixel);
        if (!f->IsAvailable()) f = std::make_unique<Font>(L"Segoe UI", px, style, UnitPixel);
        return f;
    };
    g_sfBody   = mk(L"Segoe UI", SS(14), FontStyleRegular);
    g_sfSmall  = mk(L"Segoe UI", SS(12.5f), FontStyleRegular);
    g_sfTitle  = mk(L"Segoe UI", SS(22), FontStyleBold);
    g_sfHead   = mk(L"Segoe UI", SS(15), FontStyleBold);
    g_sfIcon   = mk(icoFam, SS(16), FontStyleRegular);
    g_sfIconSm = mk(icoFam, SS(12), FontStyleRegular);

    HFONT old = g_sEditFont;
    g_sEditFont = CreateFontW(-(int)std::round(SS(14)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (g_setEdit) {
        HDC dc = GetDC(g_setEdit);
        HGDIOBJ prev = SelectObject(dc, g_sEditFont);
        TEXTMETRICW tm = {};
        GetTextMetricsW(dc, &tm);
        g_sEditH = tm.tmHeight;
        SelectObject(dc, prev);
        ReleaseDC(g_setEdit, dc);
        SendMessageW(g_setEdit, WM_SETFONT, (WPARAM)g_sEditFont, TRUE);
        if (g_setSearch) SendMessageW(g_setSearch, WM_SETFONT, (WPARAM)g_sEditFont, TRUE);
    }
    if (old) DeleteObject(old);
}

// ---- view building ---------------------------------------------------------
static std::wstring SLower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}
static std::wstring SSearchText() {
    if (!g_setSearch) return std::wstring();
    wchar_t buf[256] = {};
    GetWindowTextW(g_setSearch, buf, 256);
    return TrimWs(buf);
}
static void SClampScroll() {
    SGeo G = Geo();
    float maxS = std::max(0.f, g_sContentH - G.vpH);
    g_sScroll = std::max(0.f, std::min(g_sScroll, maxS));
}
static void SScrollBy(float d) {
    g_sScroll += d;
    SClampScroll();
    InvalidateRect(g_setHwnd, nullptr, FALSE);
}

static void RebuildView() {
    g_view.clear();
    g_groups.clear();
    const std::wstring q = SLower(SSearchText());
    float y = SS(8);
    auto pushHeader = [&](const std::wstring& t) {
        SView v; v.heading = t; v.y = y; v.h = SS(46); y += v.h; g_view.push_back(v);
    };
    auto pushRow = [&](SRow* r) {
        SView v; v.row = r; v.y = y;
        v.h = (r->type == SRow::Info) ? SS(46) : (r->desc.empty() ? SS(52) : SS(68));
        y += v.h; g_view.push_back(v);
    };
    if (q.empty()) {
        if (g_sPage >= 0 && g_sPage < (int)g_pages.size())
            for (auto& r : g_pages[g_sPage].rows) {
                if (r.type == SRow::Header) pushHeader(r.label); else pushRow(&r);
            }
    } else {
        for (auto& p : g_pages) {
            bool any = false;
            for (auto& r : p.rows) {
                if (r.type == SRow::Header) continue;
                if (SLower(r.label + L" " + r.desc + L" " + r.right).find(q) == std::wstring::npos) continue;
                if (!any) { pushHeader(p.name); any = true; }
                pushRow(&r);
            }
        }
    }
    int cur = -1;
    for (size_t i = 0; i < g_view.size(); ++i) {
        SView& v = g_view[i];
        if (!v.row || v.row->type == SRow::Header) { cur = -1; continue; }
        if (cur < 0) { cur = (int)g_groups.size(); g_groups.emplace_back(v.y, v.y + v.h); v.first = true; }
        else g_groups[cur].second = v.y + v.h;
        v.group = cur;
    }
    for (size_t i = 0; i < g_view.size(); ++i)
        if (g_view[i].group >= 0 && (i + 1 >= g_view.size() || g_view[i + 1].group != g_view[i].group))
            g_view[i].last = true;

    g_sContentH = y + SS(32);
    SClampScroll();
    if (g_setHwnd) InvalidateRect(g_setHwnd, nullptr, FALSE);
}

// ---- inline editing / search box -------------------------------------------
static void SPositionSearch() {
    if (!g_setSearch) return;
    SGeo G = Geo();
    RectF sp = SSearchRect(G);
    MoveWindow(g_setSearch, (int)(sp.X + SS(34)), (int)(sp.Y + (sp.Height - (float)g_sEditH) / 2),
               (int)std::max(10.f, sp.Width - SS(34) - SS(14)), g_sEditH, TRUE);
}
static void ShowSearch() {
    SPositionSearch();
    ShowWindow(g_setSearch, SW_SHOW);
    SetFocus(g_setSearch);
    InvalidateRect(g_setHwnd, nullptr, FALSE);
}
static void SBeginTextEdit(SRow* r, const RectF& ctl) {
    g_sEditRow = r;
    SetWindowTextW(g_setEdit, r->getStr().c_str());
    MoveWindow(g_setEdit, (int)(ctl.X + SS(12)), (int)(ctl.Y + (ctl.Height - (float)g_sEditH) / 2),
               (int)std::max(10.f, ctl.Width - SS(24)), g_sEditH, TRUE);
    ShowWindow(g_setEdit, SW_SHOW);
    SetFocus(g_setEdit);
    SendMessageW(g_setEdit, EM_SETSEL, 0, -1);
    InvalidateRect(g_setHwnd, nullptr, FALSE);
}
static void SCommitTextEdit() {
    if (!g_sEditRow) return;
    SRow* r = g_sEditRow;
    g_sEditRow = nullptr;
    wchar_t buf[1024] = {};
    GetWindowTextW(g_setEdit, buf, ARRAYSIZE(buf));
    ShowWindow(g_setEdit, SW_HIDE);
    if (r->setStr) r->setStr(buf);
    InvalidateRect(g_setHwnd, nullptr, FALSE);
}
static void SSelectPage(int i) {
    if (i < 0 || i >= (int)g_pages.size()) return;
    g_sPage = i;
    g_sScroll = 0.f;
    SetWindowTextW(g_setSearch, L"");
    ShowWindow(g_setSearch, SW_HIDE);
    RebuildView();
}

static LRESULT CALLBACK SEditProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN) {
        if (h == g_setEdit) {
            if (w == VK_RETURN) { SCommitTextEdit(); SetFocus(g_setHwnd); return 0; }
            if (w == VK_ESCAPE) {
                g_sEditRow = nullptr;
                ShowWindow(g_setEdit, SW_HIDE);
                SetFocus(g_setHwnd);
                InvalidateRect(g_setHwnd, nullptr, FALSE);
                return 0;
            }
        } else if (h == g_setSearch) {
            if (w == VK_ESCAPE) { SetWindowTextW(h, L""); SetFocus(g_setHwnd); return 0; }
            if (w == VK_RETURN) return 0;
        }
        if (w == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) { SendMessageW(h, EM_SETSEL, 0, -1); return 0; }
    }
    if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE || w == 1)) return 0;
    return CallWindowProcW(g_oldSEditProc, h, m, w, l);
}

// ---- drawing ---------------------------------------------------------------
static void SText(Graphics& g, const std::wstring& s, Font* f, const RectF& r, const Color& c,
                  StringAlignment h = StringAlignmentNear, StringAlignment v = StringAlignmentCenter) {
    StringFormat sf;
    sf.SetAlignment(h);
    sf.SetLineAlignment(v);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush b(c);
    g.DrawString(s.c_str(), -1, f, r, &sf, &b);
}

static void DrawSettings(Graphics& g) {
    const Theme& T = g_th;
    const SGeo G = Geo();
    const COLORREF fc = SFieldColor();
    const Color field(GetRValue(fc), GetGValue(fc), GetBValue(fc));
    const std::wstring query = SSearchText();
    const bool searching = !query.empty();
    const Color sepSoft(g_dark ? 70 : 28, 0, 0, 0);

    { SolidBrush b(T.toolbar); g.FillRectangle(&b, 0.f, 0.f, G.W, G.H); }

    // Sidebar
    {
        SolidBrush b(T.tabbar);
        g.FillRectangle(&b, 0.f, 0.f, G.sideW, G.H);
        Pen sep(T.sep, 1.f);
        g.DrawLine(&sep, G.sideW - 0.5f, 0.f, G.sideW - 0.5f, G.H);
        SText(g, L"Settings", g_sfTitle.get(), RectF(SS(24), SS(18), G.sideW - SS(48), SS(44)), T.text);
        for (int i = 0; i < (int)g_pages.size(); ++i) {
            RectF r = SSideRect(G, i);
            bool sel = (i == g_sPage) && !searching;
            if (sel) FillRound(g, WithAlpha(T.accent, 46), r, SS(8));
            else if (i == g_sHoverSide) FillRound(g, T.hover, r, SS(8));
            DrawGlyph(g, g_pages[i].icon, RectF(r.X + SS(6), r.Y, SS(32), r.Height),
                      sel ? T.accent : T.icon, g_sfIcon.get());
            SText(g, g_pages[i].name, g_sfBody.get(), RectF(r.X + SS(44), r.Y, r.Width - SS(52), r.Height), T.text);
        }
    }

    // Header (page title + search)
    {
        std::wstring title = searching ? std::wstring(L"Search results")
                                       : (g_sPage < (int)g_pages.size() ? g_pages[g_sPage].name : std::wstring());
        SText(g, title, g_sfTitle.get(), RectF(G.colX, 0, std::max(SS(80), G.colW - SS(300)), G.headerH), T.text);
        RectF sp = SSearchRect(G);
        FillRound(g, field, sp, sp.Height / 2);
        if (g_setSearch && GetFocus() == g_setSearch) {
            RectF ring = sp;
            ring.Inflate(SS(1), SS(1));
            GraphicsPath p;
            AddRound(p, ring, ring.Height / 2);
            Pen pen(WithAlpha(T.accent, 140), SS(2));
            g.DrawPath(&pen, &p);
        }
        DrawGlyph(g, L"\uE721", RectF(sp.X + SS(6), sp.Y, SS(28), sp.Height), T.text2, g_sfIconSm.get());
        if (!(g_setSearch && IsWindowVisible(g_setSearch)))
            SText(g, L"Search settings", g_sfSmall.get(),
                  RectF(sp.X + SS(34), sp.Y, sp.Width - SS(44), sp.Height), T.text2);
        Pen sep(T.sep, 1.f);
        g.DrawLine(&sep, G.sideW, G.vpY - 0.5f, G.W, G.vpY - 0.5f);
    }

    // Content
    g.SetClip(RectF(G.sideW, G.vpY, std::max(0.f, G.W - G.sideW), G.vpH));

    for (auto& gr : g_groups) {
        RectF c(G.colX, G.vpY + gr.first - g_sScroll, G.colW, gr.second - gr.first);
        if (c.GetBottom() < G.vpY || c.Y > G.H) continue;
        FillRound(g, T.tabActive, c, SS(10));
        GraphicsPath p;
        AddRound(p, c, SS(10));
        Pen pen(T.tabBorder, 1.f);
        g.DrawPath(&pen, &p);
    }

    for (int i = 0; i < (int)g_view.size(); ++i) {
        const SView& v = g_view[i];
        RectF r = SRowRect(G, v);
        if (r.GetBottom() < G.vpY || r.Y > G.H) continue;

        if (!v.row || v.row->type == SRow::Header) {
            SText(g, v.row ? v.row->label : v.heading, g_sfHead.get(),
                  RectF(r.X + SS(4), r.Y + SS(14), r.Width, r.Height - SS(14)), T.text);
            continue;
        }
        SRow& row = *v.row;
        if (v.group >= 0 && !v.first) {
            Pen sp(sepSoft, 1.f);
            g.DrawLine(&sp, r.X + SS(20), r.Y, r.GetRight() - SS(20), r.Y);
        }

        const RectF ctl = SCtlRect(row, r);
        const bool hv = (g_sHoverRow == i && g_sHoverCtl);
        const float lx = r.X + SS(20);
        float labelRight;
        if (row.type == SRow::Info) labelRight = r.X + r.Width * 0.38f;
        else if (row.type == SRow::Slider) labelRight = ctl.X - SS(98);
        else labelRight = ctl.X - SS(16);
        const float lw = std::max(SS(40), labelRight - lx);

        if (row.desc.empty()) {
            SText(g, row.label, g_sfBody.get(), RectF(lx, r.Y, lw, r.Height), T.text);
        } else {
            SText(g, row.label, g_sfBody.get(), RectF(lx, r.Y + SS(11), lw, SS(24)), T.text);
            SText(g, row.desc, g_sfSmall.get(), RectF(lx, r.Y + SS(35), lw, SS(22)), T.text2);
        }

        switch (row.type) {
        case SRow::Toggle: {
            bool on = row.getInt() != 0;
            FillRound(g, on ? T.accent : SOffColor(), ctl, ctl.Height / 2);
            float kd = ctl.Height - SS(6);
            float kx = on ? ctl.GetRight() - SS(3) - kd : ctl.X + SS(3);
            SolidBrush kb(Color(255, 255, 255, 255));
            g.FillEllipse(&kb, kx, ctl.Y + SS(3), kd, kd);
            break;
        }
        case SRow::Choice: {
            int cur = row.getInt();
            std::wstring val = (cur >= 0 && cur < (int)row.options.size()) ? row.options[cur] : std::wstring();
            FillRound(g, field, ctl, SS(8));
            if (hv) {
                GraphicsPath p; AddRound(p, ctl, SS(8));
                Pen pen(WithAlpha(T.accent, 160), 1.f); g.DrawPath(&pen, &p);
            }
            SText(g, val, g_sfBody.get(), RectF(ctl.X + SS(12), ctl.Y, ctl.Width - SS(40), ctl.Height), T.text);
            DrawGlyph(g, L"\uE70D", RectF(ctl.GetRight() - SS(30), ctl.Y, SS(24), ctl.Height), T.text2, g_sfIconSm.get());
            break;
        }
        case SRow::Slider: {
            int val = std::max(row.minV, std::min(row.maxV, row.getInt()));
            float t = (row.maxV > row.minV) ? (float)(val - row.minV) / (float)(row.maxV - row.minV) : 0.f;
            float cy = ctl.Y + ctl.Height / 2;
            FillRound(g, SOffColor(), RectF(ctl.X, cy - SS(2), ctl.Width, SS(4)), SS(2));
            if (t > 0.f) FillRound(g, T.accent, RectF(ctl.X, cy - SS(2), ctl.Width * t, SS(4)), SS(2));
            float kd = SS(16);
            float kx = ctl.X + ctl.Width * t - kd / 2;
            SolidBrush kb(Color(255, 255, 255, 255));
            g.FillEllipse(&kb, kx, cy - kd / 2, kd, kd);
            Pen kp(Color(70, 0, 0, 0), 1.f);
            g.DrawEllipse(&kp, kx, cy - kd / 2, kd, kd);
            std::wstring txt = row.fmt ? row.fmt(val) : std::to_wstring(val);
            SText(g, txt, g_sfBody.get(), RectF(ctl.X - SS(90), r.Y, SS(80), r.Height), T.text2, StringAlignmentFar);
            break;
        }
        case SRow::Text: {
            const bool editing = (g_sEditRow == &row);
            FillRound(g, field, ctl, SS(8));
            if (editing || hv) {
                GraphicsPath p; AddRound(p, ctl, SS(8));
                Pen pen(WithAlpha(T.accent, editing ? 220 : 160), editing ? 2.f : 1.f);
                g.DrawPath(&pen, &p);
            }
            if (!editing)
                SText(g, row.getStr(), g_sfBody.get(), RectF(ctl.X + SS(12), ctl.Y, ctl.Width - SS(24), ctl.Height), T.text);
            break;
        }
        case SRow::Button: {
            if (hv) FillRound(g, WithAlpha(T.accent, 30), ctl, SS(8));
            GraphicsPath p; AddRound(p, ctl, SS(8));
            Pen pen(WithAlpha(T.accent, 200), 1.f);
            g.DrawPath(&pen, &p);
            SText(g, row.right, g_sfBody.get(), ctl, T.accent, StringAlignmentCenter);
            break;
        }
        case SRow::Info: {
            if (row.kbd) {
                StringFormat fmt(StringFormat::GenericTypographic());
                RectF m;
                g.MeasureString(row.right.c_str(), -1, g_sfSmall.get(), RectF(0, 0, 10000, 1000), &fmt, &m);
                float w = m.Width + SS(22), h = SS(28);
                RectF pill(r.GetRight() - SS(20) - w, r.Y + (r.Height - h) / 2, w, h);
                FillRound(g, field, pill, SS(7));
                SText(g, row.right, g_sfSmall.get(), pill, T.text, StringAlignmentCenter);
            } else {
                SText(g, row.right, g_sfBody.get(),
                      RectF(r.X + r.Width * 0.38f, r.Y, r.Width * 0.62f - SS(20), r.Height), T.text2, StringAlignmentFar);
            }
            break;
        }
        default: break;
        }
    }

    if (g_view.empty())
        SText(g, L"No settings found", g_sfBody.get(), RectF(G.sideW, G.vpY + SS(40), G.W - G.sideW, SS(40)),
              T.text2, StringAlignmentCenter);

    // Scrollbar
    float maxS = g_sContentH - G.vpH;
    if (maxS > 1.f) {
        float th = std::max(SS(36), G.vpH * G.vpH / g_sContentH);
        float ty = G.vpY + (G.vpH - th) * (g_sScroll / maxS);
        FillRound(g, WithAlpha(T.text2, 110), RectF(G.W - SS(10), ty, SS(5), th), SS(2.5f));
    }
    g.ResetClip();
}

// ---- pages -----------------------------------------------------------------
static void ResetAllSettings() {
    g_prefs = Prefs();
    g_homePage = L"https://example.com";
    g_searchEngine = L"https://duckduckgo.com/?q=";
    ApplyWebPrefs();
    SetZoom(1.0);                         // also saves
    ApplyTheme(ResolveDark());
    RefreshLayout();
    SaveSettings();
    if (g_setHwnd) InvalidateRect(g_setHwnd, nullptr, FALSE);
}

struct EngineDef { const wchar_t* name; const wchar_t* url; };
static const EngineDef kEngines[] = {
    { L"DuckDuckGo", L"https://duckduckgo.com/?q=" },
    { L"Google",     L"https://www.google.com/search?q=" },
    { L"Bing",       L"https://www.bing.com/search?q=" },
    { L"Brave",      L"https://search.brave.com/search?q=" },
    { L"Startpage",  L"https://www.startpage.com/do/search?q=" },
    { L"Ecosia",     L"https://www.ecosia.org/search?q=" },
    { L"Yandex",     L"https://yandex.com/search/?text=" },
};
constexpr int kEngineCount = (int)(sizeof(kEngines) / sizeof(kEngines[0]));

static void BuildPages() {
    g_pages.clear();
    const std::function<void()> save    = []() { SaveSettings(); };
    const std::function<void()> web     = []() { ApplyWebPrefs(); SaveSettings(); };
    const std::function<void()> theme   = []() { ApplyTheme(ResolveDark()); SaveSettings(); };
    const std::function<void()> layout  = []() { RefreshLayout(); SaveSettings(); };
    const std::function<void()> repaint = []() { InvalidateChrome(); SaveSettings(); };

    {   // General
        SPage p; p.name = L"General"; p.icon = L"\uE713";
        p.rows.push_back(RHeader(L"Startup"));
        p.rows.push_back(RText(L"Home page", L"Opened when the browser starts.",
            []() { return g_homePage; },
            [](const std::wstring& s) { std::wstring t = TrimWs(s); if (!t.empty()) { g_homePage = t; SaveSettings(); } }));
        p.rows.push_back(RButton(L"Use current page", L"Make the page in the active tab your home page.", L"Set",
            []() { if (Tab* t = ActiveTab()) if (!t->url.empty() && t->url != L"about:blank") { g_homePage = t->url; SaveSettings(); } }));
        p.rows.push_back(RHeader(L"Page zoom"));
        p.rows.push_back(RSlider(L"Default zoom", L"Applied to every page. Shortcuts: Ctrl +, Ctrl -, Ctrl 0.", 25, 300, 5,
            []() { return (int)std::lround(g_zoom * 100.0); },
            [](int v) { SetZoom(v / 100.0); },
            [](int v) { return std::to_wstring(v) + L"%"; }));
        g_pages.push_back(std::move(p));
    }
    {   // Appearance
        SPage p; p.name = L"Appearance"; p.icon = L"\uE790";
        p.rows.push_back(RHeader(L"Theme"));
        p.rows.push_back(RChoice(L"Color scheme", L"Follow Windows or force a light or dark browser.",
            { L"Use system setting", L"Light", L"Dark" },
            []() { return g_prefs.themeMode; },
            [theme](int i) { g_prefs.themeMode = i; theme(); }));
        std::vector<std::wstring> accents;
        for (int i = 0; i < kAccentCount; ++i) accents.push_back(kAccents[i].name);
        p.rows.push_back(RChoice(L"Accent color", L"Used for focus rings, progress and toggles.", accents,
            []() { return g_prefs.accent; },
            [theme](int i) { g_prefs.accent = i; theme(); }));
        p.rows.push_back(RHeader(L"Toolbar"));
        p.rows.push_back(RToggle(L"Compact layout", L"Smaller toolbar and tab bar to leave more room for pages.",
            &g_prefs.compact, layout));
        g_pages.push_back(std::move(p));
    }
    {   // Tabs
        SPage p; p.name = L"Tabs"; p.icon = L"\uE7C4";
        p.rows.push_back(RHeader(L"New tabs"));
        p.rows.push_back(RChoice(L"New tab opens", L"What you see when you press Ctrl+T or the + button.",
            { L"Home page", L"Blank page" },
            []() { return g_prefs.newTabMode; },
            [save](int i) { g_prefs.newTabMode = i; save(); }));
        p.rows.push_back(RHeader(L"Tab bar"));
        p.rows.push_back(RToggle(L"Show close button on every tab", L"Otherwise it appears on the active and hovered tab.",
            &g_prefs.closeAlways, repaint));
        p.rows.push_back(RInfo(L"Maximum open tabs", std::to_wstring(metrics::kMaxTabs)));
        p.rows.push_back(RInfo(L"Closing the last tab", L"Closes the window"));
        g_pages.push_back(std::move(p));
    }
    {   // Search
        SPage p; p.name = L"Search"; p.icon = L"\uE721";
        p.rows.push_back(RHeader(L"Address bar"));
        p.rows.push_back(RToggle(L"Show full address", L"Show the complete URL instead of just the domain.",
            &g_prefs.showFullUrl, repaint));
        p.rows.push_back(RHeader(L"Search engine"));
        std::vector<std::wstring> names;
        for (int i = 0; i < kEngineCount; ++i) names.push_back(kEngines[i].name);
        names.push_back(L"Custom");
        p.rows.push_back(RChoice(L"Default search engine", L"Used for anything typed in the address bar that is not a website.",
            names,
            []() { for (int i = 0; i < kEngineCount; ++i) if (g_searchEngine == kEngines[i].url) return i; return kEngineCount; },
            [save](int i) { if (i < kEngineCount) { g_searchEngine = kEngines[i].url; save(); } }));
        p.rows.push_back(RText(L"Search URL", L"The search query is appended to this address.",
            []() { return g_searchEngine; },
            [](const std::wstring& s) { std::wstring t = TrimWs(s); if (!t.empty()) { g_searchEngine = t; SaveSettings(); } }));
        g_pages.push_back(std::move(p));
    }
    {   // Webpages
        SPage p; p.name = L"Webpages"; p.icon = L"\uE774";
        p.rows.push_back(RHeader(L"Content"));
        p.rows.push_back(RToggle(L"Enable JavaScript", L"Turning this off breaks most modern websites.", &g_prefs.js, web));
        p.rows.push_back(RToggle(L"Load images automatically", L"Disable to save data on slow connections.", &g_prefs.images, web));
        p.rows.push_back(RToggle(L"Allow full screen", L"Let pages such as video players use the whole screen.", &g_prefs.fullscreen, web));
        p.rows.push_back(RToggle(L"Enable Web Audio", L"Has no effect yet: Web Audio is disabled in the current WebKit build.", &g_prefs.webAudio, web));
        p.rows.push_back(RHeader(L"Fonts"));
        p.rows.push_back(RSlider(L"Default font size", L"Base size for pages that do not set their own.", 9, 32, 1,
            []() { return g_prefs.defaultFont; },
            [web](int v) { g_prefs.defaultFont = v; web(); },
            [](int v) { return std::to_wstring(v) + L" px"; }));
        p.rows.push_back(RSlider(L"Minimum font size", L"Text smaller than this is enlarged. 0 turns it off.", 0, 24, 1,
            []() { return g_prefs.minFont; },
            [web](int v) { g_prefs.minFont = v; web(); },
            [](int v) { return v == 0 ? std::wstring(L"Off") : std::to_wstring(v) + L" px"; }));
        p.rows.push_back(RHeader(L"Developer"));
        p.rows.push_back(RToggle(L"Developer tools", L"Adds Inspect Element to the page context menu.", &g_prefs.devExtras, web));
        g_pages.push_back(std::move(p));
    }
    {   // Privacy
        SPage p; p.name = L"Privacy"; p.icon = L"\uE72E";
        p.rows.push_back(RHeader(L"Website data"));
        p.rows.push_back(RToggle(L"Allow local storage", L"Lets sites remember settings and sign-ins in this browser.", &g_prefs.localStorage, web));
        p.rows.push_back(RToggle(L"Allow web databases", L"Used by some offline-capable web apps.", &g_prefs.databases, web));
        p.rows.push_back(RInfo(L"Note", L"Changes apply to pages loaded afterwards"));
        g_pages.push_back(std::move(p));
    }
    {   // Keyboard
        SPage p; p.name = L"Keyboard"; p.icon = L"\uE765";
        p.rows.push_back(RHeader(L"Tabs"));
        p.rows.push_back(RInfo(L"New tab", L"Ctrl+T", true));
        p.rows.push_back(RInfo(L"Close tab", L"Ctrl+W", true));
        p.rows.push_back(RInfo(L"Next tab", L"Ctrl+Tab", true));
        p.rows.push_back(RInfo(L"Previous tab", L"Ctrl+Shift+Tab", true));
        p.rows.push_back(RInfo(L"Go to tab 1-8", L"Ctrl+1 ... 8", true));
        p.rows.push_back(RInfo(L"Go to last tab", L"Ctrl+9", true));
        p.rows.push_back(RInfo(L"Reopen closed tab", L"Ctrl+Shift+T", true));
        p.rows.push_back(RHeader(L"Navigation"));
        p.rows.push_back(RInfo(L"Focus address bar", L"Ctrl+L", true));
        p.rows.push_back(RInfo(L"Reload page", L"Ctrl+R  /  F5", true));
        p.rows.push_back(RInfo(L"Back", L"Alt+Left", true));
        p.rows.push_back(RInfo(L"Forward", L"Alt+Right", true));
        p.rows.push_back(RHeader(L"View"));
        p.rows.push_back(RInfo(L"Zoom in", L"Ctrl++", true));
        p.rows.push_back(RInfo(L"Zoom out", L"Ctrl+-", true));
        p.rows.push_back(RInfo(L"Actual size", L"Ctrl+0", true));
        p.rows.push_back(RHeader(L"Browser"));
        p.rows.push_back(RInfo(L"Settings", L"Ctrl+,", true));
        p.rows.push_back(RInfo(L"Developer tools", L"F12  /  Ctrl+Shift+I", true));
        g_pages.push_back(std::move(p));
    }
    {   // About
        SPage p; p.name = L"About"; p.icon = L"\uE946";
        p.rows.push_back(RHeader(kAppName));
        p.rows.push_back(RInfo(L"Engine", L"WebKit"));
        p.rows.push_back(RInfo(L"Version", L"1.0"));
        p.rows.push_back(RInfo(L"Settings file", g_settingsFile));
        p.rows.push_back(RButton(L"Settings folder", L"", L"Open",
            []() { ShellExecuteW(nullptr, L"open", g_settingsDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }));
        p.rows.push_back(RHeader(L"Reset"));
        p.rows.push_back(RButton(L"Reset all settings", L"Restore every option to its default value.", L"Reset...",
            []() {
                if (MessageBoxW(g_setHwnd, L"Reset all settings to their defaults?", L"Settings",
                                MB_YESNO | MB_ICONQUESTION) == IDYES)
                    ResetAllSettings();
            }));
        g_pages.push_back(std::move(p));
    }
}

// ---- hit testing helpers ---------------------------------------------------
static int SSideHit(const SGeo& G, float x, float y) {
    for (int i = 0; i < (int)g_pages.size(); ++i)
        if (SSideRect(G, i).Contains(x, y)) return i;
    return -1;
}
static int SRowAt(const SGeo& G, float x, float y) {
    if (y < G.vpY || x < G.sideW) return -1;
    for (int i = 0; i < (int)g_view.size(); ++i)
        if (SRowRect(G, g_view[i]).Contains(x, y)) return i;
    return -1;
}
static bool SOverControl(const SGeo& G, int vi, float x, float y) {
    if (vi < 0 || !g_view[vi].row) return false;
    SRow& r = *g_view[vi].row;
    RectF row = SRowRect(G, g_view[vi]);
    RectF ctl = SCtlRect(r, row);
    if (ctl.Width <= 0) return false;
    if (r.type == SRow::Slider) {
        RectF hit(ctl.X - SS(10), row.Y, ctl.Width + SS(20), row.Height);
        return hit.Contains(x, y);
    }
    return ctl.Contains(x, y);
}
static void SSetSliderFromX(SRow* row, const RectF& ctl, float x) {
    if (ctl.Width <= 0 || row->step <= 0) return;
    float t = std::max(0.f, std::min(1.f, (x - ctl.X) / ctl.Width));
    int range = row->maxV - row->minV;
    int v = row->minV + (int)std::lround(t * (float)range / (float)row->step) * row->step;
    v = std::max(row->minV, std::min(row->maxV, v));
    if (v != row->getInt()) row->setInt(v);
}

static void SChoiceMenu(SRow* r, const RectF& ctl) {
    HMENU m = CreatePopupMenu();
    int cur = r->getInt();
    for (int i = 0; i < (int)r->options.size(); ++i)
        AppendMenuW(m, MF_STRING | (i == cur ? MF_CHECKED : 0), 1000 + i, r->options[i].c_str());
    POINT pt = { (LONG)ctl.X, (LONG)ctl.GetBottom() };
    ClientToScreen(g_setHwnd, &pt);
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY,
                                  pt.x, pt.y, 0, g_setHwnd, nullptr);
    DestroyMenu(m);
    if (cmd >= 1000) r->setInt(cmd - 1000);
}

static void SettingsThemeChanged() {
    if (!g_setHwnd) return;
    if (g_fieldBrush) DeleteObject(g_fieldBrush);
    g_fieldBrush = CreateSolidBrush(SFieldColor());
    ApplyDwm(g_setHwnd);
    InvalidateRect(g_setHwnd, nullptr, FALSE);
    if (g_setSearch) InvalidateRect(g_setSearch, nullptr, TRUE);
    if (g_setEdit) InvalidateRect(g_setEdit, nullptr, TRUE);
}

// ---- window procedure ------------------------------------------------------
static LRESULT CALLBACK SettingsWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_setHwnd = hWnd;
        g_sDpi = GetDpiForWindow(hWnd);
        HINSTANCE hi = (HINSTANCE)GetWindowLongPtrW(hWnd, GWLP_HINSTANCE);
        g_setSearch = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL | ES_LEFT,
                                      0, 0, 10, 10, hWnd, (HMENU)(INT_PTR)SET_ID_SEARCH, hi, nullptr);
        g_setEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL | ES_LEFT,
                                    0, 0, 10, 10, hWnd, (HMENU)(INT_PTR)SET_ID_EDIT, hi, nullptr);
        g_oldSEditProc = (WNDPROC)SetWindowLongPtrW(g_setSearch, GWLP_WNDPROC, (LONG_PTR)SEditProc);
        SetWindowLongPtrW(g_setEdit, GWLP_WNDPROC, (LONG_PTR)SEditProc);
        SRebuildFonts();
        SettingsThemeChanged();
        BuildPages();
        g_sPage = 0; g_sScroll = 0.f; g_sEditRow = nullptr;
        g_sHoverSide = g_sHoverRow = g_sDragRow = -1;
        RebuildView();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = (LONG)SS(760);
        mmi->ptMinTrackSize.y = (LONG)SS(520);
        return 0;
    }
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            SCommitTextEdit();
            SPositionSearch();
            SClampScroll();
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;

    case WM_DPICHANGED: {
        g_sDpi = HIWORD(wParam);
        SRebuildFonts();
        const RECT* r = (const RECT*)lParam;
        SetWindowPos(hWnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        RebuildView();
        SPositionSearch();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT cr;
        GetClientRect(hWnd, &cr);
        if (cr.right > 0 && cr.bottom > 0) {
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, cr.right, cr.bottom);
            HGDIOBJ old = SelectObject(mem, bmp);
            {
                Graphics g(mem);
                g.SetSmoothingMode(SmoothingModeHighQuality);
                g.SetPixelOffsetMode(PixelOffsetModeHalf);
                g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
                DrawSettings(g);
            }
            BitBlt(hdc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, ToColorRef(g_th.text));
        SetBkColor(hdc, SFieldColor());
        return (LRESULT)g_fieldBrush;
    }
    case WM_COMMAND: {
        HWND from = (HWND)lParam;
        WORD code = HIWORD(wParam);
        if (from == g_setSearch) {
            if (code == EN_CHANGE) { g_sScroll = 0.f; RebuildView(); }
            else if (code == EN_KILLFOCUS) {
                if (SSearchText().empty()) ShowWindow(g_setSearch, SW_HIDE);
                InvalidateRect(hWnd, nullptr, FALSE);
            } else if (code == EN_SETFOCUS) InvalidateRect(hWnd, nullptr, FALSE);
        } else if (from == g_setEdit && code == EN_KILLFOCUS) {
            SCommitTextEdit();
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        const float x = (float)GET_X_LPARAM(lParam), y = (float)GET_Y_LPARAM(lParam);
        const SGeo G = Geo();
        if (g_sDragRow >= 0 && g_sDragRow < (int)g_view.size() && (wParam & MK_LBUTTON)) {
            SRow* r = g_view[g_sDragRow].row;
            if (r) SSetSliderFromX(r, SCtlRect(*r, SRowRect(G, g_view[g_sDragRow])), x);
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        int si = SSideHit(G, x, y);
        int vi = SRowAt(G, x, y);
        bool ctl = SOverControl(G, vi, x, y);
        bool pill = SSearchRect(G).Contains(x, y);
        if (si != g_sHoverSide || vi != g_sHoverRow || ctl != g_sHoverCtl || pill != g_sHoverPill) {
            g_sHoverSide = si; g_sHoverRow = vi; g_sHoverCtl = ctl; g_sHoverPill = pill;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        if (!g_sTracking) {
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&t);
            g_sTracking = true;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_sTracking = false;
        g_sHoverSide = g_sHoverRow = -1;
        g_sHoverCtl = g_sHoverPill = false;
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && (g_sHoverSide >= 0 || g_sHoverCtl || g_sHoverPill)) {
            SetCursor(LoadCursor(nullptr, IDC_HAND));
            return TRUE;
        }
        break;

    case WM_LBUTTONDOWN: {
        const float x = (float)GET_X_LPARAM(lParam), y = (float)GET_Y_LPARAM(lParam);
        const SGeo G = Geo();
        SCommitTextEdit();
        int si = SSideHit(G, x, y);
        if (si >= 0) { SSelectPage(si); SetFocus(hWnd); return 0; }
        if (SSearchRect(G).Contains(x, y)) { ShowSearch(); return 0; }
        int vi = SRowAt(G, x, y);
        if (vi >= 0 && g_view[vi].row) {
            SRow* r = g_view[vi].row;
            RectF row = SRowRect(G, g_view[vi]);
            RectF ctl = SCtlRect(*r, row);
            switch (r->type) {
            case SRow::Toggle:  r->setInt(r->getInt() ? 0 : 1); break;
            case SRow::Choice:  if (ctl.Contains(x, y)) SChoiceMenu(r, ctl); break;
            case SRow::Slider:
                if (SOverControl(G, vi, x, y)) {
                    g_sDragRow = vi;
                    SetCapture(hWnd);
                    SSetSliderFromX(r, ctl, x);
                }
                break;
            case SRow::Text:    if (ctl.Contains(x, y)) SBeginTextEdit(r, ctl); break;
            case SRow::Button:  if (ctl.Contains(x, y) && r->action) r->action(); break;
            default: break;
            }
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        if (!g_sEditRow) SetFocus(hWnd);
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_sDragRow >= 0) { g_sDragRow = -1; if (GetCapture() == hWnd) ReleaseCapture(); }
        return 0;

    case WM_CAPTURECHANGED:
        g_sDragRow = -1;
        return 0;

    case WM_MOUSEWHEEL: {
        SCommitTextEdit();
        short delta = GET_WHEEL_DELTA_WPARAM(wParam);
        SScrollBy(-(float)delta / 120.f * SS(64));
        return 0;
    }
    case WM_KEYDOWN: {
        const SGeo G = Geo();
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (wParam == VK_ESCAPE) {
            if (!SSearchText().empty()) { SetWindowTextW(g_setSearch, L""); ShowWindow(g_setSearch, SW_HIDE); }
            else DestroyWindow(hWnd);
            return 0;
        }
        if (ctrl && wParam == 'F') { ShowSearch(); return 0; }
        switch (wParam) {
        case VK_UP:    SScrollBy(-SS(48)); return 0;
        case VK_DOWN:  SScrollBy(SS(48)); return 0;
        case VK_PRIOR: SScrollBy(-G.vpH * 0.9f); return 0;
        case VK_NEXT:  SScrollBy(G.vpH * 0.9f); return 0;
        case VK_HOME:  g_sScroll = 0.f; SScrollBy(0); return 0;
        case VK_END:   g_sScroll = g_sContentH; SScrollBy(0); return 0;
        }
        break;
    }
    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        g_sEditRow = nullptr;
        g_view.clear();
        g_groups.clear();
        g_pages.clear();
        if (g_fieldBrush) { DeleteObject(g_fieldBrush); g_fieldBrush = nullptr; }
        if (g_sEditFont)  { DeleteObject(g_sEditFont);  g_sEditFont = nullptr; }
        g_sfBody.reset(); g_sfSmall.reset(); g_sfTitle.reset(); g_sfHead.reset();
        g_sfIcon.reset(); g_sfIconSm.reset();
        g_setSearch = g_setEdit = nullptr;
        g_setHwnd = nullptr;
        return 0;                                  // no PostQuitMessage: this is not the main window
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void ShowSettingsWindow() {
    if (g_setHwnd) {
        if (IsIconic(g_setHwnd)) ShowWindow(g_setHwnd, SW_RESTORE);
        SetForegroundWindow(g_setHwnd);
        return;
    }
    HINSTANCE hi = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = SettingsWndProc;
        wc.hInstance = hi;
        wc.lpszClassName = kSetClass;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        if (!RegisterClassExW(&wc)) return;
        registered = true;
    }
    int w = MulDiv(980, (int)g_dpi, 96), h = MulDiv(700, (int)g_dpi, 96);
    RECT pr = {};
    GetWindowRect(g_hWnd, &pr);
    int x = pr.left + ((pr.right - pr.left) - w) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - h) / 2;
    CreateWindowExW(0, kSetClass, L"Settings", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                    x, y, w, h, g_hWnd, nullptr, hi, nullptr);
    if (g_setHwnd) {
        ShowWindow(g_setHwnd, SW_SHOW);
        UpdateWindow(g_setHwnd);
        SetFocus(g_setHwnd);
    }
}

// ----------------------------------------------------------------------------
//  Window procedure
// ----------------------------------------------------------------------------
static LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_fullScreenMsg && msg == g_fullScreenMsg)
        return OnFullScreenMessage(wParam != 0, (HWND)lParam);
    switch (msg) {
    case WM_CREATE: {
        g_hWnd = hWnd;
        g_fullScreenMsg = RegisterWindowMessageW(L"WebKitFullScreen");
        g_dpi = GetDpiForWindow(hWnd);
        g_urlEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL | ES_LEFT,
                                    0, 0, 10, 10, hWnd, (HMENU)(INT_PTR)ID_URL_EDIT,
                                    (HINSTANCE)GetWindowLongPtrW(hWnd, GWLP_HINSTANCE), nullptr);
        g_oldEditProc = (WNDPROC)SetWindowLongPtrW(g_urlEdit, GWLP_WNDPROC, (LONG_PTR)UrlEditProc);
        RebuildResources();
        ApplyTheme(ResolveDark());
        SetTimer(hWnd, TIMER_UI, 30, nullptr);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = (LONG)S(720);
        mmi->ptMinTrackSize.y = (LONG)S(420);
        return 0;
    }
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) RefreshLayout();
        return 0;

    case WM_DPICHANGED: {
        g_dpi = HIWORD(wParam);
        RebuildResources();
        const RECT* r = (const RECT*)lParam;
        SetWindowPos(hWnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        RefreshLayout();
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (lParam && wcscmp((LPCWSTR)lParam, L"ImmersiveColorSet") == 0) {
            ApplyTheme(ResolveDark());
            EnableDarkMenus();
        }
        break;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT cr;
        GetClientRect(hWnd, &cr);
        // Area under the web views (only visible if there are no tabs yet)
        RECT content = { 0, (LONG)g_L.chromeH, cr.right, cr.bottom };
        HBRUSH dc = (HBRUSH)GetStockObject(DC_BRUSH);
        SetDCBrushColor(hdc, ToColorRef(g_th.toolbar));
        FillRect(hdc, &content, dc);
        PaintChrome(hdc, cr.right);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_CTLCOLOREDIT:
        if ((HWND)lParam == g_urlEdit) {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, ToColorRef(g_th.text));
            SetBkColor(hdc, ToColorRef(g_th.pillFocus));
            return (LRESULT)g_editBrush;
        }
        break;

    case WM_COMMAND:
        if ((HWND)lParam == g_urlEdit && HIWORD(wParam) == EN_KILLFOCUS) {
            ShowWindow(g_urlEdit, SW_HIDE);
            InvalidateChrome();
            return 0;
        }
        break;

    case WM_MOUSEMOVE: {
        Hit h = HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (!(h == g_hover)) { g_hover = h; InvalidateChrome(); }
        if (!g_trackingMouse) {
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&t);
            g_trackingMouse = true;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_trackingMouse = false;
        g_hover = Hit();
        InvalidateChrome();
        return 0;

    case WM_LBUTTONDOWN: {
        Hit h = HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        g_down = h;
        if (h.kind == Hit::Pill) { g_down = Hit(); ShowUrlEdit(); return 0; }
        if (h.kind == Hit::TabBody) SelectTab(h.index);
        if (h.kind != Hit::None) SetCapture(hWnd);
        InvalidateChrome();
        return 0;
    }
    case WM_LBUTTONUP: {
        Hit up = HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        Hit down = g_down;
        g_down = Hit();
        if (GetCapture() == hWnd) ReleaseCapture();
        if (up == down) {
            switch (up.kind) {
            case Hit::Back:     GoBack(); break;
            case Hit::Forward:  GoForward(); break;
            case Hit::Reload:   ReloadActive(true); break;
            case Hit::More:     ShowMoreMenu(); break;
            case Hit::NewTab:   RunCommand(CMD_NEWTAB); break;
            case Hit::TabClose: CloseTab(up.index); break;
            default: break;
            }
        }
        InvalidateChrome();
        return 0;
    }
    case WM_CAPTURECHANGED:
        g_down = Hit();
        InvalidateChrome();
        return 0;

    case WM_MBUTTONUP: {
        Hit h = HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (h.kind == Hit::TabBody || h.kind == Hit::TabClose) CloseTab(h.index);
        return 0;
    }
    case WM_TIMER: {
        if (wParam != TIMER_UI) break;
        if (g_closing || g_tabOp) return 0;      // never poll tabs while one is being created/destroyed
        bool dirty = false;
        ++g_tick;
        Tab* act = ActiveTab();
        for (auto& tp : g_tabs) {
            Tab& t = *tp;
            if (t.loading && t.page) {
                float target = std::max(0.08f, (float)WKPageGetEstimatedProgress(t.page));
                target = std::min(target, 0.92f);
                if (t.progress < target) { t.progress += (target - t.progress) * 0.25f; if (&t == act) dirty = true; }
            } else if (t.fade > 0.f) {
                t.progress += (1.f - t.progress) * 0.4f;
                t.fade = std::max(0.f, t.fade - 0.07f);
                if (&t == act) dirty = true;
            }
        }
        // Title / URL / history changes arrive through the page state client; this slow
        // poll (~1 s) is only a safety net.
        if (g_tick % 32 == 0) {
            for (auto& tp : g_tabs)
                if (SyncTab(*tp)) dirty = true;
        }
        if (dirty) InvalidateChrome();
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE && !IsEditing()) FocusWebView();
        break;

    case WM_APP_CLOSE_TAB: {             // window.close(), or an empty popup that became a download
        int index = -1;
        if (FindTab((unsigned long long)wParam, &index)) CloseTab(index);
        return 0;
    }
    case WM_APP_TAB_CRASHED:
        HandleTabCrashed((unsigned long long)wParam, (int)lParam);
        return 0;

    case WM_CLOSE:
        if (g_closing) return 0;         // repeated Ctrl+W / extra WM_CLOSE must not run teardown twice
        if (g_tabOp) {                   // arrived from a nested message loop inside a tab operation
            Log("WM_CLOSE during a tab operation -> re-posted");
            PostMessageW(hWnd, WM_CLOSE, 0, 0);
            return 0;
        }
        if (g_modalDepth > 0) {          // a page dialog (alert, file chooser...) is still open
            Log("WM_CLOSE while a page dialog is open -> ignored");
            g_closeRequested = false;
            return 0;
        }
        Log("WM_CLOSE: shutting down (tabs=%d active=%d)", (int)g_tabs.size(), g_active);
        g_closing = true;
        KillTimer(hWnd, TIMER_UI);
        CloseAllTabs();
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hWnd, TIMER_UI);
        if (g_editBrush) { DeleteObject(g_editBrush); g_editBrush = nullptr; }
        if (g_editFont) { DeleteObject(g_editFont); g_editFont = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* er = ep ? ep->ExceptionRecord : nullptr;
    if (er) {
        Log("!!! UNHANDLED EXCEPTION code=0x%08lX flags=0x%lX", er->ExceptionCode, er->ExceptionFlags);
        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
            Log("!!! access violation: %s address %p",
                er->ExceptionInformation[0] == 0 ? "reading" : er->ExceptionInformation[0] == 1 ? "writing" : "executing",
                (void*)er->ExceptionInformation[1]);
        LogAddress("!!! faulting instruction", er->ExceptionAddress);
    }
    Log("!!! state: tabs=%d active=%d tabOp=%d closing=%d", (int)g_tabs.size(), g_active, (int)g_tabOp, (int)g_closing);
    void* frames[48] = {};
    USHORT n = CaptureStackBackTrace(0, 48, frames, nullptr);
    for (USHORT k = 0; k < n; ++k) {
        char label[16];
        _snprintf_s(label, sizeof(label), _TRUNCATE, "!!! #%u", (unsigned)k);
        LogAddress(label, frames[k]);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// When this exe runs on a WebKit built with the GStreamer media backend, CMake copies
// the GStreamer plugins to <exe dir>\gstreamer-1.0. GStreamer only finds them through
// its environment variables, which the WebKit child processes (where media actually
// plays) inherit from us. With the regular Media Foundation build the folder does not
// exist and nothing is changed.
static void ConfigureGStreamerEnvironment() {
    wchar_t exe[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    std::wstring dir(exe, n);
    size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    dir.resize(slash);

    const std::wstring plugins = dir + L"\\gstreamer-1.0";
    if (GetFileAttributesW(plugins.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    SetEnvironmentVariableW(L"GST_PLUGIN_SYSTEM_PATH_1_0", plugins.c_str());
    const std::wstring scanner = dir + L"\\gst-plugin-scanner.exe";
    if (GetFileAttributesW(scanner.c_str()) != INVALID_FILE_ATTRIBUTES)
        SetEnvironmentVariableW(L"GST_PLUGIN_SCANNER_1_0", scanner.c_str());
    // Plugin cache: rebuilt automatically when the plugin set changes.
    const std::wstring registry = g_settingsDir + L"\\gstreamer-registry.bin";
    SetEnvironmentVariableW(L"GST_REGISTRY_1_0", registry.c_str());
    Log("GStreamer plugins: %ls", plugins.c_str());
}

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

    std::wstring startUrl = g_homePage;
    if (lpCmdLine && *lpCmdLine) {
        std::wstring arg = TrimWs(lpCmdLine);
        if (arg.size() >= 2 && arg.front() == L'"' && arg.back() == L'"') arg = arg.substr(1, arg.size() - 2);
        if (!arg.empty()) startUrl = arg;
    }
    CreateTab(startUrl);

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
    g_fText.reset(); g_fTab.reset(); g_fIcon.reset(); g_fIconSm.reset();
    GdiplusShutdown(gdipToken);
    OleUninitialize();
    return (int)msg.wParam;
}
