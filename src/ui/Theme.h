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
    Color toolbar, tabbar, tabActive, tabHover, tabBorder, sep;
    Color pill, pillHover, pillFocus;
    Color text, text2, icon, iconDis, accent, hover, pressed;
    int   progressAlpha = 70;
};
extern Theme g_th;

COLORREF ToColorRef(const Color& c);
float S(float v);
void EnableDarkMenus();
bool ResolveDark();
void ApplyDwm(HWND hwnd);
void ApplyTheme(bool dark);
