// Lyra — окна без системного заголовка: рамка, перетаскивание, растягивание за верхний край.
#include "ui/WindowFrame.h"

int FrameThicknessY(HWND hWnd) {
    const UINT dpi = GetDpiForWindow(hWnd);
    return GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

LRESULT FrameCalcSize(HWND hWnd, WPARAM wParam, LPARAM lParam, bool fullscreen) {
    NCCALCSIZE_PARAMS* p = (NCCALCSIZE_PARAMS*)lParam;
    const LONG top = p->rgrc[0].top, left = p->rgrc[0].left;
    LRESULT r = DefWindowProcW(hWnd, WM_NCCALCSIZE, wParam, lParam);
    p->rgrc[0].top = top;
    // A maximised window extends past the monitor by its frame thickness: the same amount Windows
    // just took off the left side (it depends on the window style, so measure it instead).
    if (IsZoomed(hWnd) && !fullscreen) {
        const LONG inset = p->rgrc[0].left - left;
        p->rgrc[0].top += inset > 0 ? inset : FrameThicknessY(hWnd);
    }
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

HWND CreateOpaqueEdit(HWND parent, int id, HINSTANCE instance) {
    const DWORD style = WS_CHILD | ES_AUTOHSCROLL | ES_LEFT;
    HWND h = CreateWindowExW(WS_EX_LAYERED, L"EDIT", L"", style, 0, 0, 10, 10, parent,
                             (HMENU)(INT_PTR)id, instance, nullptr);
    if (h && !SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA)) { DestroyWindow(h); h = nullptr; }
    if (!h) h = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, instance, nullptr);
    if (h) SetWindowTheme(h, L"", L"");          // no visual style: our WM_CTLCOLOREDIT colours apply
    return h;
}
