// Lyra — цвета интерфейса, акцент, светлая/тёмная тема, оформление заголовка окна (DWM).
#include "ui/Theme.h"
#include "browser/CustomThemes.h"
#include "ui/Crossfade.h"
#include "ui/Chrome.h"
#include "browser/StartPage.h"
#include "browser/TabManager.h"
#include "core/Settings.h"
#include "ui/AddressBar.h"
#include "ui/MainWindow.h"
#include "ui/UiLayout.h"
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
    t.popup        = dark ? Color(41, 43, 48) : Color(255, 255, 255);
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
    if (CustomThemeColorsOn()) return CustomThemeActive()->dark;
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

bool MicaSupported() {
    static int supported = -1;
    if (supported < 0) {
        // DWMWA_SYSTEMBACKDROP_TYPE needs Windows 11 22H2 (build 22621). RtlGetVersion is not
        // affected by the manifest-based version lie of GetVersionEx.
        typedef LONG (WINAPI *RtlGetVersionFn)(OSVERSIONINFOW*);
        OSVERSIONINFOW v = { sizeof(v) };
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        RtlGetVersionFn fn = nt ? (RtlGetVersionFn)GetProcAddress(nt, "RtlGetVersion") : nullptr;
        supported = (fn && fn(&v) == 0 && v.dwMajorVersion >= 10 && v.dwBuildNumber >= 22621) ? 1 : 0;
    }
    return supported == 1;
}

bool MicaActive() {
    HIGHCONTRASTW hc = { sizeof(hc) };
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON)) return false;
    return g_prefs.mica && MicaSupported() && !CustomThemeColorsOn()      // a theme brings its own colours
        && !UiFrameColorSet();                                            // ... and so may the layout (ui/UiLayout)
}

void ApplyBackdrop(HWND hwnd, int type, MARGINS margins) {
    if (!hwnd || !MicaSupported()) return;
    const bool on = MicaActive();
    const MARGINS none = { 0, 0, 0, 0 };
    DwmExtendFrameIntoClientArea(hwnd, on ? &margins : &none);
    const int backdrop = on ? type : 1;            // 1 = DWMSBT_NONE
    DwmSetWindowAttribute(hwnd, 38 /* DWMWA_SYSTEMBACKDROP_TYPE */, &backdrop, sizeof(backdrop));
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

static Color Mix(const Color& a, const Color& b, float t) {
    auto m = [t](BYTE x, BYTE y) { return (BYTE)std::lround(x + (y - x) * t); };
    return Color(m(a.GetR(), b.GetR()), m(a.GetG(), b.GetG()), m(a.GetB(), b.GetB()));
}
static Color WithAlpha(const Color& c, BYTE a) { return Color(a, c.GetR(), c.GetG(), c.GetB()); }
static float Lum(const Color& c) { return (0.2126f * c.GetR() + 0.7152f * c.GetG() + 0.0722f * c.GetB()) / 255.f; }

// A theme's five colours spread over Lyra's palette: background (or the accent, when the
// theme puts it on the window) for the chrome, foreground for text and icons, highlight as the
// accent, the accent colour for the active tab.
static void ApplyThemeColors(Theme& t, const CustomTheme& v) {
    const Color frame = v.accentOnWindow ? v.accent : v.bg;
    t.frame = t.toolbar = frame;
    t.tabbar = v.accentOnWindow ? v.accent : Mix(v.bg, Color(0, 0, 0), v.dark ? 0.25f : 0.06f);
    t.page = v.hasWindow ? v.window : v.bg;
    t.field = t.pill = t.pillFocus = Mix(frame, v.fg, 0.10f);
    t.fieldBorder = t.tabBorder = Mix(frame, v.fg, 0.20f);
    t.pillHover = Mix(frame, v.fg, 0.15f);
    t.tabActive = (!v.accentOnWindow && std::fabs(Lum(v.accent) - Lum(frame)) > 0.04f) ? v.accent : Mix(frame, v.fg, 0.14f);
    t.tabHover = WithAlpha(v.fg, 22);
    t.sep = Mix(frame, v.fg, 0.12f);
    t.text = v.fg;
    t.text2 = Mix(v.fg, frame, 0.35f);
    t.icon = Mix(v.fg, frame, 0.10f);
    t.iconDis = Mix(v.fg, frame, 0.62f);
    t.accent = v.highlight;
    t.hover = WithAlpha(v.fg, v.dark ? 26 : 22);
    t.pressed = WithAlpha(v.fg, v.dark ? 44 : 38);
    t.popup = Mix(v.bg, v.fg, 0.06f);
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

static Theme s_baseTheme;                 // g_th before the layout's colours (ThemeReapplyUiColors)
static bool s_haveBase = false;

// The layout's colours changed (ui/Customize): the same theme with the new colours over it, without
// the crossfade and the pages' refresh ApplyTheme does.
void ThemeReapplyUiColors() {
    if (!s_haveBase) return;              // (ApplyTheme has not run yet)
    g_th = s_baseTheme;
    UiApplyColors(g_th);
    if (g_editBrush) DeleteObject(g_editBrush);
    g_editBrush = CreateSolidBrush(ToColorRef(g_th.pillFocus));
    if (g_hWnd) {
        ApplyDwm(g_hWnd);
        UpdateMainBackdrop();
        InvalidateRect(g_hWnd, nullptr, FALSE);
        if (g_urlEdit) InvalidateRect(g_urlEdit, nullptr, TRUE);
    }
}

void ApplyTheme(bool dark) {
    if (g_hWnd) CrossfadeBegin();         // light/dark and accent changes dissolve too
    const CustomTheme* theme = CustomThemeActive();
    const bool colors = theme && theme->useColors;
    if (colors) dark = theme->dark;
    g_dark = dark;
    g_th = MakeTheme(dark);
    if (colors) ApplyThemeColors(g_th, *theme);
    if (theme && theme->useAccent) {
        g_th.accent = colors ? theme->highlight : ReadableAccent(theme->highlight, dark);
    } else {
        Color accent = WindowsAccentColor();
        if (g_prefs.accent > 0 && g_prefs.accent < kAccentCount) {
            const AccentDef& a = kAccents[g_prefs.accent];
            accent = Color(a.r, a.g, a.b);
        }
        g_th.accent = ReadableAccent(accent, dark);
    }
    s_baseTheme = g_th;
    s_haveBase = true;
    UiApplyColors(g_th);                 // the layout's own colours over all of it
    if (g_editBrush) DeleteObject(g_editBrush);
    g_editBrush = CreateSolidBrush(ToColorRef(g_th.pillFocus));

    if (g_hWnd) {
        ApplyDwm(g_hWnd);
        UpdateMainBackdrop();
        InvalidateRect(g_hWnd, nullptr, FALSE);
        if (g_urlEdit) InvalidateRect(g_urlEdit, nullptr, TRUE);
    }
    SettingsThemeChanged();
    TabsApplyAppearance();                // sites see it as prefers-color-scheme
    RefreshStartPages();                  // the start page follows the theme and accent
}
