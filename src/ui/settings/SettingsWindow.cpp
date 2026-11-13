// Lyra — окно настроек: отрисовка, прокрутка, мышь и клавиатура.
//
// Layout (macOS System Settings style):
//   left   - sidebar: title, search field, categories with coloured icons
//   right  - header with the page title (and a back arrow on detail pages) and the window
//            buttons; below it the page: group titles over rounded cards of rows
// The window has no system title bar (see ui/WindowFrame.h).
#include "ui/settings/SettingsWindow.h"
#include "app/Log.h"
#include "core/Localization.h"
#include "core/Settings.h"
#include "core/StringUtils.h"
#include "ui/Animation.h"
#include "ui/Draw.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/WindowFrame.h"
#include "ui/settings/SettingsModel.h"
#include "ui/settings/SettingsPages.h"

HWND g_setHwnd = nullptr;
std::vector<SPage> g_pages;

static HWND    g_setSearch = nullptr;   // unused: the search field is drawn by Lyra (see s_query)
static HWND    g_setEdit = nullptr;
static HBRUSH  g_fieldBrush = nullptr;
static HFONT   g_sEditFont = nullptr;
static int     g_sEditH = 18;
static UINT    g_sDpi = 96;
static WNDPROC g_oldSEditProc = nullptr;
static std::unique_ptr<Font> g_sfBody, g_sfSmall, g_sfTitle, g_sfGroup, g_sfSide, g_sfIcon, g_sfIconSm, g_sfCaption, g_sfBadge;

static std::vector<SView> g_view;
static std::vector<std::pair<float, float>> g_groups;
static int   g_sPage = 0;
static float g_sScroll = 0.f, g_sContentH = 0.f;
// Smooth scrolling: the wheel and keys move the target, the view follows it on the frame clock.
static float s_scrollTarget = 0.f;
static bool  s_scrollAnim = false;
static int   g_sHoverSide = -1, g_sHoverRow = -1, g_sHoverPart = -1, g_sDragRow = -1;
static bool  g_sHoverCtl = false, g_sHoverBack = false, g_sTracking = false, g_sTrackingNc = false;
static int   g_sCapHover = -1, g_sCapDown = -1;      // window buttons: 0 minimise, 1 maximise, 2 close
static SRow* g_sEditRow = nullptr;

static float SS(float v) { return v * (float)g_sDpi / 96.f; }
static bool s_sideTransparent = false;      // sidebar painted over Mica
static void UpdateSettingsBackdrop();

// ---- colours ---------------------------------------------------------------
struct SPal { Color bg, side, card, field, border, text, text2, off, hover, pressed; };
static SPal Pal() {
    SPal p;
    if (!g_dark) {
        p.bg = Color(243, 244, 247);   p.side = Color(234, 236, 240); p.card = Color(255, 255, 255);
        p.field = Color(255, 255, 255); p.border = Color(218, 221, 227);
        p.text = Color(27, 29, 33);    p.text2 = Color(89, 96, 107);  p.off = Color(191, 196, 204);
        p.hover = Color(20, 0, 0, 0);  p.pressed = Color(36, 0, 0, 0);
    } else {
        p.bg = Color(29, 31, 35);      p.side = Color(25, 27, 30);    p.card = Color(38, 40, 45);
        p.field = Color(46, 48, 54);   p.border = Color(54, 57, 64);
        p.text = Color(236, 237, 239); p.text2 = Color(166, 171, 180); p.off = Color(74, 78, 87);
        p.hover = Color(24, 255, 255, 255); p.pressed = Color(40, 255, 255, 255);
    }
    return p;
}
static Color WithAlpha(const Color& c, int a) { return Color((BYTE)a, c.GetR(), c.GetG(), c.GetB()); }

// ---- geometry --------------------------------------------------------------
static SGeo Geo() {
    RECT cr = {};
    GetClientRect(g_setHwnd, &cr);
    SGeo g;
    g.W = (float)cr.right; g.H = (float)cr.bottom;
    g.sideW = SS(240); g.headerH = SS(52);
    g.vpY = g.headerH; g.vpH = std::max(0.f, g.H - g.headerH);
    const float vpW = std::max(0.f, g.W - g.sideW);
    g.colW = std::min(SS(720), std::max(0.f, vpW - SS(56)));
    g.colX = g.sideW + (vpW - g.colW) / 2;
    return g;
}

// Pages shown in the sidebar (detail pages are reached from their parent page)
static std::vector<int> SidebarPages() {
    std::vector<int> v;
    for (int i = 0; i < (int)g_pages.size(); ++i) if (g_pages[i].parent < 0) v.push_back(i);
    return v;
}
static int SidebarPageOf(int page) {
    while (page >= 0 && page < (int)g_pages.size() && g_pages[page].parent >= 0) page = g_pages[page].parent;
    return page;
}

static RectF SSearchRect(const SGeo& G) { return RectF(SS(10), SS(44), G.sideW - SS(20), SS(32)); }
static RectF SSideRect(const SGeo&, int slot) {
    return RectF(SS(8), SS(90) + SS(36) * (float)slot, SS(240) - SS(16), SS(34));
}
static RectF SCapRect(const SGeo& G, int i) {           // 0 minimise, 1 maximise, 2 close
    const float w = SS(46), h = SS(36);
    return RectF(G.W - w * (float)(3 - i), 0, w, h);
}
static RectF SBackRect(const SGeo& G) { return RectF(G.colX - SS(6), (G.headerH - SS(32)) / 2, SS(32), SS(32)); }
static bool OnDetailPage() { return g_sPage >= 0 && g_sPage < (int)g_pages.size() && g_pages[g_sPage].parent >= 0; }

static RectF SRowRect(const SGeo& G, const SView& v) {
    return RectF(G.colX, G.vpY + v.y - g_sScroll, G.colW, v.h);
}

static RectF SCtlRect(const SRow& r, const RectF& row) {
    const float right = row.GetRight() - SS(16);
    float w = 0, h = SS(30);
    switch (r.type) {
    case SRow::Toggle:
    case SRow::ToggleGear: w = SS(40); h = SS(22); break;
    case SRow::Choice:    w = std::min(SS(220), row.Width * 0.42f); break;
    case SRow::Segmented: w = std::min(SS(330), row.Width * 0.56f); break;
    case SRow::Slider:    w = std::min(SS(200), row.Width * 0.36f); h = SS(22); break;
    case SRow::Text:      w = std::min(SS(300), row.Width * 0.5f); break;
    case SRow::Button:    w = SS(150); break;
    case SRow::Swatches: {
        const float d = SS(24), gap = SS(10);
        const float n = (float)r.colors.size();
        w = n * d + std::max(0.f, n - 1) * gap; h = d;
        break;
    }
    case SRow::Cards:
        return RectF(row.X + SS(16), row.Y + SS(14), row.Width - SS(32), row.Height - SS(28));
    default: return RectF(0, 0, 0, 0);
    }
    return RectF(right - w, row.Y + (row.Height - h) / 2, w, h);
}

static RectF SGearRect(const RectF& toggle) {
    const float s = SS(30);
    return RectF(toggle.X - SS(10) - s, toggle.Y + (toggle.Height - s) / 2, s, s);
}

