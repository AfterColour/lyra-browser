// Lyra — история посещений: запись, поиск, часто посещаемые сайты, сохранение на диск.
#pragma once

#include "common/Platform.h"

// One address the user has been to. Lyra keeps the last visit and a visit count per address
// in %LOCALAPPDATA%\Lyra\history.tsv.
struct HistoryEntry {
    std::wstring url, title;
    long long lastVisit = 0;    // Unix time, milliseconds
    int visits = 0;
};

void HistoryLoad();
void HistorySave();                         // writes only when something changed
void HistorySaveSoon();                     // main window timer: saves every ~30 s while changed

// A page was opened (`title` may still be empty) / got its title.
void HistoryVisit(const std::wstring& url, const std::wstring& title);
void HistorySetTitle(const std::wstring& url, const std::wstring& title);
bool HistoryRecordable(const std::wstring& url);

void HistoryRemove(const std::wstring& url);
// Removes visits newer than `sinceMs` (Unix ms); 0 clears everything.
void HistoryClearSince(long long sinceMs);
long long NowUnixMs();

// Newest first.
std::vector<HistoryEntry> HistoryNewest(size_t max);
// Address bar suggestions for `text`, best first.
std::vector<HistoryEntry> HistoryMatch(const std::wstring& text, size_t max);
// Most visited sites (one per site, the site's address), for the start page.
std::vector<HistoryEntry> HistoryTopSites(size_t max);
