// Lyra — главное окно: оконная процедура, размещение страниц.
#include "ui/MainWindow.h"
#include "app/Log.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "core/Constants.h"
#include "ui/AddressBar.h"
#include "ui/Chrome.h"
#include "ui/Commands.h"
#include "ui/Dialogs.h"
#include "ui/FullScreen.h"
#include "ui/Theme.h"

HWND   g_hWnd = nullptr;

// Element fullscreen (video players). WebKit sends the registered message "WebKitFullScreen" to
// this window (wParam: TRUE enter / FALSE exit, lParam: the WKView's HWND); see OnFullScreenMessage.
static UINT   g_fullScreenMsg = 0;

HWND   g_fullScreenView = nullptr;      // WKView shown fullscreen, nullptr otherwise

LONG_PTR g_savedStyle = 0;

WINDOWPLACEMENT g_savedPlacement = { sizeof(WINDOWPLACEMENT) };

UINT   g_dpi = 96;

int    g_clientW = 0;

static int    g_tick = 0;

static bool   g_trackingMouse = false;

void LayoutWebViews() {
    if (!g_hWnd) return;
    RECT cr;
    GetClientRect(g_hWnd, &cr);
    int top = g_fullScreenView ? 0 : (int)g_L.chromeH;   // fullscreen: the page covers the tab strip and toolbar
    int h = std::max(0, (int)cr.bottom - top);
    // Only the visible tab is resized. Hidden tabs are brought to the right size
    // by SelectTab() (which calls this function) right after they are shown, so
    // a window resize no longer sends resize IPC to every background page.
    if (Tab* t = ActiveTab())
        if (t->hwnd) MoveWindow(t->hwnd, 0, top, cr.right, h, TRUE);
}

void RefreshLayout() {
    RECT cr;
    GetClientRect(g_hWnd, &cr);
    ComputeLayout(cr.right);
    PositionEdit();
    LayoutWebViews();
    InvalidateChrome();
}

void FocusWebView() {
    if (Tab* t = ActiveTab()) if (t->hwnd) SetFocus(t->hwnd);
}

