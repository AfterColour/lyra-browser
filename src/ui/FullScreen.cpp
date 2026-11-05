// Lyra — полноэкранный режим для видео и элементов страницы.
#include "ui/FullScreen.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "ui/AddressBar.h"
#include "ui/MainWindow.h"

// Leaves element fullscreen on the window side. Called for WebKit's exit message and when the
// fullscreen tab goes away without WebKit telling us (tab closed, page crashed).
void RestoreFromFullScreen() {
    if (!g_fullScreenView) return;
    g_fullScreenView = nullptr;
    SetWindowLongPtrW(g_hWnd, GWL_STYLE, g_savedStyle);
    SetWindowPlacement(g_hWnd, &g_savedPlacement);
    SetWindowPos(g_hWnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    RefreshLayout();
    InvalidateRect(g_hWnd, nullptr, FALSE);
}

LRESULT OnFullScreenMessage(bool enter, HWND view) {
    if (!enter) {
        RestoreFromFullScreen();
        return TRUE;
    }
    if (g_fullScreenView) return TRUE;              // already fullscreen
    Tab* t = ActiveTab();
    if (!t || t->hwnd != view) return FALSE;        // a background tab: let WebKit handle it
    if (IsEditing()) ShowWindow(g_urlEdit, SW_HIDE);

    g_savedStyle = GetWindowLongPtrW(g_hWnd, GWL_STYLE);
    g_savedPlacement.length = sizeof(g_savedPlacement);
    GetWindowPlacement(g_hWnd, &g_savedPlacement);
    g_fullScreenView = view;

    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(g_hWnd, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& m = mi.rcMonitor;
    SetWindowLongPtrW(g_hWnd, GWL_STYLE, (g_savedStyle & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
    // WM_SIZE -> RefreshLayout() gives the view the whole client area (see LayoutWebViews).
    SetWindowPos(g_hWnd, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top,
                 SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    RefreshLayout();
    SetFocus(view);
    return TRUE;
}

// The page shown fullscreen must leave fullscreen before another tab is shown.
void ExitFullScreenForTabSwitch() {
    if (!g_fullScreenView) return;
    for (auto& tp : g_tabs) {
        if (tp->hwnd == g_fullScreenView && tp->page) {
            WKPageRequestExitFullScreen(tp->page);   // WebKit answers with the exit message
            break;
        }
    }
    RestoreFromFullScreen();                          // in case the page does not answer
}