// Segment / card / swatch `i` of a multi-part control
static RectF SPartRect(const SRow& r, const RectF& ctl, int i) {
    const int n = (int)(r.type == SRow::Swatches ? r.colors.size() : r.options.size());
    if (n <= 0) return RectF();
    if (r.type == SRow::Swatches) {
        const float d = SS(24), gap = SS(10);
        return RectF(ctl.X + (d + gap) * (float)i, ctl.Y, d, d);
    }
    const float gap = r.type == SRow::Cards ? SS(14) : SS(3);
    const float inset = r.type == SRow::Segmented ? SS(3) : 0.f;
    const float w = (ctl.Width - inset * 2 - gap * (float)(n - 1)) / (float)n;
    return RectF(ctl.X + inset + (w + gap) * (float)i, ctl.Y + inset, w, ctl.Height - inset * 2);
}

// ---- fonts -----------------------------------------------------------------
static void SRebuildFonts() {
    const wchar_t* icoFam = L"Segoe MDL2 Assets";
    { FontFamily ff(L"Segoe Fluent Icons"); if (ff.IsAvailable()) icoFam = L"Segoe Fluent Icons"; }
    const wchar_t* uiFam = L"Segoe UI";
    { FontFamily ff(L"Segoe UI Variable Text"); if (ff.IsAvailable()) uiFam = L"Segoe UI Variable Text"; }
    auto mk = [](const wchar_t* fam, float px, int style) {
        auto f = std::make_unique<Font>(fam, px, style, UnitPixel);
        if (!f->IsAvailable()) f = std::make_unique<Font>(L"Segoe UI", px, style, UnitPixel);
        return f;
    };
    g_sfBody    = mk(uiFam, SS(13), FontStyleRegular);
    g_sfSmall   = mk(uiFam, SS(12), FontStyleRegular);
    g_sfTitle   = mk(uiFam, SS(18), FontStyleBold);
    g_sfGroup   = mk(uiFam, SS(12), FontStyleBold);
    g_sfSide    = mk(uiFam, SS(13), FontStyleBold);
    g_sfIcon    = mk(icoFam, SS(16), FontStyleRegular);
    g_sfIconSm  = mk(icoFam, SS(12), FontStyleRegular);
    g_sfCaption = mk(icoFam, SS(10), FontStyleRegular);
    g_sfBadge   = mk(uiFam, SS(10), FontStyleBold);

    HFONT old = g_sEditFont;
    g_sEditFont = CreateFontW(-(int)std::round(SS(13)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (g_setEdit) {
        HDC dc = GetDC(g_setEdit);
        HGDIOBJ prev = SelectObject(dc, g_sEditFont);
        TEXTMETRICW tm = {};
        GetTextMetricsW(dc, &tm);
        g_sEditH = tm.tmHeight;
        SelectObject(dc, prev);
        ReleaseDC(g_setEdit, dc);
        SendMessageW(g_setEdit, WM_SETFONT, (WPARAM)g_sEditFont, TRUE);
        if (g_setSearch) SendMessageW(g_setSearch, WM_SETFONT, (WPARAM)g_sEditFont, TRUE);
    }
    if (old) DeleteObject(old);
}

// ---- view building ---------------------------------------------------------
static std::wstring SLower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}

// The search field is drawn by Lyra itself (text, selection, system caret) instead of being a
// Windows edit control: an edit control in the top band of a window without a system title bar
// is shown by DWM with a grey, see-through background.
static std::wstring s_query;
static bool s_searchActive = false, s_queryAllSelected = false;

static std::wstring SSearchText() { return TrimWs(s_query); }

static float SMaxScroll() { return std::max(0.f, g_sContentH - Geo().vpH); }

static void SClampScroll() {
    const float maxS = SMaxScroll();
    g_sScroll = std::max(0.f, std::min(g_sScroll, maxS));
    s_scrollTarget = std::max(0.f, std::min(s_scrollTarget, maxS));
}

// Scrolls to `y` smoothly (instantly with animations off).
static void SScrollTo(float y) {
    s_scrollTarget = std::max(0.f, std::min(y, SMaxScroll()));
    if (!g_prefs.animations) {
        g_sScroll = s_scrollTarget;
        s_scrollAnim = false;
        InvalidateRect(g_setHwnd, nullptr, FALSE);
        return;
    }
    if (std::abs(s_scrollTarget - g_sScroll) < 0.5f) return;
    s_scrollAnim = true;
    AnimRequest();
}

// Wheel notches in a row add up: each one moves the target on from where the last one is going.
static void SScrollBy(float d) {
    SScrollTo((s_scrollAnim ? s_scrollTarget : g_sScroll) + d);
}

bool SettingsAnimStep(float dt) {
    if (!s_scrollAnim || !g_setHwnd) { s_scrollAnim = false; return false; }
    const float diff = s_scrollTarget - g_sScroll;
    if (std::abs(diff) < 0.5f) {
        g_sScroll = s_scrollTarget;
        s_scrollAnim = false;
    } else {
        g_sScroll += diff * AnimFactor(dt, 70.f);
    }
    InvalidateRect(g_setHwnd, nullptr, FALSE);
    return s_scrollAnim;
}

static float RowHeight(const SRow& r) {
    switch (r.type) {
    case SRow::Info:     return SS(44);
    case SRow::Cards:    return SS(150);
    case SRow::Swatches: return SS(56);
    default:             return r.desc.empty() ? SS(48) : SS(62);
    }
}

static void RebuildView() {
    g_view.clear();
    g_groups.clear();
    const std::wstring q = SLower(SSearchText());
    float y = SS(4);
    auto pushHeader = [&](const std::wstring& t) {
        SView v; v.heading = t; v.y = y; v.h = SS(38); y += v.h; g_view.push_back(v);
    };
    auto pushRow = [&](SRow* r) {
        SView v; v.row = r; v.y = y; v.h = RowHeight(*r); y += v.h; g_view.push_back(v);
    };
    if (q.empty()) {
        if (g_sPage >= 0 && g_sPage < (int)g_pages.size())
            for (auto& r : g_pages[g_sPage].rows) {
                if (r.type == SRow::Header) pushHeader(r.label); else pushRow(&r);
            }
    } else {
        for (auto& p : g_pages) {
            bool any = false;
            for (auto& r : p.rows) {
                if (r.type == SRow::Header) continue;
                std::wstring hay = r.label + L" " + r.desc + L" " + r.right;
                for (auto& o : r.options) hay += L" " + o;
                if (SLower(hay).find(q) == std::wstring::npos) continue;
                if (!any) { pushHeader(p.name); any = true; }
                pushRow(&r);
            }
        }
    }
    int cur = -1;
    for (size_t i = 0; i < g_view.size(); ++i) {
        SView& v = g_view[i];
        if (!v.row || v.row->type == SRow::Header) { cur = -1; continue; }
        if (cur < 0) { cur = (int)g_groups.size(); g_groups.emplace_back(v.y, v.y + v.h); v.first = true; }
        else g_groups[cur].second = v.y + v.h;
        v.group = cur;
    }
    for (size_t i = 0; i < g_view.size(); ++i)
        if (g_view[i].group >= 0 && (i + 1 >= g_view.size() || g_view[i + 1].group != g_view[i].group))
            g_view[i].last = true;

    g_sContentH = y + SS(28);
    SClampScroll();
    if (g_setHwnd) InvalidateRect(g_setHwnd, nullptr, FALSE);
}

// ---- inline editing / search box -------------------------------------------
static void SPositionSearch() {
    if (!g_setSearch) return;
    SGeo G = Geo();
    RectF sp = SSearchRect(G);
    MoveWindow(g_setSearch, (int)(sp.X + SS(30)), (int)(sp.Y + (sp.Height - (float)g_sEditH) / 2),
               (int)std::max(10.f, sp.Width - SS(30) - SS(10)), g_sEditH, TRUE);
}

static void SBeginTextEdit(SRow* r, const RectF& ctl) {
    g_sEditRow = r;
    SetWindowTextW(g_setEdit, r->getStr().c_str());
    MoveWindow(g_setEdit, (int)(ctl.X + SS(10)), (int)(ctl.Y + (ctl.Height - (float)g_sEditH) / 2),
               (int)std::max(10.f, ctl.Width - SS(20)), g_sEditH, TRUE);
    ShowWindow(g_setEdit, SW_SHOW);
    SetFocus(g_setEdit);
    SendMessageW(g_setEdit, EM_SETSEL, 0, -1);
    InvalidateRect(g_setHwnd, nullptr, FALSE);
}

static void SCommitTextEdit() {
    if (!g_sEditRow) return;
    SRow* r = g_sEditRow;
    g_sEditRow = nullptr;
    wchar_t buf[1024] = {};
    GetWindowTextW(g_setEdit, buf, ARRAYSIZE(buf));
    ShowWindow(g_setEdit, SW_HIDE);
    if (r->setStr) r->setStr(buf);
    InvalidateRect(g_setHwnd, nullptr, FALSE);
}

void SettingsOpenPage(int i) {
    if (i < 0 || i >= (int)g_pages.size()) return;
    g_sPage = i;
    g_sScroll = 0.f; s_scrollAnim = false;
    s_query.clear();
    s_queryAllSelected = false;
    RebuildView();
}

// ---- search field ------------------------------------------------------------
static float SearchTextWidth(const std::wstring& text) {
    if (text.empty() || !g_setHwnd) return 0.f;
    HDC dc = GetDC(g_setHwnd);
    float w = 0.f;
    {
        Graphics g(dc);
        StringFormat fmt(StringFormat::GenericTypographic());
        fmt.SetFormatFlags(fmt.GetFormatFlags() | StringFormatFlagsMeasureTrailingSpaces | StringFormatFlagsNoWrap);
        RectF m;
        g.MeasureString(text.c_str(), -1, g_sfBody.get(), RectF(0, 0, 100000, 1000), &fmt, &m);
        w = m.Width;
    }
    ReleaseDC(g_setHwnd, dc);
    return w;
}

// Text start, visible width and scroll offset (long queries scroll left to keep the end visible)
static void SearchTextGeometry(float* x0, float* avail, float* offset) {
    const SGeo G = Geo();
    const RectF sp = SSearchRect(G);
    *x0 = sp.X + SS(30);
    *avail = sp.Width - SS(40);
    *offset = std::max(0.f, SearchTextWidth(s_query) - *avail);
}

static void SearchUpdateCaret() {
    if (!s_searchActive || !g_setHwnd || GetFocus() != g_setHwnd) return;
    float x0, avail, off;
    SearchTextGeometry(&x0, &avail, &off);
    const RectF sp = SSearchRect(Geo());
    const float h = SS(16);
    SetCaretPos((int)(x0 + SearchTextWidth(s_query) - off), (int)(sp.Y + (sp.Height - h) / 2));
}

static void SearchActivate() {
    if (!g_setHwnd) return;
    SCommitTextEdit();
    if (GetFocus() != g_setHwnd) SetFocus(g_setHwnd);
    if (!s_searchActive) {
        s_searchActive = true;
        CreateCaret(g_setHwnd, nullptr, std::max(1, (int)SS(1)), (int)SS(16));
        ShowCaret(g_setHwnd);
    }
    SearchUpdateCaret();
    InvalidateRect(g_setHwnd, nullptr, FALSE);
}

static void SearchDeactivate() {
    if (!s_searchActive) return;
    s_searchActive = false;
    s_queryAllSelected = false;
    DestroyCaret();
    if (g_setHwnd) InvalidateRect(g_setHwnd, nullptr, FALSE);
}

static void SearchChanged() {
    g_sScroll = 0.f; s_scrollAnim = false;
    RebuildView();
    SearchUpdateCaret();
}

static void SearchType(wchar_t ch) {
    if (s_queryAllSelected) { s_query.clear(); s_queryAllSelected = false; }
    if (ch == 8) {                                              // Backspace
        if (!s_query.empty()) s_query.pop_back();
    } else if (ch == 127) {                                     // Ctrl+Backspace: the last word
        while (!s_query.empty() && iswspace(s_query.back())) s_query.pop_back();
        while (!s_query.empty() && !iswspace(s_query.back())) s_query.pop_back();
    } else if (ch >= 32 && s_query.size() < 200) {
        s_query += ch;
    } else {
        return;
    }
    SearchChanged();
}

static void SearchPaste() {
    if (!OpenClipboard(g_setHwnd)) return;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* text = (const wchar_t*)GlobalLock(h)) {
            std::wstring t = text;
            GlobalUnlock(h);
            for (auto& c : t) if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
            if (s_queryAllSelected) { s_query.clear(); s_queryAllSelected = false; }
            s_query += t.substr(0, 200 - std::min<size_t>(200, s_query.size()));
            SearchChanged();
        }
    }
    CloseClipboard();
}


