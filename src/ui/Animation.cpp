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
