// Lyra — разрешения сайтов: уведомления и местоположение (запрос, запоминание, показ).
#pragma once

#include "common/Platform.h"

// A site asks to show notifications or to know the location: Lyra asks once per site (origin)
// and remembers the answer in %LOCALAPPDATA%\Lyra\permissions.txt. Notifications appear as
// Windows notifications (a click brings Lyra and the tab forward); the location comes from
// Windows' location service (Settings > Privacy > Location must allow desktop apps).

void PermissionsInit();                               // after g_context
void PermissionsShutdown();

// UI client callbacks (PageClients.cpp).
void PermissionsDecideNotifications(WKPageRef page, WKSecurityOriginRef origin, WKNotificationPermissionRequestRef request);
void PermissionsDecideGeolocation(WKPageRef page, WKSecurityOriginRef origin, WKGeolocationPermissionRequestRef request);

void PermissionsTrayMessage(WPARAM wParam, LPARAM lParam);   // WM_APP_TRAY (main window)
void PermissionsGeolocationResult(LPARAM lParam);              // WM_APP_GEO (main window)
void PermissionsReset();                              // Settings > Privacy: forget every answer
size_t PermissionsCount();
