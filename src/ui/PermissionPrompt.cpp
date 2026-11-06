// Lyra — окошко запроса разрешения под адресной строкой («Сайт запрашивает: показывать уведомления»).
#include "ui/PermissionPrompt.h"
#include "browser/Permissions.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "core/Localization.h"
#include "ui/Animation.h"
#include "ui/Chrome.h"
#include "ui/Draw.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"

static HWND s_popup = nullptr;
static unsigned long long s_seq = 0;          // the question shown
static std::wstring s_site, s_what;
static const wchar_t* s_icon = L"";
static int s_hover = 0, s_down = 0;           // 1 block, 2 allow, 3 close
static bool s_tracking = false;
static POINT s_pos = {};
static std::unique_ptr<Font> s_fTitle, s_fText, s_fBold, s_fGlyph, s_fIcon;

// Logical (96 DPI) layout
constexpr float kW = 330, kPad = 16, kTitleY = 14, kRowY = 44, kButtonsY = 82, kBtnW = 100, kBtnH = 32, kH = 130;

static Color PopupBg() { return g_dark ? Color(37, 39, 44) : Color(255, 255, 255); }

static RectF AllowRect() { return RectF(S(kW - kPad - kBtnW), S(kButtonsY), S(kBtnW), S(kBtnH)); }
static RectF BlockRect() { return RectF(S(kW - kPad - kBtnW * 2 - 8), S(kButtonsY), S(kBtnW), S(kBtnH)); }
static RectF CloseRect() { return RectF(S(kW - 38), S(10), S(28), S(28)); }

static int HitAt(int x, int y) {
    const float fx = (float)x, fy = (float)y;
    if (BlockRect().Contains(fx, fy)) return 1;
    if (AllowRect().Contains(fx, fy)) return 2;
    if (CloseRect().Contains(fx, fy)) return 3;
    return 0;
}

static void MakeFonts() {
    auto make = [](const wchar_t* family, float px, int style) {
        auto f = std::make_unique<Font>(family, px, style, UnitPixel);
        if (!f->IsAvailable()) f = std::make_unique<Font>(L"Segoe UI", px, style, UnitPixel);
        return f;
    };
    s_fTitle = make(L"Segoe UI Variable Text", S(14), FontStyleBold);
    s_fText = make(L"Segoe UI Variable Text", S(13), FontStyleRegular);
    s_fBold = make(L"Segoe UI Variable Text", S(13), FontStyleBold);
    s_fGlyph = make(L"Segoe Fluent Icons", S(10), FontStyleRegular);
    if (!s_fGlyph->IsAvailable()) s_fGlyph = make(L"Segoe MDL2 Assets", S(10), FontStyleRegular);
    s_fIcon = make(L"Segoe Fluent Icons", S(16), FontStyleRegular);
    if (!s_fIcon->IsAvailable()) s_fIcon = make(L"Segoe MDL2 Assets", S(16), FontStyleRegular);
}

static Color Lighter(const Color& c, int d) {
    return Color(255, (BYTE)std::min(255, c.GetR() + d), (BYTE)std::min(255, c.GetG() + d), (BYTE)std::min(255, c.GetB() + d));
}

static void Paint(HDC hdc) {
    RECT rc;
    GetClientRect(s_popup, &rc);
    PaintBuffered(hdc, rc, true, [&](Graphics& g) {
        const Theme& T = g_th;
        g.SetSmoothingMode(SmoothingModeHighQuality);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        SolidBrush bg(PopupBg());
        g.FillRectangle(&bg, 0, 0, rc.right, rc.bottom);

        StringFormat sf(StringFormat::GenericTypographic());
        sf.SetFormatFlags(sf.GetFormatFlags() | StringFormatFlagsNoWrap);
        sf.SetTrimming(StringTrimmingEllipsisCharacter);
        sf.SetLineAlignment(StringAlignmentCenter);
        SolidBrush text(T.text), text2(T.text2);

        const float x = S(kPad);
        std::wstring title = s_site + Tr(L" wants to", L" запрашивает");
        g.DrawString(title.c_str(), -1, s_fTitle.get(), RectF(x, S(kTitleY), S(kW - kPad - 44), S(22)), &sf, &text);

        DrawGlyph(g, s_icon, RectF(x, S(kRowY), S(20), S(24)), T.text2, s_fIcon.get());
        g.DrawString(s_what.c_str(), -1, s_fText.get(), RectF(x + S(30), S(kRowY), S(kW - kPad * 2 - 30), S(24)), &sf, &text);

        {   // ×
            const RectF r = CloseRect();
            if (s_hover == 3) FillRound(g, T.hover, r, S(6));
            DrawGlyph(g, L"", r, T.text2, s_fGlyph.get());
        }

        StringFormat cf(StringFormat::GenericTypographic());
        cf.SetAlignment(StringAlignmentCenter);
        cf.SetLineAlignment(StringAlignmentCenter);
        {
            const RectF r = BlockRect();
            if (s_hover == 1) FillRound(g, T.hover, r, S(7));
            GraphicsPath p;
            AddRound(p, r, S(7));
            Pen pen(T.fieldBorder, 1.f);
            g.DrawPath(&pen, &p);
            g.DrawString(Tr(L"Block", L"Блокировать"), -1, s_fText.get(), r, &cf, &text);
        }
        {
            const RectF r = AllowRect();
            FillRound(g, s_hover == 2 ? Lighter(T.accent, 18) : T.accent, r, S(7));
            SolidBrush white(Color(255, 255, 255));
            g.DrawString(Tr(L"Allow", L"Разрешить"), -1, s_fBold.get(), r, &cf, &white);
        }
    });
}