// Rebuilding the pages destroys the row whose setter is running, so it happens later (posted).
constexpr UINT WM_APP_SET_REBUILD = WM_APP + 20;
void SettingsLanguageChanged() {
    if (g_setHwnd) PostMessageW(g_setHwnd, WM_APP_SET_REBUILD, 0, 0);
}

static void RebuildForLanguage() {
    const int page = g_sPage;
    SCommitTextEdit();
    BuildPages();
    g_sPage = std::min(page, (int)g_pages.size() - 1);
    SetWindowTextW(g_setHwnd, Tr(L"Lyra Settings", L"Настройки Lyra"));
    RebuildView();
}

static LRESULT CALLBACK SEditProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN) {
        if (h == g_setEdit) {
            if (w == VK_RETURN) { SCommitTextEdit(); SetFocus(g_setHwnd); return 0; }
            if (w == VK_ESCAPE) {
                g_sEditRow = nullptr;
                ShowWindow(g_setEdit, SW_HIDE);
                SetFocus(g_setHwnd);
                InvalidateRect(g_setHwnd, nullptr, FALSE);
                return 0;
            }
        } else if (h == g_setSearch) {
            if (w == VK_ESCAPE) { SetWindowTextW(h, L""); SetFocus(g_setHwnd); return 0; }
            if (w == VK_RETURN) return 0;
        }
        if (w == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) { SendMessageW(h, EM_SETSEL, 0, -1); return 0; }
    }
    if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE || w == 1)) return 0;
    return CallWindowProcW(g_oldSEditProc, h, m, w, l);
}

// ---- drawing ---------------------------------------------------------------
static void SText(Graphics& g, const std::wstring& s, Font* f, const RectF& r, const Color& c,
                  StringAlignment h = StringAlignmentNear, StringAlignment v = StringAlignmentCenter) {
    StringFormat sf;
    sf.SetAlignment(h);
    sf.SetLineAlignment(v);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush b(c);
    g.DrawString(s.c_str(), -1, f, r, &sf, &b);
}

static void StrokeRound(Graphics& g, const Color& c, const RectF& r, float rad, float width) {
    GraphicsPath p;
    AddRound(p, r, rad);
    Pen pen(c, width);
    g.DrawPath(&pen, &p);
}

