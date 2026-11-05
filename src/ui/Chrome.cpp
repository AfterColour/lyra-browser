// Lyra — раскладка, отрисовка и попадание мышью для панели вкладок и инструментов.
#include "ui/Chrome.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "core/Constants.h"
#include "core/Settings.h"
#include "core/Url.h"
#include "ui/AddressBar.h"
#include "ui/Draw.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"

static Hit MakeHit(Hit::Kind k, int i = -1);
static RectF CloseRect(int i);
static bool CloseVisible(int i);
static std::unique_ptr<Font> MakeFont(const wchar_t* const* families, int count, float px);
static void DrawChrome(Graphics& g, int w);

Layout g_L;

static Hit MakeHit(Hit::Kind k, int i) { Hit h; h.kind = k; h.index = i; return h; }

Hit g_hover, g_down;

void InvalidateChrome() {
    if (!g_hWnd) return;
    RECT r = { 0, 0, g_clientW, (LONG)std::ceil(g_L.chromeH) };
    InvalidateRect(g_hWnd, &r, FALSE);
}

void ComputeLayout(int clientW) {
    using namespace metrics;
    g_clientW = clientW;
    const float w = (float)clientW;
    Layout& L = g_L;

    L.toolbarH = std::floor(S((float)(g_prefs.compact ? 40 : kToolbarH)));
    L.tabBarH  = std::floor(S((float)(g_prefs.compact ? 30 : kTabBarH)));
    L.chromeH  = L.toolbarH + L.tabBarH;

    const float btn = S((float)(g_prefs.compact ? 28 : kButton)), edge = S((float)kEdge), gap = S(2);
    const float by = std::floor((L.toolbarH - btn) / 2);
    L.back = RectF(edge, by, btn, btn);
    L.fwd  = RectF(edge + btn + gap, by, btn, btn);
    L.more = RectF(w - edge - btn, by, btn, btn);

    const float avL = L.fwd.GetRight() + S(16);
    const float avR = L.more.X - S(16);
    const float avail = std::max(0.f, avR - avL);
    const float pw = std::min(S((float)kPillMaxW), avail);
    const float ph = S((float)(g_prefs.compact ? 26 : kPillH));
    float px = w / 2 - pw / 2;
    px = std::max(avL, std::min(px, avR - pw));
    L.pill = RectF(std::floor(px), std::floor((L.toolbarH - ph) / 2), std::floor(pw), ph);

    const float rb = S(g_prefs.compact ? 22.f : 26.f);
    L.reload = RectF(L.pill.GetRight() - rb - S(3), L.pill.Y + (ph - rb) / 2, rb, rb);

    // Tab strip
    L.newTab = RectF(w - S((float)kNewTabW), L.toolbarH, S((float)kNewTabW), L.tabBarH);
    L.tabs.clear();
    const size_t n = g_tabs.size();
    const float margin = S(6);
    const float stripW = std::max(0.f, w - S((float)kNewTabW) - margin * 2);
    float tabW = n ? stripW / (float)n : 0.f;
    tabW = std::min(tabW, S((float)kTabMaxW));
    tabW = std::max(tabW, S((float)kTabMinW));
    const float inset = S((float)kTabInsetY);
    for (size_t i = 0; i < n; ++i) {
        L.tabs.push_back(RectF(std::floor(margin + tabW * (float)i), L.toolbarH + inset,
                               std::floor(tabW - S(2)), L.tabBarH - inset * 2));
    }
}

static RectF CloseRect(int i) {
    const RectF& r = g_L.tabs[i];
    const float c = S((float)metrics::kCloseBtn);
    return RectF(r.X + S(6), r.Y + (r.Height - c) / 2, c, c);
}

static bool CloseVisible(int i) {
    if (i < 0 || i >= (int)g_L.tabs.size()) return false;
    bool hov = (g_hover.kind == Hit::TabBody || g_hover.kind == Hit::TabClose) && g_hover.index == i;
    return g_L.tabs[i].Width >= S(44) && (i == g_active || hov || g_prefs.closeAlways);
}

Hit HitTest(int px, int py) {
    const float x = (float)px, y = (float)py;
    if (y < 0 || y >= g_L.chromeH) return MakeHit(Hit::None);
    if (g_L.back.Contains(x, y))  return MakeHit(Hit::Back);
    if (g_L.fwd.Contains(x, y))   return MakeHit(Hit::Forward);
    if (g_L.more.Contains(x, y))  return MakeHit(Hit::More);
    if (!IsEditing() && g_L.reload.Contains(x, y)) return MakeHit(Hit::Reload);
    if (g_L.pill.Contains(x, y))  return MakeHit(Hit::Pill);
    if (g_L.newTab.Contains(x, y)) return MakeHit(Hit::NewTab);
    if (y >= g_L.toolbarH) {
        for (int i = (int)g_L.tabs.size() - 1; i >= 0; --i) {
            if (CloseVisible(i) && CloseRect(i).Contains(x, y)) return MakeHit(Hit::TabClose, i);
            const RectF& r = g_L.tabs[i];
            if (x >= r.X && x < r.GetRight() + S(2)) return MakeHit(Hit::TabBody, i);
        }
    }
    return MakeHit(Hit::None);
}

