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
// Every script Lyra injects into pages goes through here (document start). The list is rebuilt
// when blocking is switched or a site allowed, since YouTube's ads are handled by a script too.
void AdBlockAddUserScript(const char* source, bool mainFrameOnly);
// Extensions add their content scripts (in worlds of their own) each time the list is rebuilt.
void AdBlockSetExtraUserScripts(void (*add)(WKUserContentControllerRef));
void AdBlockRefreshUserScripts();
void AdBlockSetEnabled(bool enabled);        // Settings > Privacy
void AdBlockUpdateNow();                     // download and compile the lists again
void AdBlockListsReady(LPARAM);              // WM_APP_ADBLOCK (main thread)

// Sites where nothing is blocked (by host, subdomains included).
bool AdBlockAllowedOn(const std::wstring& url);   // true = blocking on for this page
void AdBlockSetAllowedOn(const std::wstring& url, bool blocking);
std::wstring AdBlockStatusText();            // for Settings: when the lists were updated, rule counts
