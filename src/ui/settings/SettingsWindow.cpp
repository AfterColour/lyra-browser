// Lyra - the settings window: resources/settings.html in a window of its own.
//
// The settings themselves are the model in SettingsPages.cpp (pages of rows, each with a getter
// and a setter). This window shows a WebKit view with settings.html and keeps the two in step:
//   Lyra -> page   lyraSettings(<json>): every page and row with its current value, the look
//                  (theme, accent, language) and the start page's layout (start.json).
//   page -> Lyra   lyra://settings-... navigations (never loaded), carried out on the window's
//                  own message loop, never inside a WebKit callback:
//                    settings-ready             the page is up: send the model, show the window
//                    settings-set?p=&r=&v=      a row's new value (toggle, choice, slider...)
//                    settings-text?p=&r=&s=     a text row's new value
//                    settings-press?p=&r=       a button (or a toggle's gear)
//                    settings-page?i=           the page now shown (reopened next time)
//                    settings-start?cfg=        the start page's layout/wallpaper changed
//                    settings-start-file        choose a picture for the start page
//                    settings-theme?id=         turn a theme on ("" = Lyra's own look)
//                    settings-close             Esc with nothing left to close
// After every change the whole model is sent again: one setting can change what others show.
#include "ui/settings/SettingsWindow.h"
#include "ui/settings/SettingsPages.h"
#include "app/Log.h"
#include "browser/StartPage.h"
#include "browser/TabManager.h"
#include "browser/Wallpaper.h"
#include "browser/CustomThemes.h"
#include "ui/Previews.h"
#include "browser/WebContext.h"
#include "core/Localization.h"
#include "core/Settings.h"
#include "core/StringUtils.h"
#include "core/Url.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"

HWND g_setHwnd = nullptr;
std::vector<SPage> g_pages;

static const wchar_t kSettingsClass[] = L"LyraSettingsWindow";
static constexpr UINT WM_SET_COMMAND = WM_APP + 1;   // lParam: new std::wstring (a lyra:// address)
static constexpr UINT WM_SET_REBUILD = WM_APP + 2;   // rebuild the pages, then send them
static constexpr UINT WM_SET_PUSH    = WM_APP + 3;   // send the model again
static constexpr UINT_PTR kShowTimer = 1;            // shows the window if the page never says it is ready

static WKViewRef s_view = nullptr;
static WKPageRef s_page = nullptr;
static HWND      s_viewHwnd = nullptr;
static int       s_openPage = 0;                     // the page shown (or to show when the window opens)
static bool      s_ready = false;                    // settings.html has loaded and asked for the model
static bool      s_rebuildPosted = false, s_pushPosted = false;
static bool      s_picked = false;                   // a start page picture was just chosen

// ---- the model as JSON --------------------------------------------------------------------------
static std::wstring Hex(const Color& c) {
    wchar_t s[8];
    swprintf(s, 8, L"#%02X%02X%02X", c.GetR(), c.GetG(), c.GetB());
    return s;
}

static std::wstring Strings(const std::vector<std::wstring>& v) {
    std::wstring j = L"[";
    for (size_t i = 0; i < v.size(); ++i) { if (i) j += L","; j += JsString(v[i]); }
    return j + L"]";
}

