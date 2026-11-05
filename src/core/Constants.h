// Lyra — общие константы: размеры, идентификаторы, сообщения окна, команды меню.
#pragma once

#include "common/Platform.h"

// Sizes of the window chrome depend on the toolbar size setting: see ChromeDims in ui/Chrome.h.
namespace metrics {
constexpr int kMaxTabs = 20;
}  // namespace metrics

constexpr int   ID_URL_EDIT = 1001;
constexpr UINT_PTR TIMER_UI = 1;
constexpr UINT_PTR TIMER_TAB_UNFREEZE = 2;   // tabs may widen again after a close (see FreezeTabWidths)

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
constexpr UINT WM_APP_ANIM_FRAME   = WM_APP + 3;   // next animation frame (posted once per display refresh)
constexpr UINT WM_APP_FAVICON      = WM_APP + 5;   // a site icon download finished; lParam see Favicons.cpp
constexpr UINT WM_APP_LYRA_URL     = WM_APP + 4;   // a start page asked for lyra://...; wParam tab id, lParam new std::wstring*

enum Cmd {
    CMD_NEWTAB = 301, CMD_CLOSETAB, CMD_COPYURL,
    CMD_ZOOMIN, CMD_ZOOMOUT, CMD_ZOOMRESET, CMD_SETHOME, CMD_SETTINGS,
    CMD_REOPENTAB, CMD_DEVTOOLS, CMD_HISTORY
};