// Theme preview: a miniature window, split light/dark for "as in Windows".
static void DrawThemePreview(Graphics& g, const RectF& r, int which) {
    const Color lbg(232, 235, 240), lbar(211, 216, 223), lcard(255, 255, 255);
    const Color dbg(32, 34, 39), dbar(59, 62, 69), dcard(46, 48, 54);
    auto half = [&](const RectF& h, const Color& bg, const Color& bar, const Color& card) {
        SolidBrush b(bg);
        g.FillRectangle(&b, h);
        FillRound(g, bar, RectF(h.X + SS(10), h.Y + SS(10), h.Width - SS(20), SS(10)), SS(4));
        FillRound(g, card, RectF(h.X + SS(10), h.Y + SS(26), h.Width - SS(20), h.Height - SS(36)), SS(5));
    };
    GraphicsPath clip;
    AddRound(clip, r, SS(8));
    g.SetClip(&clip, CombineModeIntersect);
    if (which == 0) {
        half(RectF(r.X, r.Y, r.Width / 2, r.Height), lbg, lbar, lcard);
        half(RectF(r.X + r.Width / 2, r.Y, r.Width / 2, r.Height), dbg, dbar, dcard);
    } else if (which == 1) {
        half(r, lbg, lbar, lcard);
    } else {
        half(r, dbg, dbar, dcard);
    }
    g.ResetClip();
}

// Tab layout preview: compact (one row), separate tab bar, vertical tabs.
static void DrawLayoutPreview(Graphics& g, const RectF& r, int which, const SPal& P) {
    const Color frame = P.side, page = P.card, mark = P.off;
    const Color accent = g_th.accent;
    GraphicsPath clip;
    AddRound(clip, r, SS(8));
    g.SetClip(&clip, CombineModeIntersect);
    { SolidBrush b(frame); g.FillRectangle(&b, r); }
    const float pad = SS(8);
    if (which == 0) {
        const float rowH = SS(22);
        FillRound(g, mark, RectF(r.X + pad, r.Y + SS(8), SS(14), SS(6)), SS(3));
        FillRound(g, page, RectF(r.X + pad + SS(20), r.Y + SS(5), r.Width * 0.45f, SS(12)), SS(4));
        FillRound(g, mark, RectF(r.X + pad + SS(26) + r.Width * 0.45f, r.Y + SS(8), SS(18), SS(6)), SS(3));
        FillRound(g, mark, RectF(r.X + pad + SS(48) + r.Width * 0.45f, r.Y + SS(8), SS(18), SS(6)), SS(3));
        SolidBrush b(page); g.FillRectangle(&b, r.X, r.Y + rowH, r.Width, r.Height - rowH);
    } else if (which == 1) {
        FillRound(g, page, RectF(r.X + pad, r.Y + SS(4), SS(34), SS(12)), SS(4));
        FillRound(g, mark, RectF(r.X + pad + SS(40), r.Y + SS(7), SS(26), SS(6)), SS(3));
        FillRound(g, mark, RectF(r.X + pad + SS(72), r.Y + SS(7), SS(26), SS(6)), SS(3));
        FillRound(g, page, RectF(r.X + pad + SS(20), r.Y + SS(22), r.Width - pad * 2 - SS(20), SS(10)), SS(4));
        SolidBrush b(page); g.FillRectangle(&b, r.X, r.Y + SS(38), r.Width, r.Height - SS(38));
    } else {
        const float rowH = SS(20), sideW = r.Width * 0.32f;
        FillRound(g, page, RectF(r.X + sideW + SS(8), r.Y + SS(4), r.Width - sideW - SS(24), SS(12)), SS(4));
        FillRound(g, accent, RectF(r.X + SS(6), r.Y + rowH + SS(6), sideW - SS(12), SS(7)), SS(3));
        FillRound(g, mark, RectF(r.X + SS(6), r.Y + rowH + SS(18), sideW - SS(12), SS(7)), SS(3));
        FillRound(g, mark, RectF(r.X + SS(6), r.Y + rowH + SS(30), sideW - SS(12), SS(7)), SS(3));
        SolidBrush b(page); g.FillRectangle(&b, r.X + sideW, r.Y + rowH, r.Width - sideW, r.Height - rowH);
    }
    g.ResetClip();
}