static std::wstring RowJson(const SRow& r) {
    static const wchar_t* kTypes[] = { L"header", L"toggle", L"gear", L"choice", L"seg", L"cards", L"swatch",
                                       L"slider", L"text", L"button", L"info", L"custom" };
    std::wstring j = L"{\"t\":\"" + std::wstring(kTypes[(int)r.type]) + L"\",\"l\":" + JsString(r.label);
    if (!r.desc.empty() || r.getStr) {
        const std::wstring desc = (r.type == SRow::Button && r.getStr) ? r.getStr() : r.desc;
        if (!desc.empty()) j += L",\"d\":" + JsString(desc);
    }
    switch (r.type) {
    case SRow::Toggle: case SRow::ToggleGear:
        j += L",\"v\":" + std::to_wstring(r.getInt ? r.getInt() : 0);
        break;
    case SRow::Choice: case SRow::Segmented: case SRow::Cards:
        j += L",\"o\":" + Strings(r.options) + L",\"v\":" + std::to_wstring(r.getInt ? r.getInt() : 0);
        if (r.type == SRow::Cards) j += L",\"k\":" + std::to_wstring(r.cardKind);
        break;
    case SRow::Swatches: {
        std::vector<std::wstring> colors;
        for (const Color& c : r.colors) colors.push_back(Hex(c));
        j += L",\"o\":" + Strings(r.options) + L",\"c\":" + Strings(colors) + L",\"v\":" + std::to_wstring(r.getInt ? r.getInt() : 0);
        break;
    }
    case SRow::Slider: {
        std::vector<std::wstring> labels;          // every position's text, so the page can follow the thumb
        for (int v = r.minV; v <= r.maxV; v += std::max(1, r.step))
            labels.push_back(r.fmt ? r.fmt(v) : std::to_wstring(v));
        j += L",\"min\":" + std::to_wstring(r.minV) + L",\"max\":" + std::to_wstring(r.maxV) +
             L",\"step\":" + std::to_wstring(std::max(1, r.step)) + L",\"f\":" + Strings(labels) +
             L",\"v\":" + std::to_wstring(r.getInt ? r.getInt() : r.minV);
        break;
    }
    case SRow::Text:
        j += L",\"s\":" + JsString(r.getStr ? r.getStr() : L"");
        break;
    case SRow::Button:
        j += L",\"b\":" + JsString(r.fmt ? r.fmt(0) : r.right);
        break;
    case SRow::Info:
        j += L",\"r\":" + JsString(r.right) + (r.kbd ? L",\"kbd\":1" : L"");
        break;
    case SRow::Custom:
        j += L",\"k\":" + JsString(r.right);
        break;
    default:
        break;
    }
    return j + L"}";
}

static std::wstring ModelJson() {
    std::wstring j = L"{\"ru\":" + std::wstring(UiRussian() ? L"true" : L"false");
    j += L",\"dark\":" + std::wstring(ResolveDark() ? L"true" : L"false");
    j += L",\"accent\":" + JsString(Hex(g_th.accent));
    j += L",\"anim\":" + std::wstring(g_prefs.animations ? L"true" : L"false");
    j += L",\"open\":" + std::to_wstring(s_openPage);
    j += L",\"picked\":" + std::wstring(s_picked ? L"true" : L"false");
    const std::wstring start = StartConfigJson();
    j += L",\"start\":" + (start.empty() ? std::wstring(L"null") : start);
    j += L",\"wpq\":" + JsString(WallpaperParams());
    // The installed themes for the previews in Settings > Themes.
    j += L",\"themes\":[";
    const std::vector<CustomTheme>& themes = CustomThemes();
    for (size_t k = 0; k < themes.size(); ++k) {
        const CustomTheme& t = themes[k];
        std::wstring pic;
        if (!t.picture.empty()) {             // its small copy (decoding the picture itself froze the page)
            const std::wstring preview = PreviewFor(t.dir + L"\\" + t.picture, t.dir + L"\\_lyra_preview.jpg");
            wchar_t url[2084] = {};
            DWORD len = ARRAYSIZE(url);
            if (!preview.empty() && SUCCEEDED(UrlCreateFromPathW(preview.c_str(), url, &len, 0))) pic = url;
        }
        std::wstring summary;
        if (t.useColors) summary += Tr(L"colors", L"цвета");
        if (t.useAccent) summary += std::wstring(summary.empty() ? L"" : L", ") + Tr(L"accent", L"акцент");
        if (t.useWallpaper && !t.picture.empty()) summary += std::wstring(summary.empty() ? L"" : L", ") + Tr(L"wallpaper", L"обои");
        if (summary.empty()) summary = Tr(L"changes nothing", L"ничего не меняет");
        summary[0] = towupper(summary[0]);
        if (k) j += L",";
        j += L"{\"id\":" + JsString(t.id) + L",\"n\":" + JsString(t.name) + L",\"d\":" + JsString(summary) +
             L",\"on\":" + std::wstring(g_activeTheme == t.id ? L"true" : L"false") +
             L",\"bg\":" + JsString(Hex(t.bg)) + L",\"fg\":" + JsString(Hex(t.fg)) + L",\"hi\":" + JsString(Hex(t.highlight)) +
             L",\"ac\":" + JsString(Hex(t.accent)) + L",\"acw\":" + std::wstring(t.accentOnWindow ? L"true" : L"false") +
             L",\"pic\":" + JsString(pic) + L",\"page\":" + std::to_wstring(SettingsThemePage(k)) + L"}";
    }
    j += L"]";
    j += L",\"pages\":[";
    for (size_t i = 0; i < g_pages.size(); ++i) {
        const SPage& p = g_pages[i];
        if (i) j += L",";
        j += L"{\"n\":" + JsString(p.name) + L",\"i\":" + JsString(p.icon) + L",\"c\":" + JsString(Hex(p.tint)) +
             L",\"g\":" + std::to_wstring(p.group) + L",\"p\":" + std::to_wstring(p.parent) + L",\"rows\":[";
        for (size_t k = 0; k < p.rows.size(); ++k) { if (k) j += L","; j += RowJson(p.rows[k]); }
        j += L"]}";
    }
    return j + L"]}";
}

