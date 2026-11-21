// Lyra - touchpad gestures through DirectManipulation.
//
// Without it Windows turns a precision touchpad into a stream of small wheel steps: no beginning
// or end of a gesture, no fingers-lifted moment, and pinching arrives as Ctrl + wheel. With a
// DirectManipulation viewport on a page window (one per tab, as Chrome does), Windows reports the
// gesture itself: when fingers go down (DM_POINTERHITTEST -> SetContact), the content's movement
// and scale while they move (OnContentUpdated, driven by Update() once per frame), and when they
// lift and the touchpad's inertia takes over (status INERTIA) and ends (READY). The viewport's
// content is never really moved: after each gesture it is put back (ZoomToRect).
//
//   moving       -> WKViewSendPreciseWheelEvent, exact pixels, not animated (inertia included);
//   sideways, and the page did not use it (didNotHandleWheelEvent: nothing to scroll that way)
//                -> the swipe back / forward (ui/SwipeNav), released when the fingers lift;
//   scale        -> pinch zoom around the pointer (WKViewScalePageAroundPoint, 100 % .. 500 %);
//   Ctrl+moving  -> the site's zoom, as Ctrl + wheel.
#include "browser/Touchpad.h"
#include "app/Log.h"
#include "browser/SiteZoom.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "core/Settings.h"
#include "ui/Animation.h"
#include "ui/MainWindow.h"
#include "ui/SwipeNav.h"

#include <commctrl.h>
#include <directmanipulation.h>

