// Lyra — разделённый экран: две вкладки рядом в одном окне.
#include "browser/SplitView.h"
#include "app/Log.h"
#include "browser/Session.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "core/Constants.h"
#include "core/Settings.h"
#include "ui/AddressBar.h"
#include "ui/Animation.h"
#include "ui/Chrome.h"
#include "ui/Commands.h"
#include "ui/Draw.h"
#include "ui/MainWindow.h"
#include "ui/TabTransition.h"
#include "ui/Theme.h"

namespace {

unsigned long long s_left = 0, s_right = 0;          // the pair (tab ids), 0 = no split
float s_ratio = 0.5f;                                // the left half's share of the width (shown)
float s_target = 0.5f;                               // where it is going (divider drag, double-click)
float s_viewRatio = 0.5f;                            // what the views are laid out at (frozen under the cover)
unsigned long long s_dragTab = 0, s_dropTab = 0;     // tab drag onto a tab: the dragged one, the target
bool s_dropRight = false;
RECT s_paneL = {}, s_paneR = {}, s_divider = {};     // last layout, client coordinates
bool s_dragging = false, s_hoverDivider = false;
int s_dragOffset = 0;
DWORD s_lastDown = 0;
HHOOK s_hook = nullptr;

int Margin() { return std::max(2, (int)std::lround(S(3))); }
int Gutter() { return std::max(6, (int)std::lround(S(8))); }
int MinPane() { return (int)std::lround(S(220)); }

Tab* Left() { return s_left ? FindTab(s_left) : nullptr; }
Tab* Right() { return s_right ? FindTab(s_right) : nullptr; }

bool Inside(const RECT& r, int x, int y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

// The half a window belongs to (the view or one of its children).
Tab* PaneOf(HWND h) {
    for (Tab* t : { Left(), Right() })
        if (t && t->hwnd && (h == t->hwnd || IsChild(t->hwnd, h))) return t;
    return nullptr;
}

// A click into the half that is not open makes it the open tab (after the click is delivered).
LRESULT CALLBACK MouseHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN || wParam == WM_MBUTTONDOWN ||
                              wParam == WM_XBUTTONDOWN) && SplitShown()) {
        const MOUSEHOOKSTRUCT* m = (const MOUSEHOOKSTRUCT*)lParam;
        if (Tab* t = PaneOf(m->hwnd))
            if (t != ActiveTab()) PostMessageW(g_hWnd, WM_APP_SPLIT_FOCUS, (WPARAM)t->id, 0);
    }
    return CallNextHookEx(s_hook, code, wParam, lParam);
}

void Hook(bool on) {
    if (on && !s_hook) s_hook = SetWindowsHookExW(WH_MOUSE, MouseHook, nullptr, GetCurrentThreadId());
    if (!on && s_hook) { UnhookWindowsHookEx(s_hook); s_hook = nullptr; }
}

void FreeCover();
void Forget() {
    // Pictures taken while split would show both halves when one of them is shown alone.
    if (s_left) TransitionForget(s_left);
    if (s_right) TransitionForget(s_right);
    s_left = s_right = 0;
    s_dragging = s_hoverDivider = false;
    s_paneL = s_paneR = s_divider = {};
    FreeCover();
    Hook(false);
}

// The two tabs side by side in the tab list, left half first: they are drawn as one tab.
void Arrange() {
    int li = -1, ri = -1;
    if (!FindTab(s_left, &li) || !FindTab(s_right, &ri) || ri == li + 1) return;
    MoveTab(ri, ri > li ? li + 1 : li);
}

void Pair(unsigned long long left, unsigned long long right) {
    if (s_left || s_right) {
        // A tab of the old pair that is not in the new one: back to the background.
        for (unsigned long long id : { s_left, s_right })
            if (id && id != left && id != right)
                if (Tab* t = FindTab(id)) if (t != ActiveTab()) t->hiddenSince = GetTickCount64();
        Forget();
    }
    s_left = left;
    s_right = right;
    s_ratio = s_target = s_viewRatio = 0.5f;
    Hook(true);
    TransitionCancel();
    Log("split: tab#%llu | tab#%llu", left, right);
    Arrange();
    RefreshLayout(true);
    InvalidateRect(g_hWnd, nullptr, FALSE);
}