static void IgnoreResult(WKTypeRef, WKErrorRef, void*) {}

static void RunScript(const std::string& script) {
    if (!s_page) return;
    if (WKStringRef s = WKStringCreateWithUTF8CString(script.c_str())) {
        WKPageEvaluateJavaScriptInMainFrame(s_page, s, nullptr, IgnoreResult);
        WKRelease(s);
    }
}

static void Push() {
    if (!s_ready) return;
    RunScript("window.lyraSettings && lyraSettings(" + ToUtf8(ModelJson()) + ")");
}

// ---- the window's frame follows the theme ---------------------------------------------------------
static COLORREF PageBackground() { return ResolveDark() ? RGB(0x1C, 0x1C, 0x1F) : RGB(0xF4, 0xF4, 0xF6); }

static void ApplyFrameLook() {
    if (!g_setHwnd) return;
    BOOL dark = ResolveDark();
    DwmSetWindowAttribute(g_setHwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    COLORREF caption = PageBackground();
    DwmSetWindowAttribute(g_setHwnd, 35 /* DWMWA_CAPTION_COLOR (Windows 11) */, &caption, sizeof(caption));
}

// ---- page -> Lyra ------------------------------------------------------------------------------
static std::wstring Param(const std::wstring& query, const wchar_t* name) {
    const std::wstring key = std::wstring(name) + L"=";
    size_t p = 0;
    while (p < query.size()) {
        size_t e = query.find(L'&', p);
        if (e == std::wstring::npos) e = query.size();
        if (query.compare(p, key.size(), key) == 0) return UrlDecode(query.substr(p + key.size(), e - p - key.size()));
        p = e + 1;
    }
    return std::wstring();
}

static SRow* RowAt(const std::wstring& query) {
    const int p = _wtoi(Param(query, L"p").c_str()), r = _wtoi(Param(query, L"r").c_str());
    if (p < 0 || p >= (int)g_pages.size() || r < 0 || r >= (int)g_pages[p].rows.size()) return nullptr;
    return &g_pages[p].rows[r];
}

static void ShowNow() {
    KillTimer(g_setHwnd, kShowTimer);
    if (!IsWindowVisible(g_setHwnd)) {
        ShowWindow(g_setHwnd, SW_SHOW);
        SetForegroundWindow(g_setHwnd);
    }
    if (s_viewHwnd) SetFocus(s_viewHwnd);
}

static void OnCommand(const std::wstring& url) {
    const size_t q = url.find(L'?');
    std::wstring command = url.substr(0, q);
    const std::wstring query = q == std::wstring::npos ? L"" : url.substr(q + 1);
    while (!command.empty() && command.back() == L'/') command.pop_back();
    CharLowerBuffW(&command[0], (DWORD)command.size());

    if (command == L"lyra://settings-ready") {
        s_ready = true;
        Push();
        ShowNow();
    } else if (command == L"lyra://settings-set") {
        if (SRow* r = RowAt(query)) {
            auto set = r->setInt;                   // a copy: the setter may rebuild the pages
            if (set) set(_wtoi(Param(query, L"v").c_str()));
        }
        Push();
    } else if (command == L"lyra://settings-text") {
        if (SRow* r = RowAt(query)) {
            auto set = r->setStr;
            if (set) set(Param(query, L"s"));
        }
        Push();
    } else if (command == L"lyra://settings-press") {
        if (SRow* r = RowAt(query)) {
            auto action = r->action;
            if (action) action();
        }
        Push();
    } else if (command == L"lyra://settings-page") {
        const int i = _wtoi(Param(query, L"i").c_str());
        if (i >= 0 && i < (int)g_pages.size()) s_openPage = i;
    } else if (command == L"lyra://settings-start") {
        StartConfigSave(ToUtf8(Param(query, L"cfg")));
        WallpaperBingEnsure();
        RefreshStartPages();
        Push();
    } else if (command == L"lyra://settings-perf") {     // the page's smoothness log
        Log("page frames: page=settings&%ls", query.c_str());
    } else if (command == L"lyra://settings-theme") {
        CustomThemeActivate(Param(query, L"id"));
        Push();
    } else if (command == L"lyra://settings-start-file") {
        if (WallpaperPickFile()) {
            s_picked = true;
            Push();
            s_picked = false;
        }
    } else if (command == L"lyra://settings-close") {
        DestroyWindow(g_setHwnd);
    } else {
        Log("settings: unknown command %ls", url.c_str());
    }
}

// lyra:// addresses are commands; the page's own file loads; anything else opens in a tab.
static void OnDecidePolicy(WKPageRef, WKNavigationActionRef action, WKFramePolicyListenerRef listener, WKTypeRef, const void*) {
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
        if (g_setHwnd) PostMessageW(g_setHwnd, WM_SET_COMMAND, 0, (LPARAM)new std::wstring(url));
        return;
    }
    if (_wcsnicmp(url.c_str(), L"file:", 5) == 0 || _wcsnicmp(url.c_str(), L"about:", 6) == 0) {
        WKFramePolicyListenerUse(listener);
        return;
    }
    WKFramePolicyListenerIgnore(listener);
    if (g_setHwnd) PostMessageW(g_setHwnd, WM_SET_COMMAND, 1, (LPARAM)new std::wstring(url));
}

