// Lyra — настройка интерфейса прямо в окне (см. Customize.h, UiLayout.h).
//
// While customizing, the chrome shows every button (also the ones that only appear sometimes),
// a dashed outline around each zone and, under the toolbar, a row of the hidden buttons with
// "Done". Buttons are dragged between zones and into or out of that row; a click selects one.
// A panel at the right (resources/customize.html in a small window of its own, talking to Lyra
// like the settings window does) holds the layout's name, sizes, radii, borders, colours, the
// font and the selected button's own size and colour:
//   cust-ready               the page is up: send the model
//   cust-set?k=&v=           a style value (number, separators 0/1, fontFamily)
//   cust-color?k=&v=         a colour ("" = the theme's)
//   cust-item?k=&v=          the selected button: size, color, hide
//   cust-sel?i=              select a button (-1: none)
//   cust-name?s=             rename the layout
//   cust-pick?id=            switch to another layout ("builtin:N" or a custom one's id)
//   cust-delete              delete the active custom layout
//   cust-done                end customizing
// A built-in layout is never changed: the first change makes "My layout" (UiEdit).
#include "ui/Customize.h"
#include "app/Log.h"
#include "browser/WebContext.h"
#include "core/Localization.h"
#include "core/Settings.h"
#include "core/StringUtils.h"
#include "core/Url.h"
#include "ui/Chrome.h"
#include "ui/Draw.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/UiLayout.h"
#include <set>

static bool s_active = false;
static int  s_sel = -1;                              // the selected item, -1: none
static int  s_hover = -1;                            // the item under the pointer

// A press on a button: a drag once the pointer has moved a little.
static int    s_pressItem = -1;
static POINT  s_pressPt = {};
static bool   s_dragging = false;
static POINT  s_dragPt = {};
struct Drop { int zone = -2; int index = 0; RectF bar; };   // zone -1: the palette, -2: nowhere
static Drop   s_drop;

// ---- the panel window ------------------------------------------------------------------------------
static const wchar_t kPanelClass[] = L"LyraCustomizePanel";
static constexpr UINT WM_CUST_COMMAND = WM_APP + 1;  // lParam: new std::wstring (a lyra:// address)
static HWND      s_panel = nullptr;
static WKViewRef s_view = nullptr;
static WKPageRef s_page = nullptr;
static HWND      s_viewHwnd = nullptr;
static bool      s_ready = false;

static Color Alpha(const Color& c, float a) { return Color((BYTE)std::lround(c.GetA() * a), c.GetR(), c.GetG(), c.GetB()); }

static const Layout::Slot* SlotOf(int item) {
    for (const Layout::Slot& s : g_L.slots) if (s.item == item) return &s;
    return nullptr;
}

static const Layout::Slot* SlotAt(float x, float y) {
    for (const Layout::Slot& s : g_L.slots) {
        RectF r = s.r;
        r.Inflate(S(1), S(1));
        if (r.Contains(x, y)) return &s;
    }
    return nullptr;
}

static bool InChrome(int x, int y) {
    return (y >= 0 && y < (int)g_L.chromeH && x >= 0 && x < g_clientW) ||
           (g_L.sideW > 0 && x >= 0 && x < (int)g_L.sideW && y >= 0);
}

