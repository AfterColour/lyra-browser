// Lyra — главное окно: оконная процедура, размещение страниц.
#include "ui/MainWindow.h"
#include "browser/Translate.h"
#include "browser/Reader.h"
#include "browser/AdBlock.h"
#include "browser/MediaControls.h"
#include "browser/PictureInPicture.h"
#include "browser/Passwords.h"
#include "browser/Permissions.h"
#include "ui/PermissionPrompt.h"
#include "browser/Session.h"
#include "ui/FindBar.h"
#include "ui/Print.h"
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
#include "browser/Extensions.h"
#include "browser/ExtensionStore.h"
#include "ui/Animation.h"
#include "ui/Chrome.h"
#include "ui/Commands.h"
#include "ui/Dialogs.h"
#include "core/Localization.h"
#include "ui/FullScreen.h"
#include "ui/Crossfade.h"
#include "browser/TabSleep.h"
#include "browser/ContextMenu.h"
#include "browser/SplitView.h"
#include "ui/TabDrag.h"
#include "ui/Theme.h"
#include "ui/Menu.h"
#include "browser/CustomThemes.h"
#include "ui/WindowFrame.h"
#include "app/Updater.h"

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

// A tab pressed with the left button: once the pointer has moved a few pixels it is a drag, and
// dropping it on another tab opens the two in split view (browser/SplitView).
static unsigned long long s_tabPressId = 0;
static POINT s_tabPressPt = {};
static bool s_tabDragging = false;
static POINT s_tabLastPt = {};
static RectF s_tabPressRect;                     // where the grabbed tab (or half) was on screen when pressed

// The pressed tab as it is drawn now (before the press selects it and the tab list moves): its
// half of the split view's tab, the address field (compact layout, open tab), or its own place.
static RectF PressedTabRect(int hitIndex, int tabIndex) {
    if (hitIndex < 0 || hitIndex >= (int)g_L.tabs.size()) return RectF();
    if (g_L.mode == kLayoutCompact && hitIndex == g_active) return g_L.pill;
    RectF r = g_L.tabs[hitIndex];
    int li = -1, ri = -1;
    if (SplitStripPair(&li, &ri) && hitIndex == li && (tabIndex == li || tabIndex == ri)) {
        const float half = std::floor(r.Width / 2);
        r = RectF(tabIndex == li ? r.X : r.X + half, r.Y, tabIndex == li ? half : r.Width - half, r.Height);
    }
    return r;
}

// The tab list as it is seen: one item per tab, the split view's two tabs one item.
struct StripItem { int first, last; RectF r; };
static std::vector<StripItem> StripItems() {
    std::vector<StripItem> items;
    int li = -1, ri = -1;
    const bool pair = SplitStripPair(&li, &ri);
    const int n = (int)std::min(g_tabs.size(), g_L.tabs.size());
    for (int i = 0; i < n; ++i) {
        if (pair && i == ri) continue;                   // (with the left half)
        RectF r = (g_L.mode == kLayoutCompact && i == g_active) ? g_L.pill : g_L.tabs[i];
        if (pair && i == li) {
            if (g_L.mode == kLayoutCompact && (g_active == li || g_active == ri)) r = g_L.pill;
            items.push_back({ li, ri, r });
            continue;
        }
        if (r.Width < 1.f || r.Height < 1.f) continue;
        items.push_back({ i, i, r });
    }
    return items;
}

