// Lyra - tab switches, new tabs and closing tabs without an empty page area.
//
// WebKit draws a page that comes into view only with its next rendering update, and a new tab
// only once its page has loaded: on their own both show the bare window background for a moment.
// So the page area is covered by a picture of what is about to appear, and the cover goes
// (at once, no dissolve) once WebKit has drawn the real thing underneath:
//   switching to a tab   its own last picture (taken when it was left);
//   a new start page     the last picture of the start page (it looks the same in every tab);
//   closing a tab        the last picture of the tab that comes forward.
// Only when there is no such picture does the cover show the tab being left (or closed).
// Pictures are taken only of a tab that has been drawn, never while a cover is up, and from the
// window itself (PrintWindow), so neither an empty frame nor an overlapping window gets in. One
// change after another keeps the cover up and only swaps its picture: there is no gap between.
#include "ui/TabTransition.h"
#include "app/Log.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "core/Constants.h"
#include "core/Settings.h"
#include "core/Url.h"
#include "ui/Chrome.h"
#include "ui/FullScreen.h"
#include "ui/MainWindow.h"
#include "ui/SwipeNav.h"

#include <set>

namespace {

struct Picture {
    HBITMAP bmp = nullptr;
    int w = 0, h = 0;
    DWORD used = 0;                  // GetTickCount of the last capture, for dropping old ones
};

std::map<unsigned long long, Picture> s_pictures;     // tab id -> its last picture
Picture s_startPicture;                                // the start page, for new tabs
constexpr size_t kMaxPictures = 6;
std::set<unsigned long long> s_fresh;                  // new tabs not drawn yet: never pictured

HWND s_cover = nullptr;              // layered popup over the page area
HDC s_coverDC = nullptr;             // memory DC with the picture shown
HGDIOBJ s_coverOld = nullptr;
HBITMAP s_coverBmp = nullptr;        // a copy owned by the cover
SIZE s_coverSize = {};
POINT s_coverPos = {};
unsigned long long s_waitTab = 0;    // the tab whose drawing ends the cover
DWORD s_shownAt = 0, s_fadeAt = 0, s_paintedAt = 0;
int s_repaints = 0;                  // rendering updates still to wait for (see TransitionBegin)
// When WebKit reports the page drawn its tiles are painted, but the compositor still fades new
// tiles in (TextureMapperSparseBackingStore: 160 ms over the empty background). A tab coming
// forward always gets new tiles - a "ready" (parked) view lies outside the window and keeps none
// - so the cover waits for two full updates and then that fade-in before it dissolves. The cover
// shows the same page, so the wait is not seen.
constexpr DWORD kTileFadeInMs = 160, kSettleMs = kTileFadeInMs + 70;
DWORD s_fadeMs = 160;                // a picture of the same page dissolves quickly
DWORD s_timeoutMs = 2000, s_settleMs = kSettleMs;
bool s_waitLoad = false;             // the cover waits for didFinish first (a new tab)

void FreePicture(Picture& p) {
    if (p.bmp) DeleteObject(p.bmp);
    p = Picture();
}

bool CoverUp() { return s_cover && IsWindowVisible(s_cover); }

// The page area in client coordinates.
RECT PageArea() {
    RECT cr;
    GetClientRect(g_hWnd, &cr);
    RECT r = { (LONG)g_L.sideW, (LONG)g_L.chromeH, cr.right, cr.bottom };
    return r;
}

HBITMAP NewDib(HDC ref, int w, int h) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    return CreateDIBSection(ref, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
}

// The page area as the window shows it now, into `p`. While Lyra is the active window nothing
// lies over it and the screen is copied (fast); otherwise the window itself is asked for its
// picture (PrintWindow with PW_RENDERFULLCONTENT: DWM's picture, GPU-drawn pages included), so
// another window over Lyra never ends up in the picture.
bool CaptureFromScreen(Picture& p, const RECT& pa, int w, int h) {
    POINT tl = { pa.left, pa.top };
    ClientToScreen(g_hWnd, &tl);
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    if (!p.bmp || p.w != w || p.h != h) {
        FreePicture(p);
        p.bmp = NewDib(screen, w, h);
        p.w = w;
        p.h = h;
    }
    bool ok = false;
    if (p.bmp && mem) {
        HGDIOBJ old = SelectObject(mem, p.bmp);
        ok = BitBlt(mem, 0, 0, w, h, screen, tl.x, tl.y, SRCCOPY) != 0;
        SelectObject(mem, old);
        if (ok) p.used = GetTickCount();
    }
    if (mem) DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok;
}

bool CaptureInto(Picture& p) {
    const RECT pa = PageArea();
    const int w = pa.right - pa.left, h = pa.bottom - pa.top;
    if (w <= 0 || h <= 0 || IsIconic(g_hWnd) || !IsWindowVisible(g_hWnd)) return false;
    if (GetForegroundWindow() == g_hWnd) return CaptureFromScreen(p, pa, w, h);
    RECT cr;
    GetClientRect(g_hWnd, &cr);
    HDC screen = GetDC(nullptr);
    HDC whole = CreateCompatibleDC(screen), mem = CreateCompatibleDC(screen);
    HBITMAP all = NewDib(screen, cr.right, cr.bottom);
    bool ok = false;
    if (all && whole && mem) {
        HGDIOBJ oldAll = SelectObject(whole, all);
        if (PrintWindow(g_hWnd, whole, 0x1 | 0x2)) {          // PW_CLIENTONLY | PW_RENDERFULLCONTENT
            if (!p.bmp || p.w != w || p.h != h) {
                FreePicture(p);
                p.bmp = NewDib(screen, w, h);
                p.w = w;
                p.h = h;
            }
            if (p.bmp) {
                HGDIOBJ old = SelectObject(mem, p.bmp);
                BitBlt(mem, 0, 0, w, h, whole, pa.left, pa.top, SRCCOPY);
                SelectObject(mem, old);
                p.used = GetTickCount();
                ok = true;
            }
        }
        SelectObject(whole, oldAll);
    }
    if (all) DeleteObject(all);
    if (whole) DeleteDC(whole);
    if (mem) DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok;
}

// A picture of tab `t` if what is on screen really is that tab, drawn (not under a cover, not a
// new tab still loading). The start page's picture is kept apart for new tabs.
void Capture(const Tab& t) {
    if (CoverUp() || s_fresh.count(t.id) || !t.hwnd || !IsWindowVisible(t.hwnd)) return;
    if (!CaptureInto(s_pictures[t.id])) { s_pictures.erase(t.id); return; }
    if (IsStartPageUrl(t.url)) CaptureInto(s_startPicture);
    while (s_pictures.size() > kMaxPictures) {         // keep only the newest few
        auto oldest = s_pictures.begin();
        for (auto it = s_pictures.begin(); it != s_pictures.end(); ++it)
            if (it->second.used < oldest->second.used) oldest = it;
        FreePicture(oldest->second);
        s_pictures.erase(oldest);
    }
}

void UpdateCover(BYTE alpha) {
    if (!s_cover || !s_coverDC) return;
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, alpha, 0 };       // the picture is opaque: no per-pixel alpha
    POINT src = { 0, 0 };
    UpdateLayeredWindow(s_cover, nullptr, &s_coverPos, &s_coverSize, s_coverDC, &src, 0, &bf, ULW_ALPHA);
}

