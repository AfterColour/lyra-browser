// Lyra - swiping back and forward (see the header).
//
// The swipe is drawn in a click-through popup laid exactly over the page window: a picture of the
// page as it is now on top, sliding with the fingers, and the picture of where the swipe leads
// underneath (dimmed, with a slight parallax), with a soft shadow along the moving edge - all
// composed with GDI into one frame per display refresh. The live page is not moved: when the tab
// navigates, the picture of the target stays until WebKit has drawn the new page underneath
// (two forced repaints plus the tiles' fade-in), then the popup goes and the real page is there.
//
// Pictures are copies of the screen (or of the window, when Lyra is not in front) taken right
// before a page is left; they are kept per back/forward item (WKBackForwardListItemRef), the
// newest few only.
#include "ui/SwipeNav.h"
#include "app/Log.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "core/Constants.h"
#include "core/Settings.h"
#include "ui/Animation.h"
#include "ui/MainWindow.h"
#include "ui/TabTransition.h"
#include "ui/Theme.h"

namespace {

struct Shot {
    unsigned long long tab = 0;
    WKBackForwardListItemRef item = nullptr;   // retained
    HBITMAP bmp = nullptr;
    int w = 0, h = 0;
    DWORD used = 0;
};
std::vector<Shot> s_shots;
constexpr size_t kMaxShots = 12;               // ~7 MB each for a large window

enum class Phase { Idle, Tracking, Settling, Waiting };
Phase s_phase = Phase::Idle;
unsigned long long s_tab = 0;
bool s_back = true;
int s_w = 0, s_h = 0;                          // the page window, device pixels
float s_off = 0, s_end = 0;                    // the top picture's offset (back: 0..w, forward: -w..0)
float s_vel = 0;                               // px/s, smoothed
LARGE_INTEGER s_lastMove = {};
bool s_commit = false;
DWORD s_waitStart = 0, s_paintedAt = 0;
int s_repaints = 0;
bool s_repaintAsked = false;

HWND s_wnd = nullptr;
HDC s_frameDC = nullptr, s_curDC = nullptr, s_targetDC = nullptr, s_shadowDC = nullptr, s_blackDC = nullptr;
HBITMAP s_frameBmp = nullptr, s_curBmp = nullptr, s_targetBmp = nullptr, s_shadowBmp = nullptr, s_blackBmp = nullptr;
HGDIOBJ s_frameOld = nullptr, s_curOld = nullptr, s_targetOld = nullptr, s_shadowOld = nullptr, s_blackOld = nullptr;
int s_shadowW = 24;
int s_dumpNext = 0;                            // LYRA_SWIPE_DUMP: the next mark

HBITMAP NewDib(int w, int h, void** bits = nullptr) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* p = nullptr;
    HBITMAP b = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
    if (bits) *bits = p;
    return b;
}

// The page window `view` as it is on screen now, into a new bitmap.
HBITMAP CaptureView(HWND view, int& w, int& h) {
    RECT wr;
    if (!view || !GetWindowRect(view, &wr) || IsIconic(g_hWnd) || !IsWindowVisible(view)) return nullptr;
    w = wr.right - wr.left;
    h = wr.bottom - wr.top;
    if (w <= 0 || h <= 0) return nullptr;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = NewDib(w, h);
    bool ok = false;
    if (bmp && mem) {
        HGDIOBJ old = SelectObject(mem, bmp);
        if (GetForegroundWindow() == g_hWnd) {
            ok = BitBlt(mem, 0, 0, w, h, screen, wr.left, wr.top, SRCCOPY) != 0;
        } else {                                       // another window may lie over Lyra
            RECT cr;
            GetClientRect(g_hWnd, &cr);
            POINT tl = { wr.left, wr.top };
            ScreenToClient(g_hWnd, &tl);
            HDC whole = CreateCompatibleDC(screen);
            HBITMAP all = NewDib(cr.right, cr.bottom);
            if (whole && all) {
                HGDIOBJ oldAll = SelectObject(whole, all);
                if (PrintWindow(g_hWnd, whole, 0x1 | 0x2))  // PW_CLIENTONLY | PW_RENDERFULLCONTENT
                    ok = BitBlt(mem, 0, 0, w, h, whole, tl.x, tl.y, SRCCOPY) != 0;
                SelectObject(whole, oldAll);
            }
            if (all) DeleteObject(all);
            if (whole) DeleteDC(whole);
        }
        SelectObject(mem, old);
    }
    if (mem) DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    if (!ok && bmp) { DeleteObject(bmp); bmp = nullptr; }
    return bmp;
}

