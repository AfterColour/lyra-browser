// Lyra — снимок страницы: видимая часть или вся страница, в буфер обмена или в PNG-файл.
#include "ui/Screenshot.h"
#include "app/Log.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "core/Localization.h"
#include "core/StringUtils.h"
#include "core/Url.h"
#include "ui/Chrome.h"
#include "ui/Dialogs.h"
#include "ui/Draw.h"
#include "ui/MainWindow.h"
#include "ui/Menu.h"
#include "ui/Theme.h"

#include <WebKit/WKPagePrivate.h>
#include <WebKit/WKMutableDictionary.h>

namespace {

enum Action { kCopyVisible = 1, kSaveVisible, kCopyFull, kSaveFull };

// The whole page can be very long: the picture is scaled down to stay within these.
constexpr double kMaxSide = 16384, kMaxPixels = 60e6;

struct Request {
    unsigned long long tabId = 0;
    int action = 0;
    std::wstring title, url;
};

// ---- toast -------------------------------------------------------------------------------------
HWND s_toast = nullptr;
std::wstring s_toastText;
constexpr UINT_PTR kToastTimer = 1;

LRESULT CALLBACK ToastProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_TIMER && w == kToastTimer) { KillTimer(h, kToastTimer); ShowWindow(h, SW_HIDE); return 0; }
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, msg, w, l);
}

// ---- pixels --------------------------------------------------------------------------------------
// The snapshot's rows (BGRA, premultiplied) as an opaque top-down 32-bit image.
std::vector<BYTE> OpaqueRows(const void* pixels, uint32_t w, uint32_t h, uint32_t stride) {
    std::vector<BYTE> out((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        const BYTE* src = (const BYTE*)pixels + (size_t)y * stride;
        BYTE* dst = out.data() + (size_t)y * w * 4;
        memcpy(dst, src, (size_t)w * 4);
        for (uint32_t x = 0; x < w; ++x) dst[x * 4 + 3] = 255;
    }
    return out;
}

bool PngEncoder(CLSID& clsid) {
    UINT n = 0, size = 0;
    GetImageEncodersSize(&n, &size);
    if (!size) return false;
    std::vector<BYTE> buf(size);
    auto* codecs = (ImageCodecInfo*)buf.data();
    if (GetImageEncoders(n, size, codecs) != Ok) return false;
    for (UINT i = 0; i < n; ++i)
        if (wcscmp(codecs[i].MimeType, L"image/png") == 0) { clsid = codecs[i].Clsid; return true; }
    return false;
}

std::vector<BYTE> EncodePng(std::vector<BYTE>& rows, uint32_t w, uint32_t h) {
    std::vector<BYTE> out;
    CLSID png;
    if (!PngEncoder(png)) return out;
    Bitmap bmp((INT)w, (INT)h, (INT)w * 4, PixelFormat32bppARGB, rows.data());
    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return out;
    if (bmp.Save(stream, &png) == Ok) {
        STATSTG st = {};
        stream->Stat(&st, STATFLAG_NONAME);
        out.resize((size_t)st.cbSize.QuadPart);
        LARGE_INTEGER zero = {};
        stream->Seek(zero, STREAM_SEEK_SET, nullptr);
        ULONG read = 0;
        if (!out.empty()) stream->Read(out.data(), (ULONG)out.size(), &read);
        out.resize(read);
    }
    stream->Release();
    return out;
}

// The clipboard gets the picture twice: as a bitmap (every program) and as PNG (keeps it exact).
bool CopyImage(std::vector<BYTE>& rows, uint32_t w, uint32_t h) {
    const size_t bits = (size_t)w * h * 4;
    HGLOBAL dib = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + bits);
    if (!dib) return false;
    {
        auto* bi = (BITMAPINFOHEADER*)GlobalLock(dib);
        *bi = {};
        bi->biSize = sizeof(BITMAPINFOHEADER);
        bi->biWidth = (LONG)w;
        bi->biHeight = (LONG)h;                          // bottom-up, as programs expect from CF_DIB
        bi->biPlanes = 1;
        bi->biBitCount = 32;
        bi->biCompression = BI_RGB;
        BYTE* dst = (BYTE*)(bi + 1);
        for (uint32_t y = 0; y < h; ++y) memcpy(dst + (size_t)(h - 1 - y) * w * 4, rows.data() + (size_t)y * w * 4, (size_t)w * 4);
        GlobalUnlock(dib);
    }
    std::vector<BYTE> png = EncodePng(rows, w, h);
    if (!OpenClipboard(g_hWnd)) { GlobalFree(dib); return false; }
    EmptyClipboard();
    bool ok = SetClipboardData(CF_DIB, dib) != nullptr;
    if (!ok) GlobalFree(dib);
    if (!png.empty()) {
        if (HGLOBAL hp = GlobalAlloc(GMEM_MOVEABLE, png.size())) {
            memcpy(GlobalLock(hp), png.data(), png.size());
            GlobalUnlock(hp);
            static const UINT pngFormat = RegisterClipboardFormatW(L"PNG");
            if (!SetClipboardData(pngFormat, hp)) GlobalFree(hp);
        }
    }
    CloseClipboard();
    return ok;
}

