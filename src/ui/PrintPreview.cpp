// Lyra — предпросмотр печати (Ctrl+P): страницы слева, принтер и настройки справа.
//
// The page stays in WebKit's printing mode while the preview is open (the tab's picture is frozen
// meanwhile): WKPageComputePagesForPrinting lays it out for the chosen paper, and the sheets in view
// are drawn one at a time with WKPageDrawPageToImageForPrinting at the size they are shown. Any
// change of printer, paper, orientation, margins, scale or backgrounds lays the page out again.
// "Print" closes the preview and hands the choices to the print job (ui/Print.cpp).
#include "ui/Print.h"
#include "app/Log.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "browser/WebContext.h"
#include "core/Localization.h"
#include "core/Settings.h"
#include "core/StringUtils.h"
#include "ui/Draw.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/WindowFrame.h"

#include <winspool.h>
#include <WebKit/WKPagePrivate.h>

namespace {

constexpr float kMarginMm[] = { 10, 5, 0, 25 };          // g_prefs.printMargins
constexpr int kScales[] = { 50, 67, 75, 80, 90, 100, 110, 125, 150, 175, 200 };
constexpr int kEditId = 3001;

// ---- state ---------------------------------------------------------------------------------
HWND s_wnd = nullptr, s_edit = nullptr;
WNDPROC s_oldEdit = nullptr;
HFONT s_editFont = nullptr;
HBRUSH s_fieldBrush = nullptr;
int s_editH = 18;
std::unique_ptr<Font> s_fBody, s_fSmall, s_fTitle, s_fBold, s_fIcon, s_fCaption;

unsigned long long s_tabId = 0;
WKPageRef s_page = nullptr;              // retained while the preview is open
WKFrameRef s_frame = nullptr;
PrintTexts s_texts;

struct Printer { std::wstring name, port; };
std::vector<Printer> s_printers;
int s_printer = -1;
std::vector<BYTE> s_devmode;             // DEVMODEW of the chosen printer with the choices made here
struct Paper { WORD id; std::wstring name; };
std::vector<Paper> s_papers;
int s_paper = -1;
bool s_colorDevice = false;
int s_driverCopies = 1;                  // copies the driver can make itself

bool s_landscape = false, s_color = true, s_allPages = true;
int s_copies = 1;
PageRanges s_ranges;
bool s_rangesOk = true;

PaperGeo s_geo;
SheetGeo s_sheet;
WKPrintInfo s_info {};
unsigned s_gen = 0;                      // layout generation: answers for an older one are dropped
bool s_computing = false, s_drawing = false;
std::vector<WKRect> s_rects;
std::vector<std::unique_ptr<Bitmap>> s_bitmaps;
std::vector<int> s_bitmapFor;            // the width (screen pixels) each bitmap was drawn for
float s_scroll = 0;

enum Ctl { kNone = -1, kPrinter, kPages, kRange, kCopiesMinus, kCopiesPlus, kOrientation, kColor, kPaper,
           kMargins, kScale, kHeaders, kBackground, kSystem, kCancel, kPrint, kCloseX };
int s_hover = kNone, s_hoverPart = -1, s_down = kNone;
bool s_tracking = false;

// ---- colours (as in the settings window) -----------------------------------------------------
struct Pal { Color bg, preview, card, field, border, text, text2, off, hover, pressed; };
Pal P() {
    Pal p;
    if (!g_dark) {
        p.bg = Color(243, 244, 247); p.preview = Color(222, 224, 229); p.card = Color(255, 255, 255);
        p.field = Color(255, 255, 255); p.border = Color(218, 221, 227);
        p.text = Color(27, 29, 33); p.text2 = Color(89, 96, 107); p.off = Color(191, 196, 204);
        p.hover = Color(20, 0, 0, 0); p.pressed = Color(36, 0, 0, 0);
    } else {
        p.bg = Color(29, 31, 35); p.preview = Color(20, 21, 24); p.card = Color(38, 40, 45);
        p.field = Color(46, 48, 54); p.border = Color(54, 57, 64);
        p.text = Color(236, 237, 239); p.text2 = Color(166, 171, 180); p.off = Color(74, 78, 87);
        p.hover = Color(24, 255, 255, 255); p.pressed = Color(40, 255, 255, 255);
    }
    return p;
}

std::wstring Plural(int n, const wchar_t* en1, const wchar_t* enN, const wchar_t* ru1, const wchar_t* ru2, const wchar_t* ru5) {
    if (!UiRussian()) return std::to_wstring(n) + L" " + (n == 1 ? en1 : enN);
    const int m10 = n % 10, m100 = n % 100;
    const wchar_t* w = (m10 == 1 && m100 != 11) ? ru1 : (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) ? ru2 : ru5;
    return std::to_wstring(n) + L" " + w;
}

float kLabelW() { return S(100); }            // the labels' column in the panel

// ---- printers --------------------------------------------------------------------------------
DEVMODEW* Devmode() { return s_devmode.empty() ? nullptr : (DEVMODEW*)s_devmode.data(); }
const Printer* CurrentPrinter() { return s_printer >= 0 && s_printer < (int)s_printers.size() ? &s_printers[s_printer] : nullptr; }
bool IsPdfPrinter() { const Printer* p = CurrentPrinter(); return p && p->name.find(L"PDF") != std::wstring::npos; }

void LoadPrinters() {
    s_printers.clear();
    DWORD need = 0, count = 0;
    EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 2, nullptr, 0, &need, &count);
    std::vector<BYTE> buf(need ? need : 1);
    if (need && EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 2, buf.data(), need, &need, &count)) {
        auto* pi = (PRINTER_INFO_2W*)buf.data();
        for (DWORD i = 0; i < count; ++i)
            if (pi[i].pPrinterName) s_printers.push_back({ pi[i].pPrinterName, pi[i].pPortName ? pi[i].pPortName : L"" });
    }
    wchar_t def[512] = {};
    DWORD len = ARRAYSIZE(def);
    const std::wstring windowsDefault = GetDefaultPrinterW(def, &len) ? def : L"";
    s_printer = s_printers.empty() ? -1 : 0;
    for (int i = 0; i < (int)s_printers.size(); ++i) if (s_printers[i].name == windowsDefault) s_printer = i;
    for (int i = 0; i < (int)s_printers.size(); ++i) if (!g_printPrinter.empty() && s_printers[i].name == g_printPrinter) s_printer = i;
}

