// Lyra — раскладка, отрисовка и попадание мышью для панели вкладок и инструментов.
//
// Three layouts (Settings > Tabs > Tab layout):
//   compact   - one row: back/forward, the tabs, where the active tab is the address field,
//               new tab, menu and the window buttons (like Safari's compact tab bar)
//   separate  - a tab strip with the window buttons, below it a toolbar with the address field
//   vertical  - a toolbar row with the address field, the tabs in a list on the left
// The window has no system title bar (see MainWindow.cpp): empty chrome drags the window.
#include "ui/Chrome.h"
#include "browser/SplitView.h"
#include "ui/TabDrag.h"
#include "browser/Translate.h"
#include "browser/Reader.h"
#include "ui/WindowFrame.h"
#include "browser/Downloads.h"
#include "browser/Favicons.h"
#include "browser/Passwords.h"
#include "core/Localization.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "core/Constants.h"
#include "core/Settings.h"
#include "core/Url.h"
#include "ui/AddressBar.h"
#include "ui/BookmarkPopup.h"
#include "ui/DownloadsPopup.h"
#include "ui/Animation.h"
#include "ui/Draw.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"

Layout g_L;
Hit g_hover, g_down;
std::unique_ptr<Font> g_fText, g_fTab, g_fTabBold, g_fIcon, g_fIconSm, g_fCaption, g_fBadge;

static Hit MakeHit(Hit::Kind k, int i = -1) { Hit h; h.kind = k; h.index = i; return h; }

// Chrome drawn on a transparent buffer (Mica shows through the empty parts)?
static bool s_paintTransparent = false;

// Animation state. s_target is where everything should be; g_L is what is drawn and hit-tested.
static Layout s_target;
static std::map<unsigned long long, RectF> s_tabCur;   // drawn tab rectangles by tab id
static RectF s_newTabCur;
static bool  s_haveCur = false;
// Width freeze after closing a tab with the mouse
static bool  s_frozen = false;
static float s_frozenTabW = 0, s_frozenPillW = 0;
// Hover fades: one entry per element that is or was recently hovered
struct HoverFade { Hit hit; float a = 0; };
static std::vector<HoverFade> s_fades;

constexpr float kTabTauMs = 55.f;     // tabs settle in ~170 ms at 100 % speed
constexpr float kHoverTauMs = 45.f;   // hover highlight fades in ~140 ms

ChromeDims CurrentDims() {
    //                     bar strip btn  tab icon cap  font sideW
    static const ChromeDims kDims[3] = {
        { 36, 34, 28, 26, 14, 40, 12, 220 },   // compact
        { 42, 38, 32, 30, 15, 44, 13, 232 },   // normal
        { 48, 42, 34, 34, 16, 46, 13, 244 },   // large
    };
    return kDims[std::max(0, std::min(2, g_prefs.density))];
}

void InvalidateChrome() {
    if (!g_hWnd) return;
    RECT top = { 0, 0, g_clientW, (LONG)std::ceil(g_L.chromeH) };
    InvalidateRect(g_hWnd, &top, FALSE);
    if (g_L.sideW > 0) {
        RECT cr;
        GetClientRect(g_hWnd, &cr);
        RECT side = { 0, top.bottom, (LONG)std::ceil(g_L.sideW), cr.bottom };
        InvalidateRect(g_hWnd, &side, FALSE);
    }
}

// ----------------------------------------------------------------------------
//  Layout
// ----------------------------------------------------------------------------
bool NativeCaptionButtons() {
    return g_hWnd && !g_fullScreenView && MicaActive();
}

static void PlaceCaptionButtons(Layout& L, float w, float rowH, const ChromeDims& d) {
    float cw = std::floor(S(d.cap)), cwClose = cw, capTop = 0, capH = rowH, right = w;
    if (NativeCaptionButtons()) {
        // The system buttons: Lyra's buttons take exactly their place, so that while the hover
        // fades in or out the system glyph and Lyra's lie on each other. DWM gives an area in
        // window coordinates that is wider than the buttons; they sit at its right end, 46 px
        // (at 96 dpi) each and the close button 48 px (measured on Windows 11). Vertically they
        // are drawn from the top of the client area, also when maximised (where the window
        // itself starts above the screen edge).
        cw = std::round(S(46));
        cwClose = std::round(S(48));
        capH = std::min(rowH, std::floor(S(32)));
        RECT b = {}, wr = {};
        POINT o = { 0, 0 };
        if (SUCCEEDED(DwmGetWindowAttribute(g_hWnd, DWMWA_CAPTION_BUTTON_BOUNDS, &b, sizeof(b))) &&
            b.right > b.left && b.bottom > b.top && GetWindowRect(g_hWnd, &wr) && ClientToScreen(g_hWnd, &o)) {
            const float r = (float)(b.right - (o.x - wr.left));        // client coordinates
            if (r > 0 && r <= w) right = r;
            if (cw * 2 + cwClose > (float)(b.right - b.left)) cw = cwClose = (float)(b.right - b.left) / 3;
            capH = std::min(rowH, (float)(b.bottom - b.top));
        }
    }
    L.captionH = rowH;
    L.capClose = RectF(right - cwClose, capTop, cwClose, capH);
    L.capMax   = RectF(right - cwClose - cw, capTop, cw, capH);
    L.capMin   = RectF(right - cwClose - cw * 2, capTop, cw, capH);
}

// The star sits at the right end of the address field (left of reload when that is inside).
static void PlaceStar(Layout& L) {
    const float rb = std::floor(L.pill.Height - S(6));
    const float right = L.reloadInPill ? L.reload.X - S(1) : L.pill.GetRight() - S(3);
    L.star = RectF(right - rb, L.pill.Y + (L.pill.Height - rb) / 2, rb, rb);
    if (L.pill.Width < S(220)) L.star = RectF();         // too narrow: no star
}

static void PlaceReloadInPill(Layout& L) {
    const float rb = std::floor(L.pill.Height - S(6));
    L.reload = RectF(L.pill.GetRight() - rb - S(3), L.pill.Y + (L.pill.Height - rb) / 2, rb, rb);
    L.reloadInPill = true;
    PlaceStar(L);
}

// Split view: its two tabs are one tab in the list. The left half's tab takes both places (the
// right half's gets no size); in the compact layout, while the split is open, the open half is
// the address field and the other half's tab gets no size. The tabs after them move up.
static void MergeSplitTabs(Layout& L) {
    int li = -1, ri = -1;
    if (!SplitStripPair(&li, &ri) || ri >= (int)L.tabs.size()) return;
    const bool vertical = L.mode == kLayoutVertical;
    const float step = vertical ? S(1) : S(2);
    const size_t n = L.tabs.size();
    std::vector<float> size(n);
    for (size_t i = 0; i < n; ++i) size[i] = vertical ? L.tabs[i].Height : L.tabs[i].Width;
    if (L.mode == kLayoutCompact && (g_active == li || g_active == ri)) size[g_active == li ? ri : li] = 0;
    else if (vertical) size[ri] = 0;                    // one row, the halves side by side
    else { size[li] += step + size[ri]; size[ri] = 0; }
    float pos = vertical ? L.tabs[0].Y : L.tabs[0].X;
    for (size_t i = 0; i < n; ++i) {
        if (vertical) { L.tabs[i].Y = std::floor(pos); L.tabs[i].Height = size[i]; }
        else { L.tabs[i].X = std::floor(pos); L.tabs[i].Width = size[i]; }
        if (size[i] > 0) pos += size[i] + step;
    }
    if (vertical) L.newTab.Y = std::floor(pos);
    else L.newTab.X = std::min(L.newTab.X, std::floor(pos));
    if (L.mode == kLayoutCompact && g_active >= 0 && g_active < (int)n) {
        L.pill = L.tabs[g_active];
        PlaceReloadInPill(L);
    }
}

