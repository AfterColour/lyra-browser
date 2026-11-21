// Lyra — часы анимаций: кадры в такт обновлению экрана, плавное приближение к цели.
#include "ui/Animation.h"
#include "browser/SplitView.h"
#include "ui/TabDrag.h"
#include "core/Constants.h"
#include "core/Settings.h"
#include "ui/Chrome.h"
#include "ui/MainWindow.h"
#include "ui/SwipeNav.h"
#include "browser/Touchpad.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

static std::thread             s_thread;
static std::mutex              s_lock;
static std::condition_variable s_wake;
static bool                    s_running = false;     // frames wanted (guarded by s_lock)
static bool                    s_quit = false;
static std::atomic<bool>       s_posted { false };    // a frame message is waiting in the queue
static LARGE_INTEGER           s_last = {};

// Waits for each display refresh while frames are wanted and posts one frame message at a time,
// so a busy UI thread never gets a backlog of frames.
static void ClockThread() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(s_lock);
            s_wake.wait(lock, [] { return s_running || s_quit; });
            if (s_quit) return;
        }
        if (FAILED(DwmFlush())) Sleep(7);            // no composition: ~144 Hz fallback
        if (!s_posted.exchange(true) && g_hWnd)
            PostMessageW(g_hWnd, WM_APP_ANIM_FRAME, 0, 0);
    }
}

void AnimRequest() {
    std::lock_guard<std::mutex> lock(s_lock);
    if (s_quit) return;
    if (!s_running) s_last.QuadPart = 0;              // first frame after a pause gets a nominal dt
    s_running = true;
    if (!s_thread.joinable()) s_thread = std::thread(ClockThread);
    s_wake.notify_one();
}

void AnimOnFrame() {
    s_posted = false;
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = 1.f / 120.f;
    if (s_last.QuadPart) dt = (float)(now.QuadPart - s_last.QuadPart) / (float)freq.QuadPart;
    dt = std::max(0.f, std::min(dt, 0.05f));
    s_last = now;
    const bool chrome = ChromeAnimStep(dt);
    const bool settings = SettingsAnimStep(dt);
    const bool split = SplitAnimStep(dt);
    const bool drag = TabDragAnimStep(dt);
    const bool touchpad = TouchpadAnimStep(dt);
    const bool swipe = SwipeNavAnimStep(dt);
    if (!chrome && !settings && !split && !drag && !touchpad && !swipe) {
        std::lock_guard<std::mutex> lock(s_lock);
        s_running = false;
    }
}

void AnimShutdown() {
    {
        std::lock_guard<std::mutex> lock(s_lock);
        s_quit = true;
        s_wake.notify_one();
    }
    if (s_thread.joinable()) s_thread.join();
}

bool AnimTabsEnabled()  { return g_prefs.animations && g_prefs.animTabs; }
bool AnimHoverEnabled() { return g_prefs.animations && g_prefs.animHover; }

float AnimFactor(float dt, float tauMs) {
    const float tau = tauMs / 1000.f * 100.f / (float)std::max(25, g_prefs.animSpeed);
    return 1.f - std::exp(-dt / std::max(0.001f, tau));
}

