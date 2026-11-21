// Lyra — перетаскивание вкладки: вкладка «поднимается» и летает за мышью.
#include "ui/TabDrag.h"
#include "app/Log.h"
#include "browser/SplitView.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "ui/Animation.h"
#include "ui/Chrome.h"
#include "ui/Draw.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"

namespace {

enum class Phase { None, Dragging, Landing, Returning };
Phase s_phase = Phase::None;
unsigned long long s_id = 0;
HWND s_wnd = nullptr;
HDC s_dc = nullptr;
HBITMAP s_bmp = nullptr;
HGDIOBJ s_old = nullptr;
void* s_bits = nullptr;
SIZE s_bmpSize = {};
PointF s_grab;                    // where the tab was grabbed, from its top-left corner
PointF s_pos, s_goal;             // top-left of the tab (client coordinates): shown, heading for
SizeF s_size, s_goalSize;         // its size: shown, heading for
float s_lift = 0.f;               // 0 lying in the list .. 1 lifted (bigger, with a shadow)
float s_startDist = 1.f;          // distance to the landing place when the drop happened
BYTE s_alpha = 255;
unsigned long long s_pulseId = 0;                    // the tab that has just landed
DWORD s_pulseAt = 0;
constexpr DWORD kPulseMs = 420;

float Margin() { return std::ceil(S(20)); }

LRESULT CALLBACK GhostProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, msg, w, l);
}

void FreeBitmap() {
    if (s_dc) { SelectObject(s_dc, s_old); DeleteDC(s_dc); s_dc = nullptr; }
    if (s_bmp) { DeleteObject(s_bmp); s_bmp = nullptr; }
    s_bits = nullptr;
    s_bmpSize = {};
}

void Destroy() {
    if (s_phase == Phase::Landing || s_phase == Phase::Returning) {   // it has arrived: the pulse
        Log("tab drag: tab#%llu landed", s_id);
        s_pulseId = s_id;
        s_pulseAt = AnimMs();
        AnimRequest();
    }
    if (s_wnd) { DestroyWindow(s_wnd); s_wnd = nullptr; }
    FreeBitmap();
    s_phase = Phase::None;
    s_id = 0;
    InvalidateChrome();
}

bool EnsureBitmap(int w, int h) {
    if (s_bmp && s_bmpSize.cx >= w && s_bmpSize.cy >= h) return true;
    FreeBitmap();
    HDC screen = GetDC(nullptr);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    s_bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &s_bits, nullptr, 0);
    s_dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!s_bmp || !s_dc || !s_bits) { FreeBitmap(); return false; }
    s_old = SelectObject(s_dc, s_bmp);
    s_bmpSize = { w, h };
    return true;
}

void Render() {
    const Tab* t = FindTab(s_id);
    if (!t || !s_wnd) return;
    const float scale = 1.f + 0.06f * s_lift;
    const float m = Margin();
    const float w = s_size.Width * scale, h = s_size.Height * scale;
    const int bw = (int)std::ceil(w + 2 * m), bh = (int)std::ceil(h + 2 * m);
    if (!EnsureBitmap(bw, bh)) return;
    memset(s_bits, 0, (size_t)s_bmpSize.cx * s_bmpSize.cy * 4);
    {
        Bitmap canvas(s_bmpSize.cx, s_bmpSize.cy, s_bmpSize.cx * 4, PixelFormat32bppPARGB, (BYTE*)s_bits);
        Graphics g(&canvas);
        g.SetSmoothingMode(SmoothingModeHighQuality);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        const RectF face(m, m, w, h);
        // A soft shadow, a little lower: more of it the higher the tab is lifted.
        const float dy = S(4) * s_lift;
        for (int k = 7; k >= 0; --k) {
            const float grow = S(1.6f) * (float)k;
            const BYTE a = (BYTE)std::clamp(s_lift * (g_dark ? 26.f : 16.f) * (1.f - k / 8.f), 0.f, 255.f);
            FillRound(g, Color(a, 0, 0, 0), RectF(face.X - grow, face.Y - grow + dy, face.Width + grow * 2, face.Height + grow * 2),
                      S(6) + grow);
        }
        g.TranslateTransform(face.X, face.Y);
        g.ScaleTransform(scale, scale);
        DrawTabFace(g, *t, RectF(0, 0, s_size.Width, s_size.Height));
    }
    POINT pos = { (LONG)std::lround(s_pos.X - (w - s_size.Width) / 2 - m), (LONG)std::lround(s_pos.Y - (h - s_size.Height) / 2 - m) };
    ClientToScreen(g_hWnd, &pos);
    SIZE size = { bw, bh };
    POINT src = { 0, 0 };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, s_alpha, AC_SRC_ALPHA };
    UpdateLayeredWindow(s_wnd, nullptr, &pos, &size, s_dc, &src, 0, &bf, ULW_ALPHA);
}

float Dist(const PointF& a, const PointF& b) { return std::hypot(a.X - b.X, a.Y - b.Y); }