std::wstring SafeName(std::wstring s) {
    for (auto& c : s) if (wcschr(L"\\/:*?\"<>|", c) || c < 32) c = L'_';
    while (!s.empty() && (s.back() == L' ' || s.back() == L'.')) s.pop_back();
    if (s.size() > 60) s.resize(60);
    return s;
}

bool SaveImage(std::vector<BYTE>& rows, uint32_t w, uint32_t h, const Request& r) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t stamp[64];
    swprintf_s(stamp, L"%04u-%02u-%02u %02u.%02u.%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring site = IsInternalPageUrl(r.url) ? std::wstring(L"Lyra") : HostOf(r.url);
    if (site.empty()) site = L"page";
    const std::wstring name = SafeName(Tr(L"Screenshot ", L"Снимок ") + site) + L" " + stamp + L".png";
    const std::wstring path = PickSaveFile(g_hWnd, Tr(L"Save screenshot", L"Сохранить снимок"), name,
                                           Tr(L"PNG image", L"Изображение PNG"), L"*.png", L"png");
    if (path.empty()) return false;
    std::vector<BYTE> png = EncodePng(rows, w, h);
    if (png.empty()) return false;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const BOOL ok = WriteFile(f, png.data(), (DWORD)png.size(), &wrote, nullptr);
    CloseHandle(f);
    Log("screenshot: saved %ux%u to %ls", w, h, path.c_str());
    return ok && wrote == png.size();
}

void OnSnapshot(const void* pixels, uint32_t w, uint32_t h, uint32_t stride, void* ctx) {
    std::unique_ptr<Request> r((Request*)ctx);
    if (!pixels || !w || !h) {
        Log("screenshot: no picture");
        ShowToast(Tr(L"Could not take a screenshot of this page", L"Не удалось сделать снимок этой страницы"));
        return;
    }
    std::vector<BYTE> rows = OpaqueRows(pixels, w, h, stride);
    const bool copy = r->action == kCopyVisible || r->action == kCopyFull;
    Log("screenshot: %ux%u (%s)", w, h, copy ? "copy" : "save");
    if (copy) {
        ShowToast(CopyImage(rows, w, h) ? Tr(L"Screenshot copied", L"Снимок скопирован")
                                        : Tr(L"Could not copy the screenshot", L"Не удалось скопировать снимок"));
    } else if (SaveImage(rows, w, h, *r)) {
        ShowToast(Tr(L"Screenshot saved", L"Снимок сохранён"));
    }
}

void Take(int action) {
    Tab* t = ActiveTab();
    if (!t || !t->page) return;
    auto r = std::make_unique<Request>();
    r->tabId = t->id;
    r->action = action;
    r->url = t->url;
    r->title = t->title;
    const bool full = action == kCopyFull || action == kSaveFull;
    if (!full) {
        WKPageTakeSnapshotOfPage(t->page, false, 0, 0, OnSnapshot, r.release());
        return;
    }
    // The whole page: its size first (CSS pixels and the display's scale), so a very long page is
    // scaled down instead of failing.
    WKStringRef js = WKStringCreateWithUTF8CString(
        "return [Math.max(document.documentElement.scrollWidth, document.body ? document.body.scrollWidth : 0),"
        " Math.max(document.documentElement.scrollHeight, document.body ? document.body.scrollHeight : 0),"
        " devicePixelRatio].join(',');");
    WKMutableDictionaryRef args = WKMutableDictionaryCreate();
    struct Ctx { std::unique_ptr<Request> r; };
    auto* c = new Ctx{ std::move(r) };
    WKPageCallAsyncJavaScript(t->page, js, (WKDictionaryRef)args, nullptr, c, [](WKTypeRef result, WKErrorRef, void* p) {
        std::unique_ptr<Ctx> c((Ctx*)p);
        double cw = 0, ch = 0, dpr = 1;
        if (result && WKGetTypeID(result) == WKStringGetTypeID()) {
            const std::string s = WKStrUtf8((WKStringRef)result);
            sscanf_s(s.c_str(), "%lf,%lf,%lf", &cw, &ch, &dpr);
        }
        Tab* t = FindTab(c->r->tabId);
        if (!t || !t->page) return;
        uint32_t w = 0, h = 0;
        if (cw > 0 && ch > 0 && dpr > 0) {
            double pw = cw * dpr, ph = ch * dpr;
            const double k = std::min({ 1.0, kMaxSide / pw, kMaxSide / ph, std::sqrt(kMaxPixels / (pw * ph)) });
            w = (uint32_t)std::max(1.0, std::floor(pw * k));
            h = (uint32_t)std::max(1.0, std::floor(ph * k));
        }
        WKPageTakeSnapshotOfPage(t->page, true, w, h, OnSnapshot, c->r.release());
    });
    WKRelease(args);
    WKRelease(js);
}

}  // namespace