// The printer's own settings and what it can do; the choices made here start from them.
void SelectPrinter(int i) {
    s_printer = i;
    s_devmode.clear();
    s_papers.clear();
    s_paper = -1;
    const Printer* p = CurrentPrinter();
    if (!p) return;
    HANDLE h = nullptr;
    if (OpenPrinterW((LPWSTR)p->name.c_str(), &h, nullptr)) {
        const LONG size = DocumentPropertiesW(nullptr, h, (LPWSTR)p->name.c_str(), nullptr, nullptr, 0);
        if (size > 0) {
            s_devmode.assign((size_t)size, 0);
            if (DocumentPropertiesW(nullptr, h, (LPWSTR)p->name.c_str(), Devmode(), nullptr, DM_OUT_BUFFER) != IDOK) s_devmode.clear();
        }
        ClosePrinter(h);
    }
    const wchar_t* name = p->name.c_str();
    const wchar_t* port = p->port.c_str();
    const int n = DeviceCapabilitiesW(name, port, DC_PAPERS, nullptr, nullptr);
    if (n > 0) {
        std::vector<WORD> ids((size_t)n);
        std::vector<wchar_t> names((size_t)n * 64 + 1, 0);
        DeviceCapabilitiesW(name, port, DC_PAPERS, (LPWSTR)ids.data(), nullptr);
        if (DeviceCapabilitiesW(name, port, DC_PAPERNAMES, names.data(), nullptr) == n)
            for (int k = 0; k < n; ++k) {
                std::wstring nm(&names[(size_t)k * 64], wcsnlen(&names[(size_t)k * 64], 64));
                if (!nm.empty()) s_papers.push_back({ ids[(size_t)k], nm });
            }
    }
    s_colorDevice = DeviceCapabilitiesW(name, port, DC_COLORDEVICE, nullptr, nullptr) == 1;
    s_driverCopies = std::max(1, DeviceCapabilitiesW(name, port, DC_COPIES, nullptr, nullptr));
    if (DEVMODEW* dm = Devmode()) {
        if (dm->dmFields & DM_ORIENTATION) s_landscape = dm->dmOrientation == DMORIENT_LANDSCAPE;
        if (dm->dmFields & DM_COLOR) s_color = dm->dmColor == DMCOLOR_COLOR;
        if (dm->dmFields & DM_PAPERSIZE)
            for (int k = 0; k < (int)s_papers.size(); ++k) if (s_papers[k].id == dm->dmPaperSize) s_paper = k;
    }
    if (!s_colorDevice) s_color = false;
    Log("print preview: printer \"%s\", %zu paper sizes, color %d, copies %d", ToUtf8(p->name).c_str(),
        s_papers.size(), s_colorDevice ? 1 : 0, s_driverCopies);
}

// The choices into the DEVMODE (checked by the driver), and the sheet they give.
void ApplyChoices() {
    s_geo = PaperGeo();
    const Printer* p = CurrentPrinter();
    DEVMODEW* dm = Devmode();
    if (!p) return;
    if (dm) {
        dm->dmFields |= DM_ORIENTATION;
        dm->dmOrientation = s_landscape ? DMORIENT_LANDSCAPE : DMORIENT_PORTRAIT;
        if (s_paper >= 0) {
            dm->dmFields |= DM_PAPERSIZE;
            dm->dmFields &= ~(DM_PAPERLENGTH | DM_PAPERWIDTH | DM_FORMNAME);
            dm->dmPaperSize = (short)s_papers[s_paper].id;
        }
        if (s_colorDevice) { dm->dmFields |= DM_COLOR; dm->dmColor = s_color ? DMCOLOR_COLOR : DMCOLOR_MONOCHROME; }
        HANDLE h = nullptr;
        if (OpenPrinterW((LPWSTR)p->name.c_str(), &h, nullptr)) {
            std::vector<BYTE> out(s_devmode.size());
            if (DocumentPropertiesW(nullptr, h, (LPWSTR)p->name.c_str(), (DEVMODEW*)out.data(), dm, DM_IN_BUFFER | DM_OUT_BUFFER) == IDOK)
                s_devmode.swap(out);
            ClosePrinter(h);
        }
    }
    if (HDC ic = CreateICW(L"WINSPOOL", p->name.c_str(), nullptr, Devmode())) {
        s_geo = PaperGeoOf(ic);
        DeleteDC(ic);
    }
}

// ---- layout of the page in WebKit and the sheets' pictures ------------------------------------
void DrawMore();

void OnComputed(WKRect* rects, uint32_t count, double, WKErrorRef error, void* ctx) {
    if (!s_wnd || (unsigned)(uintptr_t)ctx != s_gen) return;
    s_computing = false;
    s_rects.clear();
    if (!error && count) s_rects.assign(rects, rects + count);
    s_bitmaps.clear();
    s_bitmaps.resize(s_rects.size());
    s_bitmapFor.assign(s_rects.size(), 0);
    Log("print preview: %u page(s)", count);
    InvalidateRect(s_wnd, nullptr, FALSE);
    DrawMore();
}

void Relayout() {
    ++s_gen;
    s_computing = s_drawing = false;
    s_rects.clear();
    s_bitmaps.clear();
    s_bitmapFor.clear();
    ApplyChoices();
    s_sheet = SheetGeoOf(s_geo, kMarginMm[g_prefs.printMargins]);
    if (!s_geo.Valid() || !s_page || !s_frame) { InvalidateRect(s_wnd, nullptr, FALSE); return; }
    WKPreferencesSetShouldPrintBackgrounds(g_preferences, g_prefs.printBackground);
    s_info.pageSetupScaleFactor = g_prefs.printScale / 100.f;
    s_info.availablePaperWidth = s_sheet.areaW * 72.f / s_geo.dpiX;
    s_info.availablePaperHeight = s_sheet.areaH * 72.f / s_geo.dpiY;
    s_computing = true;
    WKPageComputePagesForPrinting(s_page, s_frame, s_info, OnComputed, (void*)(uintptr_t)s_gen);
    InvalidateRect(s_wnd, nullptr, FALSE);
}

