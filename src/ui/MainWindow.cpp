// Lyra — главное окно: оконная процедура, размещение страниц.
#include "ui/MainWindow.h"
#include "browser/AdBlock.h"
#include "browser/Favicons.h"
#include "browser/History.h"
#include "ui/Suggestions.h"
#include "browser/StartPage.h"
#include "app/Log.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "core/Constants.h"
#include "core/Settings.h"
#include "ui/AddressBar.h"
#include "ui/BookmarkPopup.h"
#include "ui/DownloadsPopup.h"
#include "ui/TabTransition.h"
#include "browser/Downloads.h"
#include "ui/Animation.h"
#include "ui/Chrome.h"
#include "ui/Commands.h"
#include "ui/Dialogs.h"
#include "core/Localization.h"
#include "ui/FullScreen.h"
#include "ui/Theme.h"
#include "ui/WindowFrame.h"

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
static bool   g_trackingNc = false;      // TME_NONCLIENT tracking for the window buttons

void LayoutWebViews() {
    if (!g_hWnd) return;
    RECT cr;
    GetClientRect(g_hWnd, &cr);
    // fullscreen: the page covers the tab strip, toolbar and vertical tab list
    int top = g_fullScreenView ? 0 : (int)g_L.chromeH;
    int left = g_fullScreenView ? 0 : (int)g_L.sideW;
    int h = std::max(0, (int)cr.bottom - top);
    // The active view in place; the "ready" tabs parked off-screen at the same size, the rest
    // hidden (they get the right size when they are selected, so a window resize does not send
    // resize IPC to every background page).
    const int w = std::max(0, (int)cr.right - left);
    if (Tab* t = ActiveTab())
        if (t->hwnd) MoveWindow(t->hwnd, left, top, w, h, TRUE);
    if (!g_fullScreenView) ParkBackgroundViews(w, h);
}

void RefreshLayout(bool animate) {
    RECT cr;
    GetClientRect(g_hWnd, &cr);
    ComputeLayout(cr.right, animate);
    UpdateMainBackdrop();
    PositionEdit();
    LayoutWebViews();
    InvalidateChrome();
}

void FocusWebView() {
    if (Tab* t = ActiveTab()) if (t->hwnd) SetFocus(t->hwnd);
}

// Re-reads the toolbar size / tab layout settings: fonts, layout and a full repaint.
void ApplyChromeSettings() {
    if (!g_hWnd) return;
    RebuildResources();
    RefreshLayout();
    InvalidateRect(g_hWnd, nullptr, FALSE);
}

// ----------------------------------------------------------------------------
//  Custom title bar
//  The client area covers the whole window except the left/right/bottom resize borders, so the
//  tabs can sit where the title bar was. Empty chrome reports HTCAPTION (drag, double-click to
//  maximise, right-click system menu) and the three window buttons report HTMINBUTTON /
//  HTMAXBUTTON / HTCLOSE, which also gives the Windows 11 snap layouts flyout on maximise.
// ----------------------------------------------------------------------------
// Hover follows the pointer even when the chrome moves under a still mouse (tabs sliding after a
// close): re-run the hit test at the current cursor position.
static void RehoverFromCursor(HWND hWnd) {
    if (GetCapture()) return;
    POINT pt;
    if (!GetCursorPos(&pt) || WindowFromPoint(pt) != hWnd) return;
    ScreenToClient(hWnd, &pt);
    Hit h = HitTest(pt.x, pt.y);
    if (h.IsCaptionButton()) return;                    // handled by the non-client messages
    SetHover(h);
}

// Tabs keep their width after a close with the mouse until the pointer has been outside the tab
// strip for Settings > Animations > "Tab resize delay".
static bool s_unfreezeArmed = false;
static void CheckTabUnfreeze(HWND hWnd) {
    if (!TabWidthsFrozen()) return;
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(hWnd, &pt);
    const bool inStrip = pt.x >= 0 && pt.x < g_clientW && pt.y >= 0 && pt.y < (LONG)TabStripBottom();
    if (inStrip) {
        if (s_unfreezeArmed) { KillTimer(hWnd, TIMER_TAB_UNFREEZE); s_unfreezeArmed = false; }
    } else if (!s_unfreezeArmed) {
        SetTimer(hWnd, TIMER_TAB_UNFREEZE, (UINT)std::max(1, g_prefs.tabResizeDelay), nullptr);
        s_unfreezeArmed = true;
    }
}

static void CloseTabWithMouse(HWND hWnd, int index) {
    FreezeTabWidths();
    CloseTab(index);
    RehoverFromCursor(hWnd);
    CheckTabUnfreeze(hWnd);
}

static Hit HitFromScreen(HWND hWnd, LPARAM lParam) {
    POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    ScreenToClient(hWnd, &pt);
    return HitTest(pt.x, pt.y);
}