namespace {

// CLSID_DirectManipulationManager (spelled out: not every SDK's uuid.lib has it).
const GUID kClsidManager = { 0x54E211B6, 0x3650, 0x4F75, { 0x83, 0x34, 0xFA, 0x35, 0x95, 0x98, 0xE1, 0xC5 } };
constexpr UINT kPointerHitTest = 0x0250;           // DM_POINTERHITTEST
constexpr UINT_PTR kSubclassId = 0x4C595450;       // 'LYTP'

IDirectManipulationManager* s_manager = nullptr;
IDirectManipulationUpdateManager* s_update = nullptr;
bool s_unavailable = false;

class Handler;
struct View {
    IDirectManipulationViewport* viewport = nullptr;
    Handler* handler = nullptr;
    DWORD cookie = 0;
    int w = 0, h = 0;
};
std::map<HWND, View> s_views;

enum class Mode { None, Scroll, Swipe, Pinch, Zoom };

struct Gesture {
    HWND hwnd = nullptr;                 // the page window the fingers are over
    Mode mode = Mode::None;
    float lastX = 0, lastY = 0, lastScale = 1;   // the content transform last seen
    float accX = 0, accY = 0;            // movement with fingers down (decides a swipe)
    int sent = 0;                        // wheel events given to the page in this gesture
    bool fingersDown = false;            // RUNNING (not yet inertia)
    double pageScale = 1;                // pinch: the page's scale
    float zoomAcc = 0;                   // Ctrl + moving: pixels towards the next zoom step
};
Gesture s_g;
DIRECTMANIPULATION_STATUS s_status = DIRECTMANIPULATION_ENABLED;
bool s_interacting = false;
DWORD s_pumpUntil = 0;                   // keep driving a little after a contact (GetTickCount)

// Simulated gestures (LYRA_TEST_TOUCHPAD).
struct SimStep { DWORD delay = 0; std::wstring kind; float amount = 0; };
std::vector<SimStep> s_sim;
size_t s_simIndex = 0;
int s_simFrame = -1;                     // >= 0 while a simulated gesture plays
float s_simX = 0, s_simY = 0, s_simScale = 1;

Tab* TabOfWindow(HWND h) {
    if (!h) return nullptr;
    for (auto& t : g_tabs) if (t->hwnd == h && t->page && t->view && t->state == TabState::Live) return t.get();
    return nullptr;
}

POINT PointerIn(HWND h) {
    POINT pt = {};
    RECT r;
    GetClientRect(h, &r);
    if (s_simFrame >= 0) return { r.right / 2, r.bottom / 2 };   // simulated: the middle (not the real pointer)
    GetCursorPos(&pt);
    ScreenToClient(h, &pt);
    pt.x = std::max<LONG>(0, std::min<LONG>(pt.x, r.right - 1));
    pt.y = std::max<LONG>(0, std::min<LONG>(pt.y, r.bottom - 1));
    return pt;
}

void ResetViewport(HWND h) {
    auto it = s_views.find(h);
    if (it == s_views.end() || !it->second.viewport) return;
    it->second.viewport->ZoomToRect(0, 0, (float)it->second.w, (float)it->second.h, FALSE);
}

void PinchEnd() {
    Tab* t = TabOfWindow(s_g.hwnd);
    if (t && s_g.pageScale < 1.03 && s_g.pageScale != 1.0) {     // close to 100 %: exactly 100 %
        s_g.pageScale = 1.0;
        WKViewScalePageAroundPoint(t->view, 1.0, PointerIn(s_g.hwnd));
    }
    if (t) Log("touchpad: pinch ended at %.0f%%", s_g.pageScale * 100);
}

// The gesture is over (READY, a new contact, or the window went away).
void EndGesture() {
    if (s_g.mode == Mode::Swipe) SwipeNavRelease();
    else if (s_g.mode == Mode::Pinch) PinchEnd();
    if (s_g.mode != Mode::None)
        Log("touchpad: gesture end (%s, %d events, moved %.0f,%.0f)", s_g.mode == Mode::Scroll ? "scroll" :
            s_g.mode == Mode::Swipe ? "swipe" : s_g.mode == Mode::Pinch ? "pinch" : "zoom", s_g.sent, s_g.accX, s_g.accY);
    s_g = Gesture();
}

void StartGesture(HWND h) {
    EndGesture();
    s_g.hwnd = h;
    s_g.fingersDown = true;
    if (Tab* t = TabOfWindow(h)) s_g.pageScale = WKPageGetScaleFactor(t->page);
}

void StatusChanged(HWND h, DIRECTMANIPULATION_STATUS current, DIRECTMANIPULATION_STATUS previous) {
    s_status = current;
    if (current == previous) return;
    switch (current) {
    case DIRECTMANIPULATION_RUNNING:
        StartGesture(h);                             // (after inertia: a new gesture)
        AnimRequest();
        break;
    case DIRECTMANIPULATION_INERTIA:                 // fingers lifted, the content glides on
        if (h != s_g.hwnd) break;
        s_g.fingersDown = false;
        if (s_g.mode == Mode::Swipe) { SwipeNavRelease(); s_g.mode = Mode::None; }
        else if (s_g.mode == Mode::Pinch) { PinchEnd(); s_g.mode = Mode::None; }
        break;
    case DIRECTMANIPULATION_READY: {                 // the gesture (and its inertia) is over
        const bool moved = s_g.lastScale != 1.f || s_g.lastX != 0.f || s_g.lastY != 0.f;
        if (h == s_g.hwnd || !s_g.hwnd) EndGesture();
        if (moved) ResetViewport(h);                 // its own content update is then ignored:
        s_g.lastX = s_g.lastY = 0;                   // it equals the identity seen last
        s_g.lastScale = 1;
        break;
    }
    default:
        break;
    }
}

unsigned Modifiers() {
    unsigned m = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) m |= 1;
    if (GetKeyState(VK_MENU) & 0x8000) m |= 4;
    return m;
}

