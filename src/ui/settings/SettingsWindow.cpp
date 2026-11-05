// Lyra — окно настроек: отрисовка, прокрутка, мышь и клавиатура.
#include "ui/settings/SettingsWindow.h"
#include "core/StringUtils.h"
#include "ui/Draw.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/settings/SettingsModel.h"
#include "ui/settings/SettingsPages.h"

static COLORREF SFieldColor();
static Color WithAlpha(const Color& c, int a);
static Color SOffColor();
static float SS(float v);
static SGeo Geo();
static RectF SSideRect(const SGeo& G, int i);
static RectF SSearchRect(const SGeo& G);
static RectF SRowRect(const SGeo& G, const SView& v);
static RectF SCtlRect(const SRow& r, const RectF& row);
static void SRebuildFonts();
static std::wstring SLower(std::wstring s);
static std::wstring SSearchText();
static void SClampScroll();
static void SScrollBy(float d);
static void RebuildView();
static void SPositionSearch();
static void ShowSearch();
static void SBeginTextEdit(SRow* r, const RectF& ctl);
static void SCommitTextEdit();
static void SSelectPage(int i);
static LRESULT CALLBACK SEditProc(HWND h, UINT m, WPARAM w, LPARAM l);
static void SText(Graphics& g, const std::wstring& s, Font* f, const RectF& r, const Color& c, StringAlignment h = StringAlignmentNear, StringAlignment v = StringAlignmentCenter);
static void DrawSettings(Graphics& g);
static int SSideHit(const SGeo& G, float x, float y);
static int SRowAt(const SGeo& G, float x, float y);
static bool SOverControl(const SGeo& G, int vi, float x, float y);
static void SSetSliderFromX(SRow* row, const RectF& ctl, float x);
static void SChoiceMenu(SRow* r, const RectF& ctl);
static LRESULT CALLBACK SettingsWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

HWND g_setHwnd = nullptr;           // settings window

static COLORREF SFieldColor() { return g_dark ? RGB(30, 30, 32) : RGB(236, 236, 238); }

static Color WithAlpha(const Color& c, int a) { return Color((BYTE)a, c.GetR(), c.GetG(), c.GetB()); }

static Color SOffColor() { return g_dark ? Color(99, 99, 104) : Color(196, 196, 201); }

static HWND   g_setSearch = nullptr, g_setEdit = nullptr;

static HBRUSH g_fieldBrush = nullptr;

static HFONT  g_sEditFont = nullptr;

static int    g_sEditH = 18;

static UINT   g_sDpi = 96;

static WNDPROC g_oldSEditProc = nullptr;

static std::unique_ptr<Font> g_sfBody, g_sfSmall, g_sfTitle, g_sfHead, g_sfIcon, g_sfIconSm;

std::vector<SPage> g_pages;

static std::vector<SView> g_view;

static std::vector<std::pair<float, float>> g_groups;

static int   g_sPage = 0;

static float g_sScroll = 0.f, g_sContentH = 0.f;

static int   g_sHoverSide = -1, g_sHoverRow = -1, g_sDragRow = -1;

static bool  g_sHoverCtl = false, g_sHoverPill = false, g_sTracking = false;

static SRow* g_sEditRow = nullptr;

static float SS(float v) { return v * (float)g_sDpi / 96.f; }

static SGeo Geo() {
    RECT cr = {};
    GetClientRect(g_setHwnd, &cr);
    SGeo g;
    g.W = (float)cr.right; g.H = (float)cr.bottom;
    g.sideW = SS(232); g.headerH = SS(72);
    g.vpY = g.headerH; g.vpH = std::max(0.f, g.H - g.headerH);
    float vpW = std::max(0.f, g.W - g.sideW);
    g.colW = std::min(SS(780), std::max(0.f, vpW - SS(64)));
    g.colX = g.sideW + (vpW - g.colW) / 2;
    return g;
}

static RectF SSideRect(const SGeo& G, int i) {
    return RectF(SS(12), SS(80) + SS(42) * (float)i, G.sideW - SS(24), SS(38));
}

static RectF SSearchRect(const SGeo& G) {
    return RectF(G.colX + G.colW - SS(280), (G.headerH - SS(34)) / 2, SS(280), SS(34));
}