static void RunCaptionButton(HWND hWnd, Hit::Kind k) {
    switch (k) {
    case Hit::CapMin:   SendMessageW(hWnd, WM_SYSCOMMAND, SC_MINIMIZE, 0); break;
    case Hit::CapMax:   SendMessageW(hWnd, WM_SYSCOMMAND, IsZoomed(hWnd) ? SC_RESTORE : SC_MAXIMIZE, 0); break;
    case Hit::CapClose: SendMessageW(hWnd, WM_SYSCOMMAND, SC_CLOSE, 0); break;
    default: break;
    }
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
        g_urlEdit = CreateOpaqueEdit(hWnd, ID_URL_EDIT, (HINSTANCE)GetWindowLongPtrW(hWnd, GWLP_HINSTANCE));
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
        UnfreezeTabWidths();
        SuggestHide();
        BookmarkPopupClose();
        DownloadsPopupClose();
        TransitionCancel();
        if (wParam != SIZE_MINIMIZED) RefreshLayout();
        return 0;
    case WM_MOVE:
        SuggestHide();
        BookmarkPopupClose();
        DownloadsPopupClose();
        TransitionCancel();
        return 0;

    case WM_NCCALCSIZE:
        if (wParam) return FrameCalcSize(hWnd, wParam, lParam, g_fullScreenView != nullptr);
        break;

    case WM_NCHITTEST: {
        LRESULT def = DefWindowProcW(hWnd, msg, wParam, lParam);
        if (def != HTCLIENT) return def;                 // left / right / bottom resize borders
        if (g_fullScreenView) return HTCLIENT;
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hWnd, &pt);
        if (LRESULT top = FrameTopResizeHit(hWnd, pt, S(5), S(16))) return top;   // top resize border
        Hit h = HitTest(pt.x, pt.y);
        switch (h.kind) {
        case Hit::CapMin:   return HTMINBUTTON;
        case Hit::CapMax:   return HTMAXBUTTON;
        case Hit::CapClose: return HTCLOSE;
        case Hit::None:     if (pt.y >= 0 && pt.y < (LONG)g_L.chromeH) return HTCAPTION; break;
        default: break;
        }
        return HTCLIENT;
    }
    case WM_NCMOUSEMOVE: {
        Hit h = IsCaptionCode(wParam) ? HitFromScreen(hWnd, lParam) : Hit();
        SetHover(h);
        if (!g_trackingNc) {
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE | TME_NONCLIENT, hWnd, 0 };
            TrackMouseEvent(&t);
            g_trackingNc = true;
        }
        CheckTabUnfreeze(hWnd);
        // Over our window buttons DefWindowProc would paint the hover state of the hidden system
        // buttons on top of ours ("ghost" buttons): do not pass the message on.
        if (IsCaptionCode(wParam)) return 0;
        break;
    }
    case WM_NCMOUSELEAVE:
        g_trackingNc = false;
        if (g_hover.IsCaptionButton() || g_down.IsCaptionButton()) {
            g_down = Hit();
            SetHover(Hit());
            InvalidateChrome();
        }
        CheckTabUnfreeze(hWnd);
        break;
    case WM_NCLBUTTONDOWN:
        if (IsCaptionCode(wParam)) {
            g_down = HitFromScreen(hWnd, lParam);
            InvalidateChrome();
            return 0;
        }
        break;
    case WM_NCLBUTTONUP:
        if (IsCaptionCode(wParam)) {
            Hit up = HitFromScreen(hWnd, lParam);
            Hit down = g_down;
            g_down = Hit();
            InvalidateChrome();
            if (up == down) RunCaptionButton(hWnd, up.kind);
            return 0;
        }
        break;
    case WM_NCLBUTTONDBLCLK:
        if (IsCaptionCode(wParam)) return 0;            // not "maximise" when double-clicking a button
        break;
    case WM_NCRBUTTONDOWN:
        if (IsCaptionCode(wParam)) return 0;
        break;
    case WM_NCRBUTTONUP:
        if (IsCaptionCode(wParam)) return 0;
        break;
    case WM_NCACTIVATE:
        // lParam -1: let Windows update the activation state without repainting a title bar.
        return DefWindowProcW(hWnd, msg, wParam, -1);

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
    case WM_DWMCOLORIZATIONCOLORCHANGED:              // Windows accent colour changed
        ApplyTheme(ResolveDark());
        break;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        PaintChrome(hdc, ps.rcPaint);       // chrome + the area under the web view (seen before it paints)
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
        if ((HWND)lParam == g_urlEdit && HIWORD(wParam) == EN_CHANGE) {
            SuggestUpdate();
            return 0;
        }
        if ((HWND)lParam == g_urlEdit && HIWORD(wParam) == EN_KILLFOCUS) {
            SuggestHide();
            ShowWindow(g_urlEdit, SW_HIDE);
            InvalidateChrome();
            return 0;
        }
        break;

    case WM_MOUSEMOVE: {
        Hit h = HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        SetHover(h);
        if (!g_trackingMouse) {
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&t);
            g_trackingMouse = true;
        }
        CheckTabUnfreeze(hWnd);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_trackingMouse = false;
        SetHover(Hit());
        CheckTabUnfreeze(hWnd);
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
            case Hit::Star:     BookmarkCurrentPage(); break;
            case Hit::Downloads: DownloadsPopupToggle(); break;
            case Hit::More:     ShowMoreMenu(); break;
            case Hit::NewTab:   RunCommand(CMD_NEWTAB); break;
            case Hit::TabClose: CloseTabWithMouse(hWnd, up.index); break;
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
        if (h.kind == Hit::TabBody || h.kind == Hit::TabClose) CloseTabWithMouse(hWnd, h.index);
        return 0;
    }
    case WM_APP_ANIM_FRAME:
        AnimOnFrame();
        RehoverFromCursor(hWnd);
        return 0;

    case WM_TIMER: {
        if (wParam == TIMER_TRANSITION) {
            TransitionTick();
            return 0;
        }
        if (wParam == TIMER_SPARE_TAB) {
            KillTimer(hWnd, TIMER_SPARE_TAB);
            if (g_tabOp || g_modalDepth > 0) ScheduleSpareTab(1000);
            else PrepareSpareTab();
            return 0;
        }
        if (wParam == TIMER_TAB_UNFREEZE) {
            KillTimer(hWnd, TIMER_TAB_UNFREEZE);
            s_unfreezeArmed = false;
            UnfreezeTabWidths();
            RefreshLayout(true);
            RehoverFromCursor(hWnd);
            return 0;
        }
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
        HistorySaveSoon();
        DownloadsTick();
        {
            // The downloads button: the finished glow goes off. (Moving parts run on the
            // animation clock, see ChromeAnimStep; without animations they step here.)
            static bool glowing = false;
            const bool glow = DownloadsJustFinished();
            const bool sliding = DownloadsRunning() > 0 && DownloadsProgress() < 0;
            if (glow != glowing) { dirty = true; glowing = glow; }
            if (sliding && g_tick % 3 == 0) {
                if (!g_prefs.animations) dirty = true;
                DownloadsPopupRefresh();
            }
            if (g_prefs.animations && (sliding || (g_prefs.loadIndicator == 0 && act && act->loading))) AnimRequest();
        }
        if (g_tick % 32 == 0) ExpireReadyTabs();
        if (g_tick % 32 == 0) {
            for (auto& tp : g_tabs)
                if (SyncTab(*tp)) dirty = true;
        }
        if (dirty) InvalidateChrome();
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) SuggestHide();
        else if (!IsEditing()) FocusWebView();
        break;

    case WM_APP_CLOSE_TAB: {             // window.close(), or an empty popup that became a download
        int index = -1;
        if (FindTab((unsigned long long)wParam, &index)) CloseTab(index);
        return 0;
    }
    case WM_APP_TAB_PAINTED:
        TransitionPainted((unsigned long long)wParam);
        return 0;
    case WM_APP_ADBLOCK:
        AdBlockListsReady(lParam);
        return 0;
    case WM_APP_FAVICON:
        FaviconArrived(lParam);
        InvalidateChrome();
        return 0;
    case WM_APP_LYRA_URL: {
        std::unique_ptr<std::wstring> url((std::wstring*)lParam);
        if (url && !g_closing) HandleLyraUrl((unsigned long long)wParam, *url);
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
        if (const int running = DownloadsRunning()) {
            // Closing stops them: ask first.
            std::wstring msg = std::wstring(Tr(L"Downloads in progress: ", L"Идут загрузки: ")) + std::to_wstring(running) +
                               Tr(L".\n\nClose Lyra and stop them?", L".\n\nЗакрыть Lyra и прервать их?");
            int r;
            { ModalScope modal; r = MessageBoxW(hWnd, msg.c_str(), kAppName, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2); }
            if (r != IDYES) { g_closeRequested = false; return 0; }
        }
        Log("WM_CLOSE: shutting down (tabs=%d active=%d)", (int)g_tabs.size(), g_active);
        g_closing = true;
        KillTimer(hWnd, TIMER_UI);
        SuggestHide();
        DownloadsPopupClose();
        BookmarkPopupClose();
        CloseAllTabs();
        HistorySave();
        DownloadsSave();
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hWnd, TIMER_UI);
        KillTimer(hWnd, TIMER_TAB_UNFREEZE);
        AnimShutdown();
        if (g_editBrush) { DeleteObject(g_editBrush); g_editBrush = nullptr; }
        if (g_editFont) { DeleteObject(g_editFont); g_editFont = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}