// ---- geometry ----------------------------------------------------------------------------------
struct Geo { float W, H, panelW, previewW, sheetW, sheetH, gap, pad; };
Geo GetGeo() {
    RECT cr = {};
    GetClientRect(s_wnd, &cr);
    Geo G;
    G.W = (float)cr.right;
    G.H = (float)cr.bottom;
    G.panelW = S(372);
    G.previewW = std::max(S(200), G.W - G.panelW);
    G.pad = S(28);
    G.gap = S(40);
    const float aspect = s_geo.Valid() ? (float)s_geo.physW / s_geo.physH : (s_landscape ? 297.f / 210.f : 210.f / 297.f);
    G.sheetH = std::max(S(100), G.H - 2 * G.pad - S(18));
    G.sheetW = G.sheetH * aspect;
    const float maxW = G.previewW - 2 * S(40);
    if (G.sheetW > maxW) { G.sheetW = maxW; G.sheetH = G.sheetW / aspect; }
    return G;
}
int SheetCount() { return std::max(1, (int)s_rects.size()); }
float ContentH(const Geo& G) { return 2 * G.pad + SheetCount() * (G.sheetH + G.gap) - G.gap + S(18); }
RectF SheetRect(const Geo& G, int i) {
    return RectF((G.previewW - G.sheetW) / 2, G.pad + i * (G.sheetH + G.gap) - s_scroll, G.sheetW, G.sheetH);
}
void ClampScroll() {
    const Geo G = GetGeo();
    s_scroll = std::max(0.f, std::min(s_scroll, ContentH(G) - G.H));
}
// The width (screen pixels) of the area between the margins on a sheet as shown.
int AreaWidthOnScreen(const Geo& G) {
    return s_geo.Valid() ? std::max(1, (int)std::lround(s_sheet.areaW * G.sheetW / s_geo.physW)) : 1;
}

struct DrawCtx { unsigned gen; uint32_t index; int width; };

void OnImage(const void* pixels, uint32_t w, uint32_t h, uint32_t bytesPerRow, void* ctx) {
    std::unique_ptr<DrawCtx> c((DrawCtx*)ctx);
    if (!s_wnd || c->gen != s_gen) return;
    s_drawing = false;
    if (c->index < s_bitmaps.size()) {
        if (pixels && w && h) {
            auto bmp = std::make_unique<Bitmap>((INT)w, (INT)h, PixelFormat32bppPARGB);
            BitmapData data;
            Gdiplus::Rect all(0, 0, (INT)w, (INT)h);
            if (bmp->LockBits(&all, ImageLockModeWrite, PixelFormat32bppPARGB, &data) == Ok) {
                for (uint32_t y = 0; y < h; ++y)
                    memcpy((BYTE*)data.Scan0 + (size_t)y * data.Stride, (const BYTE*)pixels + (size_t)y * bytesPerRow, (size_t)w * 4);
                bmp->UnlockBits(&data);
                s_bitmaps[c->index] = std::move(bmp);
            }
        }
        s_bitmapFor[c->index] = c->width;          // also when it failed: not asked again and again
    }
    InvalidateRect(s_wnd, nullptr, FALSE);
    DrawMore();
}

// Asks WebKit for the next sheet that is in view (or just below it) and not drawn at this size yet.
void DrawMore() {
    if (!s_wnd || s_computing || s_drawing || s_rects.empty()) return;
    const Geo G = GetGeo();
    const int want = AreaWidthOnScreen(G);
    int pick = -1;
    for (int i = 0; i < (int)s_rects.size() && pick < 0; ++i) {
        const RectF r = SheetRect(G, i);
        const bool inReach = r.GetBottom() > -G.H * 0.5f && r.Y < G.H * 2.f;
        if (inReach && s_bitmapFor[i] < want * 0.85f) pick = i;
    }
    if (pick < 0) return;
    const WKRect& r = s_rects[pick];
    const float scale = r.size.width > 0 ? std::min(4.f, (float)want / (float)r.size.width) : 1.f;
    s_drawing = true;
    WKPageDrawPageToImageForPrinting(s_page, s_frame, s_info, (uint32_t)pick, scale, OnImage,
                                     new DrawCtx { s_gen, (uint32_t)pick, want });
}

// ---- the panel ---------------------------------------------------------------------------------
struct Item { int id; RectF r; std::wstring label; int parts = 0; bool enabled = true; };

int SelectedPages() {
    int n = 0;
    for (int i = 0; i < (int)s_rects.size(); ++i) if (PageInRanges(s_ranges, (uint32_t)i + 1)) ++n;
    return n;
}
bool CanPrint() {
    return CurrentPrinter() && s_rangesOk && !s_computing && !s_rects.empty() && SelectedPages() > 0;
}
std::wstring MarginName(int i) {
    switch (i) {
    case 0: return Tr(L"Default", L"Обычные");
    case 1: return Tr(L"Minimum", L"Минимальные");
    case 2: return Tr(L"None", L"Без полей");
    default: return Tr(L"Wide", L"Широкие");
    }
}
std::wstring PaperName() { return s_paper >= 0 ? s_papers[s_paper].name : std::wstring(Tr(L"As set in the printer", L"Как в принтере")); }