static void DrawSettings(Graphics& g) {
    const SPal P = Pal();
    const Color accent = g_th.accent;
    const SGeo G = Geo();
    const std::wstring query = SSearchText();
    const bool searching = !query.empty();

    // With Mica the sidebar is left transparent: the backdrop shows through (macOS-like).
    { SolidBrush b(P.bg); g.FillRectangle(&b, s_sideTransparent ? G.sideW : 0.f, 0.f, G.W, G.H); }

    // Sidebar
    {
        SolidBrush b(P.side);
        if (!s_sideTransparent) g.FillRectangle(&b, 0.f, 0.f, G.sideW, G.H);
        Pen sep(P.border, 1.f);
        g.DrawLine(&sep, G.sideW - 0.5f, 0.f, G.sideW - 0.5f, G.H);
        SText(g, Tr(L"Lyra Settings", L"Настройки Lyra"), g_sfSide.get(),
              RectF(SS(18), SS(10), G.sideW - SS(36), SS(28)), P.text);

        RectF sp = SSearchRect(G);
        FillRound(g, P.field, sp, SS(8));
        const bool focused = s_searchActive;
        StrokeRound(g, focused ? accent : P.border, sp, SS(8), focused ? SS(1.5f) : 1.f);
        DrawGlyph(g, L"\uE721", RectF(sp.X + SS(6), sp.Y, SS(22), sp.Height), P.text2, g_sfIconSm.get());
        if (s_query.empty()) {
            if (!focused)
                SText(g, Tr(L"Search", L"Поиск"), g_sfBody.get(), RectF(sp.X + SS(30), sp.Y, sp.Width - SS(40), sp.Height), P.text2);
        } else {
            float x0, avail, off;
            SearchTextGeometry(&x0, &avail, &off);
            const RectF clip(x0, sp.Y, avail, sp.Height);
            StringFormat fmt(StringFormat::GenericTypographic());
            fmt.SetFormatFlags(fmt.GetFormatFlags() | StringFormatFlagsMeasureTrailingSpaces | StringFormatFlagsNoWrap);
            fmt.SetLineAlignment(StringAlignmentCenter);
            g.SetClip(clip);
            if (s_queryAllSelected && focused)
                FillRound(g, WithAlpha(accent, 90), RectF(x0 - off, sp.Y + SS(7), SearchTextWidth(s_query), sp.Height - SS(14)), SS(3));
            SolidBrush tb(P.text);
            g.DrawString(s_query.c_str(), -1, g_sfBody.get(), RectF(x0 - off, sp.Y, avail + off + SS(4), sp.Height), &fmt, &tb);
            g.ResetClip();
        }

        const std::vector<int> side = SidebarPages();
        const int selPage = searching ? -1 : SidebarPageOf(g_sPage);
        for (int s = 0; s < (int)side.size(); ++s) {
            const SPage& pg = g_pages[side[s]];
            RectF r = SSideRect(G, s);
            const bool sel = side[s] == selPage;
            if (sel) FillRound(g, accent, r, SS(7));
            else if (s == g_sHoverSide) FillRound(g, P.hover, r, SS(7));
            RectF ic(r.X + SS(7), r.Y + (r.Height - SS(22)) / 2, SS(22), SS(22));
            FillRound(g, pg.tint, ic, SS(6));
            DrawGlyph(g, pg.icon, ic, Color(255, 255, 255), g_sfIconSm.get());
            SText(g, pg.name, g_sfBody.get(), RectF(r.X + SS(38), r.Y, r.Width - SS(44), r.Height),
                  sel ? Color(255, 255, 255) : P.text);
        }
    }

    // Header: back arrow (detail pages), title, window buttons
    {
        float tx = G.colX;
        if (OnDetailPage() && !searching) {
            RectF br = SBackRect(G);
            if (g_sHoverBack) FillRound(g, P.hover, br, SS(6));
            DrawGlyph(g, L"\uE72B", br, P.text, g_sfIcon.get());
            tx = br.GetRight() + SS(6);
        }
        std::wstring title = searching ? std::wstring(Tr(L"Search results", L"Результаты поиска"))
                                       : (g_sPage < (int)g_pages.size() ? g_pages[g_sPage].name : std::wstring());
        SText(g, title, g_sfTitle.get(), RectF(tx, 0, std::max(SS(80), SCapRect(G, 0).X - tx - SS(8)), G.headerH), P.text);

        const bool zoomed = IsZoomed(g_setHwnd) != FALSE;
        const wchar_t* glyphs[3] = { L"\uE921", zoomed ? L"\uE923" : L"\uE922", L"\uE8BB" };
        for (int i = 0; i < 3; ++i) {
            RectF r = SCapRect(G, i);
            const bool h = g_sCapHover == i, p = h && g_sCapDown == i;
            if (h || p) {
                SolidBrush b(i == 2 ? (p ? g_th.closePressed : g_th.closeHover) : (p ? P.pressed : P.hover));
                g.FillRectangle(&b, r);
            }
            DrawGlyph(g, glyphs[i], r, (i == 2 && h) ? Color(255, 255, 255) : P.text, g_sfCaption.get());
        }
    }

    // Content
    g.SetClip(RectF(G.sideW, G.vpY, std::max(0.f, G.W - G.sideW), G.vpH));

    for (auto& gr : g_groups) {
        RectF c(G.colX, G.vpY + gr.first - g_sScroll, G.colW, gr.second - gr.first);
        if (c.GetBottom() < G.vpY || c.Y > G.H) continue;
        FillRound(g, P.card, c, SS(10));
        StrokeRound(g, P.border, c, SS(10), 1.f);
    }

    for (int i = 0; i < (int)g_view.size(); ++i) {
        const SView& v = g_view[i];
        RectF r = SRowRect(G, v);
        if (r.GetBottom() < G.vpY || r.Y > G.H) continue;

        if (!v.row || v.row->type == SRow::Header) {
            SText(g, v.row ? v.row->label : v.heading, g_sfGroup.get(),
                  RectF(r.X + SS(4), r.Y + SS(14), r.Width, r.Height - SS(20)), P.text2);
            continue;
        }
        SRow& row = *v.row;
        if (v.group >= 0 && !v.first) {
            Pen sp(P.border, 1.f);
            g.DrawLine(&sp, r.X + SS(16), r.Y, r.GetRight() - SS(16), r.Y);
        }

        const RectF ctl = SCtlRect(row, r);
        const bool hv = (g_sHoverRow == i && g_sHoverCtl);
        const int part = (g_sHoverRow == i) ? g_sHoverPart : -1;
        const float lx = r.X + SS(16);

        // Label (+ description)
        if (row.type != SRow::Cards) {
            float labelRight;
            if (row.type == SRow::Info) labelRight = r.X + r.Width * 0.4f;
            else if (row.type == SRow::Slider) labelRight = ctl.X - SS(70);
            else if (row.type == SRow::ToggleGear) labelRight = SGearRect(ctl).X - SS(12);
            else labelRight = ctl.X - SS(16);
            const float lw = std::max(SS(40), labelRight - lx);
            if (row.desc.empty()) {
                SText(g, row.label, g_sfBody.get(), RectF(lx, r.Y, lw, r.Height), P.text);
            } else {
                SText(g, row.label, g_sfBody.get(), RectF(lx, r.Y + SS(10), lw, SS(22)), P.text);
                SText(g, row.desc, g_sfSmall.get(), RectF(lx, r.Y + SS(31), lw, SS(20)), P.text2);
            }
        }

        switch (row.type) {
        case SRow::Toggle:
        case SRow::ToggleGear: {
            const bool on = row.getInt() != 0;
            FillRound(g, on ? accent : P.off, ctl, ctl.Height / 2);
            const float kd = ctl.Height - SS(6);
            const float kx = on ? ctl.GetRight() - SS(3) - kd : ctl.X + SS(3);
            SolidBrush kb(Color(255, 255, 255));
            g.FillEllipse(&kb, kx, ctl.Y + SS(3), kd, kd);
            if (row.type == SRow::ToggleGear) {
                RectF gr = SGearRect(ctl);
                if (part == 1) FillRound(g, P.hover, gr, SS(6));
                DrawGlyph(g, L"\uE713", gr, P.text2, g_sfIconSm.get());
            }
            break;
        }
        case SRow::Choice: {
            const int cur = row.getInt();
            std::wstring val = (cur >= 0 && cur < (int)row.options.size()) ? row.options[cur] : std::wstring();
            FillRound(g, P.field, ctl, SS(7));
            StrokeRound(g, hv ? accent : P.border, ctl, SS(7), 1.f);
            SText(g, val, g_sfBody.get(), RectF(ctl.X + SS(10), ctl.Y, ctl.Width - SS(36), ctl.Height), P.text);
            DrawGlyph(g, L"\uE70D", RectF(ctl.GetRight() - SS(28), ctl.Y, SS(22), ctl.Height), P.text2, g_sfIconSm.get());
            break;
        }
        case SRow::Segmented: {
            FillRound(g, P.side, ctl, SS(8));
            const int cur = row.getInt();
            for (int k = 0; k < (int)row.options.size(); ++k) {
                RectF s = SPartRect(row, ctl, k);
                if (k == cur) {
                    FillRound(g, P.card, s, SS(6));
                    StrokeRound(g, P.border, s, SS(6), 1.f);
                } else if (k == part) {
                    FillRound(g, P.hover, s, SS(6));
                }
                SText(g, row.options[k], k == cur ? g_sfGroup.get() : g_sfBody.get(), s, P.text, StringAlignmentCenter);
            }
            break;
        }
        case SRow::Cards: {
            const int cur = row.getInt();
            for (int k = 0; k < (int)row.options.size(); ++k) {
                RectF cell = SPartRect(row, ctl, k);
                RectF prev(cell.X, cell.Y, cell.Width, cell.Height - SS(26));
                if (row.cardKind == 0) DrawThemePreview(g, prev, k);
                else DrawLayoutPreview(g, prev, k, P);
                if (k == cur) {
                    RectF ring = prev;
                    ring.Inflate(SS(2), SS(2));
                    StrokeRound(g, accent, ring, SS(10), SS(2));
                } else {
                    StrokeRound(g, k == part ? P.text2 : P.border, prev, SS(8), 1.f);
                }
                SText(g, row.options[k], k == cur ? g_sfGroup.get() : g_sfBody.get(),
                      RectF(cell.X, prev.GetBottom() + SS(4), cell.Width, SS(22)), P.text, StringAlignmentCenter);
            }
            break;
        }
        case SRow::Swatches: {
            const int cur = row.getInt();
            for (int k = 0; k < (int)row.colors.size(); ++k) {
                RectF s = SPartRect(row, ctl, k);
                if (k == cur) {
                    RectF ring = s;
                    ring.Inflate(SS(3), SS(3));
                    Pen pen(row.colors[k], SS(2));
                    g.DrawEllipse(&pen, ring);
                } else if (k == part) {
                    RectF ring = s;
                    ring.Inflate(SS(3), SS(3));
                    Pen pen(P.border, SS(2));
                    g.DrawEllipse(&pen, ring);
                }
                SolidBrush b(row.colors[k]);
                g.FillEllipse(&b, s);
                if (k == 0) DrawGlyph(g, L"W", s, Color(255, 255, 255), g_sfBadge.get());   // Windows accent
            }
            // Name of the selected colour, left of the circles
            if (cur >= 0 && cur < (int)row.options.size())
                SText(g, row.options[cur], g_sfSmall.get(), RectF(ctl.X - SS(170), r.Y, SS(156), r.Height),
                      P.text2, StringAlignmentFar);
            break;
        }
        case SRow::Slider: {
            const int val = std::max(row.minV, std::min(row.maxV, row.getInt()));
            const float t = (row.maxV > row.minV) ? (float)(val - row.minV) / (float)(row.maxV - row.minV) : 0.f;
            const float cy = ctl.Y + ctl.Height / 2;
            FillRound(g, P.off, RectF(ctl.X, cy - SS(2), ctl.Width, SS(4)), SS(2));
            if (t > 0.f) FillRound(g, accent, RectF(ctl.X, cy - SS(2), ctl.Width * t, SS(4)), SS(2));
            const float kd = SS(16);
            const float kx = ctl.X + ctl.Width * t - kd / 2;
            SolidBrush kb(Color(255, 255, 255));
            g.FillEllipse(&kb, kx, cy - kd / 2, kd, kd);
            Pen kp(Color(70, 0, 0, 0), 1.f);
            g.DrawEllipse(&kp, kx, cy - kd / 2, kd, kd);
            std::wstring txt = row.fmt ? row.fmt(val) : std::to_wstring(val);
            SText(g, txt, g_sfBody.get(), RectF(ctl.X - SS(66), r.Y, SS(56), r.Height), P.text2, StringAlignmentFar);
            break;
        }
        case SRow::Text: {
            const bool editing = (g_sEditRow == &row);
            FillRound(g, P.field, ctl, SS(7));
            StrokeRound(g, editing ? accent : (hv ? P.text2 : P.border), ctl, SS(7), editing ? SS(1.5f) : 1.f);
            if (!editing)
                SText(g, row.getStr(), g_sfBody.get(), RectF(ctl.X + SS(10), ctl.Y, ctl.Width - SS(20), ctl.Height), P.text);
            break;
        }
        case SRow::Button: {
            FillRound(g, hv ? Color(P.field.GetA(), P.field.GetR(), P.field.GetG(), P.field.GetB()) : P.field, ctl, SS(7));
            if (hv) FillRound(g, P.hover, ctl, SS(7));
            StrokeRound(g, P.border, ctl, SS(7), 1.f);
            SText(g, row.right, g_sfBody.get(), ctl, P.text, StringAlignmentCenter);
            break;
        }
        case SRow::Info: {
            if (row.kbd) {
                StringFormat fmt(StringFormat::GenericTypographic());
                RectF m;
                g.MeasureString(row.right.c_str(), -1, g_sfSmall.get(), RectF(0, 0, 10000, 1000), &fmt, &m);
                const float w = m.Width + SS(20), h = SS(26);
                RectF pill(r.GetRight() - SS(16) - w, r.Y + (r.Height - h) / 2, w, h);
                FillRound(g, P.side, pill, SS(6));
                StrokeRound(g, P.border, pill, SS(6), 1.f);
                SText(g, row.right, g_sfSmall.get(), pill, P.text, StringAlignmentCenter);
            } else {
                SText(g, row.right, g_sfBody.get(),
                      RectF(r.X + r.Width * 0.4f, r.Y, r.Width * 0.6f - SS(16), r.Height), P.text2, StringAlignmentFar);
            }
            break;
        }
        default: break;
        }
    }

    if (g_view.empty())
        SText(g, Tr(L"No settings found", L"Ничего не найдено"), g_sfBody.get(),
              RectF(G.sideW, G.vpY + SS(40), G.W - G.sideW, SS(40)), P.text2, StringAlignmentCenter);

    // Scrollbar
    const float maxS = g_sContentH - G.vpH;
    if (maxS > 1.f) {
        const float th = std::max(SS(36), G.vpH * G.vpH / g_sContentH);
        const float ty = G.vpY + (G.vpH - th) * (g_sScroll / maxS);
        FillRound(g, WithAlpha(P.text2, 110), RectF(G.W - SS(9), ty, SS(4), th), SS(2));
    }
    g.ResetClip();
}

