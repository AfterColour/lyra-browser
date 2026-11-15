// Lyra — содержимое разделов настроек и сброс к значениям по умолчанию.
#pragma once

#include "common/Platform.h"

struct EngineDef { const wchar_t* name; const wchar_t* url; };
static const EngineDef kEngines[] = {
    { L"DuckDuckGo", L"https://duckduckgo.com/?q=" },
    { L"Google",     L"https://www.google.com/search?q=" },
    { L"Bing",       L"https://www.bing.com/search?q=" },
    { L"Brave",      L"https://search.brave.com/search?q=" },
    { L"Startpage",  L"https://www.startpage.com/do/search?q=" },
    { L"Ecosia",     L"https://www.ecosia.org/search?q=" },
    { L"Yandex",     L"https://yandex.com/search/?text=" },
};
constexpr int kEngineCount = (int)(sizeof(kEngines) / sizeof(kEngines[0]));

void BuildPages();
int  SettingsExtensionPage(size_t k);  // the page of the k-th extension in ExtensionsList(), -1 if none
int  SettingsThemePage(size_t k);    // the page of the k-th theme in CustomThemes() (its gear), -1 if none
void OpenPasswordsSettings();      // Settings > Privacy > Saved passwords (menu, key in the address field)
void OpenClearDataSettings();      // Settings > Privacy > Clear browsing data (Ctrl+Shift+Del)
