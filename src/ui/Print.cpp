// Lyra — печать страницы (Ctrl+P): предпросмотр Lyra или диалог печати Windows, страницы рисует WebKit.
//
// The job: 1. WKPageComputePagesForPrinting with the area between the margins splits the page into
// sheets. 2. One sheet at a time, WKPageDrawPageToImageForPrinting draws it at up to 300 dpi and the
// bitmap goes to the printer, with the header and footer if asked. Everything runs on the UI thread
// through WebKit's callbacks; the main window stays usable.
#include "ui/Print.h"
#include "app/Log.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "core/Localization.h"
#include "core/StringUtils.h"
#include "ui/MainWindow.h"

#include <WebKit/WKPagePrivate.h>

// ---- shared geometry and texts ------------------------------------------------------------

PaperGeo PaperGeoOf(HDC dc) {
    PaperGeo p;
    if (!dc) return p;
    p.dpiX = std::max(72, GetDeviceCaps(dc, LOGPIXELSX));
    p.dpiY = std::max(72, GetDeviceCaps(dc, LOGPIXELSY));
    p.printW = GetDeviceCaps(dc, HORZRES);
    p.printH = GetDeviceCaps(dc, VERTRES);
    p.physW = GetDeviceCaps(dc, PHYSICALWIDTH);
    p.physH = GetDeviceCaps(dc, PHYSICALHEIGHT);
    p.offX = GetDeviceCaps(dc, PHYSICALOFFSETX);
    p.offY = GetDeviceCaps(dc, PHYSICALOFFSETY);
    if (p.physW <= 0 || p.physH <= 0) { p.physW = p.printW; p.physH = p.printH; p.offX = p.offY = 0; }
    return p;
}

SheetGeo SheetGeoOf(const PaperGeo& p, float marginMm) {
    SheetGeo s;
    if (!p.Valid()) return s;
    const int wantX = (int)std::lround(marginMm / 25.4f * p.dpiX), wantY = (int)std::lround(marginMm / 25.4f * p.dpiY);
    s.left = std::max(wantX, p.offX);
    s.top = std::max(wantY, p.offY);
    s.right = std::max(wantX, p.physW - p.offX - p.printW);
    s.bottom = std::max(wantY, p.physH - p.offY - p.printH);
    s.areaW = std::max(1, p.physW - s.left - s.right);
    s.areaH = std::max(1, p.physH - s.top - s.bottom);
    return s;
}

PrintTexts PrintTextsOf(const Tab& t) {
    PrintTexts x;
    x.title = t.title;
    x.url = t.url;
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t d[64] = {}, tm[64] = {};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &now, nullptr, d, 64, nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &now, nullptr, tm, 64);
    x.date = std::wstring(d) + L", " + tm;
    return x;
}

bool HeadersFit(const PaperGeo& p, const SheetGeo& s) {
    if (!p.Valid()) return false;
    const int need = (int)(7.f / 25.4f * p.dpiY);      // about 7 mm for a line of 8 pt text
    return s.top >= need && s.bottom >= need;
}