// ---- hit testing helpers ---------------------------------------------------
static int SSideHit(const SGeo& G, float x, float y) {
    const int n = (int)SidebarPages().size();
    for (int s = 0; s < n; ++s)
        if (SSideRect(G, s).Contains(x, y)) return s;
    return -1;
}

static int SCapHit(const SGeo& G, float x, float y) {
    for (int i = 0; i < 3; ++i) if (SCapRect(G, i).Contains(x, y)) return i;
    return -1;
}

static int SRowAt(const SGeo& G, float x, float y) {
    if (y < G.vpY || x < G.sideW) return -1;
    for (int i = 0; i < (int)g_view.size(); ++i)
        if (SRowRect(G, g_view[i]).Contains(x, y)) return i;
    return -1;
}

// Control hover: whether (x, y) is over the row's control, and which part of it.
static bool SOverControl(const SGeo& G, int vi, float x, float y, int* partOut) {
    *partOut = -1;
    if (vi < 0 || !g_view[vi].row) return false;
    SRow& r = *g_view[vi].row;
    RectF row = SRowRect(G, g_view[vi]);
    RectF ctl = SCtlRect(r, row);
    if (ctl.Width <= 0) return false;
    switch (r.type) {
    case SRow::Slider: {
        RectF hit(ctl.X - SS(10), row.Y, ctl.Width + SS(20), row.Height);
        return hit.Contains(x, y);
    }
    case SRow::ToggleGear:
        if (SGearRect(ctl).Contains(x, y)) { *partOut = 1; return true; }
        if (ctl.Contains(x, y)) { *partOut = 0; return true; }
        return false;
    case SRow::Segmented:
    case SRow::Cards:
    case SRow::Swatches: {
        const int n = (int)(r.type == SRow::Swatches ? r.colors.size() : r.options.size());
        for (int k = 0; k < n; ++k) {
            RectF p = SPartRect(r, ctl, k);
            if (r.type == SRow::Swatches) p.Inflate(SS(4), SS(4));
            if (p.Contains(x, y)) { *partOut = k; return true; }
        }
        return false;
    }
    default:
        return ctl.Contains(x, y);
    }
}

static void SSetSliderFromX(SRow* row, const RectF& ctl, float x) {
    if (ctl.Width <= 0 || row->step <= 0) return;
    const float t = std::max(0.f, std::min(1.f, (x - ctl.X) / ctl.Width));
    const int range = row->maxV - row->minV;
    int v = row->minV + (int)std::lround(t * (float)range / (float)row->step) * row->step;
    v = std::max(row->minV, std::min(row->maxV, v));
    if (v != row->getInt()) row->setInt(v);
}

static void SChoiceMenu(SRow* r, const RectF& ctl) {
    HMENU m = CreatePopupMenu();
    const int cur = r->getInt();
    for (int i = 0; i < (int)r->options.size(); ++i)
        AppendMenuW(m, MF_STRING | (i == cur ? MF_CHECKED : 0), 1000 + i, r->options[i].c_str());
    POINT pt = { (LONG)ctl.X, (LONG)ctl.GetBottom() };
    ClientToScreen(g_setHwnd, &pt);
    const int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY,
                                        pt.x, pt.y, 0, g_setHwnd, nullptr);
    DestroyMenu(m);
    if (cmd >= 1000) r->setInt(cmd - 1000);
}

