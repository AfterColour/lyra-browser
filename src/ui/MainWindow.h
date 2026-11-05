// Lyra — главное окно: оконная процедура, размещение страниц.
#pragma once

#include "common/Platform.h"

extern HWND g_hWnd;

extern HWND g_fullScreenView;
extern LONG_PTR g_savedStyle;
extern WINDOWPLACEMENT g_savedPlacement;

extern UINT g_dpi;
extern int g_clientW;

void LayoutWebViews();
void RefreshLayout();
void FocusWebView();
LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
