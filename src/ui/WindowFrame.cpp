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

HWND CreateOpaqueEdit(HWND parent, int id, HINSTANCE instance) {
    const DWORD style = WS_CHILD | ES_AUTOHSCROLL | ES_LEFT;
    HWND h = CreateWindowExW(WS_EX_LAYERED, L"EDIT", L"", style, 0, 0, 10, 10, parent,
                             (HMENU)(INT_PTR)id, instance, nullptr);
    if (h && !SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA)) { DestroyWindow(h); h = nullptr; }
    if (!h) h = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, instance, nullptr);
    if (h) SetWindowTheme(h, L"", L"");          // no visual style: our WM_CTLCOLOREDIT colours apply
    return h;
}

void FrameSetSystemMenuStyle(HWND hWnd, bool withSystemMenu) {
    const LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
    const LONG_PTR wanted = withSystemMenu ? (style | WS_SYSMENU) : (style & ~(LONG_PTR)WS_SYSMENU);
    if (wanted == style) return;
    SetWindowLongPtrW(hWnd, GWL_STYLE, wanted);
    SetWindowPos(hWnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void ShowSystemMenu(HWND hWnd, POINT pt) {
    HMENU menu = GetSystemMenu(hWnd, FALSE);
    if (!menu) return;
    const bool zoomed = IsZoomed(hWnd) != FALSE, iconic = IsIconic(hWnd) != FALSE;
    EnableMenuItem(menu, SC_RESTORE, MF_BYCOMMAND | ((zoomed || iconic) ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, SC_MOVE, MF_BYCOMMAND | (zoomed ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(menu, SC_SIZE, MF_BYCOMMAND | (zoomed ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(menu, SC_MINIMIZE, MF_BYCOMMAND | MF_ENABLED);
    EnableMenuItem(menu, SC_MAXIMIZE, MF_BYCOMMAND | (zoomed ? MF_GRAYED : MF_ENABLED));
    SetMenuDefaultItem(menu, SC_CLOSE, FALSE);
    const UINT cmd = (UINT)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
                                          pt.x, pt.y, 0, hWnd, nullptr);
    if (cmd) PostMessageW(hWnd, WM_SYSCOMMAND, cmd, 0);
}