void SettingsThemeChanged() {
    if (!g_setHwnd) return;
    if (g_fieldBrush) DeleteObject(g_fieldBrush);
    g_fieldBrush = CreateSolidBrush(ToColorRef(Pal().field));
    ApplyDwm(g_setHwnd);
    UpdateSettingsBackdrop();
    // The Windows accent swatch shows the current Windows colour
    for (auto& p : g_pages)
        for (auto& r : p.rows)
            if (r.type == SRow::Swatches && !r.colors.empty()) r.colors[0] = WindowsAccentColor();
    InvalidateRect(g_setHwnd, nullptr, FALSE);
    if (g_setSearch) InvalidateRect(g_setSearch, nullptr, TRUE);
    if (g_setEdit) InvalidateRect(g_setEdit, nullptr, TRUE);
}

static void PaintSettings(HDC hdc, const RECT& cr) {
    s_sideTransparent = MicaActive();
    // While the page scrolls smoothly: fast blending (a full paint over Mica took ~67 ms with gamma-
    // correct blending, ~4 ms without); the still picture at the end is painted in full quality.
    PaintBuffered(hdc, cr, !s_sideTransparent, [](Graphics& g) { DrawSettings(g); }, s_scrollAnim);
}

static void UpdateSettingsBackdrop() {
    if (!g_setHwnd) return;
    MARGINS m = { (int)std::ceil(SS(240)), 0, 0, 0 };     // the sidebar
    ApplyBackdrop(g_setHwnd, 2 /* Mica */, m);
}

static void SGoBack() {
    if (OnDetailPage()) SettingsOpenPage(g_pages[g_sPage].parent);
}