std::vector<Item> Items(const Geo& G) {
    std::vector<Item> v;
    const float x = G.previewW + S(24), w = G.panelW - S(48);
    const float ctlX = x + kLabelW(), ctlW = w - kLabelW(), rowH = S(44), ctlH = S(32);
    float y = S(92);
    auto row = [&](int id, const std::wstring& label, int parts = 0, bool enabled = true) {
        v.push_back({ id, RectF(ctlX, y + (rowH - ctlH) / 2, ctlW, ctlH), label, parts, enabled });
        y += rowH;
    };
    row(kPrinter, Tr(L"Printer", L"Принтер"), 0, !s_printers.empty());
    row(kPages, Tr(L"Pages", L"Страницы"), 2);
    if (!s_allPages) row(kRange, L"");
    {   // copies: − n +
        const float b = ctlH;
        v.push_back({ kCopiesMinus, RectF(ctlX, y + (rowH - ctlH) / 2, b, b), Tr(L"Copies", L"Копии"), 0, s_copies > 1 });
        v.push_back({ kCopiesPlus, RectF(ctlX + S(92) - b, y + (rowH - ctlH) / 2, b, b), L"", 0, s_copies < 999 });
        y += rowH;
    }
    row(kOrientation, Tr(L"Layout", L"Ориентация"), 2);
    if (s_colorDevice) row(kColor, Tr(L"Colour", L"Цвет"), 2);
    row(kPaper, Tr(L"Paper", L"Бумага"), 0, !s_papers.empty());
    row(kMargins, Tr(L"Margins", L"Поля"));
    row(kScale, Tr(L"Scale", L"Масштаб"));
    y += S(6);
    row(kHeaders, Tr(L"Headers and footers", L"Колонтитулы"), 0, HeadersFit(s_geo, s_sheet));
    row(kBackground, Tr(L"Background graphics", L"Фон и цвета"));
    // Bottom: the system dialog link, Cancel and Print.
    const float by = G.H - S(24) - ctlH;
    v.push_back({ kPrint, RectF(x + w - S(112), by, S(112), ctlH), IsPdfPrinter() ? Tr(L"Save", L"Сохранить") : Tr(L"Print", L"Печать"), 0, CanPrint() });
    v.push_back({ kCancel, RectF(x + w - S(112) - S(8) - S(100), by, S(100), ctlH), Tr(L"Cancel", L"Отмена") });
    v.push_back({ kCloseX, RectF(G.W - S(46), 0, S(46), S(36)), L"" });   // the window has no system title bar
    v.push_back({ kSystem, RectF(x, by - S(44), w, S(28)), Tr(L"Print using the system dialog\x2026", L"Системный диалог печати\x2026") });
    return v;
}
RectF PartRect(const Item& it, int k) {
    const float pw = it.r.Width / std::max(1, it.parts);
    return RectF(it.r.X + pw * k + S(2), it.r.Y + S(2), pw - S(4), it.r.Height - S(4));
}
// Toggles are small switches at the right end of their row.
RectF SwitchRect(const Item& it) { return RectF(it.r.GetRight() - S(40), it.r.Y + (it.r.Height - S(20)) / 2, S(40), S(20)); }
bool IsSwitch(int id) { return id == kHeaders || id == kBackground; }

void Text(Graphics& g, const std::wstring& s, Font* f, const RectF& r, const Color& c,
          StringAlignment h = StringAlignmentNear, StringAlignment v = StringAlignmentCenter) {
    StringFormat sf;
    sf.SetAlignment(h);
    sf.SetLineAlignment(v);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush b(c);
    g.DrawString(s.c_str(), -1, f, r, &sf, &b);
}
void Stroke(Graphics& g, const Color& c, const RectF& r, float rad) {
    GraphicsPath p;
    AddRound(p, r, rad);
    Pen pen(c, 1.f);
    g.DrawPath(&pen, &p);
}
Color Alpha(const Color& c, int a) { return Color((BYTE)a, c.GetR(), c.GetG(), c.GetB()); }

void PositionEdit(const Geo& G) {
    if (!s_edit) return;
    for (const Item& it : Items(G))
        if (it.id == kRange) {
            MoveWindow(s_edit, (int)(it.r.X + S(10)), (int)(it.r.Y + (it.r.Height - s_editH) / 2),
                       (int)(it.r.Width - S(20)), s_editH, TRUE);
            if (!IsWindowVisible(s_edit)) ShowWindow(s_edit, SW_SHOW);
            return;
        }
    if (IsWindowVisible(s_edit)) ShowWindow(s_edit, SW_HIDE);
}

void DrawSheets(Graphics& g, const Geo& G, const Pal& p) {
    SolidBrush bg(p.preview);
    g.FillRectangle(&bg, 0.f, 0.f, G.previewW, G.H);
    const int n = SheetCount();
    for (int i = 0; i < n; ++i) {
        const RectF r = SheetRect(G, i);
        if (r.GetBottom() + S(30) < 0 || r.Y > G.H) continue;
        for (int k = 4; k >= 1; --k) {                   // a soft shadow
            RectF sh = r;
            sh.Inflate(S((float)k), S((float)k));
            sh.Offset(0, S(1.5f));
            FillRound(g, Color(g_dark ? 40 : 14, 0, 0, 0), sh, S((float)k + 1));
        }
        SolidBrush white(Color(255, 255, 255));
        g.FillRectangle(&white, r);
        const float s = s_geo.Valid() ? r.Width / s_geo.physW : 0;
        const RectF area(r.X + s_sheet.left * s, r.Y + s_sheet.top * s, s_sheet.areaW * s, s_sheet.areaH * s);
        Bitmap* bmp = i < (int)s_bitmaps.size() ? s_bitmaps[i].get() : nullptr;
        if (bmp && s > 0) {
            const float bw = (float)bmp->GetWidth(), bh = (float)bmp->GetHeight();
            const float k = std::min(area.Width / bw, area.Height / bh);
            g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            g.SetPixelOffsetMode(PixelOffsetModeHalf);
            if (s_colorDevice && !s_color) {                 // black and white: shown so
                ColorMatrix grey = { {
                    { 0.299f, 0.299f, 0.299f, 0, 0 }, { 0.587f, 0.587f, 0.587f, 0, 0 }, { 0.114f, 0.114f, 0.114f, 0, 0 },
                    { 0, 0, 0, 1, 0 }, { 0, 0, 0, 0, 1 } } };
                ImageAttributes attrs;
                attrs.SetColorMatrix(&grey);
                g.DrawImage(bmp, RectF(area.X, area.Y, bw * k, bh * k), 0, 0, bw, bh, UnitPixel, &attrs);
            } else {
                g.DrawImage(bmp, RectF(area.X, area.Y, bw * k, bh * k));
            }
            g.SetPixelOffsetMode(PixelOffsetModeDefault);
        } else if (s > 0) {                               // not drawn yet: grey lines of "text"
            SolidBrush line(Color(255, 236, 238, 242));
            const float lh = std::max(S(4), area.Height / 70), step = lh * 2.4f;
            for (float y = area.Y + step; y + lh < area.GetBottom() - step && y < area.Y + area.Height * 0.8f; y += step) {
                const float frac = ((int)((y - area.Y) / step) % 5 == 4) ? 0.55f : 0.92f;
                g.FillRectangle(&line, area.X, y, area.Width * frac, lh);
            }
        }
        if (g_prefs.printHeaders && s > 0 && !s_rects.empty())
            DrawHeadersFooters(g, s_geo, s_sheet, s, r.X, r.Y, s_texts, i + 1, (int)s_rects.size());
        if (!s_rects.empty() && !PageInRanges(s_ranges, (uint32_t)i + 1)) {   // not printed: veiled
            SolidBrush veil(Alpha(p.preview, 175));
            RectF vr = r;
            vr.Inflate(S(5), S(5));
            g.FillRectangle(&veil, vr);
        }
        if (!s_rects.empty())
            Text(g, std::to_wstring(i + 1), s_fSmall.get(), RectF(r.X, r.GetBottom() + S(6), r.Width, S(20)), p.text2, StringAlignmentCenter);
    }
    if (s_computing || !s_geo.Valid()) {
        const std::wstring msg = !CurrentPrinter() ? Tr(L"No printers found", L"Принтеры не найдены")
                               : s_computing ? Tr(L"Preparing the preview\x2026", L"Готовим предпросмотр\x2026")
                               : Tr(L"The printer cannot be used", L"Этот принтер недоступен");
        RectF pill(G.previewW / 2 - S(130), G.H / 2 - S(20), S(260), S(40));
        FillRound(g, Alpha(p.card, 235), pill, S(20));
        Text(g, msg, s_fBody.get(), pill, p.text, StringAlignmentCenter);
    }
}

