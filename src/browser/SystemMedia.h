// Lyra — плашка Windows «Сейчас играет» и медиа-клавиши (System Media Transport Controls).
#pragma once

#include "common/Platform.h"

struct SystemMediaInfo {
    std::wstring title, artist, album, artworkUrl;
    bool playing = false, canNext = false, canPrevious = false;
};

// Buttons and media keys arrive as WM_APP_MEDIA (wParam = SystemMediaButton) on the main window.
enum SystemMediaButton { kMediaPlay = 0, kMediaPause = 1, kMediaStop = 2, kMediaNext = 6, kMediaPrevious = 7 };

bool SystemMediaInit(HWND window);
void SystemMediaShow(const SystemMediaInfo& info);
void SystemMediaHide();
void SystemMediaShutdown();