// ---- moving items ----------------------------------------------------------------------------------
// Where the dragged item would go if dropped at (x, y): the nearest zone that takes it, and the
// place among that zone's items (the dragged one left out).
static Drop FindDrop(int item, float x, float y) {
    Drop d;
    const Layout& L = g_L;
    if (L.palette.Width > 0 && L.palette.Contains(x, y)) { d.zone = -1; return d; }
    float best = S(56);
    for (int z = 0; z < kZoneCount; ++z) {
        if (!UiZoneInMode(z, L.mode) || !UiItemAllowedIn(item, z)) continue;
        const RectF& r = L.zoneRect[z];
        if (r.Width <= 0 || r.Height <= 0) continue;
        const float dx = std::max({ r.X - x, 0.f, x - r.GetRight() }), dy = std::max({ r.Y - y, 0.f, y - r.GetBottom() });
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (dist < best) { best = dist; d.zone = z; }
    }
    if (d.zone < 0) { d.zone = -2; return d; }

    std::vector<RectF> order;                        // the zone's other items, in their order
    for (int it : UiActive().zones[d.zone]) {
        if (it == item) continue;
        for (const Layout::Slot& s : L.slots) if (s.item == it && s.zone == d.zone) { order.push_back(s.r); break; }
    }
    const bool rows = L.mode == kLayoutVertical && d.zone == kZoneTabEnd;   // "New tab" is a row there
    for (const RectF& r : order) {
        const bool before = rows ? (y > r.GetBottom() || (y >= r.Y && x > r.X + r.Width / 2)) : x > r.X + r.Width / 2;
        if (before) ++d.index;
    }
    const float t = std::max(2.f, std::round(S(3)));
    if (order.empty()) {
        const RectF& z = L.zoneRect[d.zone];
        d.bar = RectF(z.X + z.Width / 2 - t / 2, z.Y, t, z.Height);
    } else {
        const bool after = d.index >= (int)order.size();
        const RectF& r = after ? order.back() : order[d.index];
        if (r.Width > r.Height * 3)                  // a row: a line above or under it
            d.bar = RectF(r.X, (after ? r.GetBottom() + S(1) : r.Y - S(1)) - t / 2, r.Width, t);
        else
            d.bar = RectF((after ? r.GetRight() + S(1) : r.X - S(1)) - t / 2, r.Y, t, r.Height);
    }
    return d;
}

// Puts `item` into zone `zone` at `index` (-1: hides it). Nothing changes when it is already there.
static void MoveItem(int item, int zone, int index) {
    UiLayoutDef next = UiActive();
    for (int z = 0; z < kZoneCount; ++z) {
        auto& v = next.zones[z];
        v.erase(std::remove(v.begin(), v.end(), item), v.end());
    }
    if (zone >= 0) {
        auto& v = next.zones[zone];
        v.insert(v.begin() + std::max(0, std::min(index, (int)v.size())), item);
    }
    const UiLayoutDef& cur = UiActive();
    bool same = true;
    for (int z = 0; z < kZoneCount; ++z) if (next.zones[z] != cur.zones[z]) same = false;
    if (same) return;
    UiLayoutDef& e = UiEdit();
    for (int z = 0; z < kZoneCount; ++z) e.zones[z] = next.zones[z];
    UiChanged(kUiLayoutOnly);
}

// ---- the panel: model and commands -------------------------------------------------------------
static std::wstring Hex(const Color& c) {
    wchar_t s[8];
    swprintf(s, 8, L"#%02X%02X%02X", c.GetR(), c.GetG(), c.GetB());
    return s;
}

// A colour for CSS: with its transparency when it has some (the hover colour).
static std::wstring Css(const Color& c) {
    if (c.GetA() == 255) return Hex(c);
    wchar_t s[48];
    swprintf(s, 48, L"rgba(%d,%d,%d,%.2f)", c.GetR(), c.GetG(), c.GetB(), c.GetA() / 255.0);
    return s;
}

static std::wstring Num(float v) {
    wchar_t s[32];
    swprintf(s, 32, L"%g", v);
    return s;
}

static const std::vector<std::wstring>& FontFamilies() {
    static std::vector<std::wstring> v;
    if (!v.empty()) return v;
    std::set<std::wstring> names;
    HDC dc = GetDC(nullptr);
    LOGFONTW lf = {};
    lf.lfCharSet = DEFAULT_CHARSET;
    EnumFontFamiliesExW(dc, &lf, [](const LOGFONTW* l, const TEXTMETRICW*, DWORD, LPARAM p) -> int {
        if (l->lfFaceName[0] && l->lfFaceName[0] != L'@') ((std::set<std::wstring>*)p)->insert(l->lfFaceName);
        return 1;
    }, (LPARAM)&names, 0);
    ReleaseDC(nullptr, dc);
    v.assign(names.begin(), names.end());
    return v;
}

static const char* const kColorKeys[kColCount] = { "frame", "field", "fieldBorder", "tabActive", "text", "text2", "icon", "accent", "hover" };

// The colour the chrome shows now for each key (with the layout's own over the theme's).
static Color Effective(int c) {
    const Theme& T = g_th;
    switch (c) {
    case kColFrame: return T.frame;
    case kColField: return T.field;
    case kColFieldBorder: return T.fieldBorder;
    case kColTabActive: return T.tabActive;
    case kColText: return T.text;
    case kColText2: return T.text2;
    case kColIcon: return T.icon;
    case kColAccent: return T.accent;
    case kColHover: return T.hover;
    }
    return Color();
}