void DrawPanel(Graphics& g, const Geo& G, const Pal& p) {
    SolidBrush bg(p.bg);
    g.FillRectangle(&bg, G.previewW, 0.f, G.panelW, G.H);
    Pen sep(p.border, 1.f);
    g.DrawLine(&sep, G.previewW, 0.f, G.previewW, G.H);
    const Color accent = g_th.accent;
    const float x = G.previewW + S(24), w = G.panelW - S(48);
    Text(g, Tr(L"Print", L"Печать"), s_fTitle.get(), RectF(x, S(20), w, S(30)), p.text);
    std::wstring summary;
    if (!s_rects.empty()) {
        const int sel = SelectedPages();
        summary = Plural(sel, L"page", L"pages", L"страница", L"страницы", L"страниц");
        if (s_copies > 1)
            summary += L" \x00B7 " + Plural(sel * s_copies, L"sheet", L"sheets", L"лист", L"листа", L"листов");
    }
    Text(g, summary, s_fSmall.get(), RectF(x, S(50), w, S(20)), p.text2);

    for (const Item& it : Items(G)) {
        const bool hv = s_hover == it.id && it.enabled;
        const Color text = it.enabled ? p.text : p.text2;
        if (!it.label.empty() && it.id != kCancel && it.id != kPrint && it.id != kSystem && it.id != kCopiesPlus) {
            // A switch's label has the whole row up to the switch.
            const float lw = IsSwitch(it.id) ? SwitchRect(it).X - x - S(8) : kLabelW() - S(4);
            Text(g, it.label, s_fBody.get(), RectF(x, it.r.Y, lw, it.r.Height), it.enabled ? p.text : p.text2);
        }
        switch (it.id) {
        case kPrinter: case kPaper: case kMargins: case kScale: {
            std::wstring val;
            if (it.id == kPrinter) val = CurrentPrinter() ? CurrentPrinter()->name : Tr(L"None", L"Нет");
            else if (it.id == kPaper) val = PaperName();
            else if (it.id == kMargins) val = MarginName(g_prefs.printMargins);
            else val = std::to_wstring(g_prefs.printScale) + L"%";
            FillRound(g, p.field, it.r, S(7));
            Stroke(g, hv ? accent : p.border, it.r, S(7));
            Text(g, val, s_fBody.get(), RectF(it.r.X + S(10), it.r.Y, it.r.Width - S(36), it.r.Height), text);
            DrawGlyph(g, L"", RectF(it.r.GetRight() - S(28), it.r.Y, S(22), it.r.Height), p.text2, s_fIcon.get());
            break;
        }
        case kPages: case kOrientation: case kColor: {
            FillRound(g, g_dark ? Color(25, 27, 30) : Color(232, 234, 238), it.r, S(8));
            const int cur = it.id == kPages ? (s_allPages ? 0 : 1) : it.id == kOrientation ? (s_landscape ? 1 : 0) : (s_color ? 0 : 1);
            const wchar_t* names[2][2] = {
                { Tr(L"All", L"Все"), Tr(L"Custom", L"Выбрать") },
                { Tr(L"Portrait", L"Книжная"), Tr(L"Landscape", L"Альбомная") } };
            for (int k = 0; k < 2; ++k) {
                const RectF s = PartRect(it, k);
                if (k == cur) { FillRound(g, p.card, s, S(6)); Stroke(g, p.border, s, S(6)); }
                else if (hv && s_hoverPart == k) FillRound(g, p.hover, s, S(6));
                std::wstring label = it.id == kColor ? (k == 0 ? Tr(L"Colour", L"Цветная") : Tr(L"Black and white", L"Ч/б"))
                                                     : names[it.id == kPages ? 0 : 1][k];
                Text(g, label, k == cur ? s_fBold.get() : s_fBody.get(), s, p.text, StringAlignmentCenter);
            }
            break;
        }
        case kRange:
            FillRound(g, p.field, it.r, S(7));
            Stroke(g, !s_rangesOk ? Color(220, 60, 60) : (GetFocus() == s_edit ? accent : p.border), it.r, S(7));
            if (GetWindowTextLengthW(s_edit) == 0 && GetFocus() != s_edit)
                Text(g, Tr(L"e.g. 1-5, 8, 11-13", L"например, 1-5, 8, 11-13"), s_fBody.get(),
                     RectF(it.r.X + S(10), it.r.Y, it.r.Width - S(20), it.r.Height), p.text2);
            break;
        case kCopiesMinus: case kCopiesPlus: {
            FillRound(g, hv ? p.hover : p.field, it.r, S(7));
            Stroke(g, p.border, it.r, S(7));
            DrawGlyph(g, it.id == kCopiesMinus ? L"" : L"", it.r, it.enabled ? p.text : p.off, s_fIcon.get());
            if (it.id == kCopiesMinus)
                Text(g, std::to_wstring(s_copies), s_fBold.get(), RectF(it.r.GetRight(), it.r.Y, S(92) - 2 * it.r.Width, it.r.Height),
                     p.text, StringAlignmentCenter);
            break;
        }
        case kHeaders: case kBackground: {
            const bool on = it.id == kHeaders ? g_prefs.printHeaders : g_prefs.printBackground;
            const RectF sw = SwitchRect(it);
            FillRound(g, on && it.enabled ? accent : p.off, sw, sw.Height / 2);
            const float kd = sw.Height - S(6);
            SolidBrush knob(Color(it.enabled ? 255 : 190, 255, 255, 255));
            g.FillEllipse(&knob, on ? sw.GetRight() - S(3) - kd : sw.X + S(3), sw.Y + S(3), kd, kd);
            break;
        }
        case kSystem:
            Text(g, it.label, s_fBody.get(), it.r, hv ? accent : p.text2);
            break;
        case kCloseX:
            if (hv) { SolidBrush b(s_down == kCloseX ? g_th.closePressed : g_th.closeHover); g.FillRectangle(&b, it.r); }
            DrawGlyph(g, L"\uE8BB", it.r, hv ? Color(255, 255, 255) : p.text, s_fCaption.get());
            break;
        case kCancel:
            FillRound(g, hv ? p.hover : p.field, it.r, S(8));
            Stroke(g, p.border, it.r, S(8));
            Text(g, it.label, s_fBody.get(), it.r, p.text, StringAlignmentCenter);
            break;
        case kPrint: {
            Color c = accent;
            if (!it.enabled) c = Alpha(accent, 110);
            else if (hv) c = Color(255, (BYTE)std::min(255, accent.GetR() + 18), (BYTE)std::min(255, accent.GetG() + 18), (BYTE)std::min(255, accent.GetB() + 18));
            FillRound(g, c, it.r, S(8));
            Text(g, it.label, s_fBold.get(), it.r, Color(255, 255, 255), StringAlignmentCenter);
            break;
        }
        }
    }
}