void ContentUpdated(HWND h, float scale, float x, float y) {
    if (!s_g.hwnd) { s_g.hwnd = h; s_g.fingersDown = s_status == DIRECTMANIPULATION_RUNNING; }
    if (h != s_g.hwnd || scale <= 0.f) return;
    auto same = [](float a, float b) { return std::fabs(a - b) <= 0.00001f * std::max(1.f, std::fabs(a)); };
    if (same(scale, s_g.lastScale) && same(x, s_g.lastX) && same(y, s_g.lastY)) return;
    const float dx = x - s_g.lastX, dy = y - s_g.lastY, ratio = scale / s_g.lastScale;
    const bool pinching = std::fabs(scale - 1.f) > 0.0001f;
    s_g.lastX = x;
    s_g.lastY = y;
    s_g.lastScale = scale;
    Tab* t = TabOfWindow(h);
    if (!t) return;

    if (pinching && s_g.mode != Mode::Swipe) {
        if (!g_prefs.pinchZoom) return;
        if (s_g.mode != Mode::Pinch) {
            s_g.mode = Mode::Pinch;
            s_g.pageScale = WKPageGetScaleFactor(t->page);
            Log("touchpad: pinch from %.0f%%", s_g.pageScale * 100);
        }
        const double next = std::max(1.0, std::min(5.0, s_g.pageScale * ratio));
        if (std::fabs(next - s_g.pageScale) > 0.0005) {
            s_g.pageScale = next;
            WKViewScalePageAroundPoint(t->view, next, PointerIn(h));
        }
        return;
    }
    if (s_g.mode == Mode::Pinch) return;             // fingers settling after a pinch
    if (s_g.mode == Mode::Swipe) {
        if (s_g.fingersDown) SwipeNavMove(dx);
        return;
    }
    if (s_g.mode == Mode::Zoom || ((GetKeyState(VK_CONTROL) & 0x8000) && s_g.fingersDown && s_g.mode == Mode::None)) {
        s_g.mode = Mode::Zoom;                       // Ctrl + two fingers: the site's zoom
        if (!s_g.fingersDown) return;
        s_g.zoomAcc += dy;
        while (s_g.zoomAcc >= 90.f) { s_g.zoomAcc -= 90.f; SiteZoomStep(1); }
        while (s_g.zoomAcc <= -90.f) { s_g.zoomAcc += 90.f; SiteZoomStep(-1); }
        return;
    }
    s_g.mode = Mode::Scroll;
    if (s_g.fingersDown) { s_g.accX += dx; s_g.accY += dy; }
    WKViewSendPreciseWheelEvent(t->view, PointerIn(h), dx, dy, Modifiers());
    ++s_g.sent;
}

class Handler final : public IDirectManipulationViewportEventHandler, public IDirectManipulationInteractionEventHandler {
public:
    explicit Handler(HWND h) : m_hwnd(h) {}
    void Detach() { m_hwnd = nullptr; }

    STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirectManipulationViewportEventHandler))
            *out = static_cast<IDirectManipulationViewportEventHandler*>(this);
        else if (riid == __uuidof(IDirectManipulationInteractionEventHandler))
            *out = static_cast<IDirectManipulationInteractionEventHandler*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG r = InterlockedDecrement(&m_ref);
        if (!r) delete this;
        return (ULONG)r;
    }

    STDMETHODIMP OnViewportStatusChanged(IDirectManipulationViewport*, DIRECTMANIPULATION_STATUS current,
                                         DIRECTMANIPULATION_STATUS previous) override {
        if (m_hwnd) StatusChanged(m_hwnd, current, previous);
        return S_OK;
    }
    STDMETHODIMP OnViewportUpdated(IDirectManipulationViewport*) override { return S_OK; }
    STDMETHODIMP OnContentUpdated(IDirectManipulationViewport*, IDirectManipulationContent* content) override {
        float xf[6] = {};
        if (m_hwnd && content && SUCCEEDED(content->GetContentTransform(xf, 6))) ContentUpdated(m_hwnd, xf[0], xf[4], xf[5]);
        return S_OK;
    }
    STDMETHODIMP OnInteraction(IDirectManipulationViewport2*, DIRECTMANIPULATION_INTERACTION_TYPE type) override {
        if (type == DIRECTMANIPULATION_INTERACTION_BEGIN) { s_interacting = true; AnimRequest(); }
        else if (type == DIRECTMANIPULATION_INTERACTION_END) s_interacting = false;
        return S_OK;
    }

private:
    LONG m_ref = 1;
    HWND m_hwnd;
};

void Detach(HWND h) {
    auto it = s_views.find(h);
    if (it == s_views.end()) return;
    View& v = it->second;
    if (s_g.hwnd == h) { EndGesture(); s_g = Gesture(); }
    if (v.viewport) {
        v.viewport->RemoveEventHandler(v.cookie);
        v.viewport->Abandon();
        v.viewport->Release();
    }
    if (v.handler) { v.handler->Detach(); v.handler->Release(); }
    if (s_manager) s_manager->Deactivate(h);
    s_views.erase(it);
}

void SetSize(HWND h) {
    auto it = s_views.find(h);
    if (it == s_views.end() || !it->second.viewport) return;
    RECT r;
    GetClientRect(h, &r);
    const int w = std::max<LONG>(1, r.right), hh = std::max<LONG>(1, r.bottom);
    if (w == it->second.w && hh == it->second.h) return;
    it->second.w = w;
    it->second.h = hh;
    it->second.viewport->Stop();
    RECT vr = { 0, 0, w, hh };
    it->second.viewport->SetViewportRect(&vr);
}