// Places the halves for a page area of w and the current ratio.
void GeometryFor(float ratio, int left, int top, int w, int h, RECT* pl, RECT* pr, RECT* div) {
    const int m = Margin(), g = Gutter();
    const int inner = std::max(0, w - 2 * m - g);
    int lw = (int)std::lround(inner * ratio);
    const int minW = std::min(MinPane(), inner / 2);
    lw = std::clamp(lw, minW, inner - minW);
    *pl = { left + m, top + m, left + m + lw, top + h - m };
    *pr = { pl->right + g, top + m, left + w - m, top + h - m };
    *div = { pl->right, top, pr->left, top + h };
}

void Geometry(int left, int top, int w, int h) {
    GeometryFor(s_viewRatio, left, top, w, h, &s_paneL, &s_paneR, &s_divider);
}

void Place(Tab* t, const RECT& r) {
    if (!t) return;
    t->hiddenSince = 0;
    if (!t->hwnd) return;                                // in picture-in-picture
    MoveWindow(t->hwnd, r.left, r.top, std::max(0L, r.right - r.left), std::max(0L, r.bottom - r.top), TRUE);
    if (!IsWindowVisible(t->hwnd)) ShowWindow(t->hwnd, SW_SHOWNA);
}

// A half that moved and grew keeps showing a frame of its old size: the window's swap chain
// takes the new size only when a frame is presented after the resize, so the page has to draw
// twice more. Ask both pages for fresh frames: now (throttled while dragging) and once more a
// moment after the resizing stopped.
void NoOpPainted(WKErrorRef, void*) {}
DWORD s_lastRepaint = 0;
void ForceBoth() {
    for (Tab* h : { Left(), Right() })
        if (h && h->page) WKPageForceRepaint(h->page, nullptr, NoOpPainted);
}
void RepaintHalves(bool now) {
    const DWORD t = GetTickCount();
    SetTimer(g_hWnd, TIMER_SPLIT_REPAINT, 150, nullptr);   // (re-armed: fires once things settle)
    if (!now && t - s_lastRepaint < 60) return;
    s_lastRepaint = t;
    ForceBoth();
}

RECT PageArea() {
    RECT cr;
    GetClientRect(g_hWnd, &cr);
    return { (LONG)g_L.sideW, (LONG)g_L.chromeH, cr.right, cr.bottom };
}

// --- The divider picture ---------------------------------------------------------------
// Resizing two pages for every mouse move makes the divider step (WebKit relays each page
// out and draws it again first). While the divider is dragged, a picture of the page area
// taken when the drag began is shown over it instead: the divider and the halves move in it
// at the display rate. The views get their new size once, when the drag ends; the picture
// goes away (fading) once both have drawn at that size.
HWND s_cover = nullptr;
HDC s_coverDC = nullptr, s_snapDC = nullptr;
HBITMAP s_coverBmp = nullptr, s_snapBmp = nullptr;
HGDIOBJ s_coverOld = nullptr, s_snapOld = nullptr;
void* s_coverBits = nullptr;
SIZE s_coverSize = {};
POINT s_coverPos = {};
RECT s_snapL = {}, s_snapR = {};                     // the halves in the picture (cover coordinates)
COLORREF s_fillL = 0, s_fillR = 0;                   // what a growing half shows past its picture
enum class Cover { None, Moving, Waiting, Fading } s_coverState = Cover::None;
int s_waitPaints = 0;
DWORD s_coverSince = 0;
BYTE s_coverAlpha = 255;

LRESULT CALLBACK CoverProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, msg, w, l);
}

HBITMAP NewDib(HDC ref, int w, int h, void** bits) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* b = nullptr;
    HBITMAP bmp = CreateDIBSection(ref, &bi, DIB_RGB_COLORS, &b, nullptr, 0);
    if (bits) *bits = b;
    return bmp;
}

