// Lyra — адрес ссылки под курсором: плашка в нижнем левом углу страницы.
//
// A layered popup owned by the main window (it has to float over the page's child window),
// drawn with per-pixel alpha (rounded corners). It goes away a moment after the pointer leaves
// the link (no flicker between neighbouring links), when the pointer leaves the page or the
// window is not in front; it moves to the right corner when the pointer comes near it.
#include "ui/LinkStatus.h"
#include "browser/SplitView.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "core/StringUtils.h"
#include "ui/Chrome.h"
#include "ui/Draw.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"

namespace {

HWND s_wnd = nullptr;
std::wstring s_url;                     // shown (empty: hidden)
unsigned long long s_tabId = 0;
HWND s_viewWnd = nullptr;               // the page window the link is in
DWORD s_lostAt = 0;                     // when the pointer left the link (0: still on it)
bool s_right = false;                   // at the right corner (the pointer is near the left one)
constexpr UINT_PTR kTimer = 1;

// What to show: the address without "http(s)://" when it is a plain web address, decoded.
std::wstring Shown(const std::wstring& url) {
    std::wstring s = url;
    if (s.compare(0, 8, L"https://") == 0) s = s.substr(8);
    else if (s.compare(0, 7, L"http://") == 0) s = s.substr(7);
    if (s.size() > 1 && s.back() == L'/' && s.find(L'/') == s.size() - 1) s.pop_back();   // "site.com/" -> "site.com"
    const std::wstring decoded = UrlDecode(s);
    // (keep the encoded form when decoding gives control characters or broken text)
    bool ok = true;
    for (wchar_t c : decoded) if (c < 0x20 || c == 0xFFFD) ok = false;
    return ok ? decoded : s;
}

// The page area of the window in screen coordinates.
RECT PageRect() {
    RECT r = {};
    if (s_viewWnd && IsWindow(s_viewWnd)) GetWindowRect(s_viewWnd, &r);
    return r;
}

void Paint() {
    if (!s_wnd || s_url.empty()) return;
    const RECT page = PageRect();
    const int pageW = page.right - page.left, pageH = page.bottom - page.top;
    if (pageW < 50 || pageH < 30) { ShowWindow(s_wnd, SW_HIDE); return; }
    const std::wstring text = Shown(s_url);
    Font* font = g_fText.get();
    if (!font) return;

    // Size: the text, up to half the page width (longer: "…" in the middle part is cut at the end).
    float textW = 0;
    {
        Bitmap probe(1, 1, PixelFormat32bppPARGB);
        Graphics pg(&probe);
        StringFormat sf(StringFormat::GenericTypographic());
        RectF box;
        pg.MeasureString(text.c_str(), -1, font, PointF(0, 0), &sf, &box);
        textW = box.Width;
    }
    const float padX = std::floor(S(8)), h = std::floor(S(24));
    const int maxW = std::max((int)S(200), pageW / 2);
    const int w = std::min(maxW, (int)std::ceil(textW + padX * 2 + S(2)));
    const int hi = (int)h;

    // Where: the bottom left corner of the page, or the right one when the pointer is near.
    POINT cur;
    GetCursorPos(&cur);
    const int margin = 0;
    RECT left = { page.left + margin, page.bottom - hi - margin, page.left + margin + w, page.bottom - margin };
    RECT nearLeft = left;
    InflateRect(&nearLeft, (int)S(20), (int)S(20));
    if (PtInRect(&nearLeft, cur)) s_right = true;
    else if (cur.x < page.right - w - (int)S(40)) s_right = false;
    const int x = s_right ? page.right - margin - w : page.left + margin;
    const int y = page.bottom - hi - margin;
    // Nothing changed since the last picture: nothing to do (the timer calls this often).
    static std::wstring lastText;
    static int lastX = 0, lastY = 0, lastW = 0, lastH = 0;
    static bool lastDark = false;
    if (IsWindowVisible(s_wnd) && text == lastText && x == lastX && y == lastY && w == lastW && hi == lastH && g_dark == lastDark) return;
    lastText = text; lastX = x; lastY = y; lastW = w; lastH = hi; lastDark = g_dark;

    // Draw on a 32-bit top-down DIB (premultiplied alpha for UpdateLayeredWindow).
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -hi;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) { DeleteDC(mem); ReleaseDC(nullptr, screen); return; }
    HGDIOBJ old = SelectObject(mem, dib);
    {
        Bitmap canvas(w, hi, w * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        Graphics g(&canvas);
        g.Clear(Color(0, 0, 0, 0));
        g.SetSmoothingMode(SmoothingModeHighQuality);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        const float rad = S(6);
        // Square on the side that touches the page's edge corner, rounded elsewhere.
        const RectF r(0.5f, 0.5f, (float)w - 1, (float)hi - 1);
        GraphicsPath p;
        AddRound(p, r, rad);
        SolidBrush bg(g_th.popup);
        g.FillPath(&bg, &p);
        Pen border(g_th.fieldBorder, 1.f);
        g.DrawPath(&border, &p);
        StringFormat sf(StringFormat::GenericTypographic());
        sf.SetFormatFlags(sf.GetFormatFlags() | StringFormatFlagsNoWrap);
        sf.SetTrimming(StringTrimmingEllipsisCharacter);
        sf.SetLineAlignment(StringAlignmentCenter);
        SolidBrush tb(g_th.text);
        g.DrawString(text.c_str(), -1, font, RectF(padX, 0, (float)w - padX * 2, (float)hi), &sf, &tb);
    }
    POINT dst = { x, y }, src = { 0, 0 };
    SIZE size = { w, hi };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(s_wnd, screen, &dst, &size, mem, &src, 0, &blend, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    // Above the main window (it is owned by it), without taking the focus.
    SetWindowPos(s_wnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void HideNow() {
    s_url.clear();
    s_lostAt = 0;
    if (s_wnd) {
        KillTimer(s_wnd, kTimer);
        ShowWindow(s_wnd, SW_HIDE);
    }
}

// The pointer is over the page window (or a window inside it) and Lyra is in front.
bool PointerOnPage() {
    static const bool test = GetEnvironmentVariableW(L"LYRA_LINKSTATUS_TEST", nullptr, 0) > 0;   // (automated tests)
    if (test) return s_viewWnd && IsWindow(s_viewWnd);
    if (!s_viewWnd || !IsWindow(s_viewWnd) || !g_hWnd || GetForegroundWindow() != g_hWnd || IsIconic(g_hWnd)) return false;
    POINT cur;
    GetCursorPos(&cur);
    HWND under = WindowFromPoint(cur);
    return under && (under == s_viewWnd || IsChild(s_viewWnd, under));
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_TIMER:
        if (w == kTimer) {
            Tab* t = FindTab(s_tabId);
            const bool onScreen = t && (t == ActiveTab() || SplitVisible(*t));
            if (!onScreen || !PointerOnPage() || (s_lostAt && GetTickCount() - s_lostAt > 160)) { HideNow(); return 0; }
            Paint();                                     // follows window moves and the pointer
        }
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    }
    return DefWindowProcW(h, msg, w, l);
}

bool EnsureWindow() {
    if (s_wnd) return true;
    if (!g_hWnd) return false;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"LyraLinkStatus";
        if (!RegisterClassExW(&wc)) return false;
        registered = true;
    }
    s_wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                            L"LyraLinkStatus", L"", WS_POPUP, 0, 0, 1, 1, g_hWnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    return s_wnd != nullptr;
}

}  // namespace

void LinkStatusUpdate(const Tab& t, const std::wstring& url) {
    if (url.empty() || url.compare(0, 11, L"javascript:") == 0) {
        if (!s_url.empty() && t.id == s_tabId && !s_lostAt) s_lostAt = GetTickCount();   // hides a moment later
        return;
    }
    if (&t != ActiveTab() && !SplitVisible(t)) return;
    if (!EnsureWindow()) return;
    s_tabId = t.id;
    s_viewWnd = t.hwnd;
    s_lostAt = 0;
    if (url == s_url && IsWindowVisible(s_wnd)) return;
    s_url = url;
    Paint();
    SetTimer(s_wnd, kTimer, 100, nullptr);
}

void LinkStatusHide() { HideNow(); }