static void ComputeTarget(Layout& L, int clientW) {
    const ChromeDims d = CurrentDims();
    const float w = (float)clientW;
    L = Layout();
    L.mode = std::max(0, std::min(2, g_prefs.tabLayout));

    const float bar = std::floor(S(d.bar)), btn = std::floor(S(d.btn)), tabH = std::floor(S(d.tab));
    const float edge = S(8), gap = S(2);
    const size_t n = g_tabs.size();
    const bool dl = !DownloadsList().empty();        // the downloads button shows once there are any
    L.tabs.resize(n);

    if (L.mode == kLayoutCompact) {
        L.chromeH = bar;
        PlaceCaptionButtons(L, w, bar, d);
        const float by = std::floor((bar - btn) / 2);
        L.back = RectF(edge, by, btn, btn);
        L.fwd  = RectF(edge + btn + gap, by, btn, btn);
        L.more = RectF(L.capMin.X - S(4) - btn, by, btn, btn);
        if (dl) L.downloads = RectF(L.more.X - gap - btn, by, btn, btn);
        const float trail = dl ? L.downloads.X : L.more.X;

        // Tabs between "forward" and "+": inactive tabs up to 150 px wide, the active tab (the
        // address field) takes the rest, up to 860 px. Many tabs shrink the inactive ones first.
        const float left = L.fwd.GetRight() + S(8);
        const float right = trail - S(4) - btn - gap;
        const float ty = std::floor((bar - tabH) / 2);
        const float avail = std::max(0.f, right - left - gap * (float)(n ? n - 1 : 0));
        const int inactive = n ? (int)n - 1 : 0;
        float inW = S(150), pillW = 0;
        if (inactive == 0) {
            pillW = std::min(avail, S(860));
        } else if (avail - inactive * inW >= S(300)) {
            pillW = std::min(avail - inactive * inW, S(860));
        } else {
            inW = std::max(S(28), (avail - S(300)) / (float)inactive);
            pillW = std::max(S(160), avail - inactive * inW);
        }
        if (s_frozen) {                                  // keep the widths from before the close
            inW = std::min(inW, s_frozenTabW);
            pillW = std::min(pillW, s_frozenPillW);
        }
        float x = left;
        for (size_t i = 0; i < n; ++i) {
            const float tw = ((int)i == g_active) ? pillW : inW;
            L.tabs[i] = RectF(std::floor(x), ty, std::floor(tw), tabH);
            x += tw + gap;
        }
        if (g_active >= 0 && g_active < (int)n) L.pill = L.tabs[g_active];
        else L.pill = RectF(left, ty, pillW, tabH);
        PlaceReloadInPill(L);
        const float nx = std::min(n ? x : left, right + gap);
        L.newTab = RectF(std::floor(nx), by, btn, btn);
    } else if (L.mode == kLayoutSeparate) {
        const float strip = std::floor(S(d.strip));
        L.chromeH = strip + bar;
        PlaceCaptionButtons(L, w, strip, d);

        // Tab strip
        const float left = edge, right = L.capMin.X - S(8) - btn - gap;
        const float ty = std::floor((strip - tabH) / 2);
        const float avail = std::max(0.f, right - left - gap * (float)(n ? n - 1 : 0));
        float tw = n ? avail / (float)n : 0.f;
        tw = std::max(S(28), std::min(tw, S(200)));
        if (s_frozen) tw = std::min(tw, s_frozenTabW);
        float x = left;
        for (size_t i = 0; i < n; ++i) {
            L.tabs[i] = RectF(std::floor(x), ty, std::floor(tw), tabH);
            x += tw + gap;
        }
        L.newTab = RectF(std::floor(std::min(x, right + gap)), std::floor((strip - btn) / 2), btn, btn);

        // Toolbar
        const float by = strip + std::floor((bar - btn) / 2);
        L.back   = RectF(edge, by, btn, btn);
        L.fwd    = RectF(edge + btn + gap, by, btn, btn);
        L.reload = RectF(edge + (btn + gap) * 2, by, btn, btn);
        L.reloadInPill = false;
        L.more   = RectF(w - edge - btn, by, btn, btn);
        if (dl) L.downloads = RectF(L.more.X - gap - btn, by, btn, btn);
        const float pl = L.reload.GetRight() + S(8), pr = (dl ? L.downloads.X : L.more.X) - S(8);
        L.pill = RectF(std::floor(pl), strip + std::floor((bar - tabH) / 2), std::floor(std::max(S(80), pr - pl)), tabH);
        PlaceStar(L);
    } else {   // vertical
        L.chromeH = bar;
        L.sideW = std::floor(S(d.sideW));
        PlaceCaptionButtons(L, w, bar, d);
        const float by = std::floor((bar - btn) / 2);
        L.back = RectF(edge, by, btn, btn);
        L.fwd  = RectF(edge + btn + gap, by, btn, btn);
        L.more = RectF(L.capMin.X - S(4) - btn, by, btn, btn);
        if (dl) L.downloads = RectF(L.more.X - gap - btn, by, btn, btn);
        const float pl = L.fwd.GetRight() + S(12), pr = (dl ? L.downloads.X : L.more.X) - S(12);
        const float pw = std::max(S(120), std::min(pr - pl, S(640)));
        float px = std::floor(w / 2 - pw / 2);
        px = std::max(pl, std::min(px, pr - pw));
        L.pill = RectF(px, std::floor((bar - tabH) / 2), std::floor(pw), tabH);
        PlaceReloadInPill(L);

        const float rowH = tabH + S(2);
        float y = bar + S(4);
        for (size_t i = 0; i < n; ++i) {
            L.tabs[i] = RectF(S(6), std::floor(y), L.sideW - S(12), rowH);
            y += rowH + S(1);
        }
        L.newTab = RectF(S(6), std::floor(y), L.sideW - S(12), rowH);
    }
    MergeSplitTabs(L);
}

// g_L = s_target, with the tab rectangles (and the compact "+" / address field) taken from the
// animated positions.
static void ApplyCurrent() {
    g_L = s_target;
    const size_t n = std::min(g_tabs.size(), s_target.tabs.size());
    for (size_t i = 0; i < n; ++i) {
        auto it = s_tabCur.find(g_tabs[i]->id);
        if (it != s_tabCur.end()) g_L.tabs[i] = it->second;
    }
    if (g_L.mode != kLayoutVertical) g_L.newTab = s_newTabCur;
    if (g_L.mode == kLayoutCompact && g_active >= 0 && g_active < (int)n) {
        g_L.pill = g_L.tabs[g_active];
        PlaceReloadInPill(g_L);
    }
}

static bool Near(const RectF& a, const RectF& b) {
    return std::fabs(a.X - b.X) < 0.5f && std::fabs(a.Y - b.Y) < 0.5f &&
           std::fabs(a.Width - b.Width) < 0.5f && std::fabs(a.Height - b.Height) < 0.5f;
}

static RectF Lerp(const RectF& a, const RectF& b, float k) {
    return RectF(a.X + (b.X - a.X) * k, a.Y + (b.Y - a.Y) * k,
                 a.Width + (b.Width - a.Width) * k, a.Height + (b.Height - a.Height) * k);
}

void ComputeLayout(int clientW, bool animate) {
    g_clientW = clientW;
    const int oldMode = s_target.mode;
    ComputeTarget(s_target, clientW);
    const bool anim = animate && s_haveCur && oldMode == s_target.mode && AnimTabsEnabled();
    const bool vertical = s_target.mode == kLayoutVertical;

    std::map<unsigned long long, RectF> next;
    bool moving = false;
    for (size_t i = 0; i < g_tabs.size() && i < s_target.tabs.size(); ++i) {
        const RectF& to = s_target.tabs[i];
        auto it = s_tabCur.find(g_tabs[i]->id);
        RectF cur = to;
        if (anim) {
            if (it != s_tabCur.end()) cur = it->second;
            else if (vertical) cur = RectF(to.X, to.Y, to.Width, 0);       // a new tab grows in
            else cur = RectF(to.X, to.Y, 0, to.Height);
        }
        if (!Near(cur, to)) moving = true;
        next[g_tabs[i]->id] = cur;
    }
    s_tabCur.swap(next);
    if (!anim) s_newTabCur = s_target.newTab;
    else if (!Near(s_newTabCur, s_target.newTab)) moving = true;
    s_haveCur = true;
    ApplyCurrent();
    if (moving) AnimRequest();
}

void FreezeTabWidths() {
    if (s_target.mode == kLayoutVertical || g_prefs.tabResizeDelay <= 0) return;
    float tabW = 0, pillW = 0;
    for (size_t i = 0; i < s_target.tabs.size(); ++i) {
        if ((int)i == g_active && s_target.mode == kLayoutCompact) pillW = s_target.tabs[i].Width;
        else tabW = std::max(tabW, s_target.tabs[i].Width);
    }
    if (tabW <= 0) return;
    s_frozenTabW = tabW;
    s_frozenPillW = pillW > 0 ? pillW : 1e6f;
    s_frozen = true;
}