// ----------------------------------------------------------------------------
//  Window procedure
// ----------------------------------------------------------------------------
LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_fullScreenMsg && msg == g_fullScreenMsg)
        return OnFullScreenMessage(wParam != 0, (HWND)lParam);
    switch (msg) {
    case WM_CREATE: {
        g_hWnd = hWnd;
        g_fullScreenMsg = RegisterWindowMessageW(L"WebKitFullScreen");
        g_dpi = GetDpiForWindow(hWnd);
        g_urlEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL | ES_LEFT,
                                    0, 0, 10, 10, hWnd, (HMENU)(INT_PTR)ID_URL_EDIT,
                                    (HINSTANCE)GetWindowLongPtrW(hWnd, GWLP_HINSTANCE), nullptr);
        g_oldEditProc = (WNDPROC)SetWindowLongPtrW(g_urlEdit, GWLP_WNDPROC, (LONG_PTR)UrlEditProc);
        RebuildResources();
        ApplyTheme(ResolveDark());
        SetTimer(hWnd, TIMER_UI, 30, nullptr);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = (LONG)S(720);
        mmi->ptMinTrackSize.y = (LONG)S(420);
        return 0;
    }
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) RefreshLayout();
        return 0;

    case WM_DPICHANGED: {
        g_dpi = HIWORD(wParam);
        RebuildResources();
        const RECT* r = (const RECT*)lParam;
        SetWindowPos(hWnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        RefreshLayout();
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (lParam && wcscmp((LPCWSTR)lParam, L"ImmersiveColorSet") == 0) {
            ApplyTheme(ResolveDark());
            EnableDarkMenus();
        }
        break;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT cr;
        GetClientRect(hWnd, &cr);
        // Area under the web views (only visible if there are no tabs yet)
        RECT content = { 0, (LONG)g_L.chromeH, cr.right, cr.bottom };
        HBRUSH dc = (HBRUSH)GetStockObject(DC_BRUSH);
        SetDCBrushColor(hdc, ToColorRef(g_th.toolbar));
        FillRect(hdc, &content, dc);
        PaintChrome(hdc, cr.right);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_CTLCOLOREDIT:
        if ((HWND)lParam == g_urlEdit) {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, ToColorRef(g_th.text));
            SetBkColor(hdc, ToColorRef(g_th.pillFocus));
            return (LRESULT)g_editBrush;
        }
        break;

    case WM_COMMAND:
        if ((HWND)lParam == g_urlEdit && HIWORD(wParam) == EN_KILLFOCUS) {
            ShowWindow(g_urlEdit, SW_HIDE);
            InvalidateChrome();
            return 0;
        }
        break;

    case WM_MOUSEMOVE: {
        Hit h = HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (!(h == g_hover)) { g_hover = h; InvalidateChrome(); }
        if (!g_trackingMouse) {
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&t);
            g_trackingMouse = true;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_trackingMouse = false;
        g_hover = Hit();
        InvalidateChrome();
        return 0;

    case WM_LBUTTONDOWN: {
        Hit h = HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        g_down = h;
        if (h.kind == Hit::Pill) { g_down = Hit(); ShowUrlEdit(); return 0; }
        if (h.kind == Hit::TabBody) SelectTab(h.index);
        if (h.kind != Hit::None) SetCapture(hWnd);
        InvalidateChrome();
        return 0;
    }
    case WM_LBUTTONUP: {
        Hit up = HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        Hit down = g_down;
        g_down = Hit();
        if (GetCapture() == hWnd) ReleaseCapture();
        if (up == down) {
            switch (up.kind) {
            case Hit::Back:     GoBack(); break;
            case Hit::Forward:  GoForward(); break;
            case Hit::Reload:   ReloadActive(true); break;
            case Hit::More:     ShowMoreMenu(); break;
            case Hit::NewTab:   RunCommand(CMD_NEWTAB); break;
            case Hit::TabClose: CloseTab(up.index); break;
            default: break;
            }
        }
        InvalidateChrome();
        return 0;
    }
    case WM_CAPTURECHANGED:
        g_down = Hit();
        InvalidateChrome();
        return 0;

    case WM_MBUTTONUP: {
        Hit h = HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (h.kind == Hit::TabBody || h.kind == Hit::TabClose) CloseTab(h.index);
        return 0;
    }
    case WM_TIMER: {
        if (wParam != TIMER_UI) break;
        if (g_closing || g_tabOp) return 0;      // never poll tabs while one is being created/destroyed
        bool dirty = false;
        ++g_tick;
        Tab* act = ActiveTab();
        for (auto& tp : g_tabs) {
            Tab& t = *tp;
            if (t.loading && t.page) {
                float target = std::max(0.08f, (float)WKPageGetEstimatedProgress(t.page));
                target = std::min(target, 0.92f);
                if (t.progress < target) { t.progress += (target - t.progress) * 0.25f; if (&t == act) dirty = true; }
            } else if (t.fade > 0.f) {
                t.progress += (1.f - t.progress) * 0.4f;
                t.fade = std::max(0.f, t.fade - 0.07f);
                if (&t == act) dirty = true;
            }
        }
        // Title / URL / history changes arrive through the page state client; this slow
        // poll (~1 s) is only a safety net.
        if (g_tick % 32 == 0) {
            for (auto& tp : g_tabs)
                if (SyncTab(*tp)) dirty = true;
        }
        if (dirty) InvalidateChrome();
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE && !IsEditing()) FocusWebView();
        break;

    case WM_APP_CLOSE_TAB: {             // window.close(), or an empty popup that became a download
        int index = -1;
        if (FindTab((unsigned long long)wParam, &index)) CloseTab(index);
        return 0;
    }
    case WM_APP_TAB_CRASHED:
        HandleTabCrashed((unsigned long long)wParam, (int)lParam);
        return 0;

    case WM_CLOSE:
        if (g_closing) return 0;         // repeated Ctrl+W / extra WM_CLOSE must not run teardown twice
        if (g_tabOp) {                   // arrived from a nested message loop inside a tab operation
            Log("WM_CLOSE during a tab operation -> re-posted");
            PostMessageW(hWnd, WM_CLOSE, 0, 0);
            return 0;
        }
        if (g_modalDepth > 0) {          // a page dialog (alert, file chooser...) is still open
            Log("WM_CLOSE while a page dialog is open -> ignored");
            g_closeRequested = false;
            return 0;
        }
        Log("WM_CLOSE: shutting down (tabs=%d active=%d)", (int)g_tabs.size(), g_active);
        g_closing = true;
        KillTimer(hWnd, TIMER_UI);
        CloseAllTabs();
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hWnd, TIMER_UI);
        if (g_editBrush) { DeleteObject(g_editBrush); g_editBrush = nullptr; }
        if (g_editFont) { DeleteObject(g_editFont); g_editFont = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}