// The most common of a few pixels down a column of a half, a little left of its right edge
// (past the scroll bar): the page's background, as far as it can be told.
COLORREF SampleFill(const RECT& r) {
    const int x = std::max((int)r.left, (int)r.right - 1 - (int)std::lround(S(24)));
    std::map<COLORREF, int> n;
    for (int i = 1; i <= 9; ++i) ++n[GetPixel(s_snapDC, x, r.top + (int)((r.bottom - r.top) * i / 10))];
    COLORREF best = RGB(255, 255, 255);
    int most = 0;
    for (auto& [c, k] : n) if (k > most) { most = k; best = c; }
    return best;
}

void FreeCover() {
    if (s_cover) { DestroyWindow(s_cover); s_cover = nullptr; }
    if (s_coverDC) { SelectObject(s_coverDC, s_coverOld); DeleteDC(s_coverDC); s_coverDC = nullptr; }
    if (s_coverBmp) { DeleteObject(s_coverBmp); s_coverBmp = nullptr; }
    if (s_snapDC) { SelectObject(s_snapDC, s_snapOld); DeleteDC(s_snapDC); s_snapDC = nullptr; }
    if (s_snapBmp) { DeleteObject(s_snapBmp); s_snapBmp = nullptr; }
    s_coverBits = nullptr;
    s_coverState = Cover::None;
    if (g_hWnd) KillTimer(g_hWnd, TIMER_SPLIT_COVER);
}

void PushCover() {
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, s_coverAlpha, 0 };
    POINT src = { 0, 0 };
    UpdateLayeredWindow(s_cover, nullptr, &s_coverPos, &s_coverSize, s_coverDC, &src, 0, &bf, ULW_ALPHA);
}

// Draws the halves at s_ratio into the cover.
void RenderCover() {
    if (!s_coverDC) return;
    const int w = s_coverSize.cx, h = s_coverSize.cy;
    RECT nl, nr, nd;
    GeometryFor(s_ratio, 0, 0, w, h, &nl, &nr, &nd);
    HBRUSH gutter = CreateSolidBrush(g_dark ? RGB(24, 25, 28) : RGB(226, 228, 233));
    RECT all = { 0, 0, w, h };
    FillRect(s_coverDC, &all, gutter);
    DeleteObject(gutter);
    auto half = [&](const RECT& to, const RECT& from, COLORREF fill) {
        const int tw = to.right - to.left, th = to.bottom - to.top, fw = from.right - from.left;
        const int cw = std::min(tw, fw);
        BitBlt(s_coverDC, to.left, to.top, cw, th, s_snapDC, from.left, from.top, SRCCOPY);
        if (tw > cw) {
            HBRUSH b = CreateSolidBrush(fill);
            RECT rest = { to.left + cw, to.top, to.right, to.bottom };
            FillRect(s_coverDC, &rest, b);
            DeleteObject(b);
        }
    };
    half(nl, s_snapL, s_fillL);
    half(nr, s_snapR, s_fillR);
    Graphics g(s_coverDC);
    g.SetSmoothingMode(SmoothingModeHighQuality);
    const Theme& T = g_th;
    const RECT& act = (ActiveTab() && ActiveTab()->id == s_left) ? nl : nr;
    const float m = (float)Margin(), o = m / 2.f;
    Pen outline(T.accent, std::max(2.f, std::round(S(2))));
    g.DrawRectangle(&outline, act.left - o, act.top - o, (REAL)(act.right - act.left) + m, (REAL)(act.bottom - act.top) + m);
    const float gw = std::max(2.f, std::round(S(3))), gh = std::round(S(36));
    const float cx = (nd.left + nd.right) / 2.f, cy = (nd.top + nd.bottom) / 2.f;
    FillRound(g, T.accent, RectF(cx - gw / 2, cy - gh / 2, gw, gh), gw / 2);
    PushCover();
}

