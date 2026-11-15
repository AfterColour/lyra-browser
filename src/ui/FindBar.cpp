// Lyra — поиск на странице (Ctrl+F): панель с полем, счётчиком совпадений и стрелками.
//
// The panel is a small window owned by the main window (like the bookmark bubble), kept over the
// top right corner of the page. WebKit does the searching (WKPageFindString): it highlights
// every match, selects the next one and scrolls to it, and reports how many there are. WebKit
// does not say which match is the current one, so the panel counts along: the first search
// shows 1, every step forward or back moves the number with wrap-around, as the search does.
#include "ui/FindBar.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "core/Localization.h"
#include "core/StringUtils.h"
#include "ui/Animation.h"
#include "ui/Chrome.h"
#include "ui/Draw.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"

#include <commctrl.h>

static HWND s_bar = nullptr, s_edit = nullptr;
static WNDPROC s_oldEditProc = nullptr;
static HFONT s_editFont = nullptr;
static HBRUSH s_fieldBrush = nullptr;
static std::unique_ptr<Font> s_fCount, s_fGlyph;
static bool s_tracking = false;
static int s_hover = 0, s_down = 0;               // 1 previous, 2 next, 3 close

static unsigned long long s_tabId = 0;            // the tab being searched
static std::wstring s_query;                      // what was last searched for
static int s_count = -1;                          // -1 nothing searched yet; kMany = over the limit
static int s_index = 0;                           // 1-based current match
static bool s_pendingStep = false;                // a search was sent and its answer is still due
static int s_pendingDir = 0;                      // 0 new search, +1 next, -1 previous

constexpr unsigned kMaxMatches = 1000;
constexpr int kMany = (int)kMaxMatches + 1;

// Logical (96 DPI) layout
constexpr float kW = 380, kH = 48, kPad = 8, kBtn = 30;

static Color PanelBg() { return g_dark ? Color(37, 39, 44) : Color(255, 255, 255); }
static Color FieldBg() { return g_dark ? Color(46, 48, 54) : Color(244, 245, 247); }

static RectF FieldRect() { return RectF(S(kPad), S(kPad), S(kW - kPad * 2 - kBtn * 3 - 8), S(kH - kPad * 2)); }
static RectF ButtonRect(int i) {   // 1 previous, 2 next, 3 close
    const float x = S(kW - kPad) - S(kBtn) * (4 - i);
    return RectF(x, S((kH - kBtn) / 2), S(kBtn), S(kBtn));
}
static int CountWidth() { return (int)S(64); }

static int HitAt(int x, int y) {
    for (int i = 1; i <= 3; ++i)
        if (ButtonRect(i).Contains((float)x, (float)y)) return i;
    return 0;
}

static Tab* SearchedTab() {
    Tab* t = ActiveTab();
    return t && t->id == s_tabId ? t : nullptr;
}

static std::wstring CountText() {
    if (s_query.empty() || s_count < 0) return std::wstring();
    if (s_count == 0) return L"0/0";
    std::wstring total = s_count >= kMany ? std::to_wstring(kMaxMatches) + L"+" : std::to_wstring(s_count);
    return std::to_wstring(std::max(1, s_index)) + L"/" + total;
}