LRESULT CALLBACK ViewSubclass(HWND h, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR) {
    switch (msg) {
    case kPointerHitTest: {
        // A touchpad contact begins over this page: DirectManipulation takes the gesture.
        const UINT32 pointer = GET_POINTERID_WPARAM(wParam);
        POINTER_INPUT_TYPE type = PT_POINTER;
        if (!GetPointerType(pointer, &type) || type != PT_TOUCHPAD) break;
        auto it = s_views.find(h);
        if (it == s_views.end() || !it->second.viewport) break;
        SetSize(h);
        if (SUCCEEDED(it->second.viewport->SetContact(pointer))) {
            s_pumpUntil = GetTickCount() + 400;
            AnimRequest();
        }
        return 0;
    }
    case WM_SIZE:
        SetSize(h);
        break;
    case WM_NCDESTROY:
        Detach(h);
        RemoveWindowSubclass(h, ViewSubclass, id);
        break;
    default:
        break;
    }
    return DefSubclassProc(h, msg, wParam, lParam);
}

bool EnsureManager() {
    if (s_manager) return true;
    if (s_unavailable) return false;
    HRESULT hr = CoCreateInstance(kClsidManager, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&s_manager));
    if (SUCCEEDED(hr)) hr = s_manager->GetUpdateManager(IID_PPV_ARGS(&s_update));
    if (FAILED(hr)) {
        Log("touchpad: DirectManipulation unavailable (0x%08lx): the touchpad scrolls as a wheel", (unsigned long)hr);
        if (s_manager) { s_manager->Release(); s_manager = nullptr; }
        s_unavailable = true;
        return false;
    }
    return true;
}

// --- LYRA_TEST_TOUCHPAD --------------------------------------------------------------------
constexpr int kSimFrames = 36;

void CALLBACK SimTimer(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    Tab* t = ActiveTab();
    if (!t || !t->hwnd || s_simIndex >= s_sim.size()) return;
    Log("touchpad test: %ls %.2f", s_sim[s_simIndex].kind.c_str(), s_sim[s_simIndex].amount);
    s_simFrame = 0;
    s_simX = s_simY = 0;
    s_simScale = 1;
    StatusChanged(t->hwnd, DIRECTMANIPULATION_RUNNING, DIRECTMANIPULATION_READY);
    AnimRequest();
}

void ScheduleSim() {
    if (s_simIndex < s_sim.size()) SetTimer(nullptr, 0, s_sim[s_simIndex].delay, SimTimer);
}

void SimStepFrame() {
    Tab* t = ActiveTab();
    if (!t || !t->hwnd || s_simIndex >= s_sim.size()) { s_simFrame = -1; return; }
    const SimStep& st = s_sim[s_simIndex];
    HWND h = t->hwnd;
    if (s_simFrame < kSimFrames) {
        const float f = (float)(s_simFrame + 1) / kSimFrames;
        const float ease = f;                         // even speed: a steady swipe
        if (st.kind == L"back") s_simX = st.amount * ease;
        else if (st.kind == L"fwd") s_simX = -st.amount * ease;
        else if (st.kind == L"scroll") s_simY = st.amount * ease;
        else if (st.kind == L"pinch") s_simScale = 1.f + (st.amount - 1.f) * ease;
        ContentUpdated(h, s_simScale, s_simX, s_simY);
    } else if (s_simFrame == kSimFrames) {
        StatusChanged(h, DIRECTMANIPULATION_INERTIA, DIRECTMANIPULATION_RUNNING);
    } else {
        StatusChanged(h, DIRECTMANIPULATION_READY, DIRECTMANIPULATION_INERTIA);
        s_simFrame = -1;
        ++s_simIndex;
        ScheduleSim();
        return;
    }
    ++s_simFrame;
}

void StartSimFromEnv() {
    static bool started = false;
    if (started) return;
    started = true;
    wchar_t buf[512] = {};
    if (!GetEnvironmentVariableW(L"LYRA_TEST_TOUCHPAD", buf, 512)) return;
    std::wstring spec = buf;
    size_t pos = 0;
    while (pos < spec.size()) {
        size_t end = spec.find(L';', pos);
        if (end == std::wstring::npos) end = spec.size();
        const std::wstring item = spec.substr(pos, end - pos);
        const size_t a = item.find(L':'), b = item.find(L':', a == std::wstring::npos ? 0 : a + 1);
        if (a != std::wstring::npos && b != std::wstring::npos) {
            SimStep st;
            st.delay = (DWORD)_wtoi(item.substr(0, a).c_str());
            st.kind = item.substr(a + 1, b - a - 1);
            st.amount = (float)_wtof(item.substr(b + 1).c_str());
            s_sim.push_back(st);
        }
        pos = end + 1;
    }
    Log("touchpad test: %d gesture(s)", (int)s_sim.size());
    ScheduleSim();
}

}  // namespace