static std::wstring SettingsPageUrl() {
    std::wstring url = StartPageFileUrl();                      // file:///.../resources/start.html
    const size_t slash = url.rfind(L'/');
    if (slash != std::wstring::npos) url = url.substr(0, slash + 1) + L"settings.html";
    url += L"?dark=" + std::wstring(ResolveDark() ? L"1" : L"0");
    url += L"&ru=" + std::wstring(UiRussian() ? L"1" : L"0");
    if (GetEnvironmentVariableW(L"LYRA_PAGE_PERF", nullptr, 0)) url += L"&perf=1";   // the page's smoothness log
    return url;
}

static void CreateView() {
    RECT cr;
    GetClientRect(g_setHwnd, &cr);
    WKPageConfigurationRef conf = WKPageConfigurationCreate();
    WKPageConfigurationSetContext(conf, g_context);
    // Its own preferences: the page needs JavaScript even if the user turned it off for sites.
    // (Made once and kept: it follows the frame rate - the display's, not WebKit's default 60 fps.)
    static WKPreferencesRef prefs = [] {
        WKPreferencesRef p = WKPreferencesCreate();
        WKPreferencesSetJavaScriptEnabled(p, true);
        WKPreferencesSetSuppressesIncrementalRendering(p, false);
        FollowFrameRate(p);
        return p;
    }();
    WKPageConfigurationSetPreferences(conf, prefs);
    if (g_dataStore) WKPageConfigurationSetWebsiteDataStore(conf, g_dataStore);
    s_view = WKViewCreate(cr, conf, g_setHwnd);
    WKRelease(conf);
    if (!s_view) { Log("settings: WKViewCreate failed"); return; }
    s_page = WKViewGetPage(s_view);
    s_viewHwnd = WKViewGetWindow(s_view);
    WKViewSetIsInWindow(s_view, true);

    WKPageNavigationClientV0 nc = {};
    nc.base.version = 0;
    nc.decidePolicyForNavigationAction = OnDecidePolicy;
    WKPageSetPageNavigationClient(s_page, &nc.base);

    const std::string url = ToUtf8(SettingsPageUrl());
    if (WKURLRef u = WKURLCreateWithUTF8CString(url.c_str())) {
        WKPageLoadURL(s_page, u);
        WKRelease(u);
    }
}

// The order of teardown matters (see DestroyTab in TabManager.cpp): no more callbacks, then the
// view's window (which closes the page), then our reference.
static void DestroyView() {
    if (s_page) {
        WKPageNavigationClientV0 empty = {};
        WKPageSetPageNavigationClient(s_page, &empty.base);
    }
    if (s_viewHwnd && IsWindow(s_viewHwnd)) DestroyWindow(s_viewHwnd);
    else if (s_page) WKPageClose(s_page);
    if (s_view) WKRelease(s_view);
    s_view = nullptr; s_page = nullptr; s_viewHwnd = nullptr;
    s_ready = false;
}