// ----------------------------------------------------------------------------
//  GDI+ resources
// ----------------------------------------------------------------------------
std::unique_ptr<Font> g_fText, g_fTab, g_fIcon, g_fIconSm;

static std::unique_ptr<Font> MakeFont(const wchar_t* const* families, int count, float px) {
    for (int i = 0; i < count; ++i) {
        auto f = std::make_unique<Font>(families[i], px, FontStyleRegular, UnitPixel);
        if (f->IsAvailable()) return f;
    }
    return std::make_unique<Font>(L"Segoe UI", px, FontStyleRegular, UnitPixel);
}

void RebuildResources() {
    static const wchar_t* ui[]   = { L"Segoe UI" };
    static const wchar_t* icons[] = { L"Segoe Fluent Icons", L"Segoe MDL2 Assets" };
    g_fText   = MakeFont(ui, 1, S(14));
    g_fTab    = MakeFont(ui, 1, S(13));
    g_fIcon   = MakeFont(icons, 2, S(16));
    g_fIconSm = MakeFont(icons, 2, S(12));

    HFONT old = g_editFont;
    g_editFont = CreateFontW(-(int)std::round(S(14)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (g_urlEdit) {
        HDC dc = GetDC(g_urlEdit);
        HGDIOBJ prev = SelectObject(dc, g_editFont);
        TEXTMETRICW tm = {};
        GetTextMetricsW(dc, &tm);
        g_editTextH = tm.tmHeight;
        SelectObject(dc, prev);
        ReleaseDC(g_urlEdit, dc);
        SendMessageW(g_urlEdit, WM_SETFONT, (WPARAM)g_editFont, TRUE);
    }
    if (old) DeleteObject(old);
}

static void DrawChrome(Graphics& g, int w) {
    const Theme& T = g_th;
    const float fw = (float)w;
    Tab* cur = ActiveTab();
    const bool editing = IsEditing();

    auto hov = [&](Hit::Kind k, int i) { return g_hover == MakeHit(k, i); };
    auto dwn = [&](Hit::Kind k, int i) { return g_down == MakeHit(k, i) && hov(k, i); };
    auto toolButton = [&](const RectF& r, const wchar_t* glyph, bool enabled, bool h, bool p, Font* f) {
        if (enabled && (h || p)) FillRound(g, p ? T.pressed : T.hover, r, S(6));
        DrawGlyph(g, glyph, r, enabled ? T.icon : T.iconDis, f);
    };

    // Backgrounds + hairlines
    {
        SolidBrush b1(T.toolbar), b2(T.tabbar);
        g.FillRectangle(&b1, 0.f, 0.f, fw, g_L.toolbarH);
        g.FillRectangle(&b2, 0.f, g_L.toolbarH, fw, g_L.tabBarH);
        Pen sep(T.sep, 1.f);
        g.DrawLine(&sep, 0.f, g_L.toolbarH - 0.5f, fw, g_L.toolbarH - 0.5f);
        g.DrawLine(&sep, 0.f, g_L.chromeH - 0.5f, fw, g_L.chromeH - 0.5f);
    }

    // Back / forward / more
    toolButton(g_L.back, L"\uE76B", cur && cur->canBack, hov(Hit::Back, -1), dwn(Hit::Back, -1), g_fIcon.get());
    toolButton(g_L.fwd,  L"\uE76C", cur && cur->canFwd,  hov(Hit::Forward, -1), dwn(Hit::Forward, -1), g_fIcon.get());
    toolButton(g_L.more, L"\uE712", true, hov(Hit::More, -1), dwn(Hit::More, -1), g_fIcon.get());

    // Address pill
    {
        const RectF& pill = g_L.pill;
        const float rad = S((float)metrics::kPillRadius);
        Color base = editing ? T.pillFocus : (hov(Hit::Pill, -1) || hov(Hit::Reload, -1) ? T.pillHover : T.pill);
        FillRound(g, base, pill, rad);

        // Safari-style loading fill
        if (!editing && cur && cur->fade > 0.01f && cur->progress > 0.f) {
            GraphicsPath path;
            AddRound(path, pill, rad);
            g.SetClip(&path);
            Color a((BYTE)(T.progressAlpha * cur->fade), T.accent.GetR(), T.accent.GetG(), T.accent.GetB());
            SolidBrush b(a);
            g.FillRectangle(&b, pill.X, pill.Y, pill.Width * std::min(1.f, cur->progress), pill.Height);
            g.ResetClip();
        }

        if (editing) {
            RectF ring = pill;
            ring.Inflate(S(1.5f), S(1.5f));
            GraphicsPath p;
            AddRound(p, ring, rad + S(1.5f));
            Pen pen(Color(120, T.accent.GetR(), T.accent.GetG(), T.accent.GetB()), S(3));
            g.DrawPath(&pen, &p);
        } else {
            std::wstring label = cur ? (g_prefs.showFullUrl ? DisplayUrl(cur->url) : HostOf(cur->url)) : L"";
            const bool placeholder = label.empty();
            if (placeholder) label = L"Search or enter website name";
            const bool secure = cur && cur->url.compare(0, 8, L"https://") == 0;

            const float sideInset = S(34);
            const float maxText = pill.Width - sideInset * 2;
            const float lockW = secure ? S(14) : 0.f, lockGap = secure ? S(4) : 0.f;

            StringFormat fmt(StringFormat::GenericTypographic());
            fmt.SetFormatFlags(fmt.GetFormatFlags() | StringFormatFlagsNoWrap);
            fmt.SetTrimming(StringTrimmingEllipsisCharacter);
            fmt.SetLineAlignment(StringAlignmentCenter);

            RectF m;
            g.MeasureString(label.c_str(), -1, g_fText.get(), RectF(0, 0, 10000, 1000), &fmt, &m);
            float tw = std::min(m.Width, maxText - lockW - lockGap);
            float total = lockW + lockGap + tw;
            float x0 = pill.X + pill.Width / 2 - total / 2;

            if (secure)
                DrawGlyph(g, L"\uE72E", RectF(x0, pill.Y, lockW, pill.Height), T.text2, g_fIconSm.get());
            SolidBrush tb(placeholder ? T.text2 : T.text);
            g.DrawString(label.c_str(), -1, g_fText.get(),
                         RectF(x0 + lockW + lockGap, pill.Y, tw + 1.f, pill.Height), &fmt, &tb);

            // Reload / stop
            bool loading = cur && cur->loading;
            toolButton(g_L.reload, loading ? L"\uE711" : L"\uE72C", cur != nullptr,
                       hov(Hit::Reload, -1), dwn(Hit::Reload, -1), g_fIconSm.get());
        }
    }

    // Tabs
    {
        StringFormat tf;
        tf.SetAlignment(StringAlignmentCenter);
        tf.SetLineAlignment(StringAlignmentCenter);
        tf.SetTrimming(StringTrimmingEllipsisCharacter);
        tf.SetFormatFlags(StringFormatFlagsNoWrap);

        const int n = (int)std::min(g_L.tabs.size(), g_tabs.size());   // layout may lag behind g_tabs by one frame
        for (int i = 0; i < n; ++i) {
            const RectF& r = g_L.tabs[i];
            const bool act = (i == g_active);
            const bool h = hov(Hit::TabBody, i) || hov(Hit::TabClose, i);
            const float rad = S((float)metrics::kTabRadius);

            if (act) {
                FillRound(g, T.tabActive, r, rad);
                GraphicsPath p;
                AddRound(p, r, rad);
                Pen pen(T.tabBorder, 1.f);
                g.DrawPath(&pen, &p);
            } else if (h) {
                FillRound(g, T.tabHover, r, rad);
            } else if (i + 1 < n && (i + 1) != g_active &&
                       !hov(Hit::TabBody, i + 1) && !hov(Hit::TabClose, i + 1)) {
                Pen sp(T.sep, 1.f);
                float x = r.GetRight() + S(1);
                g.DrawLine(&sp, x, r.Y + S(7), x, r.GetBottom() - S(7));
            }

            const bool showClose = CloseVisible(i);
            if (showClose) {
                RectF c = CloseRect(i);
                if (hov(Hit::TabClose, i))
                    FillRound(g, dwn(Hit::TabClose, i) ? T.pressed : T.hover, c, c.Width / 2);
                DrawGlyph(g, L"\uE711", c, T.text2, g_fIconSm.get());
            }

            if (r.Width >= S(56)) {
                float inset = (r.Width >= S(70)) ? S(28) : S(6);
                RectF tr(r.X + inset, r.Y, r.Width - inset * 2, r.Height);
                SolidBrush tb(act ? T.text : T.text2);
                g.DrawString(g_tabs[i]->title.c_str(), -1, g_fTab.get(), tr, &tf, &tb);
            }
        }

        // New tab button
        const float bw = S(28), bh = S(24);
        RectF nt(g_L.newTab.X + (g_L.newTab.Width - bw) / 2, g_L.newTab.Y + (g_L.newTab.Height - bh) / 2, bw, bh);
        toolButton(nt, L"\uE710", (int)g_tabs.size() < metrics::kMaxTabs,
                   hov(Hit::NewTab, -1), dwn(Hit::NewTab, -1), g_fIconSm.get());
    }
}

void PaintChrome(HDC hdc, int w) {
    int h = (int)std::ceil(g_L.chromeH);
    if (w <= 0 || h <= 0) return;
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    {
        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeHighQuality);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        DrawChrome(g, w);
    }
    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}
