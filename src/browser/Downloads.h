// Lyra — загрузки: сохранение файлов, прогресс, список загрузок, команды «открыть / в папке / отменить».
#pragma once

#include "common/Platform.h"

// WebKit hands a download over in one of three ways (see PageClients.cpp).
void OnActionBecameDownload(WKPageRef, WKNavigationActionRef, WKDownloadRef d, const void* ci);
void OnResponseBecameDownload(WKPageRef, WKNavigationResponseRef, WKDownloadRef d, const void* ci);
void OnContextMenuDownload(WKPageRef, WKDownloadRef d, const void*);

struct DownloadItem {
    enum State { Running, Done, Failed, Cancelled };
    long long id = 0;
    State state = Running;
    std::wstring url, path, name, error;
    long long received = 0, total = -1;     // bytes; total -1 = unknown
    long long started = 0, finished = 0;    // Unix ms
    double speed = 0;                       // bytes per second, smoothed (running only)
};

// The list survives restarts: %LOCALAPPDATA%\Lyra\downloads.tsv (newest first, at most 300).
void DownloadsLoad();
void DownloadsSave();
const std::vector<DownloadItem>& DownloadsList();      // newest first
const DownloadItem* DownloadGet(long long id);
int DownloadsRunning();
// Total progress of the running downloads, 0..1; -1 while some size is unknown.
float DownloadsProgress();
// A download finished a moment ago (the toolbar button lights up for a few seconds).
bool DownloadsJustFinished();

void DownloadCancel(long long id);
void DownloadRetry(long long id);           // downloads the address again (from the active tab)
void DownloadOpen(long long id);            // opens the file with its program
void DownloadShowInFolder(long long id);    // Explorer with the file selected
void DownloadRemove(long long id);          // from the list only, the file stays
void DownloadsClear();                      // every finished / failed entry
bool DownloadFileExists(const DownloadItem& d);

// Where files go: Settings > Downloads (empty = the Windows Downloads folder).
std::wstring DownloadsFolder();
void OpenDownloadsFolder();
// Folder picker for the setting; true if a new folder was chosen.
bool ChooseDownloadsFolder(HWND owner);

// Main window timer (every ~30 ms): passes progress on to the screen a few times a second.
void DownloadsTick();
// Called when the list or a download's progress changed (set by the UI): repaint, refresh pages.
extern void (*g_downloadsChanged)(bool listChanged);
