// Lyra — разрешения сайтов: уведомления и местоположение (запрос, запоминание, показ).
#pragma once

#include "common/Platform.h"
#include <functional>

// A site asks to show notifications or to know the location: Lyra asks once per site (origin)
// (a bubble under the address field) and remembers the answer in %LOCALAPPDATA%\Lyra\permissions.txt. Notifications appear as
// Windows notifications (a click brings Lyra and the tab forward); the location comes from
// Windows' location service (Settings > Privacy > Location must allow desktop apps).

// A question waiting for the user (shown by PermissionPrompt for the active tab).
struct PermissionAsk {
    unsigned long long seq = 0;      // unique per question
    unsigned long long tabId = 0;
    std::string kind;                // "notifications" | "geolocation"
    std::string origin;              // "https://example.com"
    std::wstring site;               // "example.com"
};
const PermissionAsk* PermissionsPendingForTab(unsigned long long tabId);
// answer: 1 allow (remembered), 0 block (remembered), -1 not now (asked again next time)
void PermissionsAnswer(unsigned long long seq, int answer);

void PermissionsInit();                               // after g_context
void PermissionsShutdown();

// UI client callbacks (PageClients.cpp).
void PermissionsDecideNotifications(WKPageRef page, WKSecurityOriginRef origin, WKNotificationPermissionRequestRef request);
void PermissionsDecideGeolocation(WKPageRef page, WKSecurityOriginRef origin, WKGeolocationPermissionRequestRef request);

void PermissionsTrayMessage(WPARAM wParam, LPARAM lParam);   // WM_APP_TRAY (main window)
// Lyra's own notification (extensions'), shown the way sites' are; one at a time: a new one
// replaces it (`closed(false)`). A click calls `click`, then `closed(true)`; timing out `closed(false)`.
void NotificationShow(const std::wstring& title, const std::wstring& body, std::function<void()> click, std::function<void(bool)> closed);
void NotificationHide();                              // takes Lyra's own notification away (no callback)
void PermissionsGeolocationResult(LPARAM lParam);              // WM_APP_GEO (main window)
void PermissionsReset();                              // Settings > Privacy: forget every answer
size_t PermissionsCount();
