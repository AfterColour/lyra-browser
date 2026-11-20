// Lyra — карточка вкладки при наведении: название, адрес и картинка страницы.
//
// A layered popup owned by the main window (like ui/LinkStatus): it never takes the focus or the
// mouse. The picture comes from WKPageTakeSnapshot (the web process paints the visible part of the
// page into a bitmap), asked for when the card is shown and kept per tab for a while, so the
// card of a tab seen a moment ago shows at once.
#include "ui/TabHoverCard.h"
#include "app/Log.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "core/Localization.h"
#include "core/Url.h"
#include "ui/Chrome.h"
#include "ui/Draw.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"

#include <WebKit/WKPagePrivate.h>
#include <set>

namespace {

HWND s_wnd = nullptr;
int s_index = -1;                       // the tab under the pointer (-1: none)
unsigned long long s_tabId = 0;         // the tab the card shows (0: hidden)
bool s_shown = false;
DWORD s_hiddenAt = 0;                   // a card shown recently: the next one comes at once
constexpr UINT_PTR kShowTimer = 1, kWatchTimer = 2;
constexpr DWORD kDelayMs = 450, kWarmMs = 900;

// Automated tests (developer): LYRA_HOVERCARD_TEST=<file.png> shows cards without Lyra in front,
// keeps a card when the (posted) pointer "leaves", and saves each card's picture to that file.
const std::wstring& TestFile() {
    static std::wstring path = []() {
        wchar_t buf[MAX_PATH] = {};
        return GetEnvironmentVariableW(L"LYRA_HOVERCARD_TEST", buf, MAX_PATH) ? std::wstring(buf) : std::wstring();
    }();
    return path;
}

struct Picture { std::unique_ptr<Bitmap> bmp; ULONGLONG at = 0; };
std::map<unsigned long long, Picture> s_pictures;
std::set<unsigned long long> s_asking;
constexpr size_t kMaxPictures = 24;
constexpr ULONGLONG kFreshMs = 15000;   // older pictures are asked for again (still shown meanwhile)

void Paint();

void OnSnapshot(const void* pixels, uint32_t w, uint32_t h, uint32_t stride, void* ctx) {
    const unsigned long long id = (unsigned long long)(uintptr_t)ctx;
    s_asking.erase(id);
    if (!pixels || !w || !h || !FindTab(id)) return;
    auto bmp = std::make_unique<Bitmap>((INT)w, (INT)h, PixelFormat32bppPARGB);
    BitmapData d;
    Rect all(0, 0, (INT)w, (INT)h);
    if (bmp->LockBits(&all, ImageLockModeWrite, PixelFormat32bppPARGB, &d) != Ok) return;
    for (uint32_t y = 0; y < h; ++y) {
        BYTE* dst = (BYTE*)d.Scan0 + (size_t)y * d.Stride;
        memcpy(dst, (const BYTE*)pixels + (size_t)y * stride, (size_t)w * 4);
        for (uint32_t x = 0; x < w; ++x) dst[x * 4 + 3] = 255;     // (the page is opaque)
    }
    bmp->UnlockBits(&d);
    if (s_pictures.size() >= kMaxPictures && !s_pictures.count(id)) {
        auto oldest = s_pictures.begin();
        for (auto it = s_pictures.begin(); it != s_pictures.end(); ++it) if (it->second.at < oldest->second.at) oldest = it;
        s_pictures.erase(oldest);
    }
    s_pictures[id] = Picture{ std::move(bmp), GetTickCount64() };
    if (s_shown && s_tabId == id) Paint();
}

bool WantsPicture(const Tab& t) {
    return t.state == TabState::Live && t.page && t.hwnd && !t.lazyPending && !t.sleeping && &t != ActiveTab() &&
           !t.url.empty() && t.url != L"about:blank";
}

void AskPicture(Tab& t) {
    if (!WantsPicture(t) || s_asking.count(t.id)) return;
    auto it = s_pictures.find(t.id);
    if (it != s_pictures.end() && GetTickCount64() - it->second.at < kFreshMs) return;
    s_asking.insert(t.id);
    const int maxW = (int)S(264) * 2, maxH = (int)S(264) * 2;       // sharp enough at the card's size
    WKPageTakeSnapshot(t.page, (uint32_t)maxW, (uint32_t)maxH, OnSnapshot, (void*)(uintptr_t)t.id);
}

std::wstring Site(const Tab& t) {
    const std::wstring url = t.lazyPending ? t.lazyUrl : t.url;
    if (url.empty() || url == L"about:blank") return std::wstring();
    if (IsInternalPageUrl(url)) return L"Lyra";
    const std::wstring host = HostOf(url);
    return host.empty() ? DisplayUrl(url) : host;
}

float TextHeight(Graphics& g, const std::wstring& s, Font* f, float w, int maxLines, StringFormat& sf) {
    RectF box;
    g.MeasureString(s.c_str(), -1, f, RectF(0, 0, w, 10000), &sf, &box);
    const float line = f->GetHeight(&g);
    return std::min(box.Height, line * maxLines + 0.5f);
}

void Paint() {
    Tab* t = FindTab(s_tabId);
    int index = -1;
    if (!s_wnd || !t || !FindTab(s_tabId, &index) || index >= (int)g_L.tabs.size()) { TabHoverCardHide(); return; }
    const RectF tr = g_L.tabs[index];
    if (tr.Width < 2 || tr.Height < 2) { TabHoverCardHide(); return; }
    Font* titleFont = g_fTabBold ? g_fTabBold.get() : g_fText.get();
    Font* textFont = g_fTab ? g_fTab.get() : g_fText.get();
    if (!titleFont || !textFont) return;

    const std::wstring title = t->title.empty() ? Site(*t) : t->title;
    std::wstring site = Site(*t);
    if (t->sleeping || t->lazyPending) site += std::wstring(site.empty() ? L"" : L" \x00B7 ") + Tr(L"Not loaded", L"Не загружена");
    else if (t->audible && !t->muted) site += std::wstring(site.empty() ? L"" : L" \x00B7 ") + Tr(L"Playing sound", L"Воспроизводит звук");
    Bitmap* pic = nullptr;
    if (WantsPicture(*t)) {
        auto it = s_pictures.find(t->id);
        if (it != s_pictures.end()) pic = it->second.bmp.get();
    }

    const int w = (int)std::round(S(264));
    const float pad = std::round(S(12)), innerW = (float)w - pad * 2;
    Bitmap probe(1, 1, PixelFormat32bppPARGB);
    Graphics pg(&probe);
    StringFormat sfTitle(StringFormat::GenericTypographic());
    sfTitle.SetTrimming(StringTrimmingEllipsisWord);
    StringFormat sfLine(StringFormat::GenericTypographic());
    sfLine.SetFormatFlags(sfLine.GetFormatFlags() | StringFormatFlagsNoWrap);
    sfLine.SetTrimming(StringTrimmingEllipsisCharacter);
    const float titleH = TextHeight(pg, title, titleFont, innerW, 2, sfTitle);
    const float siteH = site.empty() ? 0 : textFont->GetHeight(&pg);
    const float gap = std::round(S(4));
    float textH = pad + titleH + (site.empty() ? 0 : gap + siteH) + pad;
    float picH = 0;
    if (pic) picH = std::min(std::round((float)w * pic->GetHeight() / std::max(1u, pic->GetWidth())), std::round(S(170)));
    const int h = (int)std::ceil(textH + picH);

    // Under the tab (beside it in the vertical layout), inside the window's monitor.
    POINT origin = { 0, 0 };
    ClientToScreen(g_hWnd, &origin);
    int x, y;
    if (g_L.mode == kLayoutVertical) { x = origin.x + (int)(tr.GetRight() + S(6)); y = origin.y + (int)tr.Y; }
    else { x = origin.x + (int)tr.X; y = origin.y + (int)(tr.GetBottom() + S(6)); }
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfoW(MonitorFromWindow(g_hWnd, MONITOR_DEFAULTTONEAREST), &mi)) {
        x = std::max((int)mi.rcWork.left, std::min(x, (int)mi.rcWork.right - w));
        y = std::max((int)mi.rcWork.top, std::min(y, (int)mi.rcWork.bottom - h));
    }

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
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
        Bitmap canvas(w, h, w * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        Graphics g(&canvas);
        g.Clear(Color(0, 0, 0, 0));
        g.SetSmoothingMode(SmoothingModeHighQuality);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        const float rad = S(8);
        const RectF r(0.5f, 0.5f, (float)w - 1, (float)h - 1);
        GraphicsPath p;
        AddRound(p, r, rad);
        SolidBrush bg(g_th.popup);
        g.FillPath(&bg, &p);
        SolidBrush tb(g_th.text), sb(g_th.text2);
        g.DrawString(title.c_str(), -1, titleFont, RectF(pad, pad, innerW, titleH + 1), &sfTitle, &tb);
        if (!site.empty())
            g.DrawString(site.c_str(), -1, textFont, RectF(pad, pad + titleH + gap, innerW, siteH + 1), &sfLine, &sb);
        if (pic) {
            // The page's top part, the card's width, clipped to its rounded bottom.
            Region clip(&p);
            g.SetClip(&clip);
            const float scale = (float)w / pic->GetWidth();
            const float srcH = picH / scale;
            g.DrawImage(pic, RectF(0, textH, (float)w, picH), 0, 0, (REAL)pic->GetWidth(), srcH, UnitPixel);
            g.ResetClip();
            Pen line(g_th.fieldBorder, 1.f);
            g.DrawLine(&line, 0.f, textH - 0.5f, (float)w, textH - 0.5f);
        }
        Pen border(g_th.fieldBorder, 1.f);
        g.DrawPath(&border, &p);
        if (!TestFile().empty()) {
            CLSID png;
            UINT n = 0, size = 0;
            GetImageEncodersSize(&n, &size);
            std::vector<BYTE> buf(size);
            ImageCodecInfo* codecs = (ImageCodecInfo*)buf.data();
            if (size && GetImageEncoders(n, size, codecs) == Ok)
                for (UINT i = 0; i < n; ++i) if (wcscmp(codecs[i].MimeType, L"image/png") == 0) { png = codecs[i].Clsid; canvas.Save(TestFile().c_str(), &png); }
        }
    }
    POINT dst = { x, y }, src = { 0, 0 };
    SIZE size = { w, h };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(s_wnd, screen, &dst, &size, mem, &src, 0, &blend, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    SetWindowPos(s_wnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void Show() {
    if (s_index < 0 || s_index >= (int)g_tabs.size()) return;
    if (TestFile().empty() && (GetForegroundWindow() != g_hWnd || IsIconic(g_hWnd))) return;
    Tab& t = *g_tabs[s_index];
    s_tabId = t.id;
    s_shown = true;
    AskPicture(t);
    Paint();
    SetTimer(s_wnd, kWatchTimer, 150, nullptr);
    Log("hovercard: tab#%llu", t.id);
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_TIMER:
        if (w == kShowTimer) { KillTimer(h, kShowTimer); Show(); }
        else if (w == kWatchTimer) {                     // the window moved away or went to the back
            if (TestFile().empty() && (GetForegroundWindow() != g_hWnd || IsIconic(g_hWnd))) TabHoverCardHide();
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
        wc.lpszClassName = L"LyraTabHoverCard";
        if (!RegisterClassExW(&wc)) return false;
        registered = true;
    }
    s_wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                            L"LyraTabHoverCard", L"", WS_POPUP, 0, 0, 1, 1, g_hWnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    return s_wnd != nullptr;
}

}  // namespace

void TabHoverCardHover(int tabIndex) {
    if (tabIndex == s_index) return;
    if (tabIndex < 0 && !TestFile().empty()) return;
    s_index = tabIndex;
    if (!EnsureWindow()) return;
    KillTimer(s_wnd, kShowTimer);
    if (tabIndex < 0 || tabIndex >= (int)g_tabs.size()) {
        if (s_shown) s_hiddenAt = GetTickCount();
        s_shown = false;
        s_tabId = 0;
        KillTimer(s_wnd, kWatchTimer);
        ShowWindow(s_wnd, SW_HIDE);
        return;
    }
    // Already showing one, or one went a moment ago: the next tab's card at once.
    if (s_shown || (s_hiddenAt && GetTickCount() - s_hiddenAt < kWarmMs)) Show();
    else SetTimer(s_wnd, kShowTimer, kDelayMs, nullptr);
}

void TabHoverCardHide() {
    s_shown = false;
    s_tabId = 0;
    s_hiddenAt = 0;                      // after a click the next card waits again
    if (!s_wnd) return;
    KillTimer(s_wnd, kShowTimer);
    KillTimer(s_wnd, kWatchTimer);
    ShowWindow(s_wnd, SW_HIDE);
}

void TabHoverCardForget(unsigned long long tabId) {
    s_pictures.erase(tabId);
    s_asking.erase(tabId);
    if (s_tabId == tabId) TabHoverCardHide();
}