static std::wstring ModelJson() {
    const UiLayoutDef& d = UiActive();
    const UiStyle& s = d.style;
    std::wstring j = L"{\"ru\":" + std::wstring(UiRussian() ? L"true" : L"false");
    j += L",\"dark\":" + std::wstring(ResolveDark() ? L"true" : L"false");
    j += L",\"accent\":" + JsString(Hex(g_th.accent));
    j += L",\"id\":" + JsString(d.id) + L",\"name\":" + JsString(d.name);
    j += L",\"builtin\":" + std::wstring(UiActiveIsBuiltin() ? L"true" : L"false");
    j += L",\"mode\":" + std::to_wstring(d.mode);
    j += L",\"layouts\":[";
    for (int m = 0; m < 3; ++m) {
        if (m) j += L",";
        j += L"{\"id\":" + JsString(L"builtin:" + std::to_wstring(m)) + L",\"n\":" + JsString(UiBuiltin(m).name) + L",\"b\":true}";
    }
    for (const UiLayoutDef& c : UiCustomLayouts()) j += L",{\"id\":" + JsString(c.id) + L",\"n\":" + JsString(c.name) + L",\"b\":false}";
    j += L"]";
    j += L",\"style\":{\"bar\":" + Num(s.bar) + L",\"strip\":" + Num(s.strip) + L",\"btn\":" + Num(s.btn) + L",\"tab\":" + Num(s.tab) +
         L",\"icon\":" + Num(s.icon) + L",\"font\":" + Num(s.font) + L",\"sideW\":" + Num(s.sideW) +
         L",\"tabMaxW\":" + Num(s.tabMaxW) + L",\"fieldMaxW\":" + Num(s.fieldMaxW) +
         L",\"tabRadius\":" + Num(s.tabRadius) + L",\"fieldRadius\":" + Num(s.fieldRadius) + L",\"btnRadius\":" + Num(s.btnRadius) +
         L",\"fieldBorder\":" + Num(s.fieldBorder) + L",\"tabBorder\":" + Num(s.tabBorder) +
         L",\"separators\":" + std::wstring(s.separators ? L"1" : L"0") + L",\"fontFamily\":" + JsString(s.fontFamily) + L"}";
    j += L",\"colors\":[";
    for (int c = 0; c < kColCount; ++c) {
        if (c) j += L",";
        j += L"{\"k\":\"" + ToWide(kColorKeys[c]) + L"\",\"n\":" + JsString(UiColorName(c)) +
             L",\"v\":" + JsString(s.colors[c].GetA() ? Hex(s.colors[c]) : L"") + L",\"e\":" + JsString(Css(Effective(c))) + L"}";
    }
    j += L"]";
    j += L",\"sel\":" + std::to_wstring(s_sel);
    if (s_sel >= 0) {
        const UiItemStyle& is = d.items[s_sel];
        j += L",\"item\":{\"n\":" + JsString(UiItemName(s_sel)) + L",\"g\":" + JsString(UiItemGlyph(s_sel)) +
             L",\"size\":" + Num(is.size) + L",\"color\":" + JsString(is.color.GetA() ? Hex(is.color) : L"") +
             L",\"hidden\":" + std::wstring(UiItemZone(d, s_sel) < 0 ? L"true" : L"false") + L"}";
    }
    j += L",\"fonts\":[";
    const auto& fonts = FontFamilies();
    for (size_t i = 0; i < fonts.size(); ++i) { if (i) j += L","; j += JsString(fonts[i]); }
    return j + L"]}";
}

static void IgnoreResult(WKTypeRef, WKErrorRef, void*) {}

static void Push() {
    CustomizeReposition();                           // (the toolbar may have grown or shrunk)
    if (!s_ready || !s_page) return;
    const std::string script = "window.lyraCustomize && lyraCustomize(" + ToUtf8(ModelJson()) + ")";
    if (WKStringRef s = WKStringCreateWithUTF8CString(script.c_str())) {
        WKPageEvaluateJavaScriptInMainFrame(s_page, s, nullptr, IgnoreResult);
        WKRelease(s);
    }
}

static void Select(int item) {
    if (item == s_sel) return;
    s_sel = item;
    InvalidateChrome();
    Push();
}