static RectF SRowRect(const SGeo& G, const SView& v) {
    return RectF(G.colX, G.vpY + v.y - g_sScroll, G.colW, v.h);
}

static RectF SCtlRect(const SRow& r, const RectF& row) {
    const float right = row.GetRight() - SS(20);
    float w = 0, h = SS(32);
    switch (r.type) {
    case SRow::Toggle: w = SS(44); h = SS(24); break;
    case SRow::Choice: w = std::min(SS(190), row.Width * 0.4f); break;
    case SRow::Slider: w = std::min(SS(200), row.Width * 0.38f); h = SS(24); break;
    case SRow::Text:   w = std::min(SS(320), row.Width * 0.5f); break;
    case SRow::Button: w = SS(130); break;
    default: return RectF(0, 0, 0, 0);
    }
    return RectF(right - w, row.Y + (row.Height - h) / 2, w, h);
}

static void SRebuildFonts() {
    const wchar_t* icoFam = L"Segoe MDL2 Assets";
    { FontFamily ff(L"Segoe Fluent Icons"); if (ff.IsAvailable()) icoFam = L"Segoe Fluent Icons"; }
    auto mk = [](const wchar_t* fam, float px, int style) {
        auto f = std::make_unique<Font>(fam, px, style, UnitPixel);
        if (!f->IsAvailable()) f = std::make_unique<Font>(L"Segoe UI", px, style, UnitPixel);
        return f;
    };
    g_sfBody   = mk(L"Segoe UI", SS(14), FontStyleRegular);
    g_sfSmall  = mk(L"Segoe UI", SS(12.5f), FontStyleRegular);
    g_sfTitle  = mk(L"Segoe UI", SS(22), FontStyleBold);
    g_sfHead   = mk(L"Segoe UI", SS(15), FontStyleBold);
    g_sfIcon   = mk(icoFam, SS(16), FontStyleRegular);
    g_sfIconSm = mk(icoFam, SS(12), FontStyleRegular);

    HFONT old = g_sEditFont;
    g_sEditFont = CreateFontW(-(int)std::round(SS(14)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
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

static std::wstring SSearchText() {
    if (!g_setSearch) return std::wstring();
    wchar_t buf[256] = {};
    GetWindowTextW(g_setSearch, buf, 256);
    return TrimWs(buf);
}

static void SClampScroll() {
    SGeo G = Geo();
    float maxS = std::max(0.f, g_sContentH - G.vpH);
    g_sScroll = std::max(0.f, std::min(g_sScroll, maxS));
}

static void SScrollBy(float d) {
    g_sScroll += d;
    SClampScroll();
    InvalidateRect(g_setHwnd, nullptr, FALSE);
}

static void RebuildView() {
    g_view.clear();
    g_groups.clear();
    const std::wstring q = SLower(SSearchText());
    float y = SS(8);
    auto pushHeader = [&](const std::wstring& t) {
        SView v; v.heading = t; v.y = y; v.h = SS(46); y += v.h; g_view.push_back(v);
    };
    auto pushRow = [&](SRow* r) {
        SView v; v.row = r; v.y = y;
        v.h = (r->type == SRow::Info) ? SS(46) : (r->desc.empty() ? SS(52) : SS(68));
        y += v.h; g_view.push_back(v);
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
                if (SLower(r.label + L" " + r.desc + L" " + r.right).find(q) == std::wstring::npos) continue;
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

    g_sContentH = y + SS(32);
    SClampScroll();
    if (g_setHwnd) InvalidateRect(g_setHwnd, nullptr, FALSE);
}

// ---- inline editing / search box -------------------------------------------
static void SPositionSearch() {
    if (!g_setSearch) return;
    SGeo G = Geo();
    RectF sp = SSearchRect(G);
    MoveWindow(g_setSearch, (int)(sp.X + SS(34)), (int)(sp.Y + (sp.Height - (float)g_sEditH) / 2),
               (int)std::max(10.f, sp.Width - SS(34) - SS(14)), g_sEditH, TRUE);
}

static void ShowSearch() {
    SPositionSearch();
    ShowWindow(g_setSearch, SW_SHOW);
    SetFocus(g_setSearch);
    InvalidateRect(g_setHwnd, nullptr, FALSE);
}

static void SBeginTextEdit(SRow* r, const RectF& ctl) {
    g_sEditRow = r;
    SetWindowTextW(g_setEdit, r->getStr().c_str());
    MoveWindow(g_setEdit, (int)(ctl.X + SS(12)), (int)(ctl.Y + (ctl.Height - (float)g_sEditH) / 2),
               (int)std::max(10.f, ctl.Width - SS(24)), g_sEditH, TRUE);
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

static void SSelectPage(int i) {
    if (i < 0 || i >= (int)g_pages.size()) return;
    g_sPage = i;
    g_sScroll = 0.f;
    SetWindowTextW(g_setSearch, L"");
    ShowWindow(g_setSearch, SW_HIDE);
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
                  StringAlignment h, StringAlignment v) {
    StringFormat sf;
    sf.SetAlignment(h);
    sf.SetLineAlignment(v);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush b(c);
    g.DrawString(s.c_str(), -1, f, r, &sf, &b);
}

static void DrawSettings(Graphics& g) {
    const Theme& T = g_th;
    const SGeo G = Geo();
    const COLORREF fc = SFieldColor();
    const Color field(GetRValue(fc), GetGValue(fc), GetBValue(fc));
    const std::wstring query = SSearchText();
    const bool searching = !query.empty();
    const Color sepSoft(g_dark ? 70 : 28, 0, 0, 0);

    { SolidBrush b(T.toolbar); g.FillRectangle(&b, 0.f, 0.f, G.W, G.H); }

    // Sidebar
    {
        SolidBrush b(T.tabbar);
        g.FillRectangle(&b, 0.f, 0.f, G.sideW, G.H);
        Pen sep(T.sep, 1.f);
        g.DrawLine(&sep, G.sideW - 0.5f, 0.f, G.sideW - 0.5f, G.H);
        SText(g, L"Settings", g_sfTitle.get(), RectF(SS(24), SS(18), G.sideW - SS(48), SS(44)), T.text);
        for (int i = 0; i < (int)g_pages.size(); ++i) {
            RectF r = SSideRect(G, i);
            bool sel = (i == g_sPage) && !searching;
            if (sel) FillRound(g, WithAlpha(T.accent, 46), r, SS(8));
            else if (i == g_sHoverSide) FillRound(g, T.hover, r, SS(8));
            DrawGlyph(g, g_pages[i].icon, RectF(r.X + SS(6), r.Y, SS(32), r.Height),
                      sel ? T.accent : T.icon, g_sfIcon.get());
            SText(g, g_pages[i].name, g_sfBody.get(), RectF(r.X + SS(44), r.Y, r.Width - SS(52), r.Height), T.text);
        }
    }

    // Header (page title + search)
    {
        std::wstring title = searching ? std::wstring(L"Search results")
                                       : (g_sPage < (int)g_pages.size() ? g_pages[g_sPage].name : std::wstring());
        SText(g, title, g_sfTitle.get(), RectF(G.colX, 0, std::max(SS(80), G.colW - SS(300)), G.headerH), T.text);
        RectF sp = SSearchRect(G);
        FillRound(g, field, sp, sp.Height / 2);
        if (g_setSearch && GetFocus() == g_setSearch) {
            RectF ring = sp;
            ring.Inflate(SS(1), SS(1));
            GraphicsPath p;
            AddRound(p, ring, ring.Height / 2);
            Pen pen(WithAlpha(T.accent, 140), SS(2));
            g.DrawPath(&pen, &p);
        }
        DrawGlyph(g, L"\uE721", RectF(sp.X + SS(6), sp.Y, SS(28), sp.Height), T.text2, g_sfIconSm.get());
        if (!(g_setSearch && IsWindowVisible(g_setSearch)))
            SText(g, L"Search settings", g_sfSmall.get(),
                  RectF(sp.X + SS(34), sp.Y, sp.Width - SS(44), sp.Height), T.text2);
        Pen sep(T.sep, 1.f);
        g.DrawLine(&sep, G.sideW, G.vpY - 0.5f, G.W, G.vpY - 0.5f);
    }

    // Content
    g.SetClip(RectF(G.sideW, G.vpY, std::max(0.f, G.W - G.sideW), G.vpH));

    for (auto& gr : g_groups) {
        RectF c(G.colX, G.vpY + gr.first - g_sScroll, G.colW, gr.second - gr.first);
        if (c.GetBottom() < G.vpY || c.Y > G.H) continue;
        FillRound(g, T.tabActive, c, SS(10));
        GraphicsPath p;
        AddRound(p, c, SS(10));
        Pen pen(T.tabBorder, 1.f);
        g.DrawPath(&pen, &p);
    }

    for (int i = 0; i < (int)g_view.size(); ++i) {
        const SView& v = g_view[i];
        RectF r = SRowRect(G, v);
        if (r.GetBottom() < G.vpY || r.Y > G.H) continue;

        if (!v.row || v.row->type == SRow::Header) {
            SText(g, v.row ? v.row->label : v.heading, g_sfHead.get(),
                  RectF(r.X + SS(4), r.Y + SS(14), r.Width, r.Height - SS(14)), T.text);
            continue;
        }
        SRow& row = *v.row;
        if (v.group >= 0 && !v.first) {
            Pen sp(sepSoft, 1.f);
            g.DrawLine(&sp, r.X + SS(20), r.Y, r.GetRight() - SS(20), r.Y);
        }

        const RectF ctl = SCtlRect(row, r);
        const bool hv = (g_sHoverRow == i && g_sHoverCtl);
        const float lx = r.X + SS(20);
        float labelRight;
        if (row.type == SRow::Info) labelRight = r.X + r.Width * 0.38f;
        else if (row.type == SRow::Slider) labelRight = ctl.X - SS(98);
        else labelRight = ctl.X - SS(16);
        const float lw = std::max(SS(40), labelRight - lx);

        if (row.desc.empty()) {
            SText(g, row.label, g_sfBody.get(), RectF(lx, r.Y, lw, r.Height), T.text);
        } else {
            SText(g, row.label, g_sfBody.get(), RectF(lx, r.Y + SS(11), lw, SS(24)), T.text);
            SText(g, row.desc, g_sfSmall.get(), RectF(lx, r.Y + SS(35), lw, SS(22)), T.text2);
        }

        switch (row.type) {
        case SRow::Toggle: {
            bool on = row.getInt() != 0;
            FillRound(g, on ? T.accent : SOffColor(), ctl, ctl.Height / 2);
            float kd = ctl.Height - SS(6);
            float kx = on ? ctl.GetRight() - SS(3) - kd : ctl.X + SS(3);
            SolidBrush kb(Color(255, 255, 255, 255));
            g.FillEllipse(&kb, kx, ctl.Y + SS(3), kd, kd);
            break;
        }
        case SRow::Choice: {
            int cur = row.getInt();
            std::wstring val = (cur >= 0 && cur < (int)row.options.size()) ? row.options[cur] : std::wstring();
            FillRound(g, field, ctl, SS(8));
            if (hv) {
                GraphicsPath p; AddRound(p, ctl, SS(8));
                Pen pen(WithAlpha(T.accent, 160), 1.f); g.DrawPath(&pen, &p);
            }
            SText(g, val, g_sfBody.get(), RectF(ctl.X + SS(12), ctl.Y, ctl.Width - SS(40), ctl.Height), T.text);
            DrawGlyph(g, L"\uE70D", RectF(ctl.GetRight() - SS(30), ctl.Y, SS(24), ctl.Height), T.text2, g_sfIconSm.get());
            break;
        }
        case SRow::Slider: {
            int val = std::max(row.minV, std::min(row.maxV, row.getInt()));
            float t = (row.maxV > row.minV) ? (float)(val - row.minV) / (float)(row.maxV - row.minV) : 0.f;
            float cy = ctl.Y + ctl.Height / 2;
            FillRound(g, SOffColor(), RectF(ctl.X, cy - SS(2), ctl.Width, SS(4)), SS(2));
            if (t > 0.f) FillRound(g, T.accent, RectF(ctl.X, cy - SS(2), ctl.Width * t, SS(4)), SS(2));
            float kd = SS(16);
            float kx = ctl.X + ctl.Width * t - kd / 2;
            SolidBrush kb(Color(255, 255, 255, 255));
            g.FillEllipse(&kb, kx, cy - kd / 2, kd, kd);
            Pen kp(Color(70, 0, 0, 0), 1.f);
            g.DrawEllipse(&kp, kx, cy - kd / 2, kd, kd);
            std::wstring txt = row.fmt ? row.fmt(val) : std::to_wstring(val);
            SText(g, txt, g_sfBody.get(), RectF(ctl.X - SS(90), r.Y, SS(80), r.Height), T.text2, StringAlignmentFar);
            break;
        }
        case SRow::Text: {
            const bool editing = (g_sEditRow == &row);
            FillRound(g, field, ctl, SS(8));
            if (editing || hv) {
                GraphicsPath p; AddRound(p, ctl, SS(8));
                Pen pen(WithAlpha(T.accent, editing ? 220 : 160), editing ? 2.f : 1.f);
                g.DrawPath(&pen, &p);
            }
            if (!editing)
                SText(g, row.getStr(), g_sfBody.get(), RectF(ctl.X + SS(12), ctl.Y, ctl.Width - SS(24), ctl.Height), T.text);
            break;
        }
        case SRow::Button: {
            if (hv) FillRound(g, WithAlpha(T.accent, 30), ctl, SS(8));
            GraphicsPath p; AddRound(p, ctl, SS(8));
            Pen pen(WithAlpha(T.accent, 200), 1.f);
            g.DrawPath(&pen, &p);
            SText(g, row.right, g_sfBody.get(), ctl, T.accent, StringAlignmentCenter);
            break;
        }
        case SRow::Info: {
            if (row.kbd) {
                StringFormat fmt(StringFormat::GenericTypographic());
                RectF m;
                g.MeasureString(row.right.c_str(), -1, g_sfSmall.get(), RectF(0, 0, 10000, 1000), &fmt, &m);
                float w = m.Width + SS(22), h = SS(28);
                RectF pill(r.GetRight() - SS(20) - w, r.Y + (r.Height - h) / 2, w, h);
                FillRound(g, field, pill, SS(7));
                SText(g, row.right, g_sfSmall.get(), pill, T.text, StringAlignmentCenter);
            } else {
                SText(g, row.right, g_sfBody.get(),
                      RectF(r.X + r.Width * 0.38f, r.Y, r.Width * 0.62f - SS(20), r.Height), T.text2, StringAlignmentFar);
            }
            break;
        }
        default: break;
        }
    }

    if (g_view.empty())
        SText(g, L"No settings found", g_sfBody.get(), RectF(G.sideW, G.vpY + SS(40), G.W - G.sideW, SS(40)),
              T.text2, StringAlignmentCenter);

    // Scrollbar
    float maxS = g_sContentH - G.vpH;
    if (maxS > 1.f) {
        float th = std::max(SS(36), G.vpH * G.vpH / g_sContentH);
        float ty = G.vpY + (G.vpH - th) * (g_sScroll / maxS);
        FillRound(g, WithAlpha(T.text2, 110), RectF(G.W - SS(10), ty, SS(5), th), SS(2.5f));
    }
    g.ResetClip();
}

// ---- hit testing helpers ---------------------------------------------------
static int SSideHit(const SGeo& G, float x, float y) {
    for (int i = 0; i < (int)g_pages.size(); ++i)
        if (SSideRect(G, i).Contains(x, y)) return i;
    return -1;
}

static int SRowAt(const SGeo& G, float x, float y) {
    if (y < G.vpY || x < G.sideW) return -1;
    for (int i = 0; i < (int)g_view.size(); ++i)
        if (SRowRect(G, g_view[i]).Contains(x, y)) return i;
    return -1;
}

static bool SOverControl(const SGeo& G, int vi, float x, float y) {
    if (vi < 0 || !g_view[vi].row) return false;
    SRow& r = *g_view[vi].row;
    RectF row = SRowRect(G, g_view[vi]);
    RectF ctl = SCtlRect(r, row);
    if (ctl.Width <= 0) return false;
    if (r.type == SRow::Slider) {
        RectF hit(ctl.X - SS(10), row.Y, ctl.Width + SS(20), row.Height);
        return hit.Contains(x, y);
    }
    return ctl.Contains(x, y);
}

static void SSetSliderFromX(SRow* row, const RectF& ctl, float x) {
    if (ctl.Width <= 0 || row->step <= 0) return;
    float t = std::max(0.f, std::min(1.f, (x - ctl.X) / ctl.Width));
    int range = row->maxV - row->minV;
    int v = row->minV + (int)std::lround(t * (float)range / (float)row->step) * row->step;
    v = std::max(row->minV, std::min(row->maxV, v));
    if (v != row->getInt()) row->setInt(v);
}

static void SChoiceMenu(SRow* r, const RectF& ctl) {
    HMENU m = CreatePopupMenu();
    int cur = r->getInt();
    for (int i = 0; i < (int)r->options.size(); ++i)
        AppendMenuW(m, MF_STRING | (i == cur ? MF_CHECKED : 0), 1000 + i, r->options[i].c_str());
    POINT pt = { (LONG)ctl.X, (LONG)ctl.GetBottom() };
    ClientToScreen(g_setHwnd, &pt);
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY,
                                  pt.x, pt.y, 0, g_setHwnd, nullptr);
    DestroyMenu(m);
    if (cmd >= 1000) r->setInt(cmd - 1000);
}

void SettingsThemeChanged() {
    if (!g_setHwnd) return;
    if (g_fieldBrush) DeleteObject(g_fieldBrush);
    g_fieldBrush = CreateSolidBrush(SFieldColor());
    ApplyDwm(g_setHwnd);
    InvalidateRect(g_setHwnd, nullptr, FALSE);
    if (g_setSearch) InvalidateRect(g_setSearch, nullptr, TRUE);
    if (g_setEdit) InvalidateRect(g_setEdit, nullptr, TRUE);
}

// ---- window procedure ------------------------------------------------------
static LRESULT CALLBACK SettingsWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_setHwnd = hWnd;
        g_sDpi = GetDpiForWindow(hWnd);
        HINSTANCE hi = (HINSTANCE)GetWindowLongPtrW(hWnd, GWLP_HINSTANCE);
        g_setSearch = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL | ES_LEFT,
                                      0, 0, 10, 10, hWnd, (HMENU)(INT_PTR)SET_ID_SEARCH, hi, nullptr);
        g_setEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL | ES_LEFT,
                                    0, 0, 10, 10, hWnd, (HMENU)(INT_PTR)SET_ID_EDIT, hi, nullptr);
        g_oldSEditProc = (WNDPROC)SetWindowLongPtrW(g_setSearch, GWLP_WNDPROC, (LONG_PTR)SEditProc);
        SetWindowLongPtrW(g_setEdit, GWLP_WNDPROC, (LONG_PTR)SEditProc);
        SRebuildFonts();
        SettingsThemeChanged();
        BuildPages();
        g_sPage = 0; g_sScroll = 0.f; g_sEditRow = nullptr;
        g_sHoverSide = g_sHoverRow = g_sDragRow = -1;
        RebuildView();
        return 0;
    }
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
        if (cr.right > 0 && cr.bottom > 0) {
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, cr.right, cr.bottom);
            HGDIOBJ old = SelectObject(mem, bmp);
            {
                Graphics g(mem);
                g.SetSmoothingMode(SmoothingModeHighQuality);
                g.SetPixelOffsetMode(PixelOffsetModeHalf);
                g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
                DrawSettings(g);
            }
            BitBlt(hdc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, ToColorRef(g_th.text));
        SetBkColor(hdc, SFieldColor());
        return (LRESULT)g_fieldBrush;
    }
    case WM_COMMAND: {
        HWND from = (HWND)lParam;
        WORD code = HIWORD(wParam);
        if (from == g_setSearch) {
            if (code == EN_CHANGE) { g_sScroll = 0.f; RebuildView(); }
            else if (code == EN_KILLFOCUS) {
                if (SSearchText().empty()) ShowWindow(g_setSearch, SW_HIDE);
                InvalidateRect(hWnd, nullptr, FALSE);
            } else if (code == EN_SETFOCUS) InvalidateRect(hWnd, nullptr, FALSE);
        } else if (from == g_setEdit && code == EN_KILLFOCUS) {
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
        int si = SSideHit(G, x, y);
        int vi = SRowAt(G, x, y);
        bool ctl = SOverControl(G, vi, x, y);
        bool pill = SSearchRect(G).Contains(x, y);
        if (si != g_sHoverSide || vi != g_sHoverRow || ctl != g_sHoverCtl || pill != g_sHoverPill) {
            g_sHoverSide = si; g_sHoverRow = vi; g_sHoverCtl = ctl; g_sHoverPill = pill;
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
        g_sHoverSide = g_sHoverRow = -1;
        g_sHoverCtl = g_sHoverPill = false;
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && (g_sHoverSide >= 0 || g_sHoverCtl || g_sHoverPill)) {
            SetCursor(LoadCursor(nullptr, IDC_HAND));
            return TRUE;
        }
        break;

    case WM_LBUTTONDOWN: {
        const float x = (float)GET_X_LPARAM(lParam), y = (float)GET_Y_LPARAM(lParam);
        const SGeo G = Geo();
        SCommitTextEdit();
        int si = SSideHit(G, x, y);
        if (si >= 0) { SSelectPage(si); SetFocus(hWnd); return 0; }
        if (SSearchRect(G).Contains(x, y)) { ShowSearch(); return 0; }
        int vi = SRowAt(G, x, y);
        if (vi >= 0 && g_view[vi].row) {
            SRow* r = g_view[vi].row;
            RectF row = SRowRect(G, g_view[vi]);
            RectF ctl = SCtlRect(*r, row);
            switch (r->type) {
            case SRow::Toggle:  r->setInt(r->getInt() ? 0 : 1); break;
            case SRow::Choice:  if (ctl.Contains(x, y)) SChoiceMenu(r, ctl); break;
            case SRow::Slider:
                if (SOverControl(G, vi, x, y)) {
                    g_sDragRow = vi;
                    SetCapture(hWnd);
                    SSetSliderFromX(r, ctl, x);
                }
                break;
            case SRow::Text:    if (ctl.Contains(x, y)) SBeginTextEdit(r, ctl); break;
            case SRow::Button:  if (ctl.Contains(x, y) && r->action) r->action(); break;
            default: break;
            }
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        if (!g_sEditRow) SetFocus(hWnd);
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_sDragRow >= 0) { g_sDragRow = -1; if (GetCapture() == hWnd) ReleaseCapture(); }
        return 0;

    case WM_CAPTURECHANGED:
        g_sDragRow = -1;
        return 0;

    case WM_MOUSEWHEEL: {
        SCommitTextEdit();
        short delta = GET_WHEEL_DELTA_WPARAM(wParam);
        SScrollBy(-(float)delta / 120.f * SS(64));
        return 0;
    }
    case WM_KEYDOWN: {
        const SGeo G = Geo();
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (wParam == VK_ESCAPE) {
            if (!SSearchText().empty()) { SetWindowTextW(g_setSearch, L""); ShowWindow(g_setSearch, SW_HIDE); }
            else DestroyWindow(hWnd);
            return 0;
        }
        if (ctrl && wParam == 'F') { ShowSearch(); return 0; }
        switch (wParam) {
        case VK_UP:    SScrollBy(-SS(48)); return 0;
        case VK_DOWN:  SScrollBy(SS(48)); return 0;
        case VK_PRIOR: SScrollBy(-G.vpH * 0.9f); return 0;
        case VK_NEXT:  SScrollBy(G.vpH * 0.9f); return 0;
        case VK_HOME:  g_sScroll = 0.f; SScrollBy(0); return 0;
        case VK_END:   g_sScroll = g_sContentH; SScrollBy(0); return 0;
        }
        break;
    }
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
        g_sfBody.reset(); g_sfSmall.reset(); g_sfTitle.reset(); g_sfHead.reset();
        g_sfIcon.reset(); g_sfIconSm.reset();
        g_setSearch = g_setEdit = nullptr;
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
    int w = MulDiv(980, (int)g_dpi, 96), h = MulDiv(700, (int)g_dpi, 96);
    RECT pr = {};
    GetWindowRect(g_hWnd, &pr);
    int x = pr.left + ((pr.right - pr.left) - w) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - h) / 2;
    CreateWindowExW(0, kSetClass, L"Settings", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                    x, y, w, h, g_hWnd, nullptr, hi, nullptr);
    if (g_setHwnd) {
        ShowWindow(g_setHwnd, SW_SHOW);
        UpdateWindow(g_setHwnd);
        SetFocus(g_setHwnd);
    }
}