// Where the lifted tab lies in the list now (it can move while the tab flies back).
bool SlotOf(RectF* r) {
    int index = -1;
    if (!FindTab(s_id, &index) || index >= (int)g_L.tabs.size()) return false;
    *r = g_L.tabs[index];
    int li = -1, ri = -1;
    if (SplitStripPair(&li, &ri) && (index == li || index == ri)) {
        const bool compactOpen = g_L.mode == kLayoutCompact && (g_active == li || g_active == ri);
        if (compactOpen) {
            if (index != g_active) return false;
            *r = g_L.pill;
        } else {                                         // its half of the split view's tab
            const RectF whole = g_L.tabs[li];
            const float half = std::floor(whole.Width / 2);
            *r = RectF(index == li ? whole.X : whole.X + half, whole.Y, index == li ? half : whole.Width - half, whole.Height);
        }
    }
    if (r->Width < 1.f || r->Height < 1.f) return false;
    return true;
}

} // namespace

unsigned long long TabDragGhostId() { return s_phase == Phase::None ? 0 : s_id; }

void TabDragStart(unsigned long long tabId, POINT grab, const RectF& tabRect) {
    if (s_phase != Phase::None) Destroy();
    static bool registered = false;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = GhostProc;
        wc.hInstance = hi;
        wc.lpszClassName = L"LyraTabGhost";
        if (!RegisterClassExW(&wc)) return;
        registered = true;
    }
    s_wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                            L"LyraTabGhost", L"", WS_POPUP, 0, 0, 1, 1, g_hWnd, nullptr, hi, nullptr);
    if (!s_wnd) return;
    s_id = tabId;
    s_phase = Phase::Dragging;
    // An open-looking tab at least this wide, even if its place in the list is narrow.
    const float w = std::clamp(tabRect.Width, S(160), S(240));
    s_size = s_goalSize = SizeF(w, tabRect.Height);
    // The copy keeps the point that was grabbed under the mouse (scaled into its width when it is
    // narrower than the tab was), so the tab hangs from the pointer as it was picked up.
    const float fx = tabRect.Width > 0 ? std::clamp(((float)grab.x - tabRect.X) / tabRect.Width, 0.f, 1.f) : 0.5f;
    s_grab = PointF(std::clamp(fx * w, S(10), w - S(10)), std::clamp((float)grab.y - tabRect.Y, S(4), tabRect.Height - S(4)));
    s_pos = PointF((float)grab.x - s_grab.X, tabRect.Y);
    s_goal = PointF((float)grab.x - s_grab.X, (float)grab.y - s_grab.Y);
    s_lift = 0.f;
    s_alpha = 255;
    Render();
    ShowWindow(s_wnd, SW_SHOWNA);
    InvalidateChrome();
    AnimRequest();
}

void TabDragMoveTo(POINT pt) {
    if (s_phase != Phase::Dragging) return;
    s_goal = PointF((float)pt.x - s_grab.X, (float)pt.y - s_grab.Y);
    AnimRequest();
}

void TabDragFinish(const RectF* into) {
    if (s_phase != Phase::Dragging) return;
    if (into) {
        s_phase = Phase::Landing;
        s_goal = PointF(into->X, into->Y);
        s_goalSize = SizeF(into->Width, into->Height);
    } else {
        s_phase = Phase::Returning;
        RectF slot;
        if (SlotOf(&slot)) { s_goal = PointF(slot.X, slot.Y); s_goalSize = SizeF(slot.Width, slot.Height); }
    }
    s_startDist = std::max(1.f, Dist(s_pos, s_goal));
    AnimRequest();
}

float TabLandPulse(unsigned long long tabId) {
    if (!s_pulseId || tabId != s_pulseId) return -1.f;
    const DWORD t = AnimMs() - s_pulseAt;
    return t >= kPulseMs ? -1.f : (float)t / (float)kPulseMs;
}

bool TabDragAnimStep(float dt) {
    if (s_pulseId) {
        InvalidateChrome();
        if (AnimMs() - s_pulseAt >= kPulseMs) s_pulseId = 0;
        else if (s_phase == Phase::None) return true;
    }
    if (s_phase == Phase::None) return false;
    if (!FindTab(s_id)) { Destroy(); return false; }
    switch (s_phase) {
    case Phase::Dragging:
        s_lift += (1.f - s_lift) * AnimFactor(dt, 45.f);
        s_pos.X += (s_goal.X - s_pos.X) * AnimFactor(dt, 16.f);
        s_pos.Y += (s_goal.Y - s_pos.Y) * AnimFactor(dt, 16.f);
        break;
    case Phase::Returning:
    case Phase::Landing: {                               // into its place in the list (which can move)
        RectF slot;
        if (SlotOf(&slot)) { s_goal = PointF(slot.X, slot.Y); s_goalSize = SizeF(slot.Width, slot.Height); }
        const float k = AnimFactor(dt, 38.f);
        s_lift += (0.f - s_lift) * k;
        s_pos.X += (s_goal.X - s_pos.X) * k;
        s_pos.Y += (s_goal.Y - s_pos.Y) * k;
        s_size.Width += (s_goalSize.Width - s_size.Width) * k;
        s_size.Height += (s_goalSize.Height - s_size.Height) * k;
        const float left = Dist(s_pos, s_goal);
        if (left < 0.75f && std::fabs(s_size.Width - s_goalSize.Width) < 0.75f) {
            Destroy();
            return s_pulseId != 0;                       // (the frame clock keeps running for the pulse)
        }
        break;
    }
    default:
        break;
    }
    Render();
    return true;
}
