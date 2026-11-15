// Lyra — цвета интерфейса, акцент, светлая/тёмная тема, оформление заголовка окна (DWM).
#pragma once

#include "common/Platform.h"

// DWM attributes (own constants so they never clash with SDK enums)
constexpr DWORD kDwmDarkModeOld  = 19;
constexpr DWORD kDwmDarkMode     = 20;
constexpr DWORD kDwmBorderColor  = 34;
constexpr DWORD kDwmCaptionColor = 35;
constexpr DWORD kDwmTextColor    = 36;

extern bool g_dark;

// ----------------------------------------------------------------------------
//  Theme
// ----------------------------------------------------------------------------
struct Theme {
    // Window chrome
    Color frame;                    // background of the tab strip / toolbar / vertical tab list
    Color page;                     // shown where no page is drawn yet
    Color field, fieldBorder;       // address field and the active tab (separate / vertical layouts)
    Color badgeText;                // letter on a site badge
    Color closeHover, closePressed; // window close button
    // Shared with the settings window
    Color toolbar, tabbar, tabActive, tabHover, tabBorder, sep;
    Color pill, pillHover, pillFocus;
    Color text, text2, icon, iconDis, accent, hover, pressed;
    Color popup;                    // menus and small popups
    int   progressAlpha = 70;
};
extern Theme g_th;

COLORREF ToColorRef(const Color& c);
float S(float v);
void EnableDarkMenus();
bool ResolveDark();
// The accent colour picked in Windows Settings > Personalization > Colors.
Color WindowsAccentColor();
void ApplyDwm(HWND hwnd);

// Mica (Windows 11 22H2+): the system backdrop behind the window chrome.
bool MicaSupported();
bool MicaActive();                  // supported and turned on in Settings > Appearance
// Backdrop for `hwnd` (DWMSBT_* type: 2 main window / Mica, 3 transient / Acrylic) over the
// given band of the client area (MARGINS; -1 = whole window). No effect when Mica is off.
void ApplyBackdrop(HWND hwnd, int type, MARGINS margins);
void ApplyTheme(bool dark);