void UnfreezeTabWidths() { s_frozen = false; }
bool TabWidthsFrozen() { return s_frozen; }
float TabStripBottom() {
    if (g_L.mode == kLayoutSeparate) return g_L.captionH;
    return g_L.mode == kLayoutVertical ? 0.f : g_L.chromeH;
}

// ----------------------------------------------------------------------------
//  Hover fades
// ----------------------------------------------------------------------------
void SetHover(const Hit& h) {
    if (h == g_hover) return;
    g_hover = h;
    if (h.kind != Hit::None && AnimHoverEnabled()) {
        bool found = false;
        for (auto& f : s_fades) if (f.hit == h) { found = true; break; }
        if (!found) { HoverFade f; f.hit = h; s_fades.push_back(f); }
    }
    if (AnimHoverEnabled()) AnimRequest();
    InvalidateChrome();
}

// 0..1: how strongly the hover highlight of `h` is shown.
static float HoverAmount(const Hit& h) {
    if (!AnimHoverEnabled()) return (g_hover == h) ? 1.f : 0.f;
    for (auto& f : s_fades) if (f.hit == h) return f.a;
    return 0.f;
}

static Color Faded(const Color& c, float a) {
    return Color((BYTE)std::lround(c.GetA() * std::max(0.f, std::min(1.f, a))), c.GetR(), c.GetG(), c.GetB());
}

// A window button glyph where DWM draws the system one: the same font, but placed by its outline
// (not by the text line), centred on the button at whole pixels the way DWM does: one pixel
// higher in a maximised window, one pixel to the right in a normal one (its 1-px border). Text drawing put it a pixel or two off and blurred its 1-px lines,
// so the two glyphs showed double while the hover faded in or out.
static void DrawSystemCaptionGlyph(Graphics& g, const wchar_t* glyph, const RectF& r, const Color& c, Font* f, bool zoomed) {
    FontFamily family;
    if (!f || f->GetFamily(&family) != Ok) return;
    GraphicsPath path;
    StringFormat sf(StringFormat::GenericTypographic());
    path.AddString(glyph, -1, &family, f->GetStyle(), f->GetSize(), PointF(0, 0), &sf);
    RectF ink;
    if (path.GetBounds(&ink) != Ok || ink.Width <= 0) return;
    const float cx = r.X + std::floor(r.Width / 2) + 0.5f + (zoomed ? 0.f : 1.f);
    const float cy = r.Y + std::floor(r.Height / 2) + 0.5f - (zoomed ? 1.f : 0.f);
    // Centred, with the outline's left and top edges on whole pixels (crisp 1-px lines).
    const float dx = std::round(ink.X + (cx - (ink.X + ink.Width / 2))) - ink.X;
    const float dy = std::round(ink.Y + (cy - (ink.Y + ink.Height / 2))) - ink.Y;
    Matrix m;
    m.Translate(dx, dy);
    path.Transform(&m);
    SolidBrush b(c);
    if (ink.Height < 3) {                                 // "minimise": a crisp line, as DWM draws it
        g.FillRectangle(&b, ink.X + dx, ink.Y + dy, std::round(ink.Width), std::max(1.f, std::floor(ink.Height + 0.25f)));
        return;
    }
    const SmoothingMode old = g.GetSmoothingMode();
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.FillPath(&b, &path);
    g.SetSmoothingMode(old);
}

// `top` painted over the opaque `base` (keeps full opacity).
static Color Over(const Color& base, const Color& top) {
    const float k = top.GetA() / 255.f;
    auto m = [k](BYTE x, BYTE y) { return (BYTE)std::lround(x + ((int)y - (int)x) * k); };
    return Color(255, m(base.GetR(), top.GetR()), m(base.GetG(), top.GetG()), m(base.GetB(), top.GetB()));
}

static Color Mix(const Color& a, const Color& b, float k) {
    k = std::max(0.f, std::min(1.f, k));
    auto m = [k](BYTE x, BYTE y) { return (BYTE)std::lround(x + ((int)y - (int)x) * k); };
    return Color(m(a.GetA(), b.GetA()), m(a.GetR(), b.GetR()), m(a.GetG(), b.GetG()), m(a.GetB(), b.GetB()));
}

static DWORD s_dlAnimAt = 0;                      // when the download start animation began
constexpr DWORD kDlAnimMs = 900;

void ChromeDownloadStarted() {
    if (!g_prefs.animations) return;
    s_dlAnimAt = GetTickCount();
    if (!s_dlAnimAt) s_dlAnimAt = 1;
    AnimRequest();
}

// 0..1 through the download start animation, or -1 when it is not playing.
static float DownloadAnimT() {
    if (!s_dlAnimAt) return -1.f;
    const DWORD el = GetTickCount() - s_dlAnimAt;
    if (el >= kDlAnimMs) { s_dlAnimAt = 0; return -1.f; }
    return (float)el / (float)kDlAnimMs;
}

// The page is loading in the active tab (the address field's line moves).
static bool PageLoadingShown() {
    const Tab* t = ActiveTab();
    return t && (t->loading || t->fade > 0.01f);
}

bool ChromeAnimStep(float dt) {
    bool moving = false;
    // Things that move by themselves: the download start animation, the "size unknown" bar on
    // the downloads button and the running light of the loading line.
    if (DownloadAnimT() >= 0) moving = true;
    if (g_prefs.animations && DownloadsRunning() > 0 && DownloadsProgress() < 0) moving = true;
    if (g_prefs.animations && g_prefs.loadIndicator == 0 && PageLoadingShown()) moving = true;

    // Tabs
    if (g_tabOp) {
        moving = true;                                   // a tab is being created/closed: next frame
    } else if (s_haveCur) {
        const float k = AnimTabsEnabled() ? AnimFactor(dt, kTabTauMs) : 1.f;
        for (size_t i = 0; i < g_tabs.size() && i < s_target.tabs.size(); ++i) {
            auto it = s_tabCur.find(g_tabs[i]->id);
            if (it == s_tabCur.end()) continue;
            const RectF& to = s_target.tabs[i];
            it->second = Near(it->second, to) ? to : Lerp(it->second, to, k);
            if (!Near(it->second, to)) moving = true;
        }
        if (s_target.mode != kLayoutVertical) {
            s_newTabCur = Near(s_newTabCur, s_target.newTab) ? s_target.newTab : Lerp(s_newTabCur, s_target.newTab, k);
            if (!Near(s_newTabCur, s_target.newTab)) moving = true;
        }
        ApplyCurrent();
        if (IsEditing()) PositionEdit();
    }

    // Hover fades
    const float kh = AnimHoverEnabled() ? AnimFactor(dt, kHoverTauMs) : 1.f;
    for (auto& f : s_fades) {
        const float target = (g_hover == f.hit) ? 1.f : 0.f;
        f.a += (target - f.a) * kh;
        if (std::fabs(target - f.a) < 0.01f) f.a = target;
        else moving = true;
    }
    s_fades.erase(std::remove_if(s_fades.begin(), s_fades.end(),
                                 [](const HoverFade& f) { return f.a <= 0.f && !(g_hover == f.hit); }),
                  s_fades.end());

    InvalidateChrome();
    return moving;
}

static bool IsCompactActive(int i) { return g_L.mode == kLayoutCompact && i == g_active; }

static RectF CloseRect(int i) {
    const RectF& r = g_L.tabs[i];
    const float c = std::min(S(18), r.Height - S(6));
    if (IsCompactActive(i)) return RectF(r.X + S(5), r.Y + (r.Height - c) / 2, c, c);
    return RectF(r.GetRight() - S(5) - c, r.Y + (r.Height - c) / 2, c, c);
}

// The bookmark star: for web pages and files, while the address is not being edited.
static bool StarShown() {
    if (g_L.star.Width <= 0 || IsEditing()) return false;
    const Tab* t = ActiveTab();
    return t && CanBookmark(t->url);
}

