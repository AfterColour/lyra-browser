// Lyra — очистка данных браузера: история, cookie и данные сайтов, кэш, загрузки, разрешения, пароли.
#pragma once

#include "common/Platform.h"

// What to clear, and how far back. Settings > Privacy > Clear browsing data (Ctrl+Shift+Del).
struct ClearDataChoice {
    int range = 0;              // index into ClearDataRanges(): last hour ... all time
    bool history = true;        // visited pages, address bar suggestions, recently closed tabs
    bool cookies = true;        // cookies and site data (signs out of most sites)
    bool cache = true;          // cached images and files
    bool downloads = false;     // the downloads list (the files stay)
    bool permissions = false;   // notifications and location answers
    bool passwords = false;     // saved passwords
};

// Labels of the time ranges, in the interface language.
std::vector<std::wstring> ClearDataRanges();

// Clears what is chosen; `done` runs on the main thread once the engine has removed its part.
void ClearBrowsingData(const ClearDataChoice& choice, std::function<void()> done);
