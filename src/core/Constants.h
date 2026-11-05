// Lyra — общие константы: размеры, идентификаторы, сообщения окна, команды меню.
#pragma once

#include "common/Platform.h"

// ----------------------------------------------------------------------------
//  Metrics (in 96-DPI pixels; scaled with S())
// ----------------------------------------------------------------------------
namespace metrics {
constexpr int kToolbarH   = 46;
constexpr int kTabBarH    = 34;
constexpr int kButton     = 32;
constexpr int kEdge       = 12;
constexpr int kPillH      = 30;
constexpr int kPillMaxW   = 720;
constexpr int kPillRadius = 8;
constexpr int kTabInsetY  = 4;
constexpr int kTabRadius  = 7;
constexpr int kTabMaxW    = 360;
constexpr int kTabMinW    = 30;
constexpr int kNewTabW    = 38;
constexpr int kCloseBtn   = 18;
constexpr int kMaxTabs    = 20;
}  // namespace metrics

constexpr int   ID_URL_EDIT = 1001;
constexpr UINT_PTR TIMER_UI = 1;

// Display name used in the window title, dialogs and the About page.
static const wchar_t kAppName[] = L"Lyra";

// Appended to WebKit's standard Windows user agent. Without a "Version/... Safari/..."
// token many sites (Google, YouTube, ...) treat the browser as unknown and serve a
// degraded page. This is the same token GNOME Web / WebKitGTK use.
static const char kUserAgentAppName[] = "Version/26.0 Safari/605.1.15";

// Work that must not run inside a WebKit callback is posted back to the main window.
// wParam = tab id.
constexpr UINT WM_APP_CLOSE_TAB    = WM_APP + 1;   // window.close() from a page
constexpr UINT WM_APP_TAB_CRASHED  = WM_APP + 2;   // lParam = WKProcessTerminationReason

enum Cmd {
    CMD_NEWTAB = 301, CMD_CLOSETAB, CMD_COPYURL,
    CMD_ZOOMIN, CMD_ZOOMOUT, CMD_ZOOMRESET, CMD_SETHOME, CMD_SETTINGS,
    CMD_REOPENTAB, CMD_DEVTOOLS
};