// Where the dragged tab (index `dragged`) would go with the pointer at (x, y): on the middle of
// another tab, that tab (split view; `right`: the dragged one goes right); at a tab's edge, in a
// gap or past the tabs, before the tab at *insertAt. Neither (-1) away from the tab list or where
// nothing would change.
static void TabDropAt(int dragged, int x, int y, int* target, bool* right, int* insertAt) {
    *target = -1;
    *insertAt = -1;
    *right = false;
    const std::vector<StripItem> items = StripItems();
    if (items.empty() || dragged < 0) return;
    const bool vertical = g_L.mode == kLayoutVertical;
    const float px = (float)x, py = (float)y;
    int li = -1, ri = -1;
    const bool pair = SplitStripPair(&li, &ri);
    const bool draggedInPair = pair && (dragged == li || dragged == ri);
    int insert = -1;
    for (const StripItem& it : items) {
        RectF hit = it.r;
        hit.Inflate(vertical ? 0.f : S(2), vertical ? S(1) : S(6));
        if (!hit.Contains(px, py)) continue;
        if (draggedInPair && it.first == li) return;     // over its own split view's tab: it stays
        if (it.first == dragged && it.last == dragged) return;   // over its own place
        const bool horiz = !vertical || g_tabs[it.first]->pinned;
        const float rel = horiz ? (px - it.r.X) / std::max(1.f, it.r.Width) : (py - it.r.Y) / std::max(1.f, it.r.Height);
        const bool canSplit = it.first == it.last && !g_tabs[it.first]->pinned && !g_tabs[dragged]->pinned;
        if (canSplit && rel >= 0.28f && rel <= 0.72f) {
            *target = it.first;
            *right = rel >= 0.5f;
            return;
        }
        insert = rel < 0.5f ? it.first : it.last + 1;
        break;
    }
    if (insert < 0) {                                    // between the tabs or past them, still in the list
        const RectF& first = items.front().r;
        const RectF& last = items.back().r;
        if (vertical) {
            if (px < 0 || px > g_L.sideW || py < first.Y - S(4) || py > last.GetBottom() + S(40)) return;
            for (const StripItem& it : items)
                if (py < it.r.Y + it.r.Height / 2 && !g_tabs[it.first]->pinned) { insert = it.first; break; }
            if (insert < 0) insert = (int)g_tabs.size();
        } else {
            if (py < first.Y - S(6) || py > first.GetBottom() + S(6)) return;
            if (px < first.X) insert = items.front().first;
            else if (px > last.GetRight()) insert = (int)g_tabs.size();
            else {
                for (const StripItem& it : items)
                    if (px < it.r.X + it.r.Width / 2) { insert = it.first; break; }
                if (insert < 0) insert = (int)g_tabs.size();
            }
        }
    }
    // Nothing would change: next to its own place (and on the same side of the pinned tabs).
    if (!draggedInPair && (insert == dragged || insert == dragged + 1)) {
        const int pinnedOthers = PinnedTabCount() - (g_tabs[dragged]->pinned ? 1 : 0);
        const int to = insert > dragged ? insert - 1 : insert;
        const bool pinned = to < pinnedOthers || (g_tabs[dragged]->pinned && to == pinnedOthers);
        if (pinned == g_tabs[dragged]->pinned) return;
    }
    *insertAt = insert;
}

static void TabDragMove(int x, int y) {
    if (!s_tabPressId) return;
    int pressed = -1;
    if (!FindTab(s_tabPressId, &pressed)) { s_tabPressId = 0; return; }
    if (!s_tabDragging) {
        if (std::abs(x - s_tabPressPt.x) < GetSystemMetrics(SM_CXDRAG) &&
            std::abs(y - s_tabPressPt.y) < GetSystemMetrics(SM_CYDRAG)) return;
        s_tabDragging = true;
        RectF from = s_tabPressRect;
        if (from.Width < 1.f) from = RectF((float)s_tabPressPt.x - S(80), (float)s_tabPressPt.y - S(16), S(160), S(32));
        TabDragStart(s_tabPressId, s_tabPressPt, from);
    }
    TabDragMoveTo({ x, y });
    s_tabLastPt = { x, y };
    int target = -1, insertAt = -1;
    bool right = false;
    TabDropAt(pressed, x, y, &target, &right, &insertAt);
    SplitTabDragOver(s_tabPressId, target, right);
    SetTabInsertHint(insertAt);
}