// ----------------------------------------------------------------------------
//  Popups unfolding
// ----------------------------------------------------------------------------
namespace {
struct Reveal {
    HWND hwnd;
    RECT full;                       // final window rectangle (screen)
    DWORD start;
};
std::vector<Reveal> s_reveals;
UINT_PTR s_revealTimer = 0;
constexpr DWORD kRevealMs = 150;

// The window itself grows from its top edge (its contents stay where they are: every popup lays
// out from the top). Growing the window, not cutting it with a region, keeps DWM's rounded
// corners, border and shadow on the visible part - with a region they showed at the full size
// at once, an empty plate under the unfolding menu.
void SetRevealHeight(const Reveal& r, int h) {
    SetWindowPos(r.hwnd, nullptr, 0, 0, r.full.right - r.full.left, h,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void CALLBACK RevealStep(HWND, UINT, UINT_PTR, DWORD) {
    const DWORD now = GetTickCount();
    for (size_t i = 0; i < s_reveals.size();) {
        Reveal& r = s_reveals[i];
        const float t = std::min(1.f, (float)(now - r.start) / (float)kRevealMs * ((float)std::max(25, g_prefs.animSpeed) / 100.f));
        const float e = 1.f - (1.f - t) * (1.f - t) * (1.f - t);        // ease-out
        const int fullH = r.full.bottom - r.full.top;
        const int h = std::max(1, (int)std::lround(fullH * (0.35f + 0.65f * e)));
        if (IsWindow(r.hwnd)) {
            SetRevealHeight(r, t >= 1.f ? fullH : h);
            UpdateWindow(r.hwnd);                                         // the new strip now, not after the next step
        }
        if (t >= 1.f || !IsWindow(r.hwnd)) s_reveals.erase(s_reveals.begin() + i);
        else ++i;
    }
    if (s_reveals.empty() && s_revealTimer) {
        KillTimer(nullptr, s_revealTimer);
        s_revealTimer = 0;
    }
}
}  // namespace

void PopupReveal(HWND popup) {
    if (!popup || !g_prefs.animations || !AnimHoverEnabled()) return;
    PopupRevealStop(popup);
    Reveal r;
    r.hwnd = popup;
    GetWindowRect(popup, &r.full);
    r.start = GetTickCount();
    s_reveals.push_back(r);
    SetRevealHeight(r, std::max(1, (int)((r.full.bottom - r.full.top) * 0.35f)));
    if (!s_revealTimer) s_revealTimer = SetTimer(nullptr, 0, 10, RevealStep);
}

void PopupRevealStop(HWND popup) {
    for (size_t i = 0; i < s_reveals.size(); ++i) {
        if (s_reveals[i].hwnd != popup) continue;
        if (IsWindow(popup)) SetRevealHeight(s_reveals[i], s_reveals[i].full.bottom - s_reveals[i].full.top);
        s_reveals.erase(s_reveals.begin() + i);
        break;
    }
}

void PopupShowBegin(HWND popup) {
    const BOOL cloak = TRUE;
    DwmSetWindowAttribute(popup, 13 /* DWMWA_CLOAK */, &cloak, sizeof(cloak));
}

void PopupShowEnd(HWND popup) {
    UpdateWindow(popup);                 // WM_PAINT now, while still cloaked
    PopupReveal(popup);
    const BOOL cloak = FALSE;
    DwmSetWindowAttribute(popup, 13 /* DWMWA_CLOAK */, &cloak, sizeof(cloak));
}

// ----------------------------------------------------------------------------
//  Popups fading out
// ----------------------------------------------------------------------------
namespace {
struct Ghost {
    HWND hwnd;
    HDC dc;
    HBITMAP bmp, old;
    DWORD start;
};
std::vector<Ghost> s_ghosts;
UINT_PTR s_ghostTimer = 0;
constexpr DWORD kFadeMs = 140;

void FreeGhost(Ghost& g) {
    if (IsWindow(g.hwnd)) DestroyWindow(g.hwnd);
    SelectObject(g.dc, g.old);
    DeleteObject(g.bmp);
    DeleteDC(g.dc);
}

LRESULT CALLBACK GhostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        for (auto& g : s_ghosts)
            if (g.hwnd == h) {
                RECT r;
                GetClientRect(h, &r);
                BitBlt(dc, 0, 0, r.right, r.bottom, g.dc, 0, 0, SRCCOPY);
            }
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

void CALLBACK GhostStep(HWND, UINT, UINT_PTR, DWORD) {
    const DWORD now = GetTickCount();
    for (size_t i = 0; i < s_ghosts.size();) {
        Ghost& g = s_ghosts[i];
        const float t = std::min(1.f, (float)(now - g.start) / (float)kFadeMs * ((float)std::max(25, g_prefs.animSpeed) / 100.f));
        const float e = t * t;                                            // ease-in: leaves quickly at the end
        if (t >= 1.f || !IsWindow(g.hwnd)) { FreeGhost(g); s_ghosts.erase(s_ghosts.begin() + i); continue; }
        SetLayeredWindowAttributes(g.hwnd, 0, (BYTE)std::lround(255.f * (1.f - e)), LWA_ALPHA);
        ++i;
    }
    if (s_ghosts.empty() && s_ghostTimer) {
        KillTimer(nullptr, s_ghostTimer);
        s_ghostTimer = 0;
    }
}

bool Windows11() {
    static const bool yes = [] {
        using Fn = LONG(WINAPI*)(OSVERSIONINFOW*);
        auto fn = (Fn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
        OSVERSIONINFOW v = { sizeof(v) };
        return fn && fn(&v) == 0 && v.dwBuildNumber >= 22000;
    }();
    return yes;
}
}  // namespace

bool PopupRevealing(HWND popup) {
    for (auto& r : s_reveals) if (r.hwnd == popup) return true;
    return false;
}

void PopupFadeOut(HWND popup) {
    if (!popup || !IsWindow(popup) || !IsWindowVisible(popup) || !g_prefs.animations || !AnimHoverEnabled()) return;
    RECT wr;
    GetWindowRect(popup, &wr);
    const int w = wr.right - wr.left, h = wr.bottom - wr.top;
    if (w <= 0 || h <= 0 || w > 4000 || h > 4000) return;
    // Still unfolding: the visible part only would be wrong to copy whole; it just goes.
    for (auto& r : s_reveals) if (r.hwnd == popup) return;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = GhostProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"LyraPopupGhost";
        RegisterClassExW(&wc);
        registered = true;
    }
    Ghost g = {};
    HDC screen = GetDC(nullptr);
    g.dc = CreateCompatibleDC(screen);
    g.bmp = CreateCompatibleBitmap(screen, w, h);
    ReleaseDC(nullptr, screen);
    g.old = (HBITMAP)SelectObject(g.dc, g.bmp);
    if (!PrintWindow(popup, g.dc, 2 /* PW_RENDERFULLCONTENT */)) {
        SelectObject(g.dc, g.old); DeleteObject(g.bmp); DeleteDC(g.dc);
        return;
    }
    g.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                             L"LyraPopupGhost", L"", WS_POPUP, wr.left, wr.top, w, h,
                             GetWindow(popup, GW_OWNER), nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g.hwnd) { SelectObject(g.dc, g.old); DeleteObject(g.bmp); DeleteDC(g.dc); return; }
    SetLayeredWindowAttributes(g.hwnd, 0, 255, LWA_ALPHA);
    if (Windows11()) {                                   // the popup's own rounded corners (DWMWCP_ROUND: 8 px)
        const int d = MulDiv(16, (int)GetDpiForWindow(popup), 96) + 1;
        SetWindowRgn(g.hwnd, CreateRoundRectRgn(0, 0, w + 1, h + 1, d, d), FALSE);
    }
    g.start = GetTickCount();
    s_ghosts.push_back(g);
    // Right below the popup: it shows the same picture until the popup goes, then fades.
    SetWindowPos(g.hwnd, popup, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    UpdateWindow(g.hwnd);
    if (!s_ghostTimer) s_ghostTimer = SetTimer(nullptr, 0, 10, GhostStep);
}

void PopupShow(HWND popup, bool activate) {
    PopupShowBegin(popup);
    ShowWindow(popup, activate ? SW_SHOW : SW_SHOWNA);
    PopupShowEnd(popup);
}