void Paint(HDC hdc, const RECT& rc) {
    const Geo G = GetGeo();
    PositionEdit(G);
    PaintBuffered(hdc, rc, true, [&](Graphics& g) {
        g.SetSmoothingMode(SmoothingModeHighQuality);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        const Pal p = P();
        DrawSheets(g, G, p);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        DrawPanel(g, G, p);
    });
}

// ---- actions -----------------------------------------------------------------------------------
void Close() {
    if (!s_wnd) return;
    EnableWindow(g_hWnd, TRUE);
    DestroyWindow(s_wnd);                                 // WM_DESTROY ends WebKit's printing mode
}

void DoPrint() {
    if (!CanPrint()) { MessageBeep(MB_ICONWARNING); return; }
    const Printer* pr = CurrentPrinter();
    std::vector<BYTE> dmBuf = s_devmode;
    DEVMODEW* dm = dmBuf.empty() ? nullptr : (DEVMODEW*)dmBuf.data();
    PrintSettings settings;
    settings.marginMm = kMarginMm[g_prefs.printMargins];
    settings.scale = g_prefs.printScale / 100.f;
    settings.headers = g_prefs.printHeaders && HeadersFit(s_geo, s_sheet);
    settings.ranges = s_ranges;
    settings.grayscale = s_colorDevice && !s_color;
    if (dm && s_copies > 1 && s_driverCopies >= s_copies) {
        dm->dmFields |= DM_COPIES | DM_COLLATE;
        dm->dmCopies = (short)s_copies;
        dm->dmCollate = DMCOLLATE_TRUE;
    } else {
        settings.copiesByLyra = s_copies;
        if (dm) { dm->dmFields |= DM_COPIES; dm->dmCopies = 1; }
    }
    HDC dc = CreateDCW(L"WINSPOOL", pr->name.c_str(), nullptr, dm);
    if (!dc) { Log("print preview: CreateDC failed for \"%s\"", ToUtf8(pr->name).c_str()); MessageBeep(MB_ICONERROR); return; }
    g_printPrinter = pr->name;
    SaveSettings();
    Tab* tab = nullptr;
    for (auto& t : g_tabs) if (t->id == s_tabId && t->state == TabState::Live) tab = t.get();
    Log("print preview: print %d cop%s on \"%s\"", s_copies, s_copies == 1 ? "y" : "ies", ToUtf8(pr->name).c_str());
    Close();
    if (!tab || !StartPrintJob(*tab, dc, settings)) MessageBeep(MB_ICONWARNING);
}

void ShowMenuFor(const Item& it) {
    HMENU m = CreatePopupMenu();
    std::vector<std::wstring> opts;
    int cur = -1;
    if (it.id == kPrinter) { for (auto& p : s_printers) opts.push_back(p.name); cur = s_printer; }
    else if (it.id == kPaper) { for (auto& p : s_papers) opts.push_back(p.name); cur = s_paper; }
    else if (it.id == kMargins) { for (int i = 0; i < 4; ++i) opts.push_back(MarginName(i)); cur = g_prefs.printMargins; }
    else if (it.id == kScale) {
        for (int i = 0; i < (int)ARRAYSIZE(kScales); ++i) { opts.push_back(std::to_wstring(kScales[i]) + L"%"); if (kScales[i] == g_prefs.printScale) cur = i; }
    }
    for (int i = 0; i < (int)opts.size(); ++i) AppendMenuW(m, MF_STRING | (i == cur ? MF_CHECKED : 0), (UINT_PTR)(i + 1), opts[i].c_str());
    POINT pt = { (LONG)it.r.X, (LONG)it.r.GetBottom() + (LONG)S(2) };
    ClientToScreen(s_wnd, &pt);
    const int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY, pt.x, pt.y, 0, s_wnd, nullptr);
    DestroyMenu(m);
    if (cmd <= 0 || cmd - 1 == cur) return;
    const int i = cmd - 1;
    if (it.id == kPrinter) { SelectPrinter(i); g_printPrinter = s_printers[i].name; }
    else if (it.id == kPaper) s_paper = i;
    else if (it.id == kMargins) g_prefs.printMargins = i;
    else g_prefs.printScale = kScales[i];
    Relayout();
}

