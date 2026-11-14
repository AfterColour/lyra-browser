// Lyra - smooth changes of the whole window: the old picture dissolves over the new one.
//
// Changing the toolbar size, the tab layout or the theme rebuilds the window in one go and the
// pages are laid out again at their new size: without help that is a jump with a short freeze.
// Here the window's current picture (PrintWindow with PW_RENDERFULLCONTENT: what DWM shows,
// pages included) goes into a layered, click-through window placed exactly over the main
// window's client area and just above it in the z-order (so it never covers the settings
// window). It stays opaque for a moment while the new look is drawn underneath, then fades out.
// Quick changes one after another keep the picture already shown (the last settled look) instead
// of taking a new one mid-redraw, which could catch the empty window: it comes back to opaque if
// it had begun to fade, waits again, and dissolves once, straight to the final look.
#include "ui/Crossfade.h"
#include "core/Settings.h"
#include "ui/MainWindow.h"

#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

static HWND    s_wnd = nullptr;
static HDC     s_dc = nullptr;
static HBITMAP s_bmp = nullptr, s_oldBmp = nullptr;
static LARGE_INTEGER s_start = {}, s_freq = {};
static double  s_hold = 0, s_fade = 0, s_rise = 0;   // milliseconds
static double  s_alpha = 255, s_from = 255;      // shown now / where the rise back to opaque starts
static bool    s_period = false;                 // timeBeginPeriod(1) active (smooth timer steps)

static void Finish() {
    if (s_wnd) { KillTimer(s_wnd, 1); DestroyWindow(s_wnd); s_wnd = nullptr; }
    if (s_dc) { SelectObject(s_dc, s_oldBmp); DeleteDC(s_dc); s_dc = nullptr; }
    if (s_bmp) { DeleteObject(s_bmp); s_bmp = nullptr; }
    if (s_period) { timeEndPeriod(1); s_period = false; }
}

static void SetAlpha(BYTE a) {
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, a, 0 };
    UpdateLayeredWindow(s_wnd, nullptr, nullptr, nullptr, nullptr, nullptr, 0, &bf, ULW_ALPHA);
}

static LRESULT CALLBACK CrossfadeProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_TIMER && h == s_wnd) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const double ms = (double)(now.QuadPart - s_start.QuadPart) * 1000.0 / (double)s_freq.QuadPart;
        if (ms < s_rise) {                       // a new change came during the fade: back to opaque
            const double t = ms / s_rise;
            s_alpha = s_from + (255.0 - s_from) * (1.0 - (1.0 - t) * (1.0 - t));
            SetAlpha((BYTE)std::lround(s_alpha));
            return 0;
        }
        if (ms < s_rise + s_hold) {
            if (s_alpha < 255) { s_alpha = 255; SetAlpha(255); }
            return 0;
        }
        const double t = std::min(1.0, (ms - s_rise - s_hold) / s_fade);
        if (t >= 1.0) { Finish(); return 0; }
        const double e = 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);     // ease out
        s_alpha = 255.0 * (1.0 - e);
        SetAlpha((BYTE)std::lround(s_alpha));
        return 0;
    }
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

static void Timing() {
    const double speed = std::max(25, g_prefs.animSpeed) / 100.0;   // Settings > Animations > Speed
    s_hold = 140.0 / speed;                      // the new look gets drawn underneath meanwhile
    s_fade = 340.0 / speed;
    QueryPerformanceFrequency(&s_freq);
    QueryPerformanceCounter(&s_start);
}

void CrossfadeBegin() {
    if (s_wnd) {
        // Already showing a settled picture: keep it, rise back to opaque, wait for this change too.
        Timing();
        s_from = s_alpha;
        s_rise = s_alpha < 250 ? 110.0 * (255.0 - s_alpha) / 255.0 + 40.0 : 0.0;
        return;
    }
    if (!g_hWnd || !g_prefs.animations || !IsWindowVisible(g_hWnd) || IsIconic(g_hWnd)) return;

    RECT cr;
    GetClientRect(g_hWnd, &cr);
    const int w = cr.right, h = cr.bottom;
    if (w <= 0 || h <= 0) return;
    POINT origin = { 0, 0 };
    ClientToScreen(g_hWnd, &origin);

    HDC screen = GetDC(nullptr);
    s_dc = CreateCompatibleDC(screen);
    s_bmp = CreateCompatibleBitmap(screen, w, h);
    ReleaseDC(nullptr, screen);
    if (!s_dc || !s_bmp) { Finish(); return; }
    s_oldBmp = (HBITMAP)SelectObject(s_dc, s_bmp);
    // PW_CLIENTONLY | PW_RENDERFULLCONTENT: the composed picture, web views included.
    if (!PrintWindow(g_hWnd, s_dc, 0x1 | 0x2)) { Finish(); return; }

    static bool registered = false;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = CrossfadeProc;
        wc.hInstance = hi;
        wc.lpszClassName = L"LyraCrossfade";
        if (!RegisterClassW(&wc)) { Finish(); return; }
        registered = true;
    }
    s_wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                            L"LyraCrossfade", L"", WS_POPUP, origin.x, origin.y, w, h, g_hWnd, nullptr, hi, nullptr);
    if (!s_wnd) { Finish(); return; }

    POINT src = { 0, 0 }, dst = origin;
    SIZE size = { w, h };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, 0 };
    UpdateLayeredWindow(s_wnd, nullptr, &dst, &size, s_dc, &src, 0, &bf, ULW_ALPHA);
    // Just above the main window: under whatever is above it (the settings window, other apps).
    HWND above = GetWindow(g_hWnd, GW_HWNDPREV);
    while (above == s_wnd) above = GetWindow(above, GW_HWNDPREV);
    SetWindowPos(s_wnd, above ? above : HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);

    Timing();
    s_alpha = s_from = 255;
    s_rise = 0;
    s_period = timeBeginPeriod(1) == TIMERR_NOERROR;
    SetTimer(s_wnd, 1, 4, nullptr);
}