static void MakeFonts() {
    auto make = [](const wchar_t* family, float px, int style) {
        auto f = std::make_unique<Font>(family, px, style, UnitPixel);
        if (!f->IsAvailable()) f = std::make_unique<Font>(L"Segoe UI", px, style, UnitPixel);
        return f;
    };
    s_fCount = make(L"Segoe UI Variable Text", S(12), FontStyleRegular);
    s_fGlyph = MakeIconFont(S(11));
    if (s_editFont) DeleteObject(s_editFont);
    s_editFont = CreateFontW(-(int)std::round(S(13)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (s_fieldBrush) DeleteObject(s_fieldBrush);
    s_fieldBrush = CreateSolidBrush(ToColorRef(FieldBg()));
}

static void Paint(HDC hdc) {
    RECT rc;
    GetClientRect(s_bar, &rc);
    PaintBuffered(hdc, rc, true, [&](Graphics& g) {
        const Theme& T = g_th;
        g.SetSmoothingMode(SmoothingModeHighQuality);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        SolidBrush bg(PanelBg());
        g.FillRectangle(&bg, 0, 0, rc.right, rc.bottom);

        // Field: red frame when nothing was found.
        const RectF f = FieldRect();
        FillRound(g, FieldBg(), f, S(8));
        const bool notFound = !s_query.empty() && s_count == 0;
        const bool focused = GetFocus() == s_edit;
        GraphicsPath p;
        AddRound(p, f, S(8));
        Color frame = notFound ? (g_dark ? Color(255, 138, 128) : Color(196, 43, 28)) : focused ? T.accent : T.fieldBorder;
        Pen pen(frame, (focused || notFound) ? S(1.5f) : 1.f);
        g.DrawPath(&pen, &p);

        const std::wstring count = CountText();
        if (!count.empty()) {
            StringFormat sf(StringFormat::GenericTypographic());
            sf.SetAlignment(StringAlignmentFar);
            sf.SetLineAlignment(StringAlignmentCenter);
            SolidBrush br(notFound ? frame : T.text2);
            g.DrawString(count.c_str(), -1, s_fCount.get(),
                         RectF(f.GetRight() - CountWidth() - S(10), f.Y, (float)CountWidth(), f.Height), &sf, &br);
        }

        static const wchar_t* const kGlyphs[] = { L"", L"", L"", L"" };
        for (int i = 1; i <= 3; ++i) {
            const RectF b = ButtonRect(i);
            const bool enabled = i == 3 || s_count > 0;
            if (enabled && (s_hover == i || s_down == i)) FillRound(g, s_down == i ? T.pressed : T.hover, b, S(6));
            DrawGlyph(g, kGlyphs[i], b, enabled ? T.icon : T.iconDis, s_fGlyph.get());
        }
    });
}

static void Repaint() {
    if (s_bar) InvalidateRect(s_bar, nullptr, FALSE);
}

static void Search(int direction) {
    Tab* t = SearchedTab();
    if (!t || !t->page) return;
    if (s_query.empty()) {
        WKPageHideFindUI(t->page);
        s_count = -1;
        s_index = 0;
        Repaint();
        return;
    }
    WKFindOptions options = kWKFindOptionsCaseInsensitive | kWKFindOptionsWrapAround | kWKFindOptionsShowHighlight
                          | kWKFindOptionsShowFindIndicator;
    if (direction < 0) options |= kWKFindOptionsBackwards;
    s_pendingStep = true;
    s_pendingDir = direction;
    WKStringRef s = WKStringCreateWithUTF8CString(ToUtf8(s_query).c_str());
    WKPageFindString(t->page, s, options, kMaxMatches);
    WKRelease(s);
}

static void QueryChanged() {
    wchar_t buf[512] = {};
    if (s_edit) GetWindowTextW(s_edit, buf, ARRAYSIZE(buf));
    if (s_query == buf) return;
    s_query = buf;
    s_index = 0;
    Search(0);
}

void FindBarDidFind(unsigned long long tabId, unsigned matchCount) {
    if (!s_bar || tabId != s_tabId) return;
    const int count = matchCount == (unsigned)kWKMoreThanMaximumMatchCount || matchCount > kMaxMatches ? kMany : (int)matchCount;
    const int dir = s_pendingStep ? s_pendingDir : 0;
    s_pendingStep = false;
    const int limit = count >= kMany ? (int)kMaxMatches : count;
    if (dir == 0 || s_count <= 0 || s_index <= 0) s_index = 1;
    else s_index = ((s_index - 1 + dir) % limit + limit) % limit + 1;
    s_count = count;
    Repaint();
}

void FindBarDidFail(unsigned long long tabId) {
    if (!s_bar || tabId != s_tabId) return;
    s_pendingStep = false;
    s_count = 0;
    s_index = 0;
    Repaint();
}

static LRESULT CALLBACK EditProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    if (msg == WM_KEYDOWN) {
        if (w == VK_ESCAPE) { FindBarClose(); return 0; }
        if (w == VK_RETURN || w == VK_F3) { FindBarNext(shift); return 0; }
        if (ctrl && (w == 'A' || w == 'F')) { SendMessageW(h, EM_SETSEL, 0, -1); return 0; }
        if (ctrl && w == 'G') { FindBarNext(shift); return 0; }
    }
    if (msg == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE || w == 0x7F /* Ctrl+Backspace */)) return 0;
    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS) InvalidateRect(GetParent(h), nullptr, FALSE);
    return CallWindowProcW(s_oldEditProc, h, msg, w, l);
}

