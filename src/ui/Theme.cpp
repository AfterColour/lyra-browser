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
        t.frame       = Color(232, 235, 240);
        t.page        = Color(250, 251, 252);
        t.field       = Color(255, 255, 255);
        t.fieldBorder = Color(211, 216, 223);
        t.toolbar   = t.frame;
        t.tabbar    = Color(223, 227, 233);
        t.tabActive = t.field;
        t.tabHover  = Color(18, 0, 0, 0);
        t.tabBorder = t.fieldBorder;
        t.sep       = Color(214, 218, 224);
        t.pill      = t.field;
        t.pillHover = Color(246, 247, 249);
        t.pillFocus = t.field;
        t.text      = Color(27, 29, 33);
        t.text2     = Color(89, 96, 107);
        t.icon      = Color(52, 57, 65);
        t.iconDis   = Color(166, 172, 181);
        t.accent    = Color(0, 103, 192);
        t.hover     = Color(20, 0, 0, 0);
        t.pressed   = Color(36, 0, 0, 0);
        t.progressAlpha = 60;
    } else {
        t.frame       = Color(32, 34, 39);
        t.page        = Color(24, 25, 28);
        t.field       = Color(46, 48, 54);
        t.fieldBorder = Color(59, 62, 69);
        t.toolbar   = t.frame;
        t.tabbar    = Color(27, 29, 33);
        t.tabActive = Color(52, 55, 62);
        t.tabHover  = Color(22, 255, 255, 255);
        t.tabBorder = t.fieldBorder;
        t.sep       = Color(46, 48, 54);
        t.pill      = t.field;
        t.pillHover = Color(52, 55, 61);
        t.pillFocus = t.field;
        t.text      = Color(236, 237, 239);
        t.text2     = Color(166, 171, 180);
        t.icon      = Color(213, 216, 221);
        t.iconDis   = Color(94, 99, 107);
        t.accent    = Color(76, 160, 255);
        t.hover     = Color(24, 255, 255, 255);
        t.pressed   = Color(40, 255, 255, 255);
        t.progressAlpha = 75;
    }
    t.badgeText    = Color(255, 255, 255);
    t.closeHover   = Color(196, 43, 28);
    t.closePressed = Color(170, 38, 25);
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

Color WindowsAccentColor() {
    // HKCU\Software\Microsoft\Windows\DWM\AccentColor holds the accent as 0xAABBGGRR.
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor",
                     RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS)
        return Color((BYTE)(v & 0xFF), (BYTE)((v >> 8) & 0xFF), (BYTE)((v >> 16) & 0xFF));
    DWORD argb = 0;
    BOOL opaque = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(&argb, &opaque)))
        return Color((BYTE)((argb >> 16) & 0xFF), (BYTE)((argb >> 8) & 0xFF), (BYTE)(argb & 0xFF));
    return Color(kAccents[0].r, kAccents[0].g, kAccents[0].b);
}

// Light accents (yellow, pale Windows colours) are hard to read on white: darken them in the light
// theme, and lighten dark ones in the dark theme, keeping the hue.
static Color ReadableAccent(const Color& c, bool dark) {
    const float lum = (0.2126f * c.GetR() + 0.7152f * c.GetG() + 0.0722f * c.GetB()) / 255.f;
    float k = 1.f;
    if (!dark && lum > 0.45f) k = 0.45f / lum;              // darken
    if (dark && lum < 0.35f) {                              // lighten towards white
        const float t = std::min(0.5f, 0.35f - lum + 0.15f);
        return Color((BYTE)(c.GetR() + (255 - c.GetR()) * t), (BYTE)(c.GetG() + (255 - c.GetG()) * t),
                     (BYTE)(c.GetB() + (255 - c.GetB()) * t));
    }
    return Color((BYTE)(c.GetR() * k), (BYTE)(c.GetG() * k), (BYTE)(c.GetB() * k));
}

void ApplyDwm(HWND hwnd) {
    if (!hwnd) return;
    BOOL d = g_dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, kDwmDarkMode, &d, sizeof(d));
    DwmSetWindowAttribute(hwnd, kDwmDarkModeOld, &d, sizeof(d));
    // Windows 11: the window border (and the title bar of windows that keep one, such as
    // the settings window) blends into the browser chrome.
    COLORREF cap = ToColorRef(g_th.frame);
    COLORREF txt = ToColorRef(g_th.text);
    DwmSetWindowAttribute(hwnd, kDwmCaptionColor, &cap, sizeof(cap));
    DwmSetWindowAttribute(hwnd, kDwmBorderColor, &cap, sizeof(cap));
    DwmSetWindowAttribute(hwnd, kDwmTextColor, &txt, sizeof(txt));
}

void ApplyTheme(bool dark) {
    g_dark = dark;
    g_th = MakeTheme(dark);
    Color accent = WindowsAccentColor();
    if (g_prefs.accent > 0 && g_prefs.accent < kAccentCount) {
        const AccentDef& a = kAccents[g_prefs.accent];
        accent = Color(a.r, a.g, a.b);
    }
    g_th.accent = ReadableAccent(accent, dark);
    if (g_editBrush) DeleteObject(g_editBrush);
    g_editBrush = CreateSolidBrush(ToColorRef(g_th.pillFocus));

    if (g_hWnd) {
        ApplyDwm(g_hWnd);
        InvalidateRect(g_hWnd, nullptr, FALSE);
        if (g_urlEdit) InvalidateRect(g_urlEdit, nullptr, TRUE);
    }
    SettingsThemeChanged();
}