void DrawHeadersFooters(Graphics& g, const PaperGeo& p, const SheetGeo& s, float scale, float x0, float y0,
                        const PrintTexts& texts, int number, int total) {
    if (!HeadersFit(p, s)) return;
    const float px = 8.f / 72.f * p.dpiY * scale;     // 8 pt
    if (px < 3) return;                                // too small to read in a thumbnail
    Font font(L"Segoe UI", px, FontStyleRegular, UnitPixel);
    SolidBrush ink(Color(255, 70, 70, 70));
    StringFormat sf;
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    const float left = x0 + s.left * scale, right = x0 + (p.physW - s.right) * scale, width = right - left;
    const float lineH = px * 1.6f;
    const float topY = y0 + s.top * scale / 2 - lineH / 2;
    const float bottomY = y0 + (p.physH - s.bottom / 2.f) * scale - lineH / 2;
    auto text = [&](const std::wstring& str, float x, float y, float w, StringAlignment a) {
        if (str.empty() || w <= 0) return;
        sf.SetAlignment(a);
        g.DrawString(str.c_str(), -1, &font, RectF(x, y, w, lineH), &sf, &ink);
    };
    // Date on the left; the title in the middle, as wide as it can be without touching the date.
    RectF dateBox;
    g.MeasureString(texts.date.c_str(), -1, &font, PointF(0, 0), &dateBox);
    text(texts.date, left, topY, width, StringAlignmentNear);
    const float titleW = std::max(0.f, width - 2 * (dateBox.Width + px));
    text(texts.title, left + (width - titleW) / 2, topY, titleW, StringAlignmentCenter);
    // Address on the left, "page / total" on the right.
    const std::wstring pageText = std::to_wstring(number) + L"/" + std::to_wstring(total);
    RectF pageBox;
    g.MeasureString(pageText.c_str(), -1, &font, PointF(0, 0), &pageBox);
    text(texts.url, left, bottomY, std::max(0.f, width - pageBox.Width - 2 * px), StringAlignmentNear);
    text(pageText, left, bottomY, width, StringAlignmentFar);
}

bool ParsePageRanges(const std::wstring& text, PageRanges& out) {
    out.clear();
    std::wstring s;
    for (wchar_t c : text) s += (c == L';') ? L',' : (c == 0x2013 || c == 0x2014) ? L'-' : c;
    size_t i = 0;
    auto skip = [&]() { while (i < s.size() && iswspace(s[i])) ++i; };
    auto number = [&](uint32_t& n) {
        skip();
        if (i >= s.size() || !iswdigit(s[i])) return false;
        unsigned long long v = 0;
        while (i < s.size() && iswdigit(s[i])) { v = v * 10 + (s[i] - L'0'); if (v > 100000) return false; ++i; }
        n = (uint32_t)v;
        return n >= 1;
    };
    skip();
    if (i >= s.size()) return true;                    // empty: all pages
    for (;;) {
        uint32_t from = 0, to = 0;
        if (!number(from)) return false;
        skip();
        if (i < s.size() && s[i] == L'-') {
            ++i;
            skip();
            if (i < s.size() && iswdigit(s[i])) { if (!number(to) || to < from) return false; }
            else to = 0;                               // "5-": to the end
        } else {
            to = from;
        }
        out.push_back({ from, to });
        skip();
        if (i >= s.size()) return true;
        if (s[i] != L',') return false;
        ++i;
        skip();
        if (i >= s.size()) return true;                // a trailing comma is fine
    }
}

bool PageInRanges(const PageRanges& ranges, uint32_t n) {
    if (ranges.empty()) return true;
    for (auto [from, to] : ranges) if (n >= from && (to == 0 || n <= to)) return true;
    return false;
}

// ---- the job ------------------------------------------------------------------------------