void TouchpadAttach(HWND hwnd) {
    StartSimFromEnv();
    if (!hwnd || s_views.count(hwnd) || !EnsureManager()) return;
    View v;
    if (FAILED(s_manager->CreateViewport(nullptr, hwnd, IID_PPV_ARGS(&v.viewport)))) return;
    const DIRECTMANIPULATION_CONFIGURATION config =
        DIRECTMANIPULATION_CONFIGURATION_INTERACTION | DIRECTMANIPULATION_CONFIGURATION_TRANSLATION_X |
        DIRECTMANIPULATION_CONFIGURATION_TRANSLATION_Y | DIRECTMANIPULATION_CONFIGURATION_TRANSLATION_INERTIA |
        DIRECTMANIPULATION_CONFIGURATION_RAILS_X | DIRECTMANIPULATION_CONFIGURATION_RAILS_Y |
        DIRECTMANIPULATION_CONFIGURATION_SCALING;
    v.handler = new Handler(hwnd);
    RECT r;
    GetClientRect(hwnd, &r);
    v.w = std::max<LONG>(1, r.right);
    v.h = std::max<LONG>(1, r.bottom);
    RECT vr = { 0, 0, v.w, v.h };
    bool ok = SUCCEEDED(v.viewport->ActivateConfiguration(config)) &&
              SUCCEEDED(v.viewport->SetViewportOptions(DIRECTMANIPULATION_VIEWPORT_OPTIONS_MANUALUPDATE)) &&
              SUCCEEDED(v.viewport->AddEventHandler(hwnd, static_cast<IDirectManipulationViewportEventHandler*>(v.handler), &v.cookie)) &&
              SUCCEEDED(v.viewport->SetViewportRect(&vr)) &&
              SUCCEEDED(s_manager->Activate(hwnd)) &&
              SUCCEEDED(v.viewport->Enable());
    if (!ok) {
        Log("touchpad: viewport setup failed");
        v.viewport->Abandon();
        v.viewport->Release();
        v.handler->Release();
        return;
    }
    s_views[hwnd] = v;
    SetWindowSubclass(hwnd, ViewSubclass, kSubclassId, 0);
}

bool TouchpadAnimStep(float) {
    if (s_simFrame >= 0) { SimStepFrame(); return true; }
    const bool active = s_interacting || s_status == DIRECTMANIPULATION_RUNNING ||
                        s_status == DIRECTMANIPULATION_INERTIA || (LONG)(s_pumpUntil - GetTickCount()) > 0;
    if (active && s_update) s_update->Update(nullptr);
    return active;
}

void TouchpadWheelNotHandled(WKPageRef page) {
    if (!g_prefs.swipeNav || s_g.mode != Mode::Scroll || !s_g.fingersDown || s_g.sent > 6) return;
    Tab* t = TabOfWindow(s_g.hwnd);
    if (!t || t->page != page || t != ActiveTab()) return;
    // Mostly sideways so far (DirectManipulation's rails keep a sideways swipe sideways).
    if (std::fabs(s_g.accX) < 3.f || std::fabs(s_g.accX) < 2.f * std::fabs(s_g.accY)) return;
    const bool back = s_g.accX > 0;                  // the content follows the fingers: right = back
    if (back ? !WKPageCanGoBack(page) : !WKPageCanGoForward(page)) return;
    if (!SwipeNavBegin(*t, back, s_g.accX)) return;
    s_g.mode = Mode::Swipe;
    Log("touchpad: swipe %s", back ? "back" : "forward");
}

void TouchpadShutdown() {
    while (!s_views.empty()) {
        const HWND h = s_views.begin()->first;
        RemoveWindowSubclass(h, ViewSubclass, kSubclassId);
        Detach(h);
    }
    if (s_update) { s_update->Release(); s_update = nullptr; }
    if (s_manager) { s_manager->Release(); s_manager = nullptr; }
}