static void Answer(int answer) {
    const unsigned long long seq = s_seq;
    PermissionPromptClose();
    PermissionsAnswer(seq, answer);          // shows the next question, if any
}

static LRESULT CALLBACK PopupProc(HWND h, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        Paint(hdc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;                // the page keeps the keyboard
    case WM_MOUSEMOVE: {
        const int hit = HitAt(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (hit != s_hover) { s_hover = hit; InvalidateRect(h, nullptr, FALSE); }
        if (!s_tracking) {
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
            s_tracking = true;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        s_tracking = false;
        if (s_hover) { s_hover = 0; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONDOWN:
        s_down = HitAt(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (s_down) SetCapture(h);
        return 0;
    case WM_LBUTTONUP: {
        const int hit = HitAt(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        const int down = s_down;
        s_down = 0;
        if (GetCapture() == h) ReleaseCapture();
        if (!hit || hit != down) return 0;
        Answer(hit == 2 ? 1 : hit == 1 ? 0 : -1);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && (HWND)wParam == h) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(h, &pt);
            SetCursor(LoadCursor(nullptr, HitAt(pt.x, pt.y) ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_DESTROY:
        s_tracking = false;
        s_hover = s_down = 0;
        return 0;
    }
    return DefWindowProcW(h, msg, wParam, lParam);
}

// Under the left end of the address field, inside the window.
static POINT PopupPosition() {
    RECT cr;
    GetClientRect(g_hWnd, &cr);
    float x = std::min(g_L.pill.X, (float)cr.right - S(8) - S(kW));
    x = std::max(S(8), x);
    POINT tl = { (LONG)x, (LONG)(g_L.pill.GetBottom() + S(6)) };
    ClientToScreen(g_hWnd, &tl);
    return tl;
}

void PermissionPromptClose() {
    if (!s_popup) return;
    HWND h = s_popup;
    s_popup = nullptr;
    s_seq = 0;
    PopupRevealStop(h);
    DestroyWindow(h);
}

static void Open(const PermissionAsk& ask) {
    static bool registered = false;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = PopupProc;
        wc.hInstance = hi;
        wc.lpszClassName = L"LyraPermissionPrompt";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        if (!RegisterClassExW(&wc)) return;
        registered = true;
    }
    MakeFonts();
    s_seq = ask.seq;
    s_site = ask.site;
    if (ask.kind == "geolocation") {
        s_icon = L"";
        s_what = Tr(L"Know your location", L"Доступ к вашему местоположению");
    } else {
        s_icon = L"";
        s_what = Tr(L"Show notifications", L"Показ уведомлений");
    }
    s_pos = PopupPosition();
    s_popup = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"LyraPermissionPrompt", L"", WS_POPUP,
                              s_pos.x, s_pos.y, (int)S(kW), (int)S(kH), g_hWnd, nullptr, hi, nullptr);
    if (!s_popup) { s_seq = 0; return; }
    const DWORD round = 2;                               // DWMWCP_ROUND (Windows 11)
    DwmSetWindowAttribute(s_popup, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
    const BOOL dark = g_dark;
    DwmSetWindowAttribute(s_popup, kDwmDarkMode, &dark, sizeof(dark));
    PopupShow(s_popup, false);
}

void PermissionPromptUpdate() {
    if (!g_hWnd) return;
    Tab* t = ActiveTab();
    const PermissionAsk* ask = (t && !g_closing && !IsIconic(g_hWnd)) ? PermissionsPendingForTab(t->id) : nullptr;
    if (!ask) { PermissionPromptClose(); return; }
    if (ask->seq != s_seq) {
        PermissionPromptClose();
        Open(*ask);
        return;
    }
    const POINT p = PopupPosition();                     // follows the window and the address field
    if (p.x != s_pos.x || p.y != s_pos.y) {
        s_pos = p;
        SetWindowPos(s_popup, nullptr, p.x, p.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}