static std::wstring Param(const std::wstring& query, const wchar_t* name) {
    const std::wstring key = std::wstring(name) + L"=";
    size_t p = 0;
    while (p < query.size()) {
        size_t e = query.find(L'&', p);
        if (e == std::wstring::npos) e = query.size();
        if (query.compare(p, key.size(), key) == 0) return UrlDecode(query.substr(p + key.size(), e - p - key.size()));
        p = e + 1;
    }
    return std::wstring();
}

static Color ParseHex(const std::wstring& v) {
    if (v.size() != 7 || v[0] != L'#') return Color(0, 0, 0, 0);
    const unsigned n = (unsigned)wcstoul(v.c_str() + 1, nullptr, 16);
    return Color(255, (BYTE)(n >> 16), (BYTE)(n >> 8), (BYTE)n);
}

// A style value by its key: a pointer to it with its limits.
struct StyleKey { const wchar_t* key; float UiStyle::* field; float lo, hi; UiChange change; };
static const StyleKey kStyleKeys[] = {
    { L"bar", &UiStyle::bar, 28, 72, kUiLayoutOnly },        { L"strip", &UiStyle::strip, 26, 64, kUiLayoutOnly },
    { L"btn", &UiStyle::btn, 20, 56, kUiLayoutOnly },        { L"tab", &UiStyle::tab, 20, 56, kUiLayoutOnly },
    { L"icon", &UiStyle::icon, 10, 24, kUiFonts },           { L"font", &UiStyle::font, 9, 18, kUiFonts },
    { L"sideW", &UiStyle::sideW, 160, 420, kUiLayoutOnly },  { L"tabMaxW", &UiStyle::tabMaxW, 80, 360, kUiLayoutOnly },
    { L"fieldMaxW", &UiStyle::fieldMaxW, 0, 2000, kUiLayoutOnly },
    { L"tabRadius", &UiStyle::tabRadius, 0, 20, kUiLayoutOnly }, { L"fieldRadius", &UiStyle::fieldRadius, 0, 24, kUiLayoutOnly },
    { L"btnRadius", &UiStyle::btnRadius, 0, 20, kUiLayoutOnly },
    { L"fieldBorder", &UiStyle::fieldBorder, 0, 4, kUiLayoutOnly }, { L"tabBorder", &UiStyle::tabBorder, 0, 4, kUiLayoutOnly },
};

static void OnCommand(const std::wstring& url) {
    const size_t q = url.find(L'?');
    std::wstring command = url.substr(0, q);
    const std::wstring query = q == std::wstring::npos ? L"" : url.substr(q + 1);
    while (!command.empty() && command.back() == L'/') command.pop_back();
    CharLowerBuffW(&command[0], (DWORD)command.size());
    const std::wstring k = Param(query, L"k"), v = Param(query, L"v");

    if (command == L"lyra://cust-ready") {
        s_ready = true;
        if (s_panel && !IsWindowVisible(s_panel)) ShowWindow(s_panel, SW_SHOWNOACTIVATE);
    } else if (command == L"lyra://cust-set") {
        if (k == L"separators") {
            UiEdit().style.separators = v == L"1";
            UiChanged(kUiLayoutOnly);
        } else if (k == L"fontFamily") {
            UiEdit().style.fontFamily = v.substr(0, 64);
            UiChanged(kUiFonts);
        } else {
            for (const StyleKey& sk : kStyleKeys) {
                if (k != sk.key) continue;
                const float val = std::clamp((float)_wtof(v.c_str()), sk.lo, sk.hi);
                if (UiActive().style.*sk.field == val) break;
                UiStyle& st = UiEdit().style;
                st.*sk.field = val;
                st.tab = std::min(st.tab, st.bar);           // the field and the tabs fit in their row
                st.btn = std::min(st.btn, st.bar);
                UiChanged(sk.change);
                break;
            }
        }
    } else if (command == L"lyra://cust-color") {
        for (int c = 0; c < kColCount; ++c) {
            if (k != ToWide(kColorKeys[c])) continue;
            const Color col = ParseHex(v);
            const Color& was = UiActive().style.colors[c];
            if (was.GetValue() == col.GetValue() || (!was.GetA() && !col.GetA())) break;
            UiEdit().style.colors[c] = col;
            UiChanged(kUiColors);
            break;
        }
    } else if (command == L"lyra://cust-item") {
        if (s_sel >= 0 && s_sel < kItemCount) {
            if (k == L"hide") {
                MoveItem(s_sel, -1, 0);
            } else if (k == L"size") {
                float val = (float)_wtof(v.c_str());
                val = val <= 0 ? 0.f : std::clamp(val, 16.f, 56.f);
                if (UiActive().items[s_sel].size != val) { UiEdit().items[s_sel].size = val; UiChanged(kUiLayoutOnly); }
            } else if (k == L"color") {
                const Color col = ParseHex(v);
                if (UiActive().items[s_sel].color.GetValue() != col.GetValue()) { UiEdit().items[s_sel].color = col; UiChanged(kUiLayoutOnly); }
            }
        }
    } else if (command == L"lyra://cust-sel") {
        const int i = _wtoi(Param(query, L"i").c_str());
        Select(i >= 0 && i < kItemCount ? i : -1);
    } else if (command == L"lyra://cust-name") {
        UiEdit();                                    // (a built-in one: renaming makes the copy)
        UiRename(Param(query, L"s"));
    } else if (command == L"lyra://cust-pick") {
        const std::wstring id = Param(query, L"id");
        if (id.compare(0, 8, L"builtin:") == 0) UiSelectBuiltin(_wtoi(id.c_str() + 8));
        else UiSelectCustom(id);
        s_sel = -1;
        if (s_active) RefreshLayout();
    } else if (command == L"lyra://cust-delete") {
        if (!UiActiveIsBuiltin()) UiDelete(UiActive().id);
        s_sel = -1;
    } else if (command == L"lyra://cust-done") {
        CustomizeEnd();
        return;
    } else {
        Log("customize: unknown command %ls", url.c_str());
    }
    Push();
}