WKBackForwardListItemRef ItemOf(Tab& t, int which) {     // -1 back, 0 current, 1 forward
    WKBackForwardListRef list = t.page ? WKPageGetBackForwardList(t.page) : nullptr;
    if (!list) return nullptr;
    return which < 0 ? WKBackForwardListGetBackItem(list) : which > 0 ? WKBackForwardListGetForwardItem(list)
                     : WKBackForwardListGetCurrentItem(list);
}

void FreeShot(Shot& s) {
    if (s.bmp) DeleteObject(s.bmp);
    if (s.item) WKRelease(s.item);
    s = Shot();
}

Shot* FindShot(unsigned long long tab, WKBackForwardListItemRef item) {
    if (!item) return nullptr;
    for (auto& s : s_shots) if (s.tab == tab && s.item == item) return &s;
    return nullptr;
}

// Keeps `bmp` (taken over) as the picture of `item` in tab `tab`.
void Store(unsigned long long tab, WKBackForwardListItemRef item, HBITMAP bmp, int w, int h) {
    if (!item || !bmp) { if (bmp) DeleteObject(bmp); return; }
    Shot* s = FindShot(tab, item);
    if (s) { DeleteObject(s->bmp); }
    else {
        s_shots.push_back(Shot());
        s = &s_shots.back();
        s->tab = tab;
        s->item = item;
        WKRetain(item);
    }
    s->bmp = bmp;
    s->w = w;
    s->h = h;
    s->used = GetTickCount();
    while (s_shots.size() > kMaxShots) {               // the oldest goes
        size_t oldest = 0;
        for (size_t i = 1; i < s_shots.size(); ++i) if (s_shots[i].used < s_shots[oldest].used) oldest = i;
        FreeShot(s_shots[oldest]);
        s_shots.erase(s_shots.begin() + (ptrdiff_t)oldest);
    }
}

void SelectInto(HDC& dc, HBITMAP bmp, HGDIOBJ& old) {
    dc = CreateCompatibleDC(nullptr);
    old = SelectObject(dc, bmp);
}

void FreeDC(HDC& dc, HBITMAP& bmp, HGDIOBJ& old) {
    if (dc) { SelectObject(dc, old); DeleteDC(dc); dc = nullptr; }
    if (bmp) { DeleteObject(bmp); bmp = nullptr; }
    old = nullptr;
}

void FreeFrames() {
    FreeDC(s_frameDC, s_frameBmp, s_frameOld);
    FreeDC(s_curDC, s_curBmp, s_curOld);
    FreeDC(s_targetDC, s_targetBmp, s_targetOld);
    FreeDC(s_shadowDC, s_shadowBmp, s_shadowOld);
    FreeDC(s_blackDC, s_blackBmp, s_blackOld);
}

void Hide() {
    if (s_wnd) ShowWindow(s_wnd, SW_HIDE);
    FreeFrames();
    s_phase = Phase::Idle;
    s_tab = 0;
    s_repaints = 0;
    s_repaintAsked = false;
    s_paintedAt = 0;
}

void Blend(HDC dst, int x, int w, HDC src, int srcW, BYTE constAlpha, bool perPixel) {
    if (w <= 0) return;
    int sx = 0, sw = srcW;
    if (x < 0) {                                       // clipped on the left (strips only)
        if (srcW == w) { sx = -x; sw = srcW + x; }
        w += x;
        x = 0;
    }
    if (x + w > s_w) { const int cut = x + w - s_w; w -= cut; if (srcW != 1) sw -= cut; }
    if (w <= 0 || sw <= 0) return;
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, constAlpha, (BYTE)(perPixel ? AC_SRC_ALPHA : 0) };
    GdiAlphaBlend(dst, x, 0, w, s_h, src, srcW == 1 ? 0 : sx, 0, srcW == 1 ? 1 : sw, 1, bf);
}

