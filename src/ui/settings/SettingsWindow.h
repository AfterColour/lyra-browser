// Lyra — окно настроек: отрисовка, прокрутка, мышь и клавиатура.
#pragma once

#include "common/Platform.h"
#include "ui/settings/SettingsModel.h"

extern HWND g_setHwnd;

// ---- state -----------------------------------------------------------------
constexpr int SET_ID_SEARCH = 2001;
constexpr int SET_ID_EDIT   = 2002;
static const wchar_t kSetClass[] = L"LyraSettingsWindow";

extern std::vector<SPage> g_pages;

struct SGeo { float W, H, sideW, headerH, vpY, vpH, colX, colW; };

void SettingsThemeChanged();
void ShowSettingsWindow();