namespace {

struct PrintJob {
    unsigned long long tabId = 0;
    WKPageRef page = nullptr;            // retained
    WKFrameRef frame = nullptr;          // retained
    WKPrintInfo info {};
    HDC dc = nullptr;
    PrintSettings settings;
    PrintTexts texts;
    PaperGeo paper;
    SheetGeo sheet;
    std::vector<WKRect> pageRects;
    std::vector<uint32_t> pages;         // 0-based pages still to print, in order (copies included)
    size_t next = 0;
    int printedPages = 0;
    bool docStarted = false;
};

PrintJob* s_job = nullptr;

constexpr int kMaxDpi = 300;

void Finish(bool ok) {
    PrintJob* job = s_job;
    if (!job) return;
    s_job = nullptr;
    if (job->dc) {
        if (job->docStarted) {
            if (ok) EndDoc(job->dc);
            else AbortDoc(job->dc);
        }
        DeleteDC(job->dc);
    }
    if (job->page) {
        WKPageEndPrinting(job->page);
        WKRelease(job->page);
    }
    if (job->frame) WKRelease(job->frame);
    Log("print: %s, %d page(s)", ok ? "done" : "stopped", job->printedPages);
    delete job;
}

void DrawNextPage();

void OnPageImage(const void* pixels, uint32_t width, uint32_t height, uint32_t bytesPerRow, void* context) {
    PrintJob* job = (PrintJob*)context;
    if (job != s_job) return;                    // cancelled meanwhile
    if (!pixels || !width || !height) {
        Log("print: page %u could not be drawn", job->pages[job->next]);
        Finish(false);
        return;
    }
    if (!job->docStarted) {
        std::wstring title = !job->texts.title.empty() ? job->texts.title : std::wstring(L"Lyra");
        DOCINFOW di = { sizeof(di) };
        di.lpszDocName = title.c_str();
        if (StartDocW(job->dc, &di) <= 0) { Finish(false); return; }
        job->docStarted = true;
    }

    // Fit the bitmap into the area between the margins, keeping its proportions.
    const PaperGeo& P = job->paper;
    const SheetGeo& G = job->sheet;
    const double k = std::min((double)G.areaW / width, (double)G.areaH / height);
    const int destW = (int)std::lround(width * k), destH = (int)std::lround(height * k);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = (LONG)(bytesPerRow / 4);
    bmi.bmiHeader.biHeight = -(LONG)height;      // top-down rows
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    std::vector<BYTE> grey;
    if (job->settings.grayscale) {
        grey.assign((const BYTE*)pixels, (const BYTE*)pixels + (size_t)bytesPerRow * height);
        for (uint32_t y = 0; y < height; ++y) {
            BYTE* row = grey.data() + (size_t)y * bytesPerRow;
            for (uint32_t x = 0; x < width; ++x) {
                BYTE* px = row + x * 4;                  // B, G, R, A
                const BYTE l = (BYTE)((px[2] * 299 + px[1] * 587 + px[0] * 114) / 1000);
                px[0] = px[1] = px[2] = l;
            }
        }
        pixels = grey.data();
    }

    StartPage(job->dc);
    SetStretchBltMode(job->dc, HALFTONE);
    SetBrushOrgEx(job->dc, 0, 0, nullptr);
    // The device's origin is the printable area's corner, not the paper's.
    StretchDIBits(job->dc, G.left - P.offX, G.top - P.offY, destW, destH, 0, 0, (int)width, (int)height,
                  pixels, &bmi, DIB_RGB_COLORS, SRCCOPY);
    if (job->settings.headers) {
        Graphics g(job->dc);
        g.SetPageUnit(UnitPixel);
        DrawHeadersFooters(g, P, G, 1.f, (float)-P.offX, (float)-P.offY, job->texts,
                           (int)job->pages[job->next] + 1, (int)job->pageRects.size());
    }
    EndPage(job->dc);
    ++job->printedPages;
    ++job->next;
    DrawNextPage();
}

void DrawNextPage() {
    PrintJob* job = s_job;
    if (!job) return;
    if (job->next >= job->pages.size()) { Finish(true); return; }
    const uint32_t index = job->pages[job->next];
    const WKRect& r = job->pageRects[index];
    // Device pixels per CSS pixel: the page width spread over the area between the margins, at up to
    // 300 dpi (the printer scales the rest; a sheet at full printer resolution would take hundreds of MB).
    const double targetDpi = std::min(job->paper.dpiX, kMaxDpi);
    const double targetWidth = job->sheet.areaW * targetDpi / job->paper.dpiX;
    const float scale = r.size.width > 0 ? (float)(targetWidth / r.size.width) : 1.f;
    WKPageDrawPageToImageForPrinting(job->page, job->frame, job->info, index, scale, OnPageImage, job);
}

void OnPagesComputed(WKRect* pageRects, uint32_t pageCount, double, WKErrorRef error, void* context) {
    PrintJob* job = (PrintJob*)context;
    if (job != s_job) return;
    if (error || !pageCount) {
        Log("print: no pages (error=%d)", error ? 1 : 0);
        Finish(false);
        return;
    }
    job->pageRects.assign(pageRects, pageRects + pageCount);
    std::vector<uint32_t> once;
    for (uint32_t i = 0; i < pageCount; ++i) if (PageInRanges(job->settings.ranges, i + 1)) once.push_back(i);
    for (int c = 0; c < std::max(1, job->settings.copiesByLyra); ++c) job->pages.insert(job->pages.end(), once.begin(), once.end());
    Log("print: %u page(s) laid out, %zu to print", pageCount, job->pages.size());
    if (job->pages.empty()) { Finish(true); return; }
    DrawNextPage();
}

} // namespace