// LYRA_SWIPE_DUMP=<prefix>: developer check, the composed frame goes to <prefix>-<n>.png as the
// swipe passes 25, 50 and 75 % (and at its end).
void DumpFrame(float progress) {
    static wchar_t prefix[MAX_PATH] = {};
    static int state = -1;
    int& next = s_dumpNext;
    if (state < 0) state = GetEnvironmentVariableW(L"LYRA_SWIPE_DUMP", prefix, MAX_PATH) ? 1 : 0;
    if (!state) return;
    static const float marks[] = { 0.25f, 0.5f, 0.75f, 0.999f };
    if (next >= 4 || progress < marks[next]) return;
    ++next;
    UINT n = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&n, &size);
    std::vector<BYTE> buf(size);
    auto* codecs = (Gdiplus::ImageCodecInfo*)buf.data();
    Gdiplus::GetImageEncoders(n, size, codecs);
    static int file = 0;
    const std::wstring path = std::wstring(prefix) + L"-" + std::to_wstring(++file) + L".png";
    Gdiplus::Bitmap bmp(s_frameBmp, nullptr);
    for (UINT i = 0; i < n; ++i) if (wcscmp(codecs[i].MimeType, L"image/png") == 0) bmp.Save(path.c_str(), &codecs[i].Clsid);
}

void Draw() {
    if (!s_wnd || !s_frameDC) return;
    const int W = s_w, H = s_h;
    const float o = std::fabs(s_off), p = std::min(1.f, o / (float)W);
    const int io = (int)std::lround(o);
    const BYTE dim = (BYTE)std::lround(70.f * (1.f - p));
    if (s_back) {
        const int ux = (int)std::lround(-0.3f * W * (1.f - p));      // the previous page, below
        BitBlt(s_frameDC, ux, 0, W, H, s_targetDC, 0, 0, SRCCOPY);
        if (dim && io > 0) Blend(s_frameDC, 0, io, s_blackDC, 1, dim, false);
        if (io < W) BitBlt(s_frameDC, io, 0, W - io, H, s_curDC, 0, 0, SRCCOPY);   // this page, on top
        if (io > 0 && io < W) Blend(s_frameDC, io - s_shadowW, s_shadowW, s_shadowDC, s_shadowW, 255, true);
    } else {
        const int ux = (int)std::lround(-0.3f * o);                    // this page, below
        BitBlt(s_frameDC, ux, 0, W, H, s_curDC, 0, 0, SRCCOPY);
        if (dim && W - io > 0) Blend(s_frameDC, 0, W - io, s_blackDC, 1, dim, false);
        if (io > 0) BitBlt(s_frameDC, W - io, 0, io, H, s_targetDC, 0, 0, SRCCOPY);   // the next page, on top
        if (io > 0 && io < W) Blend(s_frameDC, W - io - s_shadowW, s_shadowW, s_shadowDC, s_shadowW, 255, true);
    }
    HDC dc = GetDC(s_wnd);
    BitBlt(dc, 0, 0, W, H, s_frameDC, 0, 0, SRCCOPY);
    ReleaseDC(s_wnd, dc);
    DumpFrame(p);
}

LRESULT CALLBACK SwipeProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (s_frameDC) BitBlt(dc, 0, 0, s_w, s_h, s_frameDC, 0, 0, SRCCOPY);
        EndPaint(h, &ps);
        return 0;
    }
    default: return DefWindowProcW(h, msg, w, l);
    }
}

bool EnsureWindow() {
    if (s_wnd) return true;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = SwipeProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"LyraSwipe";
    RegisterClassExW(&wc);
    s_wnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"LyraSwipe", L"", WS_POPUP,
                            0, 0, 1, 1, g_hWnd, nullptr, hi, nullptr);
    return s_wnd != nullptr;
}

void PaintedCallback(WKErrorRef, void* context) {
    if (g_hWnd) PostMessageW(g_hWnd, WM_APP_SWIPE_PAINTED, (WPARAM)(uintptr_t)context, 0);
}

void AskRepaint() {
    Tab* t = FindTab(s_tab);
    if (!t || !t->page) return;
    s_repaintAsked = true;
    s_repaints = 2;
    s_paintedAt = 0;
    WKPageForceRepaint(t->page, (void*)(uintptr_t)s_tab, PaintedCallback);
}

}  // namespace

bool SwipeNavActive() { return s_phase != Phase::Idle; }

