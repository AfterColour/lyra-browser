// Lyra — цвета интерфейса, акцент, светлая/тёмная тема, оформление заголовка окна (DWM).
#include "ui/Theme.h"
#include "core/Settings.h"
#include "ui/AddressBar.h"
#include "ui/MainWindow.h"
#include "ui/settings/SettingsWindow.h"

static Theme MakeTheme(bool dark);
static bool SystemUsesDarkMode();

bool   g_dark = false;

Theme g_th;

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

COLORREF ToColorRef(const Color& c) { return RGB(c.GetR(), c.GetG(), c.GetB()); }

// ----------------------------------------------------------------------------
//  Small helpers
// ----------------------------------------------------------------------------
float S(float v) { return v * (float)g_dpi / 96.0f; }

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
void EnableDarkMenus() {
    HMODULE ux = LoadLibraryW(L"uxtheme.dll");
    if (!ux) return;
    typedef int  (WINAPI *SetPreferredAppModeFn)(int);
    typedef void (WINAPI *FlushMenuThemesFn)();
    SetPreferredAppModeFn setMode = (SetPreferredAppModeFn)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    FlushMenuThemesFn flush = (FlushMenuThemesFn)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (setMode) setMode(1 /* AllowDark */);
    if (flush) flush();
}

bool ResolveDark() {
    return g_prefs.themeMode == 2 || (g_prefs.themeMode == 0 && SystemUsesDarkMode());
}

void ApplyDwm(HWND hwnd) {
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

void ApplyTheme(bool dark) {
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