static LRESULT CALLBACK SettingsWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g_setHwnd = hWnd;
        ApplyFrameLook();
        CreateView();
        SetTimer(hWnd, kShowTimer, 1500, nullptr);
        return 0;
    case WM_TIMER:
        if (wParam == kShowTimer) ShowNow();
        return 0;
    case WM_SIZE:
        if (s_viewHwnd) MoveWindow(s_viewHwnd, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
        return 0;
    case WM_SETFOCUS:
        if (s_viewHwnd) SetFocus(s_viewHwnd);
        return 0;
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(hWnd, &r);
        HBRUSH b = CreateSolidBrush(PageBackground());
        FillRect((HDC)wParam, &r, b);
        DeleteObject(b);
        return 1;
    }
    case WM_GETMINMAXINFO: {
        const UINT dpi = GetDpiForWindow(hWnd);
        auto* mm = (MINMAXINFO*)lParam;
        mm->ptMinTrackSize = { MulDiv(780, (int)dpi, 96), MulDiv(520, (int)dpi, 96) };
        return 0;
    }
    case WM_SET_COMMAND: {
        std::unique_ptr<std::wstring> url((std::wstring*)lParam);
        if (wParam == 1) { CreateTab(*url); if (g_hWnd) SetForegroundWindow(g_hWnd); }
        else OnCommand(*url);
        return 0;
    }
    case WM_SET_REBUILD:
        s_rebuildPosted = false;
        BuildPages();
        if (s_openPage >= (int)g_pages.size()) s_openPage = 0;
        Push();
        return 0;
    case WM_SET_PUSH:
        s_pushPosted = false;
        Push();
        return 0;
    case WM_DESTROY:
        KillTimer(hWnd, kShowTimer);
        DestroyView();
        g_setHwnd = nullptr;
        if (g_hWnd && IsWindowEnabled(g_hWnd)) SetForegroundWindow(g_hWnd);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ---- public -------------------------------------------------------------------------------------
void ShowSettingsWindow() {
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
        wc.lpszClassName = kSettingsClass;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = (HICON)SendMessageW(g_hWnd, WM_GETICON, ICON_BIG, 0);     // Lyra's icon
        wc.hIconSm = (HICON)SendMessageW(g_hWnd, WM_GETICON, ICON_SMALL, 0);
        if (!RegisterClassExW(&wc)) return;
        registered = true;
    }
    BuildPages();
    if (s_openPage >= (int)g_pages.size()) s_openPage = 0;
    const int w = MulDiv(1080, (int)g_dpi, 96), h = MulDiv(760, (int)g_dpi, 96);
    RECT pr = {};
    GetWindowRect(g_hWnd, &pr);
    const int x = pr.left + ((pr.right - pr.left) - w) / 2;
    const int y = pr.top + ((pr.bottom - pr.top) - h) / 2;
    // Created hidden: it appears when the page is drawn (settings-ready), so it never flashes white.
    CreateWindowExW(0, kSettingsClass, Tr(L"Lyra Settings", L"Настройки Lyra"), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                    x, y, w, h, g_hWnd, nullptr, hi, nullptr);
}

void SettingsOpenPage(int index) {
    if (index < 0) return;
    s_openPage = index;
    if (!g_setHwnd) { ShowSettingsWindow(); return; }
    if (s_ready) RunScript("window.lyraOpenPage && lyraOpenPage(" + std::to_string(index) + ")");
    ShowSettingsWindow();
}

void SettingsLanguageChanged() {
    if (!g_setHwnd || s_rebuildPosted) return;
    s_rebuildPosted = true;                 // later: the caller may be a row of the pages being rebuilt
    PostMessageW(g_setHwnd, WM_SET_REBUILD, 0, 0);
    SetWindowTextW(g_setHwnd, Tr(L"Lyra Settings", L"Настройки Lyra"));
}

void SettingsRefresh() {
    if (!g_setHwnd || s_pushPosted) return;
    s_pushPosted = true;
    PostMessageW(g_setHwnd, WM_SET_PUSH, 0, 0);
}

void SettingsThemeChanged() {
    ApplyFrameLook();
    if (g_setHwnd) InvalidateRect(g_setHwnd, nullptr, TRUE);
    SettingsRefresh();
}

bool SettingsAnimStep(float) { return false; }     // the page animates itself
