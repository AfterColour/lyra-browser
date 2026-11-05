// Lyra — кнопка загрузок: список последних загрузок под кнопкой (прогресс, открыть, показать в папке).
#pragma once

#include "common/Platform.h"

void DownloadsPopupToggle();        // the toolbar button
void DownloadsPopupShow();          // also when a download starts (Settings > Downloads)
void DownloadsPopupClose();
bool DownloadsPopupVisible();
void DownloadsPopupRefresh();       // the list or the progress changed

// "12.3 MB" / "12,3 МБ"
std::wstring FormatBytes(long long bytes);
// The grey line under a download's name: progress, speed and time left / size / what went wrong.
std::wstring DownloadStatusText(long long id);