static bool CloseVisible(int i) {
    if (i < 0 || i >= (int)g_L.tabs.size()) return false;
    const bool overTab = (g_hover.kind == Hit::TabBody || g_hover.kind == Hit::TabClose ||
                          g_hover.kind == Hit::TabAudio) && g_hover.index == i;
    if (IsCompactActive(i))      // Safari: the active tab shows its close button while hovered
        return !IsEditing() && (g_hover.kind == Hit::Pill || g_hover.kind == Hit::Reload || g_hover.kind == Hit::Star ||
                                g_hover.kind == Hit::Key ||
                                ((g_hover.kind == Hit::TabClose || g_hover.kind == Hit::TabAudio) && g_hover.index == i));
    if (g_L.tabs[i].Width < S(60)) return false;
    if (g_L.mode == kLayoutCompact) return overTab || g_prefs.closeAlways;
    return i == g_active || overTab || g_prefs.closeAlways;
}

// The speaker on a tab that plays sound (crossed out when muted); a click mutes / unmutes.
static bool AudioShown(int i) {
    if (i < 0 || i >= (int)g_tabs.size() || i >= (int)g_L.tabs.size()) return false;
    const Tab& t = *g_tabs[i];
    if (!t.audible && !t.muted) return false;
    if (IsCompactActive(i)) return !IsEditing();
    return g_L.tabs[i].Width >= S(60);
}

static RectF AudioRect(int i);

// The key in the address field: the site has saved passwords (a click shows them).
static bool KeyShown() {
    if (IsEditing() || !g_prefs.savePasswords || g_L.star.Width <= 0) return false;
    const Tab* t = ActiveTab();
    return t && PasswordsCountForUrl(t->url) > 0;
}

static RectF KeyRect() {
    const float c = S(22);
    const RectF& pill = g_L.pill;
    float right = StarShown() ? g_L.star.X - S(2) : g_L.reloadInPill ? g_L.reload.X - S(2) : pill.GetRight() - S(6);
    if (IsCompactActive(g_active) && AudioShown(g_active)) right = AudioRect(g_active).X - S(2);
    return RectF(right - c, pill.Y + (pill.Height - c) / 2, c, c);
}

// The book in the address field: the page is an article (reader view), or it is the reader view.
static bool ReaderShown() {
    if (IsEditing() || g_L.star.Width <= 0) return false;
    const Tab* t = ActiveTab();
    return t && (t->readerable || ReaderIsReaderUrl(t->url));
}

static RectF ReaderRect() {
    const float c = S(22);
    const RectF& pill = g_L.pill;
    float right = StarShown() ? g_L.star.X - S(2) : g_L.reloadInPill ? g_L.reload.X - S(2) : pill.GetRight() - S(6);
    if (IsCompactActive(g_active) && AudioShown(g_active)) right = AudioRect(g_active).X - S(2);
    if (KeyShown()) right = KeyRect().X - S(2);
    return RectF(right - c, pill.Y + (pill.Height - c) / 2, c, c);
}

// The translate icon: the page is in another language, or it is shown translated.
static bool TranslateShown() {
    if (IsEditing() || g_L.star.Width <= 0) return false;
    const Tab* t = ActiveTab();
    return t && (t->translatable || t->translated);
}

static RectF TranslateRect() {
    const float c = S(22);
    const RectF& pill = g_L.pill;
    float right = StarShown() ? g_L.star.X - S(2) : g_L.reloadInPill ? g_L.reload.X - S(2) : pill.GetRight() - S(6);
    if (IsCompactActive(g_active) && AudioShown(g_active)) right = AudioRect(g_active).X - S(2);
    if (KeyShown()) right = KeyRect().X - S(2);
    if (ReaderShown()) right = ReaderRect().X - S(2);
    return RectF(right - c, pill.Y + (pill.Height - c) / 2, c, c);
}

static RectF AudioRect(int i) {
    const float c = S(22);
    if (IsCompactActive(i)) {                        // inside the address field, left of the star
        const RectF& pill = g_L.pill;
        const float right = StarShown() ? g_L.star.X - S(2) : g_L.reloadInPill ? g_L.reload.X - S(2) : pill.GetRight() - S(6);
        return RectF(right - c, pill.Y + (pill.Height - c) / 2, c, c);
    }
    const RectF& r = g_L.tabs[i];
    const float c2 = std::min(c, r.Height - S(6));
    const float right = CloseVisible(i) ? CloseRect(i).X - S(1) : r.GetRight() - S(5);
    return RectF(right - c2, r.Y + (r.Height - c2) / 2, c2, c2);
}

Hit HitTest(int px, int py) {
    const float x = (float)px, y = (float)py;
    const Layout& L = g_L;
    const bool inTop = y >= 0 && y < L.chromeH;
    const bool inSide = L.sideW > 0 && y >= L.chromeH && x >= 0 && x < L.sideW;
    if (!inTop && !inSide) return MakeHit(Hit::None);
    if (inTop) {
        if (L.capClose.Contains(x, y)) return MakeHit(Hit::CapClose);
        if (L.capMax.Contains(x, y))   return MakeHit(Hit::CapMax);
        if (L.capMin.Contains(x, y))   return MakeHit(Hit::CapMin);
        if (L.back.Contains(x, y))     return MakeHit(Hit::Back);
        if (L.fwd.Contains(x, y))      return MakeHit(Hit::Forward);
        if (L.more.Contains(x, y))     return MakeHit(Hit::More);
        if (L.downloads.Width > 0 && L.downloads.Contains(x, y)) return MakeHit(Hit::Downloads);
        if (L.mode != kLayoutVertical && L.newTab.Contains(x, y)) return MakeHit(Hit::NewTab);
        if (!(L.reloadInPill && IsEditing()) && L.reload.Contains(x, y)) return MakeHit(Hit::Reload);
        if (StarShown() && L.star.Contains(x, y)) return MakeHit(Hit::Star);
        if (KeyShown() && KeyRect().Contains(x, y)) return MakeHit(Hit::Key);
        if (ReaderShown() && ReaderRect().Contains(x, y)) return MakeHit(Hit::Reader);
        if (TranslateShown() && TranslateRect().Contains(x, y)) return MakeHit(Hit::Translate);
    }
    const int n = (int)std::min(L.tabs.size(), g_tabs.size());
    for (int i = n - 1; i >= 0; --i)
        if (AudioShown(i) && AudioRect(i).Contains(x, y)) return MakeHit(Hit::TabAudio, i);
    for (int i = n - 1; i >= 0; --i)
        if (CloseVisible(i) && CloseRect(i).Contains(x, y)) return MakeHit(Hit::TabClose, i);
    if (inTop && L.pill.Contains(x, y)) return MakeHit(Hit::Pill);
    for (int i = n - 1; i >= 0; --i) {
        const RectF& r = L.tabs[i];
        if (x >= r.X && x < r.GetRight() + S(2) && y >= r.Y && y < r.GetBottom()) return MakeHit(Hit::TabBody, i);
    }
    if (L.mode == kLayoutVertical && L.newTab.Contains(x, y)) return MakeHit(Hit::NewTab);
    return MakeHit(Hit::None);
}

// ----------------------------------------------------------------------------
//  GDI+ resources
// ----------------------------------------------------------------------------
static std::unique_ptr<Font> MakeFont(const wchar_t* const* families, int count, float px, int style = FontStyleRegular) {
    for (int i = 0; i < count; ++i) {
        auto f = std::make_unique<Font>(families[i], px, style, UnitPixel);
        if (f->IsAvailable()) return f;
    }
    return std::make_unique<Font>(L"Segoe UI", px, style, UnitPixel);
}