void UpdateRanges() {
    wchar_t buf[256] = {};
    if (s_edit) GetWindowTextW(s_edit, buf, ARRAYSIZE(buf));
    PageRanges r;
    s_rangesOk = s_allPages || ParsePageRanges(buf, r);
    s_ranges = s_allPages || !s_rangesOk ? PageRanges() : r;
    if (!s_allPages && !s_rangesOk) s_ranges = { { 0x7fffffff, 0x7fffffff } };   // nothing (veil every sheet)
    InvalidateRect(s_wnd, nullptr, FALSE);
}

void Activate(const Item& it, int part) {
    switch (it.id) {
    case kPrinter: case kPaper: case kMargins: case kScale: ShowMenuFor(it); break;
    case kPages:
        s_allPages = part == 0;
        UpdateRanges();
        PositionEdit(GetGeo());
        if (!s_allPages) { SetFocus(s_edit); SendMessageW(s_edit, EM_SETSEL, 0, -1); }
        break;
    case kCopiesMinus: s_copies = std::max(1, s_copies - 1); break;
    case kCopiesPlus: s_copies = std::min(999, s_copies + 1); break;
    case kOrientation: if (s_landscape != (part == 1)) { s_landscape = part == 1; Relayout(); } break;
    case kColor: if (s_color != (part == 0)) { s_color = part == 0; ApplyChoices(); } break;
    case kHeaders: g_prefs.printHeaders = !g_prefs.printHeaders; SaveSettings(); break;
    case kBackground: g_prefs.printBackground = !g_prefs.printBackground; SaveSettings(); Relayout(); break;
    case kSystem: Close(); PrintActivePageSystemDialog(); return;
    case kCancel: case kCloseX: Close(); return;
    case kPrint: DoPrint(); return;
    }
    InvalidateRect(s_wnd, nullptr, FALSE);
}

bool HitTest(float x, float y, int* id, int* part) {
    *id = kNone;
    *part = -1;
    if (!s_wnd) return false;
    for (const Item& it : Items(GetGeo())) {
        RectF r = IsSwitch(it.id) ? RectF(it.r.X - kLabelW(), it.r.Y, it.r.Width + kLabelW(), it.r.Height) : it.r;   // the whole row
        if (it.id == kSystem) {                         // just the text
            RectF box;
            Graphics g(s_wnd);
            g.MeasureString(it.label.c_str(), -1, s_fBody.get(), PointF(0, 0), &box);
            r.Width = box.Width;
        }
        if (!r.Contains(x, y)) continue;
        if (!it.enabled || it.id == kRange) return false;
        *id = it.id;
        if (it.parts) *part = std::min(it.parts - 1, std::max(0, (int)((x - it.r.X) / (it.r.Width / it.parts))));
        return true;
    }
    return false;
}

LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN && w == VK_RETURN) { DoPrint(); return 0; }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { Close(); return 0; }
    if (m == WM_KEYDOWN && w == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) { SendMessageW(h, EM_SETSEL, 0, -1); return 0; }
    if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE || w == 1)) return 0;
    if (m == WM_SETFOCUS || m == WM_KILLFOCUS) InvalidateRect(s_wnd, nullptr, FALSE);
    return CallWindowProcW(s_oldEdit, h, m, w, l);
}