// The picture of the page area as it is now; false if it cannot be shown.
bool BeginCover() {
    if (s_coverState != Cover::None) FreeCover();
    RECT pa;
    GetClientRect(g_hWnd, &pa);
    pa.left = (LONG)g_L.sideW;
    pa.top = (LONG)g_L.chromeH;
    const int w = pa.right - pa.left, h = pa.bottom - pa.top;
    if (w <= 0 || h <= 0 || IsIconic(g_hWnd)) return false;
    static bool registered = false;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = CoverProc;
        wc.hInstance = hi;
        wc.lpszClassName = L"LyraSplitCover";
        if (!RegisterClassExW(&wc)) return false;
        registered = true;
    }
    POINT tl = { pa.left, pa.top };
    ClientToScreen(g_hWnd, &tl);
    HDC screen = GetDC(nullptr);
    s_snapBmp = NewDib(screen, w, h, nullptr);
    s_coverBmp = NewDib(screen, w, h, &s_coverBits);
    s_snapDC = CreateCompatibleDC(screen);
    s_coverDC = CreateCompatibleDC(screen);
    bool ok = s_snapBmp && s_coverBmp && s_snapDC && s_coverDC;
    if (ok) {
        s_snapOld = SelectObject(s_snapDC, s_snapBmp);
        s_coverOld = SelectObject(s_coverDC, s_coverBmp);
        BitBlt(s_snapDC, 0, 0, w, h, screen, tl.x, tl.y, SRCCOPY);   // DWM's picture: GPU pages come out right
    }
    ReleaseDC(nullptr, screen);
    if (!ok) { FreeCover(); return false; }
    s_snapL = { s_paneL.left - pa.left, s_paneL.top - pa.top, s_paneL.right - pa.left, s_paneL.bottom - pa.top };
    s_snapR = { s_paneR.left - pa.left, s_paneR.top - pa.top, s_paneR.right - pa.left, s_paneR.bottom - pa.top };
    s_fillL = SampleFill(s_snapL);
    s_fillR = SampleFill(s_snapR);
    s_coverPos = tl;
    s_coverSize = { w, h };
    s_coverAlpha = 255;
    s_cover = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                              L"LyraSplitCover", L"", WS_POPUP, tl.x, tl.y, w, h, g_hWnd, nullptr, hi, nullptr);
    if (!s_cover) { FreeCover(); return false; }
    s_coverState = Cover::Moving;
    RenderCover();
    SetWindowPos(s_cover, HWND_TOP, tl.x, tl.y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
}

void CoverPainted(WKErrorRef, void*) {
    if (s_coverState == Cover::Waiting && --s_waitPaints <= 0) {
        s_coverState = Cover::Fading;
        s_coverSince = GetTickCount();
    }
}

// The divider has stopped: the views take the new size, the picture waits for them.
void CommitCover() {
    s_viewRatio = s_ratio;
    LayoutWebViews();
    InvalidateRect(g_hWnd, nullptr, FALSE);
    if (s_coverState != Cover::Moving) return;
    s_coverState = Cover::Waiting;
    s_coverSince = GetTickCount();
    s_waitPaints = 0;
    for (Tab* t : { Left(), Right() })
        if (t && t->page) { ++s_waitPaints; WKPageForceRepaint(t->page, nullptr, CoverPainted); }
    if (!s_waitPaints) { s_coverState = Cover::Fading; }
    SetTimer(g_hWnd, TIMER_SPLIT_COVER, 15, nullptr);
}

} // namespace

bool SplitHas(const Tab& t) { return t.id && (t.id == s_left || t.id == s_right); }

bool SplitShown() {
    const Tab* a = ActiveTab();
    return a && SplitHas(*a) && Left() && Right();
}

bool SplitVisible(const Tab& t) { return SplitHas(t) && SplitShown() && !g_fullScreenView; }

Tab* SplitPartnerOf(const Tab& t) {
    if (!SplitHas(t)) return nullptr;
    return t.id == s_left ? Right() : Left();
}

bool SplitPaired(const Tab* a, const Tab* b) {
    return a && b && a != b && SplitHas(*a) && SplitHas(*b);
}