void ShowToast(const std::wstring& text) {
    if (!g_hWnd) return;
    if (!s_toast) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = ToastProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"LyraToast";
        RegisterClassExW(&wc);
        s_toast = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"LyraToast", L"",
                                  WS_POPUP, 0, 0, 1, 1, g_hWnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!s_toast) return;
    }
    s_toastText = text;
    Font* font = g_fText.get();
    if (!font) return;
    float textW = 0;
    {
        Bitmap probe(1, 1, PixelFormat32bppPARGB);
        Graphics pg(&probe);
        StringFormat sf(StringFormat::GenericTypographic());
        RectF box;
        pg.MeasureString(text.c_str(), -1, font, PointF(0, 0), &sf, &box);
        textW = box.Width;
    }
    const int w = (int)std::ceil(textW + S(36)), h = (int)std::round(S(36));
    RECT cr;
    GetClientRect(g_hWnd, &cr);
    POINT tl = { (cr.right - w) / 2, (LONG)(g_L.chromeH + S(14)) };
    ClientToScreen(g_hWnd, &tl);

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
        GraphicsPath p;
        AddRound(p, RectF(0.5f, 0.5f, (float)w - 1, (float)h - 1), (float)h / 2);
        SolidBrush bg(g_th.popup);
        g.FillPath(&bg, &p);
        Pen border(g_th.fieldBorder, 1.f);
        g.DrawPath(&border, &p);
        StringFormat sf(StringFormat::GenericTypographic());
        sf.SetAlignment(StringAlignmentCenter);
        sf.SetLineAlignment(StringAlignmentCenter);
        SolidBrush tb(g_th.text);
        g.DrawString(text.c_str(), -1, font, RectF(0, 0, (float)w, (float)h), &sf, &tb);
    }
    POINT src = { 0, 0 };
    SIZE size = { w, h };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(s_toast, screen, &tl, &size, mem, &src, 0, &blend, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    SetWindowPos(s_toast, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetTimer(s_toast, kToastTimer, 1800, nullptr);
}

void ScreenshotMenu() {
    Tab* t = ActiveTab();
    if (!t || !t->page) return;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, kCopyVisible, Tr(L"Copy visible part", L"Копировать видимую часть"));
    AppendMenuW(m, MF_STRING, kSaveVisible, Tr(L"Save visible part\x2026", L"Сохранить видимую часть\x2026"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kCopyFull, Tr(L"Copy whole page", L"Копировать всю страницу"));
    AppendMenuW(m, MF_STRING, kSaveFull, Tr(L"Save whole page\x2026", L"Сохранить всю страницу\x2026"));
    MenuSetIcon(m, kCopyVisible, L"");
    MenuSetIcon(m, kSaveVisible, L"");
    MenuSetIcon(m, kCopyFull, L"");
    MenuSetIcon(m, kSaveFull, L"");
    const RectF anchor = ChromeMenuAnchor();
    POINT pt = { (LONG)anchor.GetRight(), (LONG)(anchor.GetBottom() + S(4)) };
    ClientToScreen(g_hWnd, &pt);
    const int cmd = MenuTrack(m, TPM_RIGHTALIGN | TPM_TOPALIGN, pt.x, pt.y, g_hWnd);
    DestroyMenu(m);
    if (cmd) Take(cmd);
}
