// Lyra — подсказки под адресной строкой: поиск и совпадения из истории.
//
// A popup below the address field (owned by the main window, never activated, so the field
// keeps the keyboard focus). Rows: "search for" / "go to" what was typed, then history matches.
#include "ui/Suggestions.h"
#include "browser/Favicons.h"
#include "browser/History.h"
#include "browser/TabManager.h"
#include "core/Localization.h"
#include "core/StringUtils.h"
#include "core/Url.h"
#include "ui/AddressBar.h"
#include "ui/Chrome.h"
#include "ui/Draw.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"

struct SuggestRow {
    enum Kind { Search, Go, History } kind = Search;
    std::wstring title, url;
};

static HWND s_popup = nullptr;
static std::vector<SuggestRow> s_rows;
static int s_sel = -1, s_hover = -1;
static std::wstring s_typed;
static bool s_settingText = false;          // our own SetWindowText: not typing
static bool s_tracking = false;

static float RowH() { return std::floor(S(34)); }
static float Pad() { return std::floor(S(6)); }

static Color PopupBg() { return g_dark ? Color(37, 39, 44) : Color(255, 255, 255); }

// Text that ResolveInput turns into an address rather than a search.
static bool LooksLikeAddress(const std::wstring& s) {
    if (s.find(L' ') != std::wstring::npos) return false;
    return s.find(L"://") != std::wstring::npos || s.compare(0, 9, L"localhost") == 0 ||
           (s.find(L'.') != std::wstring::npos && s.front() != L'.' && s.back() != L'.');
}

static void SetEditText(const std::wstring& text) {
    s_settingText = true;
    SetWindowTextW(g_urlEdit, text.c_str());
    SendMessageW(g_urlEdit, EM_SETSEL, (WPARAM)text.size(), (LPARAM)text.size());
    s_settingText = false;
}

static void Navigate(const SuggestRow& r) {
    SuggestHide();
    NavigateActive(r.kind == SuggestRow::History ? r.url : s_typed);
    FocusWebView();
}

static void Layout(RECT* out) {
    // Under the address field, at least 460 px wide, inside the window.
    RECT cr;
    GetClientRect(g_hWnd, &cr);
    const float w = std::min(std::max(g_L.pill.Width, S(460)), (float)cr.right - S(16));
    float x = std::min(g_L.pill.X, (float)cr.right - S(8) - w);
    x = std::max(S(8), x);
    POINT tl = { (LONG)x, (LONG)(g_L.pill.GetBottom() + S(4)) };
    ClientToScreen(g_hWnd, &tl);
    const int h = (int)(RowH() * (float)s_rows.size() + Pad() * 2);
    *out = { tl.x, tl.y, tl.x + (LONG)w, tl.y + h };
}

static int RowAt(int y) {
    const int i = (int)std::floor(((float)y - Pad()) / RowH());
    return (i >= 0 && i < (int)s_rows.size()) ? i : -1;
}

static void Paint(HDC hdc) {
    RECT rc;
    GetClientRect(s_popup, &rc);
    const int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) { if (bmp) DeleteObject(bmp); return; }
    HDC mem = CreateCompatibleDC(hdc);
    HGDIOBJ old = SelectObject(mem, bmp);
    {
        const Theme& T = g_th;
        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeHighQuality);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        SolidBrush bg(PopupBg());
        g.FillRectangle(&bg, 0, 0, w, h);

        StringFormat sf(StringFormat::GenericTypographic());
        sf.SetFormatFlags(sf.GetFormatFlags() | StringFormatFlagsNoWrap);
        sf.SetTrimming(StringTrimmingEllipsisCharacter);
        sf.SetLineAlignment(StringAlignmentCenter);

        for (int i = 0; i < (int)s_rows.size(); ++i) {
            const SuggestRow& r = s_rows[i];
            RectF row(Pad(), Pad() + RowH() * (float)i, (float)w - Pad() * 2, RowH());
            if (i == s_sel) FillRound(g, Color(g_dark ? 70 : 40, T.accent.GetR(), T.accent.GetG(), T.accent.GetB()), row, S(6));
            else if (i == s_hover) FillRound(g, T.hover, row, S(6));

            const float iconS = S(16);
            RectF icon(row.X + S(10), row.Y + (row.Height - iconS) / 2, iconS, iconS);
            Bitmap* fav = r.kind == SuggestRow::History ? FaviconFor(r.url) : nullptr;
            if (fav) {
                g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
                g.DrawImage(fav, icon);
            } else {
                const wchar_t* glyph = r.kind == SuggestRow::Search ? L"\uE721" : r.kind == SuggestRow::Go ? L"\uE774" : L"\uE81C";
                DrawGlyph(g, glyph, icon, T.text2, g_fIconSm.get());
            }

            const float tx = icon.GetRight() + S(10);
            const float right = row.GetRight() - S(10);
            std::wstring main = r.title, extra;
            if (r.kind == SuggestRow::Search) extra = std::wstring(L"  \x2014  ") + Tr(L"Search", L"Поиск");
            else if (r.kind == SuggestRow::History) extra = L"  \x2014  " + DisplayUrl(r.url);
            RectF m;
            g.MeasureString(main.c_str(), -1, g_fText.get(), RectF(0, 0, 10000, 1000), &sf, &m);
            const float mainW = std::min(m.Width + S(1), (right - tx) * (extra.empty() ? 1.f : 0.6f));
            SolidBrush tb(T.text), t2(T.text2);
            g.DrawString(main.c_str(), -1, g_fText.get(), RectF(tx, row.Y, mainW, row.Height), &sf, &tb);
            if (!extra.empty() && right - (tx + mainW) > S(20))
                g.DrawString(extra.c_str(), -1, g_fText.get(), RectF(tx + mainW, row.Y, right - tx - mainW, row.Height), &sf, &t2);
        }
    }
    GdiFlush();
    DWORD* px = (DWORD*)bits;
    for (size_t i = 0, n = (size_t)w * (size_t)h; i < n; ++i) px[i] |= 0xFF000000u;
    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static LRESULT CALLBACK PopupProc(HWND h, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;                        // the address field keeps the focus
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        Paint(hdc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const int i = RowAt(GET_Y_LPARAM(lParam));
        if (i != s_hover) { s_hover = i; InvalidateRect(h, nullptr, FALSE); }
        if (!s_tracking) {
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
            s_tracking = true;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        s_tracking = false;
        s_hover = -1;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        const int i = RowAt(GET_Y_LPARAM(lParam));
        if (i >= 0) {
            const SuggestRow r = s_rows[i];
            Navigate(r);
        }
        return 0;
    }
    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, IDC_HAND));
        return TRUE;
    }
    return DefWindowProcW(h, msg, wParam, lParam);
}