static bool TabDragEnd(bool drop) {
    const bool was = s_tabDragging;
    const unsigned long long pressedId = s_tabPressId;
    s_tabPressId = 0;
    s_tabDragging = false;
    if (!was) return false;
    // Where the lifted tab lands: on the tab it was dropped on, or back in its place.
    RectF into;
    bool onTarget = false;
    for (int i = 0; i < (int)g_tabs.size() && i < (int)g_L.tabs.size(); ++i)
        if (drop && SplitDropHint(i, nullptr)) { into = g_L.tabs[i]; onTarget = true; break; }
    // Dropped at a place in the tab list: the tab moves there (out of its split view, if it was in one).
    const int insertAt = TabInsertHint();
    SetTabInsertHint(-1);
    bool moved = false;
    if (drop && !onTarget && insertAt >= 0) {
        int index = -1;
        if (Tab* t = FindTab(pressedId, &index)) {
            int ins = insertAt;
            if (SplitPartnerOf(*t)) {                    // keep the place once the two are ordinary tabs again
                const unsigned long long before = ins < (int)g_tabs.size() ? g_tabs[ins]->id : 0;
                SplitDetach(pressedId);
                int b = -1;
                ins = before && FindTab(before, &b) ? b : (int)g_tabs.size();
            }
            DropTabAt(pressedId, ins);
            moved = true;
        }
    }
    // A half of the split view's tab dropped away from that tab (not at a place in the list):
    // pulled out, an ordinary tab again (the copy then flies to its own place in the list).
    if (drop && !onTarget && !moved) {
        int li = -1, ri = -1, pressed = -1;
        if (SplitStripPair(&li, &ri) && FindTab(pressedId, &pressed) && (pressed == li || pressed == ri)) {
            const bool compactOpen = g_L.mode == kLayoutCompact && (g_active == li || g_active == ri);
            RectF whole = compactOpen ? g_L.pill : g_L.tabs[li];
            whole.Inflate(S(6), S(6));
            if (!whole.Contains((float)s_tabLastPt.x, (float)s_tabLastPt.y)) SplitDetach(pressedId);
        }
    }
    TabDragFinish(onTarget ? &into : nullptr);
    SplitTabDragEnd(drop);
    return true;
}
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
    if (g_fullScreenView) SplitHidePartner();
    if (g_fullScreenView || !SplitLayout(left, top, w, h))   // split view: both halves
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
    CrossfadeBegin();                     // the old look dissolves over the new one (ui/Crossfade.cpp)
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
    // The split view's tab is one tab: both its pages close.
    int li = -1, ri = -1;
    if (SplitStripPair(&li, &ri) && (index == li || index == ri) && !(g_L.mode == kLayoutCompact && index == g_active)) {
        const unsigned long long other = g_tabs[index == li ? ri : li]->id, self = g_tabs[index]->id;
        int j = -1;
        if (FindTab(other, &j)) CloseTab(j);
        if (FindTab(self, &j)) CloseTab(j);
        RehoverFromCursor(hWnd);
        CheckTabUnfreeze(hWnd);
        return;
    }
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
        FindBarReposition();
        return 0;
    case WM_MOVE:
        SuggestHide();
        BookmarkPopupClose();
        DownloadsPopupClose();
        TransitionCancel();
        FindBarReposition();
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
        // A command sent as a message (no control): the same as choosing it from the menu.
        if (lParam == 0 && LOWORD(wParam) >= CMD_NEWTAB && LOWORD(wParam) <= CMD_UPDATE) {
            RunCommand(LOWORD(wParam));
            return 0;
        }
        if ((HWND)lParam == g_urlEdit && HIWORD(wParam) == EN_KILLFOCUS) {
            SuggestHide();
            ShowWindow(g_urlEdit, SW_HIDE);
            InvalidateChrome();
            return 0;
        }
        break;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && (HWND)wParam == hWnd && SplitSetCursor()) return TRUE;
        if (s_tabDragging) { SetCursor(LoadCursor(nullptr, IDC_ARROW)); return TRUE; }
        break;

    case WM_MOUSEMOVE: {
        if (SplitMouse(msg, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) {
            if (!g_trackingMouse) {
                TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hWnd, 0 };
                TrackMouseEvent(&t);
                g_trackingMouse = true;
            }
            SetHover(Hit());
            return 0;
        }
        if (s_tabPressId && (wParam & MK_LBUTTON)) {
            TabDragMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            if (s_tabDragging) return 0;
        }
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
        SplitMouse(msg, 0, 0);
        SetHover(Hit());
        CheckTabUnfreeze(hWnd);
        return 0;

    case WM_LBUTTONDOWN: {
        if (SplitMouse(msg, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) return 0;   // divider / gutter
        // The active tab is in picture-in-picture: a click on its empty page brings it back.
        if (Tab* t = ActiveTab())
            if (t->pipHwnd && GET_Y_LPARAM(lParam) >= (int)g_L.chromeH && GET_X_LPARAM(lParam) >= (int)g_L.sideW) {
                PipExit(false, true);
                return 0;
            }
        Hit h = HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        g_down = h;
        if (h.kind == Hit::Pill) { g_down = Hit(); ShowUrlEdit(); return 0; }
        if (h.kind == Hit::TabBody) {
            const int hitIndex = h.index;
            h.index = SplitTabAt(h.index, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));   // a split view's half
            s_tabPressRect = PressedTabRect(hitIndex, h.index);
            g_down = h;                                  // (opened on release: a press may become a drag)
            if (h.index >= 0 && h.index < (int)g_tabs.size()) {
                s_tabPressId = g_tabs[h.index]->id;
                s_tabPressPt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                s_tabDragging = false;
            }
        }
        if (h.kind != Hit::None) SetCapture(hWnd);
        InvalidateChrome();
        return 0;
    }
    case WM_LBUTTONUP: {
        if (SplitMouse(msg, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) return 0;
        if (TabDragEnd(true)) {                          // a tab dropped (on another tab: split view)
            g_down = Hit();
            if (GetCapture() == hWnd) ReleaseCapture();
            InvalidateChrome();
            return 0;
        }
        Hit up = HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        Hit down = g_down;
        g_down = Hit();
        if (GetCapture() == hWnd) ReleaseCapture();
        // A click on a tab (released where it was pressed, not dragged) opens it.
        if (down.kind == Hit::TabBody && up.kind == Hit::TabBody &&
            SplitTabAt(up.index, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)) == down.index)
            SelectTab(down.index);
        if (up == down) {
            switch (up.kind) {
            case Hit::Back:     GoBack(); break;
            case Hit::Forward:  GoForward(); break;
            case Hit::Reload:   ReloadActive(true); break;
            case Hit::Star:     BookmarkCurrentPage(); break;
            case Hit::Downloads: DownloadsPopupToggle(); break;
            case Hit::ExtAction: ExtensionButtonClick(up.index); break;
            case Hit::More:     ShowMoreMenu(); break;
            case Hit::NewTab:   RunCommand(CMD_NEWTAB); break;
            case Hit::TabClose: CloseTabWithMouse(hWnd, up.index); break;
            case Hit::TabAudio: MediaToggleMute(up.index); break;
            case Hit::Key:      PasswordsSiteMenu(); break;
            case Hit::Reader:   ReaderToggle(); break;
            case Hit::Translate: TranslateToggle(); break;
            default: break;
            }
        }
        InvalidateChrome();
        return 0;
    }
    case WM_CAPTURECHANGED:
        SplitMouse(msg, 0, 0);
        TabDragEnd(false);
        g_down = Hit();
        InvalidateChrome();
        return 0;

    case WM_RBUTTONUP: {                                 // the tab menu
        Hit h = HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (h.kind == Hit::ExtAction) { ExtensionButtonMenu(h.index); return 0; }
        if (h.kind != Hit::TabBody && h.kind != Hit::TabClose && h.kind != Hit::TabAudio) break;
        if (h.index < 0 || h.index >= (int)g_tabs.size()) return 0;
        const unsigned long long id = g_tabs[h.index]->id;
        enum { kSleep = 1, kSleepOthers, kClose, kSplitWith, kSplitSwap, kSplitClose, kPin };
        HMENU m = CreatePopupMenu();
        {
            const Tab& target = *g_tabs[h.index];
            const bool split = SplitHas(target) && SplitPartnerOf(target);
            AppendMenuW(m, MF_STRING, kPin, target.pinned ? Tr(L"Unpin tab", L"Открепить вкладку")
                                          : split ? Tr(L"Pin tab (ends split view)", L"Закрепить вкладку (разделённый экран закроется)")
                                                  : Tr(L"Pin tab", L"Закрепить вкладку"));
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            const Tab* open = ActiveTab();
            if (target.pinned || (h.index != g_active && open && open->pinned)) {
                // (pinned tabs do not join a split view)
            } else if (split) {
                AppendMenuW(m, MF_STRING, kSplitSwap, Tr(L"Swap sides", L"Поменять половины местами"));
                AppendMenuW(m, MF_STRING, kSplitClose, Tr(L"Close split view", L"Закрыть разделённый экран"));
            } else if (h.index != g_active) {
                AppendMenuW(m, MF_STRING, kSplitWith, Tr(L"Open beside current tab (split view)", L"Открыть рядом с текущей (разделённый экран)"));
            } else {
                AppendMenuW(m, MF_STRING, kSplitWith, Tr(L"Split view with new tab", L"Разделённый экран с новой вкладкой"));
            }
            if (!(target.pinned || (h.index != g_active && open && open->pinned))) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        }
        const std::wstring why = TabSleepBlocker(*g_tabs[h.index]);
        std::wstring sleepText = Tr(L"Put tab to sleep", L"Усыпить вкладку");
        if (!why.empty() && why != L"-") sleepText += L"  (" + why + L")";
        AppendMenuW(m, MF_STRING | (why.empty() ? 0 : MF_GRAYED), kSleep, sleepText.c_str());
        AppendMenuW(m, MF_STRING, kSleepOthers, Tr(L"Put other tabs to sleep", L"Усыпить остальные вкладки"));
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        if (SplitHas(*g_tabs[h.index]) && SplitPartnerOf(*g_tabs[h.index]))
            AppendMenuW(m, MF_STRING, kClose, Tr(L"Close both pages", L"Закрыть обе страницы"));
        else
            AppendMenuW(m, MF_STRING, kClose, Tr(L"Close tab\tCtrl+W", L"Закрыть вкладку\tCtrl+W"));
        MenuSetIcon(m, kPin, g_tabs[h.index]->pinned ? L"\uE77A" : L"\uE718");
        MenuSetIcon(m, kSplitSwap, L"\uE8AB");
        MenuSetIcon(m, kSplitClose, L"\uE8A0");
        MenuSetIcon(m, kSplitWith, L"\uE8A0");
        MenuSetIcon(m, kSleep, L"\uE708");
        MenuSetIcon(m, kSleepOthers, L"\uE708");
        MenuSetIcon(m, kClose, L"\uE711");
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ClientToScreen(hWnd, &pt);
        const int cmd = MenuTrack(m, TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, hWnd);
        DestroyMenu(m);
        int index = -1;
        Tab* t = FindTab(id, &index);
        if (!t) return 0;
        if (cmd == kPin) SetTabPinned(index, !t->pinned);
        else if (cmd == kSplitWith) { if (t == ActiveTab()) SplitWithNewTab(); else SplitWith(id); }
        else if (cmd == kSplitSwap) SplitSwap();
        else if (cmd == kSplitClose) SplitClose();
        else if (cmd == kSleep) SleepTabNow(*t);
        else if (cmd == kSleepOthers) SleepOtherTabs();
        else if (cmd == kClose) CloseTabWithMouse(hWnd, index);
        return 0;
    }
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
        if (wParam == TIMER_UPDATE) {
            UpdaterTimer();
            return 0;
        }
        if (wParam == TIMER_TRANSITION) {
            TransitionTick();
            return 0;
        }
        if (wParam == TIMER_SPLIT_COVER) {
            SplitCoverTimer();
            return 0;
        }
        if (wParam == TIMER_SPLIT_REPAINT) {
            SplitRepaintTimer();
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
        FindBarTabChanged();                     // the find panel follows the active tab
        PermissionPromptUpdate();                // so does the permission bubble
        MediaControlsTick();                     // Windows' media flyout follows the playing tab
        TranslateTick();                         // text that appeared on translated pages
        SessionTick();                           // saves the open tabs when they changed
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
        if (g_tick % 32 == 16) TabSleepTick();
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
    case WM_APP_PRINT:
        if (Tab* t = ActiveTab()) if (t->id == (unsigned long long)wParam && !g_closing) PrintActivePage();
        return 0;
    case WM_APP_ADBLOCK:
        AdBlockListsReady(lParam);
        return 0;
    case WM_APP_TRANSLATE:
        TranslateResult(lParam);
        return 0;
    case WM_APP_SPLIT_FOCUS:
        SplitFocusTab((unsigned long long)wParam);
        return 0;
    case WM_APP_CONTEXT_MENU:
        ContextMenuCommand(wParam);
        return 0;
    case WM_APP_PAGE_MENU:
        ContextMenuShow();
        return 0;
    case WM_APP_THEME:
        CustomThemeDownloaded(wParam, lParam);
        return 0;
    case WM_APP_EXT_INSTALL:
        ExtensionStoreMessage(wParam, lParam);
        return 0;
    case WM_APP_DNR:
        ExtensionsDnrReady(lParam);
        return 0;
    case WM_APP_PIP:
        PipMessage(wParam, lParam);
        return 0;
    case WM_APP_MEDIA:
        MediaControlsButton(wParam);
        return 0;
    case WM_APP_TRAY:
        PermissionsTrayMessage(wParam, lParam);
        return 0;
    case WM_APP_GEO:
        PermissionsGeolocationResult(lParam);
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

    case WM_COPYDATA: {                  // a second Lyra.exe started: open its URL here (main.cpp)
        const auto* cds = (const COPYDATASTRUCT*)lParam;
        if (!cds || cds->dwData != kOpenUrlCopyData) break;
        std::wstring url;
        if (cds->lpData && cds->cbData >= sizeof(wchar_t))
            url.assign((const wchar_t*)cds->lpData, cds->cbData / sizeof(wchar_t));
        while (!url.empty() && url.back() == L'\0') url.pop_back();
        if (IsIconic(hWnd)) ShowWindow(hWnd, SW_RESTORE);
        SetForegroundWindow(hWnd);
        // Not inside the other process's SendMessage: the tab is created on our own turn.
        if (!url.empty() && !g_closing) PostMessageW(hWnd, WM_APP_OPEN_URL, 0, (LPARAM)new std::wstring(url));
        return TRUE;
    }
    case WM_APP_UPDATE:
        UpdaterChanged();
        return 0;
    case WM_APP_WALLPAPER:
        RefreshStartPages();
        return 0;
    case WM_APP_OPEN_URL: {
        std::unique_ptr<std::wstring> url((std::wstring*)lParam);
        if (url && !url->empty() && !g_closing) CreateTab(*url);
        return 0;
    }

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
        FindBarClose();
        SessionSave(true);               // the tabs, for the next start
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
