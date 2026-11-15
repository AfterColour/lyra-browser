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
constexpr UINT_PTR TIMER_SPARE_TAB = 3;      // prepare the next new tab in the background (see PrepareSpareTab)
constexpr UINT_PTR TIMER_SPLIT_COVER = 6;    // split view: the divider picture waits for the halves, then fades
constexpr UINT_PTR TIMER_SPLIT_REPAINT = 5;  // split view: both halves draw again after a resize (see SplitView)
constexpr UINT_PTR TIMER_UPDATE = 7;          // look for an update (app/Updater.cpp)
constexpr UINT_PTR TIMER_TRANSITION = 4;     // the tab switch cover fades (see TabTransition)

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
constexpr UINT WM_APP_PRINT        = WM_APP + 8;   // window.print(); wParam tab id
constexpr UINT WM_APP_TRANSLATE    = WM_APP + 13;
constexpr UINT WM_APP_SPLIT_FOCUS  = WM_APP + 15;  // a click into the other half of the split view; wParam tab id
constexpr UINT WM_APP_CONTEXT_MENU = WM_APP + 14;  // a Lyra item of a page's context menu was chosen; wParam its tag  // a translation batch came back; lParam see Translate.cpp
constexpr UINT WM_APP_PIP          = WM_APP + 12;  // a page asked for picture-in-picture; wParam tab id, lParam 1 enter / 0 exit
constexpr UINT WM_APP_MEDIA        = WM_APP + 11;  // a media key / Windows media flyout button; wParam see SystemMedia.h
constexpr UINT WM_APP_TRAY         = WM_APP + 9;   // the notifications tray icon (Permissions.cpp)
constexpr UINT WM_APP_GEO          = WM_APP + 10;  // a location fix arrived; lParam see Permissions.cpp
constexpr UINT WM_APP_ADBLOCK      = WM_APP + 7;   // a filter list was converted; lParam see AdBlock.cpp
constexpr UINT WM_APP_TAB_PAINTED  = WM_APP + 6;   // a switched-to tab has drawn; wParam tab id (TabTransition)
constexpr UINT WM_APP_WALLPAPER    = WM_APP + 18;  // the picture of the day arrived: start pages show it
constexpr UINT WM_APP_UPDATE       = WM_APP + 17;  // the update state changed (app/Updater.cpp)
constexpr UINT WM_APP_EXT_INSTALL  = WM_APP + 21;  // an extension was downloaded; lParam see browser/ExtensionStore.cpp
constexpr UINT WM_APP_THEME        = WM_APP + 20;  // a theme was downloaded; wParam ok, lParam new std::wstring* (the .zip)
constexpr UINT WM_APP_PAGE_MENU    = WM_APP + 19;  // a page's context menu is ready to be shown (browser/ContextMenu.cpp)
constexpr UINT WM_APP_OPEN_URL     = WM_APP + 16;  // a second Lyra.exe handed over a URL; lParam new std::wstring*
// WM_COPYDATA tag of that hand-over (main.cpp -> MainWindow.cpp): 'LYRA'
constexpr ULONG_PTR kOpenUrlCopyData = 0x4C595241;
constexpr UINT WM_APP_LYRA_URL     = WM_APP + 4;   // a start page asked for lyra://...; wParam tab id, lParam new std::wstring*

enum Cmd {
    CMD_NEWTAB = 301, CMD_CLOSETAB, CMD_COPYURL,
    CMD_ZOOMIN, CMD_ZOOMOUT, CMD_ZOOMRESET, CMD_SETHOME, CMD_SETTINGS,
    CMD_REOPENTAB, CMD_DEVTOOLS, CMD_HISTORY, CMD_BOOKMARKS, CMD_BOOKMARK_PAGE, CMD_DOWNLOADS, CMD_ADBLOCK_SITE, CMD_FIND, CMD_PRINT, CMD_PASSWORDS, CMD_NEWPRIVATE, CMD_PIP, CMD_READER, CMD_TRANSLATE, CMD_PALETTE, CMD_SPLIT, CMD_UPDATE
};