static void OnDecidePolicy(WKPageRef, WKNavigationActionRef action, WKFramePolicyListenerRef listener, WKTypeRef, const void*) {
    std::wstring url;
    if (WKURLRequestRef req = WKNavigationActionCopyRequest(action)) {
        if (WKURLRef u = WKURLRequestCopyURL(req)) {
            if (WKStringRef s = WKURLCopyString(u)) { url = WKStr(s); WKRelease(s); }
            WKRelease(u);
        }
        WKRelease(req);
    }
    if (_wcsnicmp(url.c_str(), L"lyra:", 5) == 0) {
        WKFramePolicyListenerIgnore(listener);
        if (s_panel) PostMessageW(s_panel, WM_CUST_COMMAND, 0, (LPARAM)new std::wstring(url));
        return;
    }
    if (_wcsnicmp(url.c_str(), L"file:", 5) == 0 || _wcsnicmp(url.c_str(), L"about:", 6) == 0) WKFramePolicyListenerUse(listener);
    else WKFramePolicyListenerIgnore(listener);
}

static std::wstring PanelUrl() {
    std::wstring url = StartPageFileUrl();
    const size_t slash = url.rfind(L'/');
    if (slash != std::wstring::npos) url = url.substr(0, slash + 1) + L"customize.html";
    url += L"?dark=" + std::wstring(ResolveDark() ? L"1" : L"0");
    url += L"&ru=" + std::wstring(UiRussian() ? L"1" : L"0");
    return url;
}

static void CreateView() {
    RECT cr;
    GetClientRect(s_panel, &cr);
    WKPageConfigurationRef conf = WKPageConfigurationCreate();
    WKPageConfigurationSetContext(conf, g_context);
    static WKPreferencesRef prefs = [] {
        WKPreferencesRef p = WKPreferencesCreate();
        WKPreferencesSetJavaScriptEnabled(p, true);
        WKPreferencesSetSuppressesIncrementalRendering(p, false);
        FollowFrameRate(p);
        return p;
    }();
    WKPageConfigurationSetPreferences(conf, prefs);
    if (g_dataStore) WKPageConfigurationSetWebsiteDataStore(conf, g_dataStore);
    s_view = WKViewCreate(cr, conf, s_panel);
    WKRelease(conf);
    if (!s_view) { Log("customize: WKViewCreate failed"); return; }
    s_page = WKViewGetPage(s_view);
    s_viewHwnd = WKViewGetWindow(s_view);
    WKViewSetIsInWindow(s_view, true);
    WKPageNavigationClientV0 nc = {};
    nc.base.version = 0;
    nc.decidePolicyForNavigationAction = OnDecidePolicy;
    WKPageSetPageNavigationClient(s_page, &nc.base);
    const std::string url = ToUtf8(PanelUrl());
    if (WKURLRef u = WKURLCreateWithUTF8CString(url.c_str())) {
        WKPageLoadURL(s_page, u);
        WKRelease(u);
    }
}

