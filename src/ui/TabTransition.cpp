// Lyra — переключение вкладок без пустого экрана: последний кадр вкладки и плавный переход.
#include "ui/TabTransition.h"
#include "app/Log.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "core/Constants.h"
#include "core/Settings.h"
#include "ui/Chrome.h"
#include "ui/FullScreen.h"
#include "ui/MainWindow.h"

namespace {

struct Picture {
    HBITMAP bmp = nullptr;
    int w = 0, h = 0;
    DWORD used = 0;                  // GetTickCount of the last capture, for dropping old ones
};

std::map<unsigned long long, Picture> s_pictures;     // tab id -> its last picture
constexpr size_t kMaxPictures = 6;

HWND s_cover = nullptr;              // layered popup over the page area
HDC s_coverDC = nullptr;             // memory DC with the picture shown
HGDIOBJ s_coverOld = nullptr;
HBITMAP s_coverBmp = nullptr;        // a copy owned by the cover
SIZE s_coverSize = {};
POINT s_coverPos = {};
unsigned long long s_waitTab = 0;    // the tab whose drawing ends the cover
DWORD s_shownAt = 0, s_fadeAt = 0, s_paintedAt = 0;
int s_repaints = 0;                  // rendering updates still to wait for (see TransitionBegin)
// After WebKit reports the page drawn, its tiles can still be arriving for a few frames: the
// cover waits a little longer before it fades - longer for a tab that was hidden (its page has
// to wake up) than for a ready one.
constexpr DWORD kFadeMs = 180;
DWORD s_timeoutMs = 900, s_settleMs = 90;
bool s_waitLoad = false;             // the cover waits for didFinish first (a new tab)

void FreePicture(Picture& p) {
    if (p.bmp) DeleteObject(p.bmp);
    p = Picture();
}

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

// What is on screen in the page area right now (DWM's composed picture, so GPU-drawn pages
// come out right).
void Capture(unsigned long long tabId) {
    const RECT pa = PageArea();
    const int w = pa.right - pa.left, h = pa.bottom - pa.top;
    if (w <= 0 || h <= 0 || IsIconic(g_hWnd)) return;
    POINT tl = { pa.left, pa.top };
    ClientToScreen(g_hWnd, &tl);
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    Picture& p = s_pictures[tabId];
    if (!p.bmp || p.w != w || p.h != h) {
        FreePicture(p);
        p.bmp = NewDib(screen, w, h);
        p.w = w;
        p.h = h;
    }
    if (p.bmp) {
        HGDIOBJ old = SelectObject(mem, p.bmp);
        BitBlt(mem, 0, 0, w, h, screen, tl.x, tl.y, SRCCOPY);
        SelectObject(mem, old);
        p.used = GetTickCount();
    }
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    // Keep only the newest few.
    while (s_pictures.size() > kMaxPictures) {
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

void HideCover() {
    if (s_cover) ShowWindow(s_cover, SW_HIDE);
    if (s_coverDC) {
        SelectObject(s_coverDC, s_coverOld);
        DeleteDC(s_coverDC);
        s_coverDC = nullptr;
    }
    if (s_coverBmp) { DeleteObject(s_coverBmp); s_coverBmp = nullptr; }
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

}  // namespace

// Shows `pic` over the page area. False if it could not.
static bool ShowCover(const Picture* pic) {
    const RECT pa = PageArea();
    const int w = pa.right - pa.left, h = pa.bottom - pa.top;
    if (!pic || !EnsureCoverWindow()) return false;
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
    s_shownAt = GetTickCount();
    return true;
}

static const Picture* FittingPicture(unsigned long long id) {
    const RECT pa = PageArea();
    auto it = s_pictures.find(id);
    if (it == s_pictures.end() || !it->second.bmp) return nullptr;
    if (it->second.w != pa.right - pa.left || it->second.h != pa.bottom - pa.top) return nullptr;
    return &it->second;
}

void TransitionBegin(Tab* from, Tab* to) {
    if (!g_hWnd || !to || from == to || g_closing || g_fullScreenView || IsIconic(g_hWnd)) return;
    HideCover();
    if (from && from->hwnd && IsWindowVisible(from->hwnd)) Capture(from->id);
    // The tab's own last picture if it still fits the page area, else the tab being left.
    const Picture* pic = FittingPicture(to->id);
    if (!pic && from) pic = FittingPicture(from->id);
    if (!to->page || !ShowCover(pic)) return;
    // A hidden tab's page wakes up first; a ready (parked) one only draws.
    const bool ready = to->hwnd && IsWindowVisible(to->hwnd);
    // A page that was hidden draws its first update while it is still waking up (tiles keep
    // arriving): wait for a second full update before the cover goes.
    s_settleMs = ready ? 90 : 160;
    s_timeoutMs = ready ? 900 : 2000;
    s_repaints = ready ? 1 : 2;
    s_waitTab = to->id;
    WKPageForceRepaint(to->page, (void*)(uintptr_t)to->id, PaintedCallback);
    SetTimer(g_hWnd, TIMER_TRANSITION, 15, nullptr);
}

bool TransitionCoverFrom(Tab* from) {
    if (!g_hWnd || !from || g_closing || g_fullScreenView || IsIconic(g_hWnd)) return false;
    HideCover();
    if (from->hwnd && IsWindowVisible(from->hwnd)) Capture(from->id);
    if (!ShowCover(FittingPicture(from->id))) return false;
    s_settleMs = 60;
    s_timeoutMs = 1500;
    s_waitLoad = true;
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
    if (!s_waitLoad || tabId != s_waitTab) return;
    s_waitLoad = false;
    if (Tab* t = FindTab(tabId))
        if (t->page) { WKPageForceRepaint(t->page, (void*)(uintptr_t)tabId, PaintedCallback); return; }
    s_paintedAt = GetTickCount();
}

void TransitionPainted(unsigned long long tabId) {
    if (!s_waitTab || tabId != s_waitTab || s_paintedAt) return;
    if (--s_repaints > 0) {
        if (Tab* t = FindTab(tabId))
            if (t->page) { WKPageForceRepaint(t->page, (void*)(uintptr_t)tabId, PaintedCallback); return; }
    }
    s_paintedAt = GetTickCount();
}

void TransitionTick() {
    if (!s_waitTab) { HideCover(); return; }
    const DWORD now = GetTickCount();
    const Tab* active = ActiveTab();
    if (s_waitTab != (unsigned long long)-1 && (!active || active->id != s_waitTab)) { HideCover(); return; }
    if (s_waitTab == (unsigned long long)-1 && GetTickCount() - s_shownAt > 300) { HideCover(); return; }
    if (!s_fadeAt) {
        // Fade once the page has drawn (and one more frame has reached the screen), or give up.
        const bool ready = s_paintedAt && now - s_paintedAt >= s_settleMs;
        if (!ready && now - s_shownAt < s_timeoutMs) return;
        if (!g_prefs.animations) { HideCover(); return; }
        s_fadeAt = now;
    }
    const float t = std::min(1.f, (float)(now - s_fadeAt) / (float)kFadeMs);
    if (t >= 1.f) { HideCover(); return; }
    UpdateCover((BYTE)std::lround(255.f * (1.f - t) * (1.f - t)));
}

void TransitionCancel() {
    if (s_waitTab || (s_cover && IsWindowVisible(s_cover))) HideCover();
}

void TransitionForget(unsigned long long tabId) {
    auto it = s_pictures.find(tabId);
    if (it == s_pictures.end()) return;
    FreePicture(it->second);
    s_pictures.erase(it);
    if (s_waitTab == tabId) HideCover();
}
