// Lyra — окна без системного заголовка: рамка, перетаскивание, растягивание за верхний край.
#pragma once

#include "common/Platform.h"

// Height of the resize frame that Windows adds on each side of a window.
int FrameThicknessY(HWND hWnd);

// WM_NCCALCSIZE (wParam != 0) for a window whose title bar is drawn by Lyra: keeps the default
// left/right/bottom resize borders and drops the title bar.
LRESULT FrameCalcSize(HWND hWnd, WPARAM wParam, LPARAM lParam, bool fullscreen = false);

// HTTOP / HTTOPLEFT / HTTOPRIGHT for a client point in the top resize band, otherwise HTNOWHERE.
LRESULT FrameTopResizeHit(HWND hWnd, POINT clientPt, float bandPx, float cornerPx);

inline bool IsCaptionCode(WPARAM code) { return code == HTMINBUTTON || code == HTMAXBUTTON || code == HTCLOSE; }

// Applies the custom frame right after the window was created (before it is shown).
void FrameApply(HWND hWnd);

// A single-line edit control placed in the top band of such a window. DWM shows plain GDI
// drawing there as see-through (grey), so the edit is a layered child with constant full
// opacity (Windows 8+); if that is refused it falls back to a normal edit.
HWND CreateOpaqueEdit(HWND parent, int id, HINSTANCE instance);
