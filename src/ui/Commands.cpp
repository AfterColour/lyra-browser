// Lyra — команды браузера, горячие клавиши и меню «Ещё».
#include "ui/Commands.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "core/Constants.h"
#include "core/Settings.h"
#include "ui/AddressBar.h"
#include "ui/Chrome.h"
#include "ui/MainWindow.h"
#include "ui/settings/SettingsWindow.h"

static void ShowWebInspector();
static void CopyToClipboard(const std::wstring& s);

static void ShowWebInspector() {
    Tab* t = ActiveTab();
    if (!t || !t->page) return;
    if (!g_prefs.devExtras) {
        MessageBoxW(g_hWnd, L"Developer tools are turned off. Enable them in Settings \x2192 Webpages.",
                    kAppName, MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (WKInspectorRef insp = WKPageGetInspector(t->page)) WKInspectorShow(insp);
}

void SetZoom(double z) {
    z = std::round(z * 100.0) / 100.0;
    g_zoom = std::max(0.25, std::min(5.0, z));
    for (auto& t : g_tabs) WKPageSetPageZoomFactor(t->page, g_zoom);
    SaveSettings();
}

void GoBack()    { if (Tab* t = ActiveTab()) if (WKPageCanGoBack(t->page))    WKPageGoBack(t->page); }

void GoForward() { if (Tab* t = ActiveTab()) if (WKPageCanGoForward(t->page)) WKPageGoForward(t->page); }

void ReloadActive(bool stopIfLoading) {
    Tab* t = ActiveTab();
    if (!t) return;
    if (stopIfLoading && t->loading) WKPageStopLoading(t->page);
    else WKPageReload(t->page);
}

static void CopyToClipboard(const std::wstring& s) {
    if (!OpenClipboard(g_hWnd)) return;
    EmptyClipboard();
    size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        memcpy(GlobalLock(h), s.c_str(), bytes);
        GlobalUnlock(h);
        if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
    }
    CloseClipboard();
}

void RunCommand(int cmd) {
    switch (cmd) {
    case CMD_NEWTAB:    CreateTab(g_prefs.newTabMode == 1 ? std::wstring(L"about:blank") : g_homePage); break;
    case CMD_SETTINGS:  ShowSettingsWindow(); break;
    case CMD_CLOSETAB:  CloseTab(g_active); break;
    case CMD_COPYURL:   if (Tab* t = ActiveTab()) CopyToClipboard(t->url); break;
    case CMD_ZOOMIN:    SetZoom(g_zoom + 0.1); break;
    case CMD_ZOOMOUT:   SetZoom(g_zoom - 0.1); break;
    case CMD_ZOOMRESET: SetZoom(1.0); break;
    case CMD_SETHOME:   if (Tab* t = ActiveTab()) { g_homePage = t->url; SaveSettings(); } break;
    case CMD_REOPENTAB: ReopenClosedTab(); break;
    case CMD_DEVTOOLS:  ShowWebInspector(); break;
    }
}

void ShowMoreMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CMD_NEWTAB,    L"New Tab\tCtrl+T");
    AppendMenuW(m, MF_STRING, CMD_CLOSETAB,  L"Close Tab\tCtrl+W");
    AppendMenuW(m, MF_STRING | (g_closedUrls.empty() ? MF_GRAYED : 0), CMD_REOPENTAB,
                L"Reopen Closed Tab\tCtrl+Shift+T");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_COPYURL,   L"Copy Address");
    AppendMenuW(m, MF_STRING, CMD_SETHOME,   L"Set Current Page as Home Page");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_ZOOMIN,    L"Zoom In\tCtrl++");
    AppendMenuW(m, MF_STRING, CMD_ZOOMOUT,   L"Zoom Out\tCtrl+-");
    AppendMenuW(m, MF_STRING, CMD_ZOOMRESET, L"Actual Size\tCtrl+0");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    if (g_prefs.devExtras) AppendMenuW(m, MF_STRING, CMD_DEVTOOLS, L"Developer Tools\tF12");
    AppendMenuW(m, MF_STRING, CMD_SETTINGS,  L"Settings...\tCtrl+,");
    POINT pt = { (LONG)g_L.more.GetRight(), (LONG)g_L.more.GetBottom() };
    ClientToScreen(g_hWnd, &pt);
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN | TPM_NONOTIFY,
                                  pt.x, pt.y, 0, g_hWnd, nullptr);
    DestroyMenu(m);
    if (cmd) RunCommand(cmd);
}

// ----------------------------------------------------------------------------
//  Keyboard shortcuts (handled before dispatch so they work while the web
//  view or the address field has focus)
// ----------------------------------------------------------------------------
bool HandleShortcut(const MSG& m) {
    if (m.message != WM_KEYDOWN && m.message != WM_SYSKEYDOWN) return false;
    if (!g_hWnd || !(m.hwnd == g_hWnd || IsChild(g_hWnd, m.hwnd))) return false;

    const bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt   = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const WPARAM k = m.wParam;

    if (ctrl && shift && !alt) {
        if (k == 'T') { ReopenClosedTab(); return true; }
        if (k == 'I') { ShowWebInspector(); return true; }
    }
    if (ctrl && !alt) {
        switch (k) {
        case 'T': RunCommand(CMD_NEWTAB); return true;
        case 'W': RunCommand(CMD_CLOSETAB); return true;
        case 'L': ShowUrlEdit(); return true;
        case VK_OEM_COMMA: RunCommand(CMD_SETTINGS); return true;
        case 'R': ReloadActive(false); return true;
        case VK_OEM_PLUS: case VK_ADD:         RunCommand(CMD_ZOOMIN); return true;
        case VK_OEM_MINUS: case VK_SUBTRACT:   RunCommand(CMD_ZOOMOUT); return true;
        case '0': case VK_NUMPAD0:             RunCommand(CMD_ZOOMRESET); return true;
        case VK_TAB: {
            int n = (int)g_tabs.size();
            if (n > 1) SelectTab(((g_active + (shift ? -1 : 1)) % n + n) % n);
            return true;
        }
        }
        if (k >= '1' && k <= '9') {
            int n = (int)g_tabs.size();
            SelectTab(k == '9' ? n - 1 : (int)(k - '1'));
            return true;
        }
    }
    if (alt && !ctrl) {
        if (k == VK_LEFT)  { GoBack(); return true; }
        if (k == VK_RIGHT) { GoForward(); return true; }
    }
    if (k == VK_F5) { ReloadActive(false); return true; }
    if (k == VK_F12) { ShowWebInspector(); return true; }
    return false;
}
