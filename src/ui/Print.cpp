// Lyra — печать страницы (Ctrl+P): диалог печати Windows, страницы рисует WebKit.
//
// 1. PrintDlgEx: printer, pages, copies. 2. WKPageComputePagesForPrinting with the printable
// size of the paper (minus Lyra's margins) splits the page into sheets. 3. One sheet at a time,
// WKPageDrawPageToImageForPrinting draws it at up to 300 dpi and the bitmap goes to the printer.
// Everything runs on the UI thread through WebKit's callbacks; the main window stays usable.
#include "ui/Print.h"
#include "app/Log.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "core/Localization.h"
#include "core/StringUtils.h"
#include "ui/MainWindow.h"

#include <WebKit/WKPagePrivate.h>

namespace {

struct PrintJob {
    unsigned long long tabId = 0;
    WKPageRef page = nullptr;            // retained
    WKFrameRef frame = nullptr;          // retained
    WKPrintInfo info {};
    HDC dc = nullptr;
    std::vector<std::pair<uint32_t, uint32_t>> ranges;   // 1-based, inclusive; empty = all pages
    std::vector<WKRect> pageRects;
    std::vector<uint32_t> pages;         // 0-based pages still to print, in order
    size_t next = 0;
    int printedPages = 0;
    bool docStarted = false;
    // Device geometry
    int dpiX = 0, dpiY = 0;
    int marginX = 0, marginY = 0;        // device pixels from the printable area's corner
    int areaW = 0, areaH = 0;            // printable width / height minus margins, device pixels
};

PrintJob* s_job = nullptr;

constexpr float kMarginInches = 0.4f;
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
        Tab* t = nullptr;
        for (auto& tab : g_tabs) if (tab->id == job->tabId) t = tab.get();
        std::wstring title = t && !t->title.empty() ? t->title : std::wstring(L"Lyra");
        DOCINFOW di = { sizeof(di) };
        di.lpszDocName = title.c_str();
        if (StartDocW(job->dc, &di) <= 0) { Finish(false); return; }
        job->docStarted = true;
    }

    // Fit the bitmap into the area between the margins, keeping its proportions.
    const double sx = (double)job->areaW / width, sy = (double)job->areaH / height;
    const double k = std::min(sx, sy);
    const int destW = (int)std::lround(width * k), destH = (int)std::lround(height * k);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = (LONG)(bytesPerRow / 4);
    bmi.bmiHeader.biHeight = -(LONG)height;      // top-down rows
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    StartPage(job->dc);
    SetStretchBltMode(job->dc, HALFTONE);
    SetBrushOrgEx(job->dc, 0, 0, nullptr);
    StretchDIBits(job->dc, job->marginX, job->marginY, destW, destH, 0, 0, (int)width, (int)height,
                  pixels, &bmi, DIB_RGB_COLORS, SRCCOPY);
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
    // Device pixels per CSS pixel: the page width spread over the printable width, at up to 300 dpi
    // (the printer scales the rest; a sheet at full printer resolution would take hundreds of MB).
    const double targetDpi = std::min(job->dpiX, kMaxDpi);
    const double targetWidth = job->areaW * targetDpi / job->dpiX;
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
    if (job->ranges.empty()) {
        for (uint32_t i = 0; i < pageCount; ++i) job->pages.push_back(i);
    } else {
        for (auto [from, to] : job->ranges)
            for (uint32_t p = std::max(1u, from); p <= std::min(to, pageCount); ++p) job->pages.push_back(p - 1);
    }
    Log("print: %u page(s) laid out, %zu to print", pageCount, job->pages.size());
    if (job->pages.empty()) { Finish(true); return; }
    DrawNextPage();
}

} // namespace

bool PrintInProgress() { return s_job != nullptr; }

void PrintActivePage() {
    if (s_job) { MessageBeep(MB_ICONWARNING); return; }
    Tab* t = ActiveTab();
    if (!t || !t->page) return;
    WKFrameRef frame = WKPageGetMainFrame(t->page);
    if (!frame) return;

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
    auto freeDialog = [&]() {
        if (pd.hDevMode) GlobalFree(pd.hDevMode);
        if (pd.hDevNames) GlobalFree(pd.hDevNames);
    };
    if (FAILED(hr) || pd.dwResultAction != PD_RESULT_PRINT || !pd.hDC) {
        if (pd.hDC) DeleteDC(pd.hDC);
        freeDialog();
        return;
    }
    freeDialog();

    auto* job = new PrintJob();
    job->tabId = t->id;
    job->dc = pd.hDC;
    if (pd.Flags & PD_PAGENUMS)
        for (DWORD i = 0; i < pd.nPageRanges; ++i) job->ranges.push_back({ ranges[i].nFromPage, ranges[i].nToPage });

    job->dpiX = std::max(72, GetDeviceCaps(job->dc, LOGPIXELSX));
    job->dpiY = std::max(72, GetDeviceCaps(job->dc, LOGPIXELSY));
    const int printableW = GetDeviceCaps(job->dc, HORZRES), printableH = GetDeviceCaps(job->dc, VERTRES);
    const int physW = GetDeviceCaps(job->dc, PHYSICALWIDTH), physH = GetDeviceCaps(job->dc, PHYSICALHEIGHT);
    const int offX = GetDeviceCaps(job->dc, PHYSICALOFFSETX), offY = GetDeviceCaps(job->dc, PHYSICALOFFSETY);
    // Lyra's margin from the paper edge, but never inside the area the printer cannot reach.
    const int wantX = (int)(kMarginInches * job->dpiX), wantY = (int)(kMarginInches * job->dpiY);
    const int left = std::max(wantX, offX), top = std::max(wantY, offY);
    const int right = std::max(wantX, physW - offX - printableW), bottom = std::max(wantY, physH - offY - printableH);
    job->marginX = left - offX;
    job->marginY = top - offY;
    job->areaW = std::max(1, (physW > 0 ? physW : printableW) - left - right);
    job->areaH = std::max(1, (physH > 0 ? physH : printableH) - top - bottom);

    // WebKit wants the paper in points.
    job->info.pageSetupScaleFactor = 1;
    job->info.availablePaperWidth = job->areaW * 72.f / job->dpiX;
    job->info.availablePaperHeight = job->areaH * 72.f / job->dpiY;

    job->page = t->page;
    WKRetain(job->page);
    job->frame = frame;
    WKRetain(job->frame);
    s_job = job;
    Log("print: paper %.0fx%.0f pt at %d dpi", job->info.availablePaperWidth, job->info.availablePaperHeight, job->dpiX);
    WKPageComputePagesForPrinting(job->page, job->frame, job->info, OnPagesComputed, job);
}