void SplitWith(unsigned long long otherId) {
    Tab* a = ActiveTab();
    Tab* o = FindTab(otherId);
    if (!a || !o || a == o) return;
    if (SplitPaired(a, o)) return;
    if (a->id == s_right) Pair(o->id, a->id);           // the open tab keeps its side
    else Pair(a->id, o->id);
    SessionLoadLazyTab(*o);                              // a restored or sleeping tab loads now
    FocusWebView();
}

void SplitWithNewTab() {
    Tab* a = ActiveTab();
    if (!a) return;
    const unsigned long long keep = a->id;
    const bool keepRight = keep == s_right;
    RunCommand(CMD_NEWTAB);
    Tab* n = ActiveTab();
    if (!n || n->id == keep || !FindTab(keep)) return;
    if (keepRight) Pair(n->id, keep);
    else Pair(keep, n->id);
    ShowUrlEdit();                                       // type where the new half goes
}

void SplitOpenLink(unsigned long long fromId, const std::wstring& url, bool privateTab) {
    if (!FindTab(fromId)) return;
    Tab* n = OpenInNewTab(url, true, privateTab);
    if (!n) return;
    int index = -1;
    if (!FindTab(fromId, &index)) return;
    if (ActiveTab() && ActiveTab()->id != fromId) SelectTab(index);
    if (fromId == s_right) Pair(n->id, fromId);
    else Pair(fromId, n->id);
}

void SplitToggle() {
    if (SplitShown()) SplitClose();
    else SplitWithNewTab();
}

void SplitSwap() {
    if (!s_left || !s_right) return;
    std::swap(s_left, s_right);
    s_ratio = s_target = s_viewRatio = 1.f - s_target;
    TransitionForget(s_left);
    TransitionForget(s_right);
    Arrange();
    RefreshLayout(true);
    RepaintHalves(true);
    InvalidateRect(g_hWnd, nullptr, FALSE);
}

void SplitClose() {
    if (!s_left && !s_right) return;
    Log("split: closed");
    const Tab* a = ActiveTab();
    for (unsigned long long id : { s_left, s_right })
        if (Tab* t = FindTab(id)) if (t != a) t->hiddenSince = GetTickCount64();
    Forget();
    RefreshLayout(true);
    InvalidateRect(g_hWnd, nullptr, FALSE);
    FocusWebView();
}

unsigned long long SplitTabClosed(unsigned long long id) {
    if (!id || (id != s_left && id != s_right)) return 0;
    const unsigned long long other = id == s_left ? s_right : s_left;
    Log("split: tab#%llu closed -> split ends", id);
    Forget();
    return other;
}

bool SplitLayout(int left, int top, int w, int h) {
    if (!SplitShown()) {
        s_paneL = s_paneR = s_divider = {};
        return false;
    }
    Geometry(left, top, w, h);
    Place(Left(), s_paneL);
    Place(Right(), s_paneR);
    return true;
}

void SplitRepaintTimer() {
    KillTimer(g_hWnd, TIMER_SPLIT_REPAINT);
    if (SplitShown()) ForceBoth();
}

void SplitHidePartner() {
    if (!SplitShown()) return;
    if (Tab* p = SplitPartnerOf(*ActiveTab())) if (p->hwnd && IsWindowVisible(p->hwnd)) ShowWindow(p->hwnd, SW_HIDE);
}