void FreeCoverPicture() {
    if (s_coverDC) {
        SelectObject(s_coverDC, s_coverOld);
        DeleteDC(s_coverDC);
        s_coverDC = nullptr;
    }
    if (s_coverBmp) { DeleteObject(s_coverBmp); s_coverBmp = nullptr; }
}

void HideCover() {
    if (s_cover) ShowWindow(s_cover, SW_HIDE);
    FreeCoverPicture();
    s_waitTab = 0;
    s_waitLoad = false;
    s_shownAt = s_fadeAt = s_paintedAt = 0;
    s_repaints = 0;
    if (g_hWnd) KillTimer(g_hWnd, TIMER_TRANSITION);
}

LRESULT CALLBACK CoverProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, msg, w, l);
}

bool EnsureCoverWindow() {
    if (s_cover) return true;
    static bool registered = false;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = CoverProc;
        wc.hInstance = hi;
        wc.lpszClassName = L"LyraTabCover";
        if (!RegisterClassExW(&wc)) return false;
        registered = true;
    }
    // A layered, click-through popup owned by the main window (so it stays above it).
    s_cover = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                              L"LyraTabCover", L"", WS_POPUP, 0, 0, 1, 1, g_hWnd, nullptr, hi, nullptr);
    return s_cover != nullptr;
}