bool PrintInProgress() { return s_job != nullptr; }

bool StartPrintJob(Tab& t, HDC dc, const PrintSettings& settings) {
    if (s_job || !dc) { if (dc) DeleteDC(dc); return false; }
    WKFrameRef frame = t.page ? WKPageGetMainFrame(t.page) : nullptr;
    if (!frame) { DeleteDC(dc); return false; }
    auto* job = new PrintJob();
    job->tabId = t.id;
    job->dc = dc;
    job->settings = settings;
    job->texts = PrintTextsOf(t);
    job->paper = PaperGeoOf(dc);
    job->sheet = SheetGeoOf(job->paper, settings.marginMm);
    // WebKit wants the area in points.
    job->info.pageSetupScaleFactor = settings.scale > 0 ? settings.scale : 1;
    job->info.availablePaperWidth = job->sheet.areaW * 72.f / job->paper.dpiX;
    job->info.availablePaperHeight = job->sheet.areaH * 72.f / job->paper.dpiY;
    job->page = t.page;
    WKRetain(job->page);
    job->frame = frame;
    WKRetain(job->frame);
    s_job = job;
    Log("print: paper %.0fx%.0f pt at %d dpi, margin %.0f mm, scale %.2f", job->info.availablePaperWidth,
        job->info.availablePaperHeight, job->paper.dpiX, settings.marginMm, job->info.pageSetupScaleFactor);
    WKPageComputePagesForPrinting(job->page, job->frame, job->info, OnPagesComputed, job);
    return true;
}

void PrintActivePageSystemDialog() {
    if (s_job) { MessageBeep(MB_ICONWARNING); return; }
    Tab* t = ActiveTab();
    if (!t || !t->page) return;

    PRINTPAGERANGE ranges[16] = {};
    PRINTDLGEXW pd = {};
    pd.lStructSize = sizeof(pd);
    pd.hwndOwner = g_hWnd;
    pd.Flags = PD_RETURNDC | PD_NOSELECTION | PD_NOCURRENTPAGE | PD_USEDEVMODECOPIESANDCOLLATE;
    pd.nPageRanges = 0;
    pd.nMaxPageRanges = ARRAYSIZE(ranges);
    pd.lpPageRanges = ranges;
    pd.nMinPage = 1;
    pd.nMaxPage = 9999;
    pd.nCopies = 1;
    pd.nStartPage = START_PAGE_GENERAL;
    HRESULT hr = PrintDlgExW(&pd);
    if (pd.hDevMode) GlobalFree(pd.hDevMode);
    if (pd.hDevNames) GlobalFree(pd.hDevNames);
    if (FAILED(hr) || pd.dwResultAction != PD_RESULT_PRINT || !pd.hDC) {
        if (pd.hDC) DeleteDC(pd.hDC);
        return;
    }
    PrintSettings settings;
    settings.marginMm = 10;
    if (pd.Flags & PD_PAGENUMS)
        for (DWORD i = 0; i < pd.nPageRanges; ++i) settings.ranges.push_back({ ranges[i].nFromPage, ranges[i].nToPage });
    StartPrintJob(*t, pd.hDC, settings);
}