// ---- window procedure ------------------------------------------------------
static LRESULT CALLBACK SettingsWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_setHwnd = hWnd;
        g_sDpi = GetDpiForWindow(hWnd);
        HINSTANCE hi = (HINSTANCE)GetWindowLongPtrW(hWnd, GWLP_HINSTANCE);
        g_setEdit = CreateOpaqueEdit(hWnd, SET_ID_EDIT, hi);
        g_oldSEditProc = (WNDPROC)SetWindowLongPtrW(g_setEdit, GWLP_WNDPROC, (LONG_PTR)SEditProc);
        s_query.clear();
        s_searchActive = s_queryAllSelected = false;
        SRebuildFonts();
        BuildPages();
        SettingsThemeChanged();
        g_sPage = 0; g_sScroll = 0.f; s_scrollAnim = false; g_sEditRow = nullptr;
        g_sHoverSide = g_sHoverRow = g_sHoverPart = g_sDragRow = g_sCapHover = g_sCapDown = -1;
        RebuildView();
        SPositionSearch();
        return 0;
    }
    case WM_NCCALCSIZE:
        if (wParam) return FrameCalcSize(hWnd, wParam, lParam);
        break;

    case WM_APP_SET_REBUILD:
        RebuildForLanguage();
        return 0;

    case WM_NCHITTEST: {
        LRESULT def = DefWindowProcW(hWnd, msg, wParam, lParam);
        if (def != HTCLIENT) return def;
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hWnd, &pt);
        if (LRESULT top = FrameTopResizeHit(hWnd, pt, SS(5), SS(16))) return top;
        const SGeo G = Geo();
        const float x = (float)pt.x, y = (float)pt.y;
        switch (SCapHit(G, x, y)) {
        case 0: return HTMINBUTTON;
        case 1: return HTMAXBUTTON;
        case 2: return HTCLOSE;
        default: break;
        }
        const bool overBack = OnDetailPage() && SBackRect(G).Contains(x, y);
        if ((x >= G.sideW && y < G.headerH && !overBack) || (x < G.sideW && y < SS(40))) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_NCMOUSEMOVE: {
        int h = -1;
        if (IsCaptionCode(wParam)) {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hWnd, &pt);
            h = SCapHit(Geo(), (float)pt.x, (float)pt.y);
        }
        if (h != g_sCapHover) { g_sCapHover = h; InvalidateRect(hWnd, nullptr, FALSE); }
        if (!g_sTrackingNc) {
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE | TME_NONCLIENT, hWnd, 0 };
            TrackMouseEvent(&t);
            g_sTrackingNc = true;
        }
        if (IsCaptionCode(wParam)) return 0;        // no ghost system buttons (see MainWindow.cpp)
        break;
    }
    case WM_NCMOUSELEAVE:
        g_sTrackingNc = false;
        if (g_sCapHover >= 0 || g_sCapDown >= 0) {
            g_sCapHover = g_sCapDown = -1;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        break;
    case WM_NCLBUTTONDOWN:
        if (IsCaptionCode(wParam)) {
            g_sCapDown = g_sCapHover;
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_NCLBUTTONUP:
        if (IsCaptionCode(wParam)) {
            const int down = g_sCapDown;
            g_sCapDown = -1;
            InvalidateRect(hWnd, nullptr, FALSE);
            if (down >= 0 && down == g_sCapHover) {
                if (down == 0) SendMessageW(hWnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
                else if (down == 1) SendMessageW(hWnd, WM_SYSCOMMAND, IsZoomed(hWnd) ? SC_RESTORE : SC_MAXIMIZE, 0);
                else SendMessageW(hWnd, WM_SYSCOMMAND, SC_CLOSE, 0);
            }
            return 0;
        }
        break;
    case WM_NCLBUTTONDBLCLK:
    case WM_NCRBUTTONDOWN:
    case WM_NCRBUTTONUP:
        if (IsCaptionCode(wParam)) return 0;
        break;
    case WM_NCACTIVATE:
        return DefWindowProcW(hWnd, msg, wParam, -1);

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = (LONG)SS(760);
        mmi->ptMinTrackSize.y = (LONG)SS(520);
        return 0;
    }
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            SCommitTextEdit();
            SPositionSearch();
            SClampScroll();
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;

    case WM_DPICHANGED: {
        g_sDpi = HIWORD(wParam);
        SRebuildFonts();
        UpdateSettingsBackdrop();
        const RECT* r = (const RECT*)lParam;
        SetWindowPos(hWnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        RebuildView();
        SPositionSearch();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT cr;
        GetClientRect(hWnd, &cr);
        PaintSettings(hdc, cr);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        const SPal P = Pal();
        SetTextColor(hdc, ToColorRef(P.text));
        SetBkColor(hdc, ToColorRef(P.field));
        // The stock DC brush with the exact field colour: the edit's background then matches the
        // field drawn around it in every theme.
        SetDCBrushColor(hdc, ToColorRef(P.field));
        return (LRESULT)GetStockObject(DC_BRUSH);
    }
    case WM_COMMAND: {
        HWND from = (HWND)lParam;
        WORD code = HIWORD(wParam);
        if (from == g_setEdit && code == EN_KILLFOCUS) {
            SCommitTextEdit();
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        const float x = (float)GET_X_LPARAM(lParam), y = (float)GET_Y_LPARAM(lParam);
        const SGeo G = Geo();
        if (g_sDragRow >= 0 && g_sDragRow < (int)g_view.size() && (wParam & MK_LBUTTON)) {
            SRow* r = g_view[g_sDragRow].row;
            if (r) SSetSliderFromX(r, SCtlRect(*r, SRowRect(G, g_view[g_sDragRow])), x);
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        const int si = SSideHit(G, x, y);
        const int vi = SRowAt(G, x, y);
        int part = -1;
        const bool ctl = SOverControl(G, vi, x, y, &part);
        const bool back = OnDetailPage() && SBackRect(G).Contains(x, y);
        if (si != g_sHoverSide || vi != g_sHoverRow || ctl != g_sHoverCtl || part != g_sHoverPart || back != g_sHoverBack ||
            g_sCapHover >= 0) {
            g_sHoverSide = si; g_sHoverRow = vi; g_sHoverCtl = ctl; g_sHoverPart = part; g_sHoverBack = back;
            g_sCapHover = -1;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        if (!g_sTracking) {
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&t);
            g_sTracking = true;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_sTracking = false;
        g_sHoverSide = g_sHoverRow = g_sHoverPart = -1;
        g_sHoverCtl = g_sHoverBack = false;
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && (g_sHoverSide >= 0 || g_sHoverCtl || g_sHoverBack)) {
            SetCursor(LoadCursor(nullptr, IDC_HAND));
            return TRUE;
        }
        break;

    case WM_LBUTTONDOWN: {
        const float x = (float)GET_X_LPARAM(lParam), y = (float)GET_Y_LPARAM(lParam);
        const SGeo G = Geo();
        SCommitTextEdit();
        if (SSearchRect(G).Contains(x, y)) { SearchActivate(); return 0; }
        SearchDeactivate();
        const int si = SSideHit(G, x, y);
        if (si >= 0) { SettingsOpenPage(SidebarPages()[si]); SetFocus(hWnd); return 0; }
        if (OnDetailPage() && SBackRect(G).Contains(x, y)) { SGoBack(); SetFocus(hWnd); return 0; }
        const int vi = SRowAt(G, x, y);
        if (vi >= 0 && g_view[vi].row) {
            SRow* r = g_view[vi].row;
            RectF row = SRowRect(G, g_view[vi]);
            RectF ctl = SCtlRect(*r, row);
            int part = -1;
            const bool over = SOverControl(G, vi, x, y, &part);
            switch (r->type) {
            case SRow::Toggle:     r->setInt(r->getInt() ? 0 : 1); break;
            case SRow::ToggleGear:
                if (part == 1) { if (r->action) r->action(); }
                else r->setInt(r->getInt() ? 0 : 1);
                break;
            case SRow::Choice:     if (ctl.Contains(x, y)) SChoiceMenu(r, ctl); break;
            case SRow::Segmented:
            case SRow::Cards:
            case SRow::Swatches:   if (over && part >= 0 && part != r->getInt()) r->setInt(part); break;
            case SRow::Slider:
                if (over) {
                    g_sDragRow = vi;
                    SetCapture(hWnd);
                    SSetSliderFromX(r, ctl, x);
                }
                break;
            case SRow::Text:       if (ctl.Contains(x, y)) SBeginTextEdit(r, ctl); break;
            case SRow::Button:     if (ctl.Contains(x, y) && r->action) r->action(); break;
            default: break;
            }
            if (g_setHwnd) InvalidateRect(hWnd, nullptr, FALSE);
        }
        if (g_setHwnd && !g_sEditRow) SetFocus(hWnd);
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_sDragRow >= 0) { g_sDragRow = -1; if (GetCapture() == hWnd) ReleaseCapture(); }
        return 0;

    case WM_XBUTTONUP:                                   // mouse "back" button
        if (GET_XBUTTON_WPARAM(wParam) == XBUTTON1) { SGoBack(); return TRUE; }
        break;

    case WM_CAPTURECHANGED:
        g_sDragRow = -1;
        return 0;

    case WM_MOUSEWHEEL: {
        SCommitTextEdit();
        const short delta = GET_WHEEL_DELTA_WPARAM(wParam);
        SScrollBy(-(float)delta / 120.f * SS(64));
        return 0;
    }
    case WM_KEYDOWN: {
        const SGeo G = Geo();
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (s_searchActive) {                        // keys for the search field
            if (wParam == VK_ESCAPE) {
                if (!s_query.empty()) { s_query.clear(); s_queryAllSelected = false; SearchChanged(); }
                else SearchDeactivate();
                return 0;
            }
            if (ctrl && wParam == 'A') { s_queryAllSelected = !s_query.empty(); InvalidateRect(hWnd, nullptr, FALSE); return 0; }
            if (ctrl && wParam == 'V') { SearchPaste(); return 0; }
            if (wParam == VK_DELETE && s_queryAllSelected) { s_query.clear(); s_queryAllSelected = false; SearchChanged(); return 0; }
            if (wParam == VK_BACK || wParam == VK_RETURN) return 0;   // handled as characters
        }
        if (wParam == VK_ESCAPE) {
            if (!SSearchText().empty()) { s_query.clear(); SearchChanged(); }
            else if (OnDetailPage()) SGoBack();
            else DestroyWindow(hWnd);
            return 0;
        }
        if (wParam == VK_BACK && OnDetailPage()) { SGoBack(); return 0; }
        if (ctrl && wParam == 'F') { SearchActivate(); return 0; }
        switch (wParam) {
        case VK_UP:    SScrollBy(-SS(48)); return 0;
        case VK_DOWN:  SScrollBy(SS(48)); return 0;
        case VK_PRIOR: SScrollBy(-G.vpH * 0.9f); return 0;
        case VK_NEXT:  SScrollBy(G.vpH * 0.9f); return 0;
        case VK_HOME:  SScrollTo(0.f); return 0;
        case VK_END:   SScrollTo(g_sContentH); return 0;
        }
        break;
    }
    case WM_CHAR:
        if (s_searchActive) { SearchType((wchar_t)wParam); return 0; }
        break;
    case WM_SETFOCUS:
        if (s_searchActive) {
            CreateCaret(hWnd, nullptr, std::max(1, (int)SS(1)), (int)SS(16));
            ShowCaret(hWnd);
            SearchUpdateCaret();
        }
        return 0;
    case WM_KILLFOCUS:
        if (s_searchActive) DestroyCaret();
        return 0;

    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        g_sEditRow = nullptr;
        g_view.clear();
        g_groups.clear();
        g_pages.clear();
        if (g_fieldBrush) { DeleteObject(g_fieldBrush); g_fieldBrush = nullptr; }
        if (g_sEditFont)  { DeleteObject(g_sEditFont);  g_sEditFont = nullptr; }
        g_sfBody.reset(); g_sfSmall.reset(); g_sfTitle.reset(); g_sfGroup.reset(); g_sfSide.reset();
        g_sfIcon.reset(); g_sfIconSm.reset(); g_sfCaption.reset(); g_sfBadge.reset();
        g_setEdit = nullptr;
        s_searchActive = false;
        g_setHwnd = nullptr;
        return 0;                                  // no PostQuitMessage: this is not the main window
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void ShowSettingsWindow() {
    if (g_setHwnd) {
        if (IsIconic(g_setHwnd)) ShowWindow(g_setHwnd, SW_RESTORE);
        SetForegroundWindow(g_setHwnd);
        return;
    }
    HINSTANCE hi = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = SettingsWndProc;
        wc.hInstance = hi;
        wc.lpszClassName = kSetClass;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        if (!RegisterClassExW(&wc)) return;
        registered = true;
    }
    const int w = MulDiv(1000, (int)g_dpi, 96), h = MulDiv(700, (int)g_dpi, 96);
    RECT pr = {};
    GetWindowRect(g_hWnd, &pr);
    const int x = pr.left + ((pr.right - pr.left) - w) / 2;
    const int y = pr.top + ((pr.bottom - pr.top) - h) / 2;
    CreateWindowExW(0, kSetClass, Tr(L"Lyra Settings", L"Настройки Lyra"), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                    x, y, w, h, g_hWnd, nullptr, hi, nullptr);
    if (g_setHwnd) {
        FrameApply(g_setHwnd);
        ShowWindow(g_setHwnd, SW_SHOW);
        UpdateWindow(g_setHwnd);
        SetFocus(g_setHwnd);
    }
}
