// Lyra — печать страницы (Ctrl+P): предпросмотр Lyra или диалог печати Windows, страницы рисует WebKit.
#pragma once

#include "common/Platform.h"

struct Tab;

// Ctrl+P, the menu and window.print(): the print preview (ui/PrintPreview.cpp).
void PrintActivePage();
// Windows' own print dialog (a link in the preview): printer, pages and copies there.
void PrintActivePageSystemDialog();
bool PrintInProgress();

// ---- shared by the preview and the print job ----------------------------------------------

// A sheet of the chosen printer and paper, in device pixels.
struct PaperGeo {
    int dpiX = 0, dpiY = 0;
    int physW = 0, physH = 0;            // the whole sheet
    int offX = 0, offY = 0;              // where the printable area starts
    int printW = 0, printH = 0;          // the printable area
    bool Valid() const { return dpiX > 0 && dpiY > 0 && physW > 0 && physH > 0; }
};
PaperGeo PaperGeoOf(HDC dc);

// Where the page goes on the sheet: margins from the paper edge (never inside what the printer
// cannot reach) and the area between them, device pixels.
struct SheetGeo { int left = 0, top = 0, right = 0, bottom = 0, areaW = 1, areaH = 1; };
SheetGeo SheetGeoOf(const PaperGeo& p, float marginMm);

// Header and footer text, as in other browsers: date and title above, address and page number below.
struct PrintTexts { std::wstring title, url, date; };
PrintTexts PrintTextsOf(const Tab& t);
// Whether a margin leaves room for them.
bool HeadersFit(const PaperGeo& p, const SheetGeo& s);
// Draws them for page `number` of `total`. `scale` maps device pixels of the sheet to `g`'s units;
// (x0, y0) is where the sheet's top left corner is in `g`.
void DrawHeadersFooters(Graphics& g, const PaperGeo& p, const SheetGeo& s, float scale, float x0, float y0,
                        const PrintTexts& texts, int number, int total);

// Page ranges typed by the user: "1-3, 5, 8-" (1-based, inclusive; to = 0 means "to the end").
// False if the text is not a list of ranges. Empty text: all pages (an empty list).
using PageRanges = std::vector<std::pair<uint32_t, uint32_t>>;
bool ParsePageRanges(const std::wstring& text, PageRanges& out);
bool PageInRanges(const PageRanges& ranges, uint32_t number);      // 1-based; empty = all

struct PrintSettings {
    float marginMm = 10;
    float scale = 1;                     // WKPrintInfo::pageSetupScaleFactor
    bool headers = false;
    bool grayscale = false;              // black and white: Lyra makes the pages grey itself (some drivers,
                                         // "Microsoft Print to PDF" among them, ignore DEVMODE's colour)
    int copiesByLyra = 1;                // the driver cannot make copies itself: Lyra sends the pages again
    PageRanges ranges;                   // empty: all pages
};
// Prints the tab's page on `dc` (taken over: deleted when done). Runs in the background through
// WebKit's callbacks; false if a job is already running.
bool StartPrintJob(Tab& t, HDC dc, const PrintSettings& settings);