void SplitPaint(Graphics& g) {
    if (!SplitShown() || g_fullScreenView || IsRectEmpty(&s_paneL)) return;
    const Theme& T = g_th;
    const RECT pa = PageArea();
    // The gutter around and between the halves.
    SolidBrush gutter(g_dark ? Color(255, 24, 25, 28) : Color(255, 226, 228, 233));
    g.FillRectangle(&gutter, (REAL)pa.left, (REAL)pa.top, (REAL)(pa.right - pa.left), (REAL)(pa.bottom - pa.top));
    SolidBrush page(T.page);
    for (const RECT* r : { &s_paneL, &s_paneR })
        g.FillRectangle(&page, (REAL)r->left, (REAL)r->top, (REAL)(r->right - r->left), (REAL)(r->bottom - r->top));
    // The open half: an accent outline in the margin around it.
    const RECT& act = ActiveTab()->id == s_left ? s_paneL : s_paneR;
    const float m = (float)Margin();
    Pen outline(T.accent, std::max(2.f, std::round(S(2))));
    outline.SetAlignment(PenAlignmentCenter);
    const float o = m / 2.f;
    g.DrawRectangle(&outline, act.left - o, act.top - o, (REAL)(act.right - act.left) + m, (REAL)(act.bottom - act.top) + m);
    // The divider grip.
    const float gw = std::max(2.f, std::round(S(3))), gh = std::round(S(36));
    const float cx = (s_divider.left + s_divider.right) / 2.f, cy = (s_divider.top + s_divider.bottom) / 2.f;
    const Color grip = (s_dragging || s_hoverDivider) ? T.accent : (g_dark ? Color(255, 88, 92, 100) : Color(255, 170, 174, 182));
    FillRound(g, grip, RectF(cx - gw / 2, cy - gh / 2, gw, gh), gw / 2);
}

bool SplitSetCursor() {
    if (!SplitShown()) return false;
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(g_hWnd, &pt);
    if (!s_dragging && !Inside(s_divider, pt.x, pt.y)) return false;
    SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
    return true;
}

bool SplitMouse(UINT msg, int x, int y) {
    if (!SplitShown() && !s_dragging) return false;
    switch (msg) {
    case WM_MOUSEMOVE: {
        if (s_dragging) {
            const RECT pa = PageArea();
            const int m = Margin(), g = Gutter();
            const int inner = std::max(1, (int)(pa.right - pa.left) - 2 * m - g);
            s_target = std::clamp((float)(x - s_dragOffset - pa.left - m) / (float)inner, 0.f, 1.f);
            const int minW = std::min(MinPane(), inner / 2);           // (the layout clamps the same way)
            s_target = std::clamp(s_target, (float)minW / inner, 1.f - (float)minW / inner);
            AnimRequest();
            return true;
        }
        const bool over = Inside(s_divider, x, y);
        if (over != s_hoverDivider) {
            s_hoverDivider = over;
            InvalidateRect(g_hWnd, &s_divider, FALSE);
        }
        return over;
    }
    case WM_LBUTTONDOWN: {
        if (Inside(s_divider, x, y)) {
            const DWORD now = GetTickCount();
            if (now - s_lastDown <= GetDoubleClickTime()) {   // double-click: half and half
                s_lastDown = 0;
                s_target = 0.5f;
                if (s_coverState != Cover::Moving) BeginCover();
                AnimRequest();
                return true;
            }
            s_lastDown = now;
            if (s_coverState != Cover::Moving) BeginCover();
            s_dragging = true;
            s_dragOffset = x - s_paneL.right;
            SetCapture(g_hWnd);
            InvalidateRect(g_hWnd, &s_divider, FALSE);
            return true;
        }
        // The margin around a half: that half becomes the open tab.
        const RECT pa = PageArea();
        if (!Inside(pa, x, y)) return false;
        Tab* t = x < s_divider.left ? Left() : Right();
        if (t && t != ActiveTab()) SplitFocusTab(t->id);
        return true;
    }
    case WM_LBUTTONUP:
        if (!s_dragging) return false;
        s_dragging = false;
        if (GetCapture() == g_hWnd) ReleaseCapture();
        AnimRequest();                                   // (SplitAnimStep commits once it has settled)
        InvalidateRect(g_hWnd, nullptr, FALSE);
        return true;
    case WM_MOUSELEAVE:
        if (s_hoverDivider) { s_hoverDivider = false; InvalidateRect(g_hWnd, &s_divider, FALSE); }
        return false;
    case WM_CAPTURECHANGED:
        if (s_dragging) { s_dragging = false; AnimRequest(); InvalidateRect(g_hWnd, nullptr, FALSE); }
        return false;
    }
    return false;
}