static void DestroyView() {
    if (s_page) {
        WKPageNavigationClientV0 empty = {};
        WKPageSetPageNavigationClient(s_page, &empty.base);
    }
    if (s_viewHwnd && IsWindow(s_viewHwnd)) DestroyWindow(s_viewHwnd);
    else if (s_page) WKPageClose(s_page);
    if (s_view) WKRelease(s_view);
    s_view = nullptr; s_page = nullptr; s_viewHwnd = nullptr;
    s_ready = false;
}

static COLORREF PanelBackground() { return ResolveDark() ? RGB(0x1C, 0x1C, 0x1F) : RGB(0xF4, 0xF4, 0xF6); }

static LRESULT CALLBACK PanelWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        s_panel = hWnd;
        const DWORD round = 2;                       // DWMWCP_ROUND
        DwmSetWindowAttribute(hWnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
        BOOL dark = ResolveDark();
        DwmSetWindowAttribute(hWnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
        CreateView();
        return 0;
    }
    case WM_SIZE:
        if (s_viewHwnd) MoveWindow(s_viewHwnd, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(hWnd, &r);
        HBRUSH b = CreateSolidBrush(PanelBackground());
        FillRect((HDC)wParam, &r, b);
        DeleteObject(b);
        return 1;
    }
    case WM_CUST_COMMAND: {
        std::unique_ptr<std::wstring> url((std::wstring*)lParam);
        OnCommand(*url);
        return 0;
    }
    case WM_DESTROY:
        DestroyView();
        s_panel = nullptr;
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static RECT PanelRect() {
    RECT cr = {};
    GetClientRect(g_hWnd, &cr);
    POINT o = { 0, 0 };
    ClientToScreen(g_hWnd, &o);
    const int w = (int)S(312), margin = (int)S(12);
    const int top = (int)g_L.chromeH + margin;
    const int h = std::max((int)S(200), std::min((int)S(620), (int)cr.bottom - top - margin));
    const int x = o.x + (int)cr.right - w - margin;
    return { x, o.y + top, x + w, o.y + top + h };
}

static void CreatePanel() {
    if (s_panel) return;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = PanelWndProc;
        wc.hInstance = hi;
        wc.lpszClassName = kPanelClass;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        if (!RegisterClassExW(&wc)) return;
        registered = true;
    }
    const RECT r = PanelRect();
    // Created hidden: shown once the page is drawn (cust-ready), so it never flashes white.
    CreateWindowExW(WS_EX_TOOLWINDOW, kPanelClass, Tr(L"Customize", L"Настройка интерфейса"), WS_POPUP | WS_CLIPCHILDREN,
                    r.left, r.top, r.right - r.left, r.bottom - r.top, g_hWnd, nullptr, hi, nullptr);
}

// ---- public ------------------------------------------------------------------------------------------
bool CustomizeActive() { return s_active; }

void CustomizeBegin() {
    if (s_active || !g_hWnd) return;
    s_active = true;
    s_sel = -1;
    s_hover = -1;
    SetFocus(g_hWnd);                                // (ends typing in the address field)
    SetHover(Hit());
    RefreshLayout();
    InvalidateRect(g_hWnd, nullptr, FALSE);
    CreatePanel();
    Log("customize: begin (%ls)", UiActive().id.c_str());
}

void CustomizeEnd() {
    if (!s_active) return;
    s_active = false;
    s_pressItem = -1;
    s_dragging = false;
    if (GetCapture() == g_hWnd) ReleaseCapture();
    if (s_panel) DestroyWindow(s_panel);
    RefreshLayout();
    InvalidateRect(g_hWnd, nullptr, FALSE);
    Log("customize: end (%ls)", UiActive().id.c_str());
}

void CustomizeModelChanged() {
    if (!s_active) return;
    if (s_sel >= 0 && UiItemZone(UiActive(), s_sel) < 0 && !SlotOf(s_sel)) s_sel = -1;
    Push();
}

