// Lyra — часы анимаций: кадры в такт обновлению экрана, плавное приближение к цели.
#include "ui/Animation.h"
#include "core/Constants.h"
#include "core/Settings.h"
#include "ui/Chrome.h"
#include "ui/MainWindow.h"

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
    if (!ChromeAnimStep(dt)) {
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

void CALLBACK RevealStep(HWND, UINT, UINT_PTR, DWORD) {
    const DWORD now = GetTickCount();
    for (size_t i = 0; i < s_reveals.size();) {
        Reveal& r = s_reveals[i];
        const float t = std::min(1.f, (float)(now - r.start) / (float)kRevealMs * ((float)std::max(25, g_prefs.animSpeed) / 100.f));
        const float e = 1.f - (1.f - t) * (1.f - t) * (1.f - t);        // ease-out
        const int fullH = r.full.bottom - r.full.top;
        const int h = std::max(1, (int)std::lround(fullH * (0.35f + 0.65f * e)));
        if (IsWindow(r.hwnd)) {
            // The window is cut from below (its contents stay where they are); the region keeps
            // the rounded corners of the final shape only at the end.
            if (t >= 1.f) SetWindowRgn(r.hwnd, nullptr, TRUE);
            else SetWindowRgn(r.hwnd, CreateRectRgn(0, 0, r.full.right - r.full.left, h), TRUE);
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
    SetWindowRgn(popup, CreateRectRgn(0, 0, r.full.right - r.full.left, (int)((r.full.bottom - r.full.top) * 0.35f)), TRUE);
    if (!s_revealTimer) s_revealTimer = SetTimer(nullptr, 0, 10, RevealStep);
}

void PopupRevealStop(HWND popup) {
    for (size_t i = 0; i < s_reveals.size(); ++i) {
        if (s_reveals[i].hwnd != popup) continue;
        if (IsWindow(popup)) SetWindowRgn(popup, nullptr, TRUE);
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

void PopupShow(HWND popup, bool activate) {
    PopupShowBegin(popup);
    ShowWindow(popup, activate ? SW_SHOW : SW_SHOWNA);
    PopupShowEnd(popup);
}