bool SwipeNavBegin(Tab& t, bool back, float offset) {
    if (s_phase == Phase::Waiting) Hide();
    if (s_phase != Phase::Idle || !g_hWnd || g_closing || !t.hwnd || !t.page || TransitionCovering()) return false;
    int w = 0, h = 0;
    HBITMAP cur = CaptureView(t.hwnd, w, h);
    if (!cur || w < 100 || h < 50 || !EnsureWindow()) { if (cur) DeleteObject(cur); return false; }

    s_w = w;
    s_h = h;
    s_curBmp = cur;
    SelectInto(s_curDC, s_curBmp, s_curOld);
    // Where the swipe leads: its picture (scaled if the window has changed size since), or the
    // empty page colour.
    s_targetBmp = NewDib(w, h);
    SelectInto(s_targetDC, s_targetBmp, s_targetOld);
    WKBackForwardListItemRef target = ItemOf(t, back ? -1 : 1);
    Shot* shot = FindShot(t.id, target);
    if (shot) {
        HDC src = CreateCompatibleDC(nullptr);
        HGDIOBJ old = SelectObject(src, shot->bmp);
        if (shot->w == w && shot->h == h) BitBlt(s_targetDC, 0, 0, w, h, src, 0, 0, SRCCOPY);
        else {
            SetStretchBltMode(s_targetDC, HALFTONE);
            SetBrushOrgEx(s_targetDC, 0, 0, nullptr);
            StretchBlt(s_targetDC, 0, 0, w, h, src, 0, 0, shot->w, shot->h, SRCCOPY);
        }
        SelectObject(src, old);
        DeleteDC(src);
        shot->used = GetTickCount();
    } else {
        const Gdiplus::Color c = g_th.page;
        HBRUSH br = CreateSolidBrush(RGB(c.GetR(), c.GetG(), c.GetB()));
        RECT r = { 0, 0, w, h };
        FillRect(s_targetDC, &r, br);
        DeleteObject(br);
    }
    // The page as it is now is the picture of its own item too (for swiping back to it later).
    {
        HBITMAP copy = NewDib(w, h);
        if (copy) {
            HDC dc = CreateCompatibleDC(nullptr);
            HGDIOBJ old = SelectObject(dc, copy);
            BitBlt(dc, 0, 0, w, h, s_curDC, 0, 0, SRCCOPY);
            SelectObject(dc, old);
            DeleteDC(dc);
            Store(t.id, ItemOf(t, 0), copy, w, h);
        }
    }
    // The shadow along the moving edge (fades out to the left) and a black pixel for the dimming.
    s_shadowW = MulDiv(26, (int)GetDpiForWindow(g_hWnd), 96);
    void* bits = nullptr;
    s_shadowBmp = NewDib(s_shadowW, 1, &bits);
    if (bits) {
        auto* px = (uint32_t*)bits;
        for (int i = 0; i < s_shadowW; ++i) {
            const float f = (float)(i + 1) / (float)s_shadowW;          // 0 far left .. 1 at the edge
            const uint32_t a = (uint32_t)std::lround(75.f * f * f);
            px[i] = a << 24;                                             // premultiplied black
        }
    }
    SelectInto(s_shadowDC, s_shadowBmp, s_shadowOld);
    void* bb = nullptr;
    s_blackBmp = NewDib(1, 1, &bb);
    if (bb) *(uint32_t*)bb = 0;
    SelectInto(s_blackDC, s_blackBmp, s_blackOld);
    s_frameBmp = NewDib(w, h);
    SelectInto(s_frameDC, s_frameBmp, s_frameOld);

    s_tab = t.id;
    s_back = back;
    s_off = back ? std::max(0.f, std::min((float)w, offset)) : std::max(-(float)w, std::min(0.f, offset));
    s_vel = 0;
    s_lastMove.QuadPart = 0;
    s_commit = false;
    s_phase = Phase::Tracking;
    s_dumpNext = 0;
    Draw();
    RECT wr;
    GetWindowRect(t.hwnd, &wr);
    PopupShowBegin(s_wnd);
    SetWindowPos(s_wnd, HWND_TOP, wr.left, wr.top, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    Draw();
    PopupShowEnd(s_wnd);
    Log("swipe: %s tab#%llu, picture %s", back ? "back" : "forward", t.id, shot ? "kept" : "none (page colour)");
    return true;
}

void SwipeNavMove(float dx) {
    if (s_phase != Phase::Tracking) return;
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (s_lastMove.QuadPart) {
        const float dt = (float)(now.QuadPart - s_lastMove.QuadPart) / (float)freq.QuadPart;
        if (dt > 0.0005f) s_vel = 0.6f * s_vel + 0.4f * (dx / std::min(dt, 0.05f));
    }
    s_lastMove = now;
    s_off += dx;
    s_off = s_back ? std::max(0.f, std::min((float)s_w, s_off)) : std::max(-(float)s_w, std::min(0.f, s_off));
    Draw();
}

void SwipeNavRelease() {
    if (s_phase != Phase::Tracking) return;
    const float dir = s_back ? 1.f : -1.f;
    const float progress = s_off * dir / (float)s_w, v = s_vel * dir;
    s_commit = (progress > 0.33f && v > -250.f) || (v > 900.f && progress > 0.04f);
    s_end = s_commit ? dir * (float)s_w : 0.f;
    s_phase = Phase::Settling;
    Log("swipe: released at %.0f%% (%.0f px/s) -> %s", progress * 100, v, s_commit ? (s_back ? "back" : "forward") : "stay");
    if (s_commit) {
        if (Tab* t = FindTab(s_tab)) {
            if (s_back) WKPageGoBack(t->page);       // loading starts while the picture slides
            else WKPageGoForward(t->page);
        }
    }
    AnimRequest();
}

bool SwipeNavAnimStep(float dt) {
    if (s_phase == Phase::Idle) return false;
    Tab* t = FindTab(s_tab);
    if (!t || t != ActiveTab() || g_closing) { Hide(); return false; }
    if (s_phase == Phase::Tracking) return false;    // drawn as the fingers move
    if (s_phase == Phase::Settling) {
        const float f = g_prefs.animations ? AnimFactor(dt, 45.f) : 1.f;
        const float rest = s_end - s_off, minStep = 1400.f * dt;
        s_off = std::fabs(rest) <= minStep ? s_end : s_off + std::copysign(std::max(std::fabs(rest) * f, minStep), rest);
        Draw();
        if (s_off != s_end) return true;
        if (!s_commit) { Hide(); return false; }
        s_phase = Phase::Waiting;
        s_waitStart = GetTickCount();
        return true;
    }
    // Waiting for the new page to draw under the picture.
    const DWORD now = GetTickCount();
    if (!s_repaintAsked && now - s_waitStart > 400) AskRepaint();    // (a same-page history step: no commit)
    if (s_paintedAt && now - s_paintedAt >= 230) { Hide(); return false; }
    if (now - s_waitStart > 3000) {
        Log("swipe: tab#%llu has not drawn after 3 s, picture taken away", s_tab);
        Hide();
        return false;
    }
    return true;
}

void SwipeNavLeavingPage(Tab& t) {
    if (s_phase != Phase::Idle || g_closing || &t != ActiveTab() || !t.hwnd || !IsWindowVisible(t.hwnd) || TransitionCovering()) return;
    WKBackForwardListItemRef item = ItemOf(t, 0);
    if (!item) return;
    int w = 0, h = 0;
    if (HBITMAP bmp = CaptureView(t.hwnd, w, h)) Store(t.id, item, bmp, w, h);
}

void SwipeNavCommitted(unsigned long long tabId) {
    if (s_phase == Phase::Idle || tabId != s_tab || !s_commit) return;
    AskRepaint();
}

void SwipeNavPainted(unsigned long long tabId) {
    if (s_phase == Phase::Idle || tabId != s_tab || !s_repaints) return;
    if (--s_repaints > 0) {
        if (Tab* t = FindTab(tabId)) { WKPageForceRepaint(t->page, (void*)(uintptr_t)tabId, PaintedCallback); return; }
    }
    s_paintedAt = std::max<DWORD>(1, GetTickCount());
}

void SwipeNavForget(unsigned long long tabId) {
    if (s_tab == tabId && s_phase != Phase::Idle) Hide();
    for (size_t i = 0; i < s_shots.size();) {
        if (s_shots[i].tab == tabId) { FreeShot(s_shots[i]); s_shots.erase(s_shots.begin() + (ptrdiff_t)i); }
        else ++i;
    }
}

void SwipeNavCancel() {
    if (s_phase != Phase::Idle) Hide();
}
