// Lyra — блокировка рекламы и трекеров: списки EasyList, перевод в правила WebKit (Content Blockers).
#pragma once

#include "common/Platform.h"

// WebKit's content blockers do the blocking (compiled rule lists, matched in the web process
// without running any script). Lyra downloads the filter lists sites like uBlock and AdBlock use
// (EasyList, EasyPrivacy, RU AdList), turns them into WebKit's JSON rules and compiles them;
// compiled lists are kept on disk and loaded at once on the next start.

void AdBlockInit();                          // after g_context: loads compiled lists, updates in the background
void AdBlockShutdown();
WKUserContentControllerRef AdBlockUserContentController();   // for new page configurations
void AdBlockSetEnabled(bool enabled);        // Settings > Privacy
void AdBlockUpdateNow();                     // download and compile the lists again
void AdBlockListsReady(LPARAM);              // WM_APP_ADBLOCK (main thread)

// Sites where nothing is blocked (by host, subdomains included).
bool AdBlockAllowedOn(const std::wstring& url);   // true = blocking on for this page
void AdBlockSetAllowedOn(const std::wstring& url, bool blocking);
std::wstring AdBlockStatusText();            // for Settings: when the lists were updated, rule counts