void CustomizeReposition() {
    if (!s_panel) return;
    if (IsIconic(g_hWnd)) return;
    const RECT r = PanelRect();
    SetWindowPos(s_panel, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
}

bool CustomizeMouse(UINT msg, int x, int y) {
    if (!s_active) return false;
    const float fx = (float)x, fy = (float)y;
    switch (msg) {
    case WM_LBUTTONDOWN: {
        if (g_L.palDone.Width > 0 && g_L.palDone.Contains(fx, fy)) { CustomizeEnd(); return true; }
        if (!InChrome(x, y)) return false;
        if (const Layout::Slot* s = SlotAt(fx, fy)) {
            s_pressItem = s->item;
            s_pressPt = { x, y };
            s_dragging = false;
            SetCapture(g_hWnd);
            Select(s->item);
        } else {
            Select(-1);
        }
        return true;
    }
    case WM_MOUSEMOVE: {
        if (s_pressItem >= 0) {
            if (!s_dragging && (std::abs(x - s_pressPt.x) > S(4) || std::abs(y - s_pressPt.y) > S(4))) s_dragging = true;
            if (s_dragging) {
                s_dragPt = { x, y };
                s_drop = FindDrop(s_pressItem, fx, fy);
                InvalidateChrome();
                return true;
            }
        }
        if (!InChrome(x, y)) {
            if (s_hover != -1) { s_hover = -1; InvalidateChrome(); }
            return false;
        }
        const Layout::Slot* s = SlotAt(fx, fy);
        const int h = s ? s->item : -1;
        if (h != s_hover) { s_hover = h; InvalidateChrome(); }
        SetHover(Hit());
        return true;
    }
    case WM_LBUTTONUP: {
        if (s_pressItem < 0) return InChrome(x, y);
        const int item = s_pressItem;
        const bool dragged = s_dragging;
        s_pressItem = -1;
        s_dragging = false;
        if (GetCapture() == g_hWnd) ReleaseCapture();
        if (dragged) {
            const Drop d = FindDrop(item, fx, fy);
            if (d.zone >= -1) MoveItem(item, d.zone, d.index);
            Push();
        }
        InvalidateChrome();
        return true;
    }
    case WM_CAPTURECHANGED:
        if (s_pressItem >= 0 || s_dragging) {
            s_pressItem = -1;
            s_dragging = false;
            InvalidateChrome();
        }
        return false;
    }
    return false;
}

bool CustomizeSetCursor(int x, int y) {
    if (!s_active) return false;
    if (s_dragging) { SetCursor(LoadCursor(nullptr, IDC_SIZEALL)); return true; }
    if (!InChrome(x, y)) return false;
    const bool over = SlotAt((float)x, (float)y) != nullptr || (g_L.palDone.Width > 0 && g_L.palDone.Contains((float)x, (float)y));
    SetCursor(LoadCursor(nullptr, over ? IDC_HAND : IDC_ARROW));
    return true;
}

void CustomizePaint(Graphics& g) {
    if (!s_active) return;
    const Theme& T = g_th;
    const Layout& L = g_L;
    const float rad = S(7);

    // The zones: a dashed outline around each (the one a drag would drop into in the accent colour).
    for (int z = 0; z < kZoneCount; ++z) {
        if (!UiZoneInMode(z, L.mode)) continue;
        RectF r = L.zoneRect[z];
        if (r.Width <= 0 || r.Height <= 0) continue;
        r.Inflate(S(3), S(3));
        GraphicsPath p;
        AddRound(p, RectF(r.X + 0.5f, r.Y + 0.5f, r.Width - 1, r.Height - 1), rad);
        const bool target = s_dragging && s_drop.zone == z;
        if (target) FillRound(g, Alpha(T.accent, 0.12f), r, rad);
        Pen pen(Alpha(target ? T.accent : T.text2, target ? 1.f : 0.5f), target ? std::max(1.f, std::round(S(1.5f))) : 1.f);
        if (!target) pen.SetDashStyle(DashStyleDash);
        g.DrawPath(&pen, &p);
    }

    // The palette: the hidden buttons, a hint and "Done".
    if (L.palette.Width > 0) {
        const RectF& pr = L.palette;
        SolidBrush tint(Alpha(T.text, s_dragging && s_drop.zone == -1 ? 0.09f : 0.035f));
        g.FillRectangle(&tint, pr);
        Pen line(T.sep, 1.f);
        g.DrawLine(&line, pr.X, pr.Y + 0.5f, pr.GetRight(), pr.Y + 0.5f);
        float textX = pr.X + S(16);
        for (const Layout::Slot& s : L.slots) {
            if (s.zone != -1) continue;
            if (s_hover == s.item && !s_dragging) FillRound(g, T.hover, s.r, S(6));
            const bool lifted = s_dragging && s_pressItem == s.item;
            DrawGlyph(g, UiItemGlyph(s.item), s.r, lifted ? Alpha(T.icon, 0.3f) : T.icon, g_fIcon.get());
            textX = std::max(textX, s.r.GetRight() + S(14));
        }
        StringFormat sf(StringFormat::GenericTypographic());
        sf.SetLineAlignment(StringAlignmentCenter);
        sf.SetFormatFlags(sf.GetFormatFlags() | StringFormatFlagsNoWrap);
        sf.SetTrimming(StringTrimmingEllipsisCharacter);
        const bool none = textX <= pr.X + S(16);
        const wchar_t* hint = none
            ? Tr(L"Drag buttons between the outlined areas. Drop one here to hide it.",
                 L"Перетаскивайте кнопки между обведёнными зонами. Перетащите сюда, чтобы скрыть.")
            : Tr(L"Hidden buttons: drag one onto the toolbar to show it.",
                 L"Скрытые кнопки: перетащите на панель, чтобы показать.");
        SolidBrush tb(T.text2);
        const float textR = L.palDone.X - S(16);
        if (textR - textX > S(40))
            g.DrawString(hint, -1, g_fTab.get(), RectF(textX, pr.Y, textR - textX, pr.Height), &sf, &tb);
        // "Done"
        const RectF& dr = L.palDone;
        FillRound(g, s_hover == -2 ? T.accent : T.accent, dr, dr.Height / 2);
        StringFormat cf(StringFormat::GenericTypographic());
        cf.SetAlignment(StringAlignmentCenter);
        cf.SetLineAlignment(StringAlignmentCenter);
        SolidBrush wb(Color(255, 255, 255, 255));
        g.DrawString(Tr(L"Done", L"Готово"), -1, g_fText.get(), dr, &cf, &wb);
    }

    // Hover and selection
    if (!s_dragging && s_hover >= 0 && s_hover != s_sel)
        if (const Layout::Slot* s = SlotOf(s_hover)) if (s->zone >= 0) {
            RectF r = s->r;
            r.Inflate(S(2), S(2));
            GraphicsPath p;
            AddRound(p, r, S(6));
            Pen pen(Alpha(T.accent, 0.5f), 1.f);
            g.DrawPath(&pen, &p);
        }
    if (s_sel >= 0)
        if (const Layout::Slot* s = SlotOf(s_sel)) {
            RectF r = s->r;
            r.Inflate(S(2), S(2));
            GraphicsPath p;
            AddRound(p, r, S(6));
            Pen pen(T.accent, std::max(1.5f, std::round(S(2))));
            g.DrawPath(&pen, &p);
        }

    // A drag: where the button would go and the button itself under the pointer.
    if (s_dragging && s_pressItem >= 0) {
        if (const Layout::Slot* s = SlotOf(s_pressItem)) {
            GraphicsPath p;
            AddRound(p, RectF(s->r.X + 0.5f, s->r.Y + 0.5f, s->r.Width - 1, s->r.Height - 1), S(6));
            Pen pen(Alpha(T.text2, 0.6f), 1.f);
            pen.SetDashStyle(DashStyleDash);
            g.DrawPath(&pen, &p);
        }
        if (s_drop.zone >= 0) FillRound(g, T.accent, s_drop.bar, std::min(s_drop.bar.Width, s_drop.bar.Height) / 2);
        const float b = std::floor(S(CurrentDims().btn));
        const RectF gr((float)s_dragPt.x - b / 2, (float)s_dragPt.y - b / 2, b, b);
        FillRound(g, Alpha(T.frame.GetA() ? T.frame : T.popup, 0.96f), gr, S(6));
        GraphicsPath p;
        AddRound(p, gr, S(6));
        Pen pen(T.accent, std::max(1.f, std::round(S(1.5f))));
        g.DrawPath(&pen, &p);
        DrawGlyph(g, UiItemGlyph(s_pressItem), gr, T.icon, g_fIcon.get());
    }
}