void PaintedCallback(WKErrorRef, void* context) {
    // WebKit's thread is the UI thread here, but go through the queue so the cover goes away
    // after the page's own paint messages.
    if (g_hWnd) PostMessageW(g_hWnd, WM_APP_TAB_PAINTED, (WPARAM)(uintptr_t)context, 0);
}

const Picture* Fitting(const Picture* p) {
    if (!p || !p->bmp) return nullptr;
    const RECT pa = PageArea();
    return p->w == pa.right - pa.left && p->h == pa.bottom - pa.top ? p : nullptr;
}

const Picture* PictureOf(unsigned long long id) {
    auto it = s_pictures.find(id);
    return it == s_pictures.end() ? nullptr : Fitting(&it->second);
}

// Shows `pic` over the page area, fully opaque. A cover already up only changes its picture:
// it never goes away in between.
bool ShowCover(const Picture* pic) {
    const RECT pa = PageArea();
    const int w = pa.right - pa.left, h = pa.bottom - pa.top;
    if (!pic || !EnsureCoverWindow()) return false;
    FreeCoverPicture();
    HDC screen = GetDC(nullptr);
    s_coverBmp = NewDib(screen, w, h);
    s_coverDC = CreateCompatibleDC(screen);
    if (!s_coverBmp || !s_coverDC) {
        ReleaseDC(nullptr, screen);
        HideCover();
        return false;
    }
    s_coverOld = SelectObject(s_coverDC, s_coverBmp);
    HDC src = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(src, pic->bmp);
    BitBlt(s_coverDC, 0, 0, w, h, src, 0, 0, SRCCOPY);
    SelectObject(src, old);
    DeleteDC(src);
    ReleaseDC(nullptr, screen);

    POINT tl = { pa.left, pa.top };
    ClientToScreen(g_hWnd, &tl);
    s_coverPos = tl;
    s_coverSize = { w, h };
    UpdateCover(255);
    SetWindowPos(s_cover, HWND_TOP, tl.x, tl.y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
}

// The cover (shown or kept) now waits for `to` to draw.
void WaitFor(Tab* to, bool samePage) {
    const DWORD now = GetTickCount();
    s_shownAt = now;
    s_fadeAt = s_paintedAt = 0;
    s_fadeMs = samePage ? 160 : 220;
    // The first update may come while the page is still waking up: wait for a second one too.
    s_settleMs = kSettleMs;
    s_timeoutMs = 4000;                          // a page waking up may take a while: no empty area meanwhile
    s_repaints = 2;
    s_waitTab = to->id;
    s_waitLoad = false;
    WKPageForceRepaint(to->page, (void*)(uintptr_t)to->id, PaintedCallback);
    SetTimer(g_hWnd, TIMER_TRANSITION, 15, nullptr);
}

}  // namespace

void TransitionNewTab(unsigned long long tabId) { s_fresh.insert(tabId); }

void TransitionBegin(Tab* from, Tab* to) {
    if (!g_hWnd || !to || from == to || g_closing || g_fullScreenView || IsIconic(g_hWnd)) {
        if (CoverUp()) HideCover();
        return;
    }
    if (from) Capture(*from);                       // (not while a cover is up, nor a fresh tab)
    // What is about to appear: the tab's own last picture, the start page's for a start page,
    // and only failing those the tab being left.
    const Picture* pic = PictureOf(to->id);
    bool samePage = pic != nullptr;
    if (!pic && IsStartPageUrl(to->url)) { pic = Fitting(&s_startPicture); samePage = pic != nullptr; }
    if (!pic && from && !CoverUp()) pic = PictureOf(from->id);
    if (!to->page) { if (CoverUp()) HideCover(); return; }
    if (pic) { if (!ShowCover(pic)) return; }
    else if (!CoverUp()) return;                    // nothing to show: no cover (as before)
    else UpdateCover(255);                          // kept: it may have begun to fade
    WaitFor(to, samePage);
}