void SplitFocusTab(unsigned long long id) {
    int index = -1;
    Tab* t = FindTab(id, &index);
    if (!t || !SplitHas(*t) || t == ActiveTab()) return;
    SelectTab(index);
    InvalidateRect(g_hWnd, nullptr, FALSE);              // the outline moves
}

bool SplitAnimStep(float dt) {
    const bool cover = s_coverState == Cover::Moving;
    if (!cover && std::fabs(s_target - s_ratio) < 1e-4f) return false;
    if (!SplitShown()) { s_ratio = s_viewRatio = s_target; FreeCover(); return false; }
    const RECT pa = PageArea();
    const float inner = (float)std::max(1L, pa.right - pa.left - 2 * Margin() - Gutter());
    if (s_ratio != s_target) {
        s_ratio += (s_target - s_ratio) * AnimFactor(dt, 14.f);
        if (std::fabs(s_target - s_ratio) * inner < 0.5f) s_ratio = s_target;
        if (cover) RenderCover();
        else { s_viewRatio = s_ratio; LayoutWebViews(); InvalidateRect(g_hWnd, &pa, FALSE); }
    }
    if (s_ratio == s_target && cover && !s_dragging) {   // released and settled: the views follow
        CommitCover();
        return false;
    }
    return s_ratio != s_target || s_dragging;
}

void SplitCoverTimer() {
    switch (s_coverState) {
    case Cover::Waiting:
        if (GetTickCount() - s_coverSince > 700) { s_coverState = Cover::Fading; s_coverSince = GetTickCount(); }
        break;
    case Cover::Fading: {
        const DWORD t = GetTickCount() - s_coverSince;
        if (t >= 120) { FreeCover(); RepaintHalves(true); return; }
        s_coverAlpha = (BYTE)(255 - 255 * t / 120);
        PushCover();
        break;
    }
    default:
        FreeCover();
        break;
    }
}

void SplitTabDragOver(unsigned long long draggedId, int targetIndex, bool right) {
    unsigned long long target = 0;
    if (targetIndex >= 0 && targetIndex < (int)g_tabs.size() && g_tabs[targetIndex]->id != draggedId)
        target = g_tabs[targetIndex]->id;
    if (s_dragTab == draggedId && s_dropTab == target && s_dropRight == right) return;
    s_dragTab = draggedId;
    s_dropTab = target;
    s_dropRight = right;
    InvalidateChrome();
}

bool SplitTabDragging() { return s_dragTab != 0; }

void SplitTabDragEnd(bool drop) {
    const unsigned long long dragged = s_dragTab, target = s_dropTab;
    const bool right = s_dropRight;
    s_dragTab = s_dropTab = 0;
    InvalidateChrome();
    if (!drop || !dragged || !target) return;
    Tab* d = FindTab(dragged);
    Tab* t = FindTab(target);
    if (!d || !t || d == t) return;
    Log("split: tab#%llu dropped on tab#%llu (%s)", dragged, target, right ? "right" : "left");
    int index = -1;
    if (ActiveTab() != d && FindTab(dragged, &index)) SelectTab(index);   // the dragged tab is the open one
    if (right) Pair(target, dragged);
    else Pair(dragged, target);
    SessionLoadLazyTab(*t);
    FocusWebView();
}

bool SplitDropHint(int index, bool* right) {
    if (!s_dropTab || index < 0 || index >= (int)g_tabs.size() || g_tabs[index]->id != s_dropTab) return false;
    if (right) *right = s_dropRight;
    return true;
}

bool SplitStripPair(int* left, int* right) {
    int li = -1, ri = -1;
    if (!s_left || !s_right || !FindTab(s_left, &li) || !FindTab(s_right, &ri) || ri != li + 1) return false;
    if (left) *left = li;
    if (right) *right = ri;
    return true;
}

void SplitDetach(unsigned long long id) {
    Tab* t = FindTab(id);
    if (!t || !SplitPartnerOf(*t)) return;
    Log("split: tab#%llu dragged out -> ordinary tabs", id);
    int index = -1;
    if (ActiveTab() != t && FindTab(id, &index)) SelectTab(index);
    SplitClose();
}