static void EnsurePopup() {
    if (s_popup) return;
    static bool registered = false;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = PopupProc;
        wc.hInstance = hi;
        wc.lpszClassName = L"LyraSuggestions";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        if (!RegisterClassExW(&wc)) return;
        registered = true;
    }
    s_popup = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"LyraSuggestions", L"", WS_POPUP,
                              0, 0, 10, 10, g_hWnd, nullptr, hi, nullptr);
    if (s_popup) {
        const DWORD round = 2;                         // DWMWCP_ROUND (Windows 11)
        DwmSetWindowAttribute(s_popup, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
    }
}

void SuggestUpdate() {
    if (s_settingText || !g_urlEdit || !IsEditing()) return;
    wchar_t buf[2048] = {};
    GetWindowTextW(g_urlEdit, buf, ARRAYSIZE(buf));
    s_typed = buf;
    const std::wstring text = TrimWs(s_typed);
    s_rows.clear();
    s_sel = -1;
    s_hover = -1;
    if (text.empty()) { SuggestHide(); return; }

    const std::vector<HistoryEntry> matches = HistoryMatch(text, 6);
    SuggestRow first;
    first.kind = LooksLikeAddress(text) ? SuggestRow::Go : SuggestRow::Search;
    first.title = text;
    // An address typed exactly like the start of a visited one goes first, as in other browsers.
    bool historyFirst = false;
    if (!matches.empty()) {
        std::wstring bare = DisplayUrl(matches[0].url);
        if (bare.compare(0, 4, L"www.") == 0) bare = bare.substr(4);
        historyFirst = _wcsnicmp(bare.c_str(), text.c_str(), text.size()) == 0;
    }
    auto pushHistory = [&](const HistoryEntry& e) {
        SuggestRow r;
        r.kind = SuggestRow::History;
        r.url = e.url;
        r.title = e.title.empty() ? HostOf(e.url) : e.title;
        s_rows.push_back(r);
    };
    if (historyFirst) pushHistory(matches[0]);
    s_rows.push_back(first);
    for (size_t i = historyFirst ? 1 : 0; i < matches.size(); ++i) pushHistory(matches[i]);

    EnsurePopup();
    if (!s_popup) return;
    RECT r;
    Layout(&r);
    SetWindowPos(s_popup, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
    InvalidateRect(s_popup, nullptr, FALSE);
}

void SuggestHide() {
    if (s_popup && IsWindowVisible(s_popup)) ShowWindow(s_popup, SW_HIDE);
    s_sel = s_hover = -1;
}

bool SuggestVisible() { return s_popup && IsWindowVisible(s_popup); }

bool SuggestMove(int delta) {
    if (!SuggestVisible() || s_rows.empty()) return false;
    const int n = (int)s_rows.size();
    int next = s_sel + delta;
    if (next < -1) next = n - 1;
    if (next >= n) next = -1;
    s_sel = next;
    // The field shows the selected address; back at "nothing selected" it shows what was typed.
    if (s_sel >= 0 && s_rows[s_sel].kind == SuggestRow::History) SetEditText(s_rows[s_sel].url);
    else SetEditText(s_typed);
    InvalidateRect(s_popup, nullptr, FALSE);
    return true;
}

bool SuggestAccept() {
    if (!SuggestVisible() || s_sel < 0 || s_sel >= (int)s_rows.size()) { SuggestHide(); return false; }
    const SuggestRow r = s_rows[s_sel];
    Navigate(r);
    return true;
}

bool SuggestCancel() {
    if (!SuggestVisible()) return false;
    SuggestHide();
    SetEditText(s_typed);
    return true;
}