static LRESULT CALLBACK BarProc(HWND h, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        Paint(hdc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wParam;
        SetTextColor(dc, ToColorRef(g_th.text));
        SetBkColor(dc, ToColorRef(FieldBg()));
        return (LRESULT)s_fieldBrush;
    }
    case WM_COMMAND:
        if ((HWND)lParam == s_edit && HIWORD(wParam) == EN_CHANGE) QueryChanged();
        return 0;
    case WM_MOUSEMOVE: {
        const int hit = HitAt(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (hit != s_hover) { s_hover = hit; Repaint(); }
        if (!s_tracking) {
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
            s_tracking = true;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        s_tracking = false;
        if (s_hover) { s_hover = 0; Repaint(); }
        return 0;
    case WM_LBUTTONDOWN:
        s_down = HitAt(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (s_down) { SetCapture(h); Repaint(); }
        else if (s_edit) SetFocus(s_edit);
        return 0;
    case WM_LBUTTONUP: {
        const int hit = HitAt(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        const int down = s_down;
        s_down = 0;
        if (GetCapture() == h) ReleaseCapture();
        Repaint();
        if (hit != down || !hit) return 0;
        if (hit == 3) FindBarClose();
        else FindBarNext(hit == 1);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && (HWND)wParam == h) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(h, &pt);
            SetCursor(LoadCursor(nullptr, HitAt(pt.x, pt.y) ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_DESTROY:
        s_tracking = false;
        s_hover = s_down = 0;
        return 0;
    }
    return DefWindowProcW(h, msg, wParam, lParam);
}

static POINT BarPosition() {
    RECT cr;
    GetClientRect(g_hWnd, &cr);
    POINT tl = { (LONG)((float)cr.right - S(kW) - S(16)), (LONG)(g_L.chromeH + S(8)) };
    tl.x = std::max<LONG>((LONG)(g_L.sideW + S(8)), tl.x);
    ClientToScreen(g_hWnd, &tl);
    return tl;
}

void FindBarReposition() {
    if (!s_bar) return;
    if (IsIconic(g_hWnd)) return;
    const POINT tl = BarPosition();
    SetWindowPos(s_bar, nullptr, tl.x, tl.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

bool FindBarVisible() { return s_bar != nullptr; }

void FindBarClose() {
    if (!s_bar) return;
    if (Tab* t = SearchedTab())
        if (t->page) WKPageHideFindUI(t->page);
    HWND h = s_bar;
    s_bar = nullptr;
    s_edit = nullptr;
    s_count = -1;
    s_index = 0;
    s_pendingStep = false;
    PopupRevealStop(h);
    DestroyWindow(h);
    // Back to the page.
    if (Tab* t = ActiveTab())
        if (t->hwnd) { SetForegroundWindow(g_hWnd); SetFocus(t->hwnd); }
}

void FindBarTabChanged() {
    if (!s_bar) return;
    Tab* t = ActiveTab();
    if (t && t->id == s_tabId) return;
    // The panel follows the active tab; the old page keeps no highlights.
    for (auto& other : g_tabs)
        if (other->id == s_tabId && other->page) WKPageHideFindUI(other->page);
    s_tabId = t ? t->id : 0;
    s_count = -1;
    s_index = 0;
    Search(0);
    Repaint();
}

void FindBarPageLoaded(unsigned long long tabId) {
    if (!s_bar || tabId != s_tabId || s_query.empty()) return;
    s_index = 0;
    Search(0);
}

void FindBarNext(bool backwards) {
    if (!s_bar || s_query.empty()) {
        FindBarShow();
        return;
    }
    Search(backwards ? -1 : 1);
}

void FindBarShow() {
    Tab* t = ActiveTab();
    if (!t || !t->page || !g_hWnd) return;
    if (s_bar) {
        if (t->id != s_tabId) FindBarTabChanged();
        SetForegroundWindow(s_bar);
        SetFocus(s_edit);
        SendMessageW(s_edit, EM_SETSEL, 0, -1);
        return;
    }
    s_tabId = t->id;
    s_count = -1;
    s_index = 0;

    static bool registered = false;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = BarProc;
        wc.hInstance = hi;
        wc.lpszClassName = L"LyraFindBar";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        if (!RegisterClassExW(&wc)) return;
        registered = true;
    }
    MakeFonts();

    const POINT tl = BarPosition();
    s_bar = CreateWindowExW(WS_EX_TOOLWINDOW, L"LyraFindBar", L"", WS_POPUP | WS_CLIPCHILDREN,
                            tl.x, tl.y, (int)S(kW), (int)S(kH), g_hWnd, nullptr, hi, nullptr);
    if (!s_bar) return;
    const DWORD round = 2;                               // DWMWCP_ROUND (Windows 11)
    DwmSetWindowAttribute(s_bar, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
    const BOOL dark = g_dark;
    DwmSetWindowAttribute(s_bar, kDwmDarkMode, &dark, sizeof(dark));

    TEXTMETRICW tm = {};
    {
        HDC dc = GetDC(s_bar);
        HGDIOBJ prev = SelectObject(dc, s_editFont);
        GetTextMetricsW(dc, &tm);
        SelectObject(dc, prev);
        ReleaseDC(s_bar, dc);
    }
    const RectF f = FieldRect();
    const int eh = tm.tmHeight ? tm.tmHeight : (int)S(18);
    const int pad = (int)S(10);
    s_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT,
                             (int)f.X + pad, (int)(f.Y + (f.Height - eh) / 2), (int)f.Width - pad * 2 - CountWidth(), eh,
                             s_bar, nullptr, hi, nullptr);
    if (s_edit) {
        SetWindowTheme(s_edit, L"", L"");
        SendMessageW(s_edit, WM_SETFONT, (WPARAM)s_editFont, FALSE);
        SendMessageW(s_edit, EM_SETCUEBANNER, TRUE, (LPARAM)Tr(L"Find on page", L"Найти на странице"));
        s_oldEditProc = (WNDPROC)SetWindowLongPtrW(s_edit, GWLP_WNDPROC, (LONG_PTR)EditProc);
        // The last search comes back, selected, ready to be searched again or typed over.
        if (!s_query.empty()) {
            const std::wstring q = s_query;
            s_query.clear();
            SetWindowTextW(s_edit, q.c_str());          // EN_CHANGE searches it
        }
    }
    PopupShow(s_bar, true);
    SetForegroundWindow(s_bar);
    if (s_edit) {
        SetFocus(s_edit);
        SendMessageW(s_edit, EM_SETSEL, 0, -1);
    }
}