bool TransitionCoverFrom(Tab* from, bool startPage) {
    if (!g_hWnd || g_closing || g_fullScreenView || IsIconic(g_hWnd)) return false;
    if (from) Capture(*from);
    const Picture* pic = startPage ? Fitting(&s_startPicture) : nullptr;
    const bool samePage = pic != nullptr;
    if (!pic && from && !CoverUp()) pic = PictureOf(from->id);
    if (pic) { if (!ShowCover(pic)) return false; }
    else if (!CoverUp()) return false;
    else UpdateCover(255);
    s_shownAt = GetTickCount();
    s_fadeAt = s_paintedAt = 0;
    s_fadeMs = samePage ? 160 : 220;
    s_settleMs = kSettleMs;
    s_timeoutMs = 4000;                          // a page waking up may take a while: no empty area meanwhile
    s_waitLoad = true;
    s_repaints = 0;
    s_waitTab = (unsigned long long)-1;          // named by TransitionWaitLoad
    SetTimer(g_hWnd, TIMER_TRANSITION, 15, nullptr);
    return true;
}

void TransitionWaitLoad(Tab* to) {
    if (!s_waitLoad) return;
    if (!to) { HideCover(); return; }
    s_waitTab = to->id;
}

void TransitionLoaded(unsigned long long tabId) {
    if (!s_waitLoad || tabId != s_waitTab) { s_fresh.erase(tabId); return; }
    s_waitLoad = false;
    if (Tab* t = FindTab(tabId))
        if (t->page) { s_repaints = 2; WKPageForceRepaint(t->page, (void*)(uintptr_t)tabId, PaintedCallback); return; }
    s_fresh.erase(tabId);
    s_paintedAt = GetTickCount();
}

void TransitionPainted(unsigned long long tabId) {
    if (!s_waitTab || tabId != s_waitTab || s_paintedAt) return;
    if (--s_repaints > 0) {
        if (Tab* t = FindTab(tabId))
            if (t->page) { WKPageForceRepaint(t->page, (void*)(uintptr_t)tabId, PaintedCallback); return; }
    }
    s_fresh.erase(tabId);
    s_paintedAt = GetTickCount();
}

void TransitionTick() {
    if (!s_waitTab) { HideCover(); return; }
    const DWORD now = GetTickCount();
    const Tab* active = ActiveTab();
    if (s_waitTab != (unsigned long long)-1 && (!active || active->id != s_waitTab)) { HideCover(); return; }
    if (s_waitTab == (unsigned long long)-1 && now - s_shownAt > 300) { HideCover(); return; }
    // Gone at once when the page has drawn (and one more frame has reached the screen), or when
    // it takes too long. No dissolve: the cover is only there so that no empty page area shows,
    // and a picture of another page dissolving into this one looked like an animation of its own.
    const bool ready = s_paintedAt && now - s_paintedAt >= s_settleMs;
    if (!ready && now - s_shownAt < s_timeoutMs) return;
    if (!ready)                                     // (an empty page area may show now: logged)
        Log("transition: tab#%llu has not drawn after %lu ms, cover taken away", s_waitTab, (unsigned long)(now - s_shownAt));
    HideCover();
    // What is on screen now is the tab itself, drawn: its picture for the next time (and the
    // start page's for new tabs).
    if (Tab* a = ActiveTab()) if (!s_fresh.count(a->id)) Capture(*a);
}

bool TransitionCovering() { return CoverUp(); }

void TransitionCancel() {
    if (s_waitTab || CoverUp()) HideCover();
    SwipeNavCancel();
}

void TransitionForget(unsigned long long tabId) {
    SwipeNavForget(tabId);                           // its pages' pictures for swiping
    s_fresh.erase(tabId);
    auto it = s_pictures.find(tabId);
    if (it != s_pictures.end()) {
        FreePicture(it->second);
        s_pictures.erase(it);
    }
    if (s_waitTab == tabId) HideCover();
}
