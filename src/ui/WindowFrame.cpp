// Lyra — окна без системного заголовка: рамка, перетаскивание, растягивание за верхний край.
#include "ui/WindowFrame.h"

int FrameThicknessY(HWND hWnd) {
    const UINT dpi = GetDpiForWindow(hWnd);
    return GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

LRESULT FrameCalcSize(HWND hWnd, WPARAM wParam, LPARAM lParam, bool fullscreen) {
    NCCALCSIZE_PARAMS* p = (NCCALCSIZE_PARAMS*)lParam;
    const LONG top = p->rgrc[0].top;
    LRESULT r = DefWindowProcW(hWnd, WM_NCCALCSIZE, wParam, lParam);
    p->rgrc[0].top = top;
    // A maximised window extends past the monitor by the frame thickness.
    if (IsZoomed(hWnd) && !fullscreen) p->rgrc[0].top += FrameThicknessY(hWnd);
    return r;
}

LRESULT FrameTopResizeHit(HWND hWnd, POINT pt, float bandPx, float cornerPx) {
    if (IsZoomed(hWnd) || pt.y < 0 || pt.y >= (LONG)bandPx) return HTNOWHERE;
    RECT cr;
    GetClientRect(hWnd, &cr);
    if (pt.x < (LONG)cornerPx) return HTTOPLEFT;
    if (pt.x >= cr.right - (LONG)cornerPx) return HTTOPRIGHT;
    return HTTOP;
}

void FrameApply(HWND hWnd) {
    SetWindowPos(hWnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}
