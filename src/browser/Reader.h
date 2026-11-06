// Lyra — режим чтения: статья без рекламы и меню, удобный шрифт и тема (Mozilla Readability).
#pragma once

#include "common/Platform.h"

struct Tab;

// After a page loads, a short check (Readability's isProbablyReaderable) decides whether it looks
// like an article; then a book appears in the address field. F9, the book or the "More" menu
// runs Readability on the page and opens the result as Lyra's reader page; again closes it.
void ReaderInit();                                   // clears the reader pages of the last run
void ReaderPageLoaded(Tab& t);                       // didFinish: may show the book
void ReaderToggle();                                 // F9 / the book / the menu, for the active tab
bool ReaderIsReaderUrl(const std::wstring& url);
std::wstring ReaderShownUrl(const std::wstring& url);   // the article's address for a reader page, else url
void ReaderLyraUrl(unsigned long long tabId, const std::wstring& command, const std::wstring& query);