void MakeFonts() {
    const wchar_t* ui = L"Segoe UI";
    { FontFamily ff(L"Segoe UI Variable Text"); if (ff.IsAvailable()) ui = L"Segoe UI Variable Text"; }
    const wchar_t* ico = L"Segoe MDL2 Assets";
    { FontFamily ff(L"Segoe Fluent Icons"); if (ff.IsAvailable()) ico = L"Segoe Fluent Icons"; }
    s_fBody = std::make_unique<Font>(ui, S(13), FontStyleRegular, UnitPixel);
    s_fSmall = std::make_unique<Font>(ui, S(12), FontStyleRegular, UnitPixel);
    s_fBold = std::make_unique<Font>(ui, S(13), FontStyleBold, UnitPixel);
    s_fTitle = std::make_unique<Font>(ui, S(20), FontStyleBold, UnitPixel);
    s_fIcon = std::make_unique<Font>(ico, S(12), FontStyleRegular, UnitPixel);
    s_fCaption = std::make_unique<Font>(ico, S(10), FontStyleRegular, UnitPixel);
    if (s_editFont) DeleteObject(s_editFont);
    s_editFont = CreateFontW(-(int)std::round(S(13)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (s_fieldBrush) DeleteObject(s_fieldBrush);
    s_fieldBrush = CreateSolidBrush(ToColorRef(P().field));
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        s_wnd = h;
        s_edit = CreateOpaqueEdit(h, kEditId, GetModuleHandleW(nullptr));   // no visual style: our colours apply
        SendMessageW(s_edit, WM_SETFONT, (WPARAM)s_editFont, FALSE);
        SendMessageW(s_edit, EM_SETLIMITTEXT, 200, 0);
        s_oldEdit = (WNDPROC)SetWindowLongPtrW(s_edit, GWLP_WNDPROC, (LONG_PTR)EditProc);
        HDC dc = GetDC(s_edit);
        HGDIOBJ old = SelectObject(dc, s_editFont);
        TEXTMETRICW tm = {};
        GetTextMetricsW(dc, &tm);
        s_editH = tm.tmHeight;
        SelectObject(dc, old);
        ReleaseDC(s_edit, dc);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NCCALCSIZE:                                   // no system title bar (ui/WindowFrame.h), as in Settings
        if (wParam) return FrameCalcSize(h, wParam, lParam);
        break;
    case WM_NCHITTEST: {
        const LRESULT def = DefWindowProcW(h, msg, wParam, lParam);
        if (def != HTCLIENT) return def;
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(h, &pt);
        if (LRESULT top = FrameTopResizeHit(h, pt, S(5), S(16))) return top;
        const Geo G = GetGeo();
        const bool overClose = pt.x >= G.W - S(46) && pt.y < S(36);
        return (pt.y < S(44) && !overClose) ? HTCAPTION : HTCLIENT;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        Paint(hdc, rc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SIZE:
        ClampScroll();
        InvalidateRect(h, nullptr, FALSE);
        DrawMore();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = (MINMAXINFO*)lParam;
        mm->ptMinTrackSize.x = (LONG)S(820);
        mm->ptMinTrackSize.y = (LONG)S(640);
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        const Pal p = P();
        SetTextColor((HDC)wParam, ToColorRef(p.text));
        SetBkColor((HDC)wParam, ToColorRef(p.field));
        return (LRESULT)s_fieldBrush;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == kEditId && HIWORD(wParam) == EN_CHANGE) UpdateRanges();
        return 0;
    case WM_MOUSEWHEEL: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(h, &pt);
        const Geo G = GetGeo();
        if (pt.x < G.previewW) {
            s_scroll -= GET_WHEEL_DELTA_WPARAM(wParam) / 120.f * S(90);
            ClampScroll();
            InvalidateRect(h, nullptr, FALSE);
            DrawMore();
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!s_tracking) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
            s_tracking = true;
        }
        int id, part;
        HitTest((float)GET_X_LPARAM(lParam), (float)GET_Y_LPARAM(lParam), &id, &part);
        if (id != s_hover || part != s_hoverPart) { s_hover = id; s_hoverPart = part; InvalidateRect(h, nullptr, FALSE); }
        SetCursor(LoadCursor(nullptr, id == kSystem ? IDC_HAND : IDC_ARROW));
        return 0;
    }
    case WM_MOUSELEAVE:
        s_tracking = false;
        if (s_hover != kNone) { s_hover = kNone; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONDOWN: {
        int id, part;
        HitTest((float)GET_X_LPARAM(lParam), (float)GET_Y_LPARAM(lParam), &id, &part);
        s_down = id;
        if (GetFocus() == s_edit && id != kNone) SetFocus(h);
        else if (id == kNone) SetFocus(h);
        return 0;
    }
    case WM_LBUTTONUP: {
        int id, part;
        HitTest((float)GET_X_LPARAM(lParam), (float)GET_Y_LPARAM(lParam), &id, &part);
        const int down = s_down;
        s_down = kNone;
        if (id != kNone && id == down)
            for (const Item& it : Items(GetGeo())) if (it.id == id) { Activate(it, part); break; }
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) { Close(); return 0; }
        if (wParam == VK_RETURN) { DoPrint(); return 0; }
        if (wParam == VK_NEXT || wParam == VK_PRIOR || wParam == VK_HOME || wParam == VK_END || wParam == VK_DOWN || wParam == VK_UP) {
            const Geo G = GetGeo();
            const float page = G.sheetH + G.gap;
            if (wParam == VK_NEXT) s_scroll += page;
            else if (wParam == VK_PRIOR) s_scroll -= page;
            else if (wParam == VK_DOWN) s_scroll += S(60);
            else if (wParam == VK_UP) s_scroll -= S(60);
            else s_scroll = wParam == VK_HOME ? 0 : 1e9f;
            ClampScroll();
            InvalidateRect(h, nullptr, FALSE);
            DrawMore();
            return 0;
        }
        break;
    case WM_CLOSE:
        Close();
        return 0;
    case WM_DESTROY:
        EnableWindow(g_hWnd, TRUE);
        if (s_page) {
            WKPageEndPrinting(s_page);
            WKRelease(s_page);
        }
        if (s_frame) WKRelease(s_frame);
        s_page = nullptr;
        s_frame = nullptr;
        s_wnd = s_edit = nullptr;
        ++s_gen;                                          // late answers are dropped
        s_rects.clear();
        s_bitmaps.clear();
        s_bitmapFor.clear();
        s_computing = s_drawing = false;
        s_hover = s_down = kNone;
        SaveSettings();
        SetForegroundWindow(g_hWnd);
        return 0;
    }
    return DefWindowProcW(h, msg, wParam, lParam);
}

} // namespace

void PrintActivePage() {
    if (s_wnd) { SetForegroundWindow(s_wnd); return; }
    if (PrintInProgress()) { MessageBeep(MB_ICONWARNING); return; }
    Tab* t = ActiveTab();
    if (!t || !t->page) return;
    WKFrameRef frame = WKPageGetMainFrame(t->page);
    if (!frame) return;

    static bool registered = false;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hi;
        wc.lpszClassName = L"LyraPrintPreview";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = (HICON)SendMessageW(g_hWnd, WM_GETICON, ICON_BIG, 0);
        if (!RegisterClassExW(&wc)) return;
        registered = true;
    }
    s_tabId = t->id;
    s_page = t->page;
    WKRetain(s_page);
    s_frame = frame;
    WKRetain(s_frame);
    s_texts = PrintTextsOf(*t);
    s_allPages = true;
    s_copies = 1;
    s_ranges.clear();
    s_rangesOk = true;
    s_scroll = 0;
    MakeFonts();
    LoadPrinters();
    SelectPrinter(s_printer);

    RECT pr = {};
    GetWindowRect(g_hWnd, &pr);
    const int w = std::min((int)S(1120), (int)((pr.right - pr.left) * 0.94f)), hgt = std::min((int)S(780), (int)((pr.bottom - pr.top) * 0.94f));
    const int x = pr.left + ((pr.right - pr.left) - w) / 2, y = pr.top + ((pr.bottom - pr.top) - hgt) / 2;
    HWND h = CreateWindowExW(0, L"LyraPrintPreview", Tr(L"Print", L"Печать"),
                             WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MAXIMIZEBOX | WS_CLIPCHILDREN,
                             x, y, std::max(w, (int)S(820)), std::max(hgt, (int)S(640)), g_hWnd, nullptr, hi, nullptr);
    if (h) FrameApply(h);
    if (!h) { WKRelease(s_page); WKRelease(s_frame); s_page = nullptr; s_frame = nullptr; return; }
    EnableWindow(g_hWnd, FALSE);                          // modal, as print dialogs are
    Log("print preview: open for tab#%llu", t->id);
    Relayout();
    ShowWindow(h, SW_SHOW);
    ApplyDwm(h);
    SetFocus(h);
}