void RebuildResources() {
    static const wchar_t* ui[]    = { L"Segoe UI Variable Text", L"Segoe UI" };
    static const wchar_t* icons[] = { L"Segoe Fluent Icons", L"Segoe MDL2 Assets" };
    const ChromeDims d = CurrentDims();
    g_fText    = MakeFont(ui, 2, S(d.font + 1));
    g_fTab     = MakeFont(ui, 2, S(d.font));
    g_fTabBold = MakeFont(ui, 2, S(d.font), FontStyleBold);
    g_fIcon    = MakeFont(icons, 2, S(d.icon));
    g_fIconSm  = MakeFont(icons, 2, S(d.icon - 3));
    g_fCaption = MakeFont(icons, 2, S(10));
    g_fBadge   = MakeFont(ui, 2, S(9), FontStyleBold);

    HFONT old = g_editFont;
    g_editFont = CreateFontW(-(int)std::round(S(d.font + 1)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
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

void ReleaseResources() {
    g_fText.reset(); g_fTab.reset(); g_fTabBold.reset(); g_fIcon.reset();
    g_fIconSm.reset(); g_fCaption.reset(); g_fBadge.reset();
}

// ----------------------------------------------------------------------------
//  Painting
// ----------------------------------------------------------------------------
// A small coloured square with the first letter of the site, standing in for a favicon.
static void DrawSiteBadge(Graphics& g, const Tab& t, const RectF& r) {
    static const Color kPalette[] = {
        Color(110, 86, 207), Color(217, 63, 63), Color(91, 104, 120), Color(59, 138, 121),
        Color(47, 111, 214), Color(199, 104, 28), Color(138, 79, 191), Color(30, 136, 168),
    };
    if (Bitmap* icon = FaviconFor(t.url)) {          // the site's own icon once it is known
        const float s = std::floor(S(16));
        g.DrawImage(icon, RectF(std::floor(r.X + (r.Width - s) / 2), std::floor(r.Y + (r.Height - s) / 2), s, s));
        return;
    }
    const std::wstring host = HostOf(t.url);
    wchar_t letter = 0;
    for (wchar_t c : host) if (iswalnum(c)) { letter = (wchar_t)towupper(c); break; }
    unsigned hash = 2166136261u;
    for (wchar_t c : host) hash = (hash ^ (unsigned)c) * 16777619u;
    const Color bg = letter ? kPalette[(hash * 2654435761u) >> 29] : Color(105, 112, 122);   // same as start.html
    FillRound(g, bg, r, S(4));
    if (letter) {
        wchar_t s[2] = { letter, 0 };
        DrawGlyph(g, s, r, g_th.badgeText, g_fBadge.get());
    } else {
        DrawGlyph(g, L"\uE774", r, g_th.badgeText, g_fCaption.get());   // globe
    }
}

// A private tab: an eye crossed out on purple, instead of the site icon.
static const Color kPrivateTint(255, 138, 79, 191);
static void DrawPrivateBadge(Graphics& g, const RectF& r) {
    FillRound(g, kPrivateTint, r, S(4));
    DrawGlyph(g, L"\uED1A", r, Color(255, 255, 255, 255), g_fCaption.get());
}

static void DrawChrome(Graphics& g, int w, int h) {
    const Theme& T = g_th;
    const Layout& L = g_L;
    const float fw = (float)w, fh = (float)h;
    Tab* cur = ActiveTab();
    const bool editing = IsEditing();

    auto hov = [&](Hit::Kind k, int i) { return g_hover == MakeHit(k, i); };
    auto dwn = [&](Hit::Kind k, int i) { return g_down == MakeHit(k, i) && hov(k, i); };
    // Hover background that fades in and out (see SetHover); pressed is immediate.
    auto hoverFill = [&](const RectF& r, const Hit& hit, bool pressed, float rad) {
        if (pressed) { FillRound(g, T.pressed, r, rad); return; }
        const float a = HoverAmount(hit);
        if (a > 0.f) FillRound(g, Faded(T.hover, a), r, rad);
    };
    auto toolButton = [&](const RectF& r, const wchar_t* glyph, bool enabled, Hit::Kind k, Font* f) {
        if (enabled) hoverFill(r, MakeHit(k, -1), dwn(k, -1), S(6));
        DrawGlyph(g, glyph, r, enabled ? T.icon : T.iconDis, f);
    };

    // Backgrounds: chrome, page area (visible only before the first page paints), hairlines
    {
        SolidBrush frame(T.frame), page(T.page);
        if (!s_paintTransparent) {                       // with Mica the backdrop is the frame
            g.FillRectangle(&frame, 0.f, 0.f, fw, L.chromeH);
            if (L.sideW > 0) g.FillRectangle(&frame, 0.f, L.chromeH, L.sideW, fh - L.chromeH);
        }
        g.FillRectangle(&page, L.sideW, L.chromeH, fw - L.sideW, fh - L.chromeH);
        Pen sep(T.sep, 1.f);
        g.DrawLine(&sep, L.sideW, L.chromeH - 0.5f, fw, L.chromeH - 0.5f);
        if (L.sideW > 0) g.DrawLine(&sep, L.sideW - 0.5f, L.chromeH, L.sideW - 0.5f, fh);
        SplitPaint(g);                                   // split view: gutter, outline, divider
        if (cur && cur->pipHwnd && !g_fullScreenView) {      // its view is in the picture-in-picture window
            StringFormat cf(StringFormat::GenericTypographic());
            cf.SetAlignment(StringAlignmentCenter);
            cf.SetLineAlignment(StringAlignmentCenter);
            SolidBrush tb(T.text2);
            g.DrawString(Tr(L"This tab is playing in picture-in-picture.\nClick here to bring it back.",
                            L"Эта вкладка играет в режиме «картинка в картинке».\nНажмите здесь, чтобы вернуть её."),
                         -1, g_fText.get(), RectF(L.sideW, L.chromeH, fw - L.sideW, fh - L.chromeH), &cf, &tb);
        }
    }

    // Window buttons (with Mica DWM draws the system ones)
    // With Mica DWM draws the system buttons underneath the chrome (Lyra's pixels cover them);
    // they show through where the chrome is transparent. Lyra then draws only the hover / pressed
    // state, opaque and fading in, with its own glyph at the same place.
    const bool nativeCaps = NativeCaptionButtons();
    {
        const bool zoomed = g_hWnd && IsZoomed(g_hWnd);
        struct { const RectF* r; Hit::Kind k; const wchar_t* glyph; bool close; } caps[] = {
            { &L.capMin, Hit::CapMin, L"\uE921", false },
            { &L.capMax, Hit::CapMax, zoomed ? L"\uE923" : L"\uE922", false },
            { &L.capClose, Hit::CapClose, L"\uE8BB", true },
        };
        for (auto& c : caps) {
            const bool p = g_down == MakeHit(c.k, -1) && hov(c.k, -1);
            const float a = p ? 1.f : HoverAmount(MakeHit(c.k, -1));
            if (nativeCaps) {
                if (a <= 0.f) continue;
                const Color fill = c.close ? Over(T.frame, p ? T.closePressed : T.closeHover)
                                           : Over(T.frame, p ? T.pressed : T.hover);
                SolidBrush b(Faded(fill, a));
                g.FillRectangle(&b, *c.r);
                // The system glyph's colour: white (black in the light theme) at about 82 %.
                const Color ink = c.close ? Color(255, 255, 255) : (g_dark ? Color(210, 255, 255, 255) : Color(225, 0, 0, 0));
                DrawSystemCaptionGlyph(g, c.glyph, *c.r, Faded(ink, a), g_fCaption.get(), zoomed);
                continue;
            }
            if (a > 0.f) {
                SolidBrush b(c.close ? (p ? T.closePressed : Faded(T.closeHover, a)) : (p ? T.pressed : Faded(T.hover, a)));
                g.FillRectangle(&b, *c.r);
            }
            DrawGlyph(g, c.glyph, *c.r, c.close ? Mix(T.icon, Color(255, 255, 255), a) : T.icon, g_fCaption.get());
        }
    }

    // Navigation
    toolButton(L.back, L"\uE76B", cur && cur->canBack, Hit::Back, g_fIcon.get());
    toolButton(L.fwd,  L"\uE76C", cur && cur->canFwd,  Hit::Forward, g_fIcon.get());
    toolButton(L.more, L"\uE712", true, Hit::More, g_fIcon.get());
    if (L.downloads.Width > 0) {
        // Downloads: the arrow, with a progress bar under it while something downloads; it
        // lights up in the accent colour for a moment when a download finishes.
        const RectF& r = L.downloads;
        hoverFill(r, MakeHit(Hit::Downloads, -1), dwn(Hit::Downloads, -1) || DownloadsPopupVisible(), S(6));
        const bool running = DownloadsRunning() > 0;
        RectF glyph = r;
        if (running) glyph.Y -= S(3);
        const float at = DownloadAnimT();
        if (at >= 0) {
            // A ring spreading out from the button and the arrow dropping in with a small bounce.
            const float ease = 1.f - (1.f - at) * (1.f - at);
            const float rad = r.Width * (0.32f + 0.5f * ease);
            Pen ring(Faded(T.accent, 0.85f * (1.f - at)), std::max(1.5f, S(2.f) * (1.f - at * 0.5f)));
            g.DrawEllipse(&ring, r.X + r.Width / 2 - rad, r.Y + r.Height / 2 - rad, rad * 2, rad * 2);
            float dy;
            if (at < 0.55f) { const float k = 1.f - at / 0.55f; dy = -S(9) * k * k; }
            else dy = -S(2.5f) * std::sin((at - 0.55f) / 0.45f * 3.14159f);
            glyph.Y += dy;
        }
        DrawGlyph(g, L"\uE896", glyph, (running || DownloadsJustFinished() || at >= 0) ? T.accent : T.icon, g_fIcon.get());
        if (running) {
            const float bw = std::floor(r.Width * 0.5f), bh = std::max(2.f, std::floor(S(3)));
            const RectF track(r.X + (r.Width - bw) / 2, r.Y + r.Height * 0.72f, bw, bh);
            FillRound(g, Faded(T.icon, 0.25f), track, bh / 2);
            const float p = DownloadsProgress();
            if (p >= 0) {
                if (p > 0) FillRound(g, T.accent, RectF(track.X, track.Y, std::max(bh, bw * p), bh), bh / 2);
            } else {                                     // size unknown: a segment sliding along
                const float t = (float)(GetTickCount() % 1200) / 1200.f;
                const float sw = bw * 0.35f, sx = track.X + (bw - sw) * (t < 0.5f ? t * 2 : 2 - t * 2);
                FillRound(g, T.accent, RectF(sx, track.Y, sw, bh), bh / 2);
            }
        }
    }
    if (!L.reloadInPill)
        toolButton(L.reload, (cur && cur->loading) ? L"\uE711" : L"\uE72C", cur != nullptr, Hit::Reload, g_fIcon.get());

    // Tabs (in the compact layout the active tab is drawn as the address field below)
    const int n = (int)std::min(L.tabs.size(), g_tabs.size());   // layout may lag behind g_tabs by one frame
    const bool vertical = L.mode == kLayoutVertical;
    StringFormat tf(StringFormat::GenericTypographic());
    tf.SetFormatFlags(tf.GetFormatFlags() | StringFormatFlagsNoWrap);
    tf.SetTrimming(StringTrimmingEllipsisCharacter);
    tf.SetLineAlignment(StringAlignmentCenter);
    int splitL = -1, splitR = -1;
    const bool splitPair = SplitStripPair(&splitL, &splitR) && splitR < n;
    for (int i = 0; i < n; ++i) {
        if (IsCompactActive(i)) continue;
        const RectF& r = L.tabs[i];
        if (r.Width < 1.f || r.Height < 1.f) continue;   // (the right half of a split view: drawn with the left)
        const Tab& tab = *g_tabs[i];
        if (TabDragGhostId() == tab.id && !(splitPair && (i == splitL || i == splitR))) {
            // Lifted by a drag (ui/TabDrag): an empty slot where it was.
            GraphicsPath p;
            AddRound(p, RectF(r.X + 0.5f, r.Y + 0.5f, r.Width - 1, r.Height - 1), S(6));
            Pen pen(Faded(T.text2, 0.45f), 1.f);
            pen.SetDashStyle(DashStyleDash);
            g.DrawPath(&pen, &p);
            continue;
        }
        const bool merged = splitPair && i == splitL;    // the split view's one tab
        const bool act = merged ? (g_active == splitL || g_active == splitR) : (i == g_active);
        const float h = std::max({ HoverAmount(MakeHit(Hit::TabBody, i)), HoverAmount(MakeHit(Hit::TabClose, i)),
                                   HoverAmount(MakeHit(Hit::TabAudio, i)) });
        const float rad = S(6);
        if (act) {
            FillRound(g, vertical ? T.tabActive : T.field, r, rad);
            GraphicsPath p;
            AddRound(p, r, rad);
            Pen pen(T.fieldBorder, 1.f);
            g.DrawPath(&pen, &p);
        } else if (SplitVisible(tab)) {                  // the other half of the split view
            FillRound(g, Faded(vertical ? T.tabActive : T.field, 0.55f), r, rad);
            if (h > 0.f) FillRound(g, Faded(T.hover, h), r, rad);
            GraphicsPath p;
            AddRound(p, r, rad);
            Pen pen(Faded(T.accent, 0.6f), 1.f);
            g.DrawPath(&pen, &p);
        } else if (h > 0.f) {
            FillRound(g, Faded(T.hover, h), r, rad);
        }
        bool dropRight = false;
        if (SplitDropHint(i, &dropRight)) {               // a tab dragged onto this one: split view
            const RectF half = vertical ? RectF(r.X, dropRight ? r.Y + r.Height / 2 : r.Y, r.Width, r.Height / 2)
                                        : RectF(dropRight ? r.X + r.Width / 2 : r.X, r.Y, r.Width / 2, r.Height);
            FillRound(g, Faded(T.accent, 0.35f), half, rad);
            GraphicsPath p;
            AddRound(p, r, rad);
            Pen pen(T.accent, std::max(1.f, std::round(S(1.5f))));
            g.DrawPath(&pen, &p);
        }

        if (merged) {                                    // [left half's page | right half's page]
            {                                            // a light accent outline: it is not an ordinary tab
                GraphicsPath p;
                AddRound(p, RectF(r.X + 0.5f, r.Y + 0.5f, r.Width - 1, r.Height - 1), rad);
                Pen pen(Faded(T.accent, act ? 0.6f : 0.38f), 1.f);
                g.DrawPath(&pen, &p);
            }
            const bool showClose = CloseVisible(i);
            const float right = showClose ? CloseRect(i).X - S(2) : r.GetRight() - S(4);
            const float mid = std::floor(r.X + r.Width / 2);
            const bool shown = act;
            for (int k = 0; k < 2; ++k) {
                const Tab& ht = *g_tabs[k ? splitR : splitL];
                const float hx = k ? mid : r.X, hr = k ? right : mid;
                if (TabDragGhostId() == ht.id) {         // being pulled out: its half is empty
                    GraphicsPath p;
                    AddRound(p, RectF(hx + S(3), r.Y + S(3), hr - hx - S(6), r.Height - S(6)), S(4));
                    Pen pen(Faded(T.text2, 0.45f), 1.f);
                    pen.SetDashStyle(DashStyleDash);
                    g.DrawPath(&pen, &p);
                    continue;
                }
                const bool open = g_tabs[k ? splitR : splitL].get() == ActiveTab();
                const float bs = S(14);
                const RectF badge(std::floor(hx + S(8)), std::floor(r.Y + (r.Height - bs) / 2), bs, bs);
                if (ht.privateTab) DrawPrivateBadge(g, badge);
                else DrawSiteBadge(g, ht, badge);
                const float tx = badge.GetRight() + S(6), tr = hr - S(6);
                if (tr - tx > S(10)) {
                    SolidBrush tb((shown && !open) || ht.sleeping ? T.text2 : T.text);
                    g.DrawString(ht.title.c_str(), -1, (shown && open && !vertical) ? g_fTabBold.get() : g_fTab.get(),
                                 RectF(tx, r.Y, tr - tx, r.Height), &tf, &tb);
                }
                if (shown && open) {                     // which half has the keyboard
                    const float uw = std::max(0.f, hr - hx - S(16));
                    FillRound(g, T.accent, RectF(hx + S(8), r.GetBottom() - S(3.5f), uw, std::max(2.f, std::round(S(2)))), S(1));
                }
            }
            Pen sep(Faded(T.text2, 0.45f), 1.f);
            g.DrawLine(&sep, mid + 0.5f, r.Y + S(8), mid + 0.5f, r.GetBottom() - S(8));
            if (showClose) {
                RectF c = CloseRect(i);
                hoverFill(c, MakeHit(Hit::TabClose, i), dwn(Hit::TabClose, i), S(4));
                DrawGlyph(g, L"\uE711", c, T.text2, g_fCaption.get());
            }
            continue;
        }

        const float bs = S(14);
        const bool badgeOnly = r.Width < S(48);
        const float bx = badgeOnly ? r.X + (r.Width - bs) / 2 : r.X + S(9);
        const RectF badge(std::floor(bx), std::floor(r.Y + (r.Height - bs) / 2), bs, bs);
        if (tab.privateTab) DrawPrivateBadge(g, badge);
        else if (!tab.sleeping) DrawSiteBadge(g, tab, badge);
        else {                                           // asleep: the icon at 45 % (drawn aside, then faded)
            const int pw = (int)std::ceil(badge.Width) + 2, ph = (int)std::ceil(badge.Height) + 2;
            Bitmap icon(pw, ph, PixelFormat32bppPARGB);
            {
                Graphics ig(&icon);
                ig.SetSmoothingMode(SmoothingModeHighQuality);
                ig.SetTextRenderingHint(TextRenderingHintAntiAlias);
                DrawSiteBadge(ig, tab, RectF(1, 1, badge.Width, badge.Height));
            }
            ColorMatrix cm = { { { 1, 0, 0, 0, 0 }, { 0, 1, 0, 0, 0 }, { 0, 0, 1, 0, 0 }, { 0, 0, 0, 0.45f, 0 }, { 0, 0, 0, 0, 1 } } };
            ImageAttributes ia;
            ia.SetColorMatrix(&cm);
            g.DrawImage(&icon, RectF(badge.X - 1, badge.Y - 1, (float)pw, (float)ph), 0, 0, (float)pw, (float)ph, UnitPixel, &ia);
        }

        const bool showClose = CloseVisible(i);
        if (!badgeOnly) {
            const float tx = bx + bs + S(7);
            float tr = showClose ? CloseRect(i).X - S(4) : r.GetRight() - S(8);
            if (AudioShown(i)) tr = AudioRect(i).X - S(2);
            if (tr - tx > S(10)) {
                SolidBrush tb(tab.sleeping ? T.text2 : T.text);
                g.DrawString(tab.title.c_str(), -1, (act && !vertical) ? g_fTabBold.get() : g_fTab.get(),
                             RectF(tx, r.Y, tr - tx, r.Height), &tf, &tb);
            }
        }
        if (showClose) {
            RectF c = CloseRect(i);
            hoverFill(c, MakeHit(Hit::TabClose, i), dwn(Hit::TabClose, i), S(4));
            DrawGlyph(g, L"\uE711", c, T.text2, g_fCaption.get());
        }
        if (AudioShown(i)) {
            RectF a = AudioRect(i);
            hoverFill(a, MakeHit(Hit::TabAudio, i), dwn(Hit::TabAudio, i), S(4));
            DrawGlyph(g, tab.muted ? L"\uE74F" : L"\uE767", a, T.text2, g_fIconSm.get());
        }
    }

    // A tab that has just landed after a drag (ui/TabDrag): a soft ring spreads out of it and fades.
    for (int i = 0; i < n; ++i) {
        const RectF& r = L.tabs[i];
        if (r.Width < 1.f || r.Height < 1.f) continue;
        float t = TabLandPulse(g_tabs[i]->id);
        if (t < 0.f && splitPair && i == splitL) t = TabLandPulse(g_tabs[splitR]->id);
        if (t < 0.f) continue;
        const float e = 1.f - (1.f - t) * (1.f - t) * (1.f - t);          // ease out
        const float grow = S(5) * e;
        const RectF ring(r.X - grow, r.Y - grow, r.Width + grow * 2, r.Height + grow * 2);
        GraphicsPath p;
        AddRound(p, ring, S(6) + grow);
        Pen pen(Faded(T.accent, 0.75f * (1.f - t)), std::max(1.f, S(1.5f)));
        g.DrawPath(&pen, &p);
        FillRound(g, Faded(T.accent, 0.16f * (1.f - e)), r, S(6));
    }

    // New tab
    if (vertical) {
        const RectF& r = L.newTab;
        hoverFill(r, MakeHit(Hit::NewTab, -1), dwn(Hit::NewTab, -1), S(6));
        const float bs = S(14);
        DrawGlyph(g, L"\uE710", RectF(r.X + S(9), r.Y, bs, r.Height), T.text2, g_fIconSm.get());
        SolidBrush tb(T.text2);
        g.DrawString(Tr(L"New tab", L"Новая вкладка"), -1, g_fTab.get(),
                     RectF(r.X + S(9) + bs + S(7), r.Y, r.Width - bs - S(24), r.Height), &tf, &tb);
    } else {
        toolButton(L.newTab, L"\uE710", (int)g_tabs.size() < metrics::kMaxTabs, Hit::NewTab, g_fIcon.get());
    }

    // Address field
    {
        const RectF& pill = L.pill;
        const float rad = S(7);
        float pillHover = HoverAmount(MakeHit(Hit::Pill, -1));
        if (L.reloadInPill) pillHover = std::max(pillHover, HoverAmount(MakeHit(Hit::Reload, -1)));
        pillHover = std::max(pillHover, HoverAmount(MakeHit(Hit::Star, -1)));
        pillHover = std::max(pillHover, HoverAmount(MakeHit(Hit::Key, -1)));
        pillHover = std::max(pillHover, HoverAmount(MakeHit(Hit::Reader, -1)));
        pillHover = std::max(pillHover, HoverAmount(MakeHit(Hit::Translate, -1)));
        if (IsCompactActive(g_active)) pillHover = std::max(pillHover, HoverAmount(MakeHit(Hit::TabClose, g_active)));
        FillRound(g, editing ? T.pillFocus : Mix(T.pill, T.pillHover, pillHover), pill, rad);

        // Page loading: a glowing line along the bottom of the field with a light running along
        // it (Settings > Appearance), or the field filling up like Safari.
        if (!editing && cur && cur->fade > 0.01f && cur->progress > 0.f) {
            GraphicsPath path;
            AddRound(path, pill, rad);
            g.SetClip(&path);
            const float p = std::min(1.f, cur->progress), w = pill.Width * p;
            if (g_prefs.loadIndicator == 1) {
                Color a((BYTE)(T.progressAlpha * cur->fade), T.accent.GetR(), T.accent.GetG(), T.accent.GetB());
                SolidBrush b(a);
                g.FillRectangle(&b, pill.X, pill.Y, w, pill.Height);
            } else if (w > 1.f) {
                const float lineH = std::max(1.f, std::round(S(1.5f))), glowH = std::floor(S(7));
                const float bottom = pill.GetBottom();
                const Color& ac = T.accent;
                // glow rising from the line
                {
                    const RectF gr(pill.X, bottom - glowH, w, glowH);
                    LinearGradientBrush glow(PointF(0, gr.Y - 1), PointF(0, gr.GetBottom()),
                                             Color(0, ac.GetR(), ac.GetG(), ac.GetB()),
                                             Color((BYTE)(90 * cur->fade), ac.GetR(), ac.GetG(), ac.GetB()));
                    g.FillRectangle(&glow, gr);
                }
                // the line, brighter towards its head
                {
                    const RectF lr(pill.X, bottom - lineH, w, lineH);
                    LinearGradientBrush line(PointF(lr.X - 1, 0), PointF(lr.GetRight() + 1, 0),
                                             Color((BYTE)(110 * cur->fade), ac.GetR(), ac.GetG(), ac.GetB()),
                                             Color((BYTE)(255 * cur->fade), ac.GetR(), ac.GetG(), ac.GetB()));
                    g.FillRectangle(&line, lr);
                }
                // a light running along the line
                if (g_prefs.animations && w > S(40)) {
                    const float period = 1300.f;
                    const float t = (float)(GetTickCount() % (DWORD)period) / period;
                    const float sw = std::min(w * 0.5f, S(90));
                    const float cx = pill.X - sw + (w + sw) * t;
                    const Color white((BYTE)(230 * cur->fade), 255, 255, 255), clear(0, 255, 255, 255);
                    LinearGradientBrush run(PointF(cx - 1, 0), PointF(cx + sw + 1, 0), clear, clear);
                    Color cols[3] = { clear, white, clear };
                    REAL pos[3] = { 0.f, 0.5f, 1.f };
                    run.SetInterpolationColors(cols, pos, 3);
                    g.SetClip(RectF(pill.X, bottom - glowH, w, glowH), CombineModeIntersect);
                    g.FillRectangle(&run, RectF(cx, bottom - lineH - S(1), sw, lineH + S(1)));
                }
            }
            g.ResetClip();
        }

        {
            RectF ring = pill;
            GraphicsPath p;
            if (editing) {
                ring.Inflate(S(1), S(1));
                AddRound(p, ring, rad + S(1));
                Pen pen(T.accent, S(2));
                g.DrawPath(&pen, &p);
            } else {
                AddRound(p, ring, rad);
                Pen pen(T.fieldBorder, 1.f);
                g.DrawPath(&pen, &p);
            }
        }

        if (!editing) {
            // Left: the active tab's close button (compact, while hovered) or a search glyph
            const bool closeHere = IsCompactActive(g_active) && CloseVisible(g_active);
            if (closeHere) {
                RectF c = CloseRect(g_active);
                hoverFill(c, MakeHit(Hit::TabClose, g_active), dwn(Hit::TabClose, g_active), S(4));
                DrawGlyph(g, L"\uE711", c, T.text2, g_fCaption.get());
            } else {
                const float s = std::min(S(18), pill.Height - S(6));
                const RectF gr(pill.X + S(5), pill.Y + (pill.Height - s) / 2, s, s);
                if (cur && cur->privateTab) DrawGlyph(g, L"\uED1A", gr, kPrivateTint, g_fIconSm.get());
                else DrawGlyph(g, L"\uE721", gr, T.text2, g_fIconSm.get());
            }

            const std::wstring shownUrl = cur ? ReaderShownUrl(cur->url) : std::wstring();
            std::wstring label = cur ? (g_prefs.showFullUrl ? DisplayUrl(shownUrl) : HostOf(shownUrl)) : L"";
            const bool placeholder = label.empty();
            if (placeholder) label = (cur && cur->privateTab)
                ? Tr(L"Private tab \x2014 search or enter address", L"Приватная вкладка \x2014 поиск или адрес")
                : Tr(L"Search or enter address", L"Поиск или адрес сайта");
            const bool secure = cur && shownUrl.compare(0, 8, L"https://") == 0;

            const float textL = pill.X + S(28);
            const bool star = StarShown();
            float textR = star ? L.star.X - S(2) : L.reloadInPill ? L.reload.X - S(4) : pill.GetRight() - S(10);
            const bool audioHere = IsCompactActive(g_active) && AudioShown(g_active);
            if (audioHere) textR = AudioRect(g_active).X - S(2);
            const bool keyHere = KeyShown();
            if (keyHere) textR = std::min(textR, KeyRect().X - S(2));
            const bool readerHere = ReaderShown();
            if (readerHere) textR = std::min(textR, ReaderRect().X - S(2));
            const bool translateHere = TranslateShown();
            if (translateHere) textR = std::min(textR, TranslateRect().X - S(2));
            const float maxText = std::max(0.f, textR - textL);
            const float lockW = secure ? S(13) : 0.f, lockGap = secure ? S(4) : 0.f;

            StringFormat fmt(StringFormat::GenericTypographic());
            fmt.SetFormatFlags(fmt.GetFormatFlags() | StringFormatFlagsNoWrap);
            fmt.SetTrimming(StringTrimmingEllipsisCharacter);
            fmt.SetLineAlignment(StringAlignmentCenter);

            RectF m;
            g.MeasureString(label.c_str(), -1, g_fText.get(), RectF(0, 0, 10000, 1000), &fmt, &m);
            const float tw = std::max(0.f, std::min(m.Width, maxText - lockW - lockGap));
            const float total = lockW + lockGap + tw;
            float x0 = pill.X + pill.Width / 2 - total / 2;
            x0 = std::max(textL, std::min(x0, textR - total));

            if (secure)
                DrawGlyph(g, L"\uE72E", RectF(x0, pill.Y, lockW, pill.Height), T.text2, g_fCaption.get());
            SolidBrush tb(placeholder ? T.text2 : T.text);
            g.DrawString(label.c_str(), -1, g_fText.get(),
                         RectF(x0 + lockW + lockGap, pill.Y, tw + 1.f, pill.Height), &fmt, &tb);

            if (L.reloadInPill)
                toolButton(L.reload, (cur && cur->loading) ? L"\uE711" : L"\uE72C", cur != nullptr, Hit::Reload, g_fIconSm.get());
            if (star) {
                const bool marked = IsBookmarkedCached(cur->url);
                hoverFill(L.star, MakeHit(Hit::Star, -1), dwn(Hit::Star, -1) || BookmarkPopupVisible(), S(6));
                DrawGlyph(g, marked ? L"\uE735" : L"\uE734", L.star, marked ? T.accent : T.icon, g_fIconSm.get());
            }
            if (audioHere) {
                RectF a = AudioRect(g_active);
                hoverFill(a, MakeHit(Hit::TabAudio, g_active), dwn(Hit::TabAudio, g_active), S(6));
                DrawGlyph(g, g_tabs[g_active]->muted ? L"\uE74F" : L"\uE767", a, T.icon, g_fIconSm.get());
            }
            if (keyHere) {
                const RectF k = KeyRect();
                hoverFill(k, MakeHit(Hit::Key, -1), dwn(Hit::Key, -1), S(6));
                DrawGlyph(g, L"\uE8D7", k, T.icon, g_fIconSm.get());
            }
            if (readerHere) {
                const RectF rr = ReaderRect();
                const bool on = ReaderIsReaderUrl(cur->url);
                hoverFill(rr, MakeHit(Hit::Reader, -1), dwn(Hit::Reader, -1), S(6));
                DrawGlyph(g, L"\uE736", rr, on ? T.accent : T.icon, g_fIconSm.get());
            }
            if (translateHere) {
                const RectF tr = TranslateRect();
                hoverFill(tr, MakeHit(Hit::Translate, -1), dwn(Hit::Translate, -1), S(6));
                DrawGlyph(g, L"\uE8C1", tr, cur->translated ? T.accent : T.icon, g_fIconSm.get());
            }
        }
    }
}

void UpdateMainBackdrop() {
    if (!g_hWnd) return;
    MARGINS m = { 0, 0, 0, 0 };
    if (!g_fullScreenView) {
        m.cxLeftWidth = (int)std::ceil(g_L.sideW);
        m.cyTopHeight = (int)std::ceil(g_L.chromeH);
    }
    ApplyBackdrop(g_hWnd, 2 /* DWMSBT_MAINWINDOW: Mica */, m);
    // Switching between Lyra's window buttons and the system ones changes the space they take.
    static int s_native = -1;
    const int native = NativeCaptionButtons() ? 1 : 0;
    if (native != s_native) {
        const bool first = s_native < 0;
        s_native = native;
        if (!first || native) {
            ComputeLayout(g_clientW);
            InvalidateChrome();
        }
    }
}

void PaintChrome(HDC hdc, const RECT& rc) {
    if (!g_hWnd) return;
    RECT cr;
    GetClientRect(g_hWnd, &cr);
    s_paintTransparent = MicaActive() && !g_fullScreenView;
    PaintBuffered(hdc, rc, !s_paintTransparent, [&](Graphics& g) {
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        DrawChrome(g, cr.right, cr.bottom);
    });
}

int SplitTabAt(int index, int x, int y) {
    int li = -1, ri = -1;
    if (!SplitStripPair(&li, &ri) || index != li || index >= (int)g_L.tabs.size()) return index;
    if (g_L.mode == kLayoutCompact && (g_active == li || g_active == ri)) return index;
    const RectF& r = g_L.tabs[index];
    (void)y;
    return (float)x >= std::floor(r.X + r.Width / 2) ? ri : li;
}

void DrawTabFace(Graphics& g, const Tab& t, const RectF& r) {
    const Theme& T = g_th;
    const float rad = S(6);
    FillRound(g, g_L.mode == kLayoutVertical ? T.tabActive : T.field, r, rad);
    GraphicsPath p;
    AddRound(p, r, rad);
    Pen pen(T.fieldBorder, 1.f);
    g.DrawPath(&pen, &p);
    const float bs = S(14);
    const RectF badge(std::floor(r.X + S(9)), std::floor(r.Y + (r.Height - bs) / 2), bs, bs);
    if (t.privateTab) DrawPrivateBadge(g, badge);
    else DrawSiteBadge(g, t, badge);
    StringFormat tf(StringFormat::GenericTypographic());
    tf.SetFormatFlags(tf.GetFormatFlags() | StringFormatFlagsNoWrap);
    tf.SetTrimming(StringTrimmingEllipsisCharacter);
    tf.SetLineAlignment(StringAlignmentCenter);
    const float tx = badge.GetRight() + S(7), tr = r.GetRight() - S(10);
    if (tr - tx > S(10)) {
        SolidBrush tb(T.text);
        g.DrawString(t.title.c_str(), -1, g_fTab.get(), RectF(tx, r.Y, tr - tx, r.Height), &tf, &tb);
    }
}
