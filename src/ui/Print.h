// Lyra — печать страницы (Ctrl+P): диалог печати Windows, страницы рисует WebKit.
#pragma once

#include "common/Platform.h"

// Shows the Windows print dialog for the active tab and prints the pages WebKit lays out for the
// chosen printer ("Microsoft Print to PDF" saves a PDF). Each page is drawn by the web process
// into a bitmap (WKPageDrawPageToImageForPrinting, a Lyra addition) and sent to the printer.
void PrintActivePage();
bool PrintInProgress();
