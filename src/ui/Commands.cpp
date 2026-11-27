// Lyra — команды браузера, горячие клавиши и меню «Ещё».
#include "browser/SiteZoom.h"
#include "ui/Commands.h"
#include "ui/FullScreen.h"
#include "browser/Translate.h"
#include "browser/Reader.h"
#include "browser/PictureInPicture.h"
#include "ui/settings/SettingsPages.h"
#include "browser/AdBlock.h"
#include "ui/FindBar.h"
#include "ui/Print.h"
#include "browser/StartPage.h"
#include "core/Url.h"
#include "core/Localization.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "core/Constants.h"
#include "core/Settings.h"
#include "ui/AddressBar.h"
#include "ui/BookmarkPopup.h"
#include "ui/Chrome.h"
#include "ui/Customize.h"
#include "browser/Workspaces.h"
#include "ui/CommandPalette.h"
#include "ui/Screenshot.h"
#include "ui/BookmarksBar.h"
#include "ui/Sidebar.h"
#include "browser/SplitView.h"
#include "ui/MainWindow.h"
#include "ui/Menu.h"
#include "ui/Theme.h"
#include "ui/settings/SettingsWindow.h"
#include "app/Updater.h"

static void ShowWebInspector();
static void CopyToClipboard(const std::wstring& s);

static void ShowWebInspector() {
    Tab* t = ActiveTab();
    if (!t || !t->page) return;
    if (!g_prefs.devExtras) {
        MessageBoxW(g_hWnd, Tr(L"Developer tools are turned off. Turn them on in Settings \x2192 Webpages.", L"Инструменты разработчика выключены. Включите их в «Настройки \x2192 Веб-страницы»."),
                    kAppName, MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (WKInspectorRef insp = WKPageGetInspector(t->page)) WKInspectorShow(insp);
}

void SetZoom(double z) {
    z = std::round(z * 100.0) / 100.0;
    g_zoom = std::max(0.25, std::min(5.0, z));
    SiteZoomDefaultChanged();            // (sites with their own zoom keep it)
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
    case CMD_NEWTAB:
        CreateTab(g_prefs.newTabMode == 1 ? std::wstring(L"about:blank")
                  : g_prefs.newTabMode == 2 ? g_homePage : std::wstring(kStartAlias));
        break;
    case CMD_NEWPRIVATE: CreatePrivateTab(); break;
    case CMD_PIP:       PipToggleActive(); break;
    case CMD_READER:    ReaderToggle(); break;
    case CMD_TRANSLATE: TranslateToggle(); break;
    case CMD_PALETTE:   CommandPaletteShow(); break;
    case CMD_TABSEARCH: CommandPaletteShowTabs(); break;
    case CMD_SCREENSHOT: ScreenshotMenu(); break;
    case CMD_BOOKMARKSBAR: BookmarksBarToggle(); break;
    case CMD_SIDEBAR: SidebarToggle(); break;
    case CMD_READINGLIST: SidebarAddCurrentToReadingList(); SidebarOpen(2); break;
    case CMD_CUSTOMIZE: if (CustomizeActive()) CustomizeEnd(); else CustomizeBegin(); break;
    case CMD_SPLIT:     SplitToggle(); break;
    case CMD_UPDATE:    UpdaterRestartAndInstall(); break;
    case CMD_SETTINGS:  ShowSettingsWindow(); break;
    case CMD_CLOSETAB:  CloseTab(g_active); break;
    case CMD_COPYURL:   if (Tab* t = ActiveTab()) CopyToClipboard(t->url); break;
    case CMD_ZOOMIN:    SiteZoomStep(1); break;           // the open tab's site (browser/SiteZoom)
    case CMD_ZOOMOUT:   SiteZoomStep(-1); break;
    case CMD_ZOOMRESET: SiteZoomReset(); break;
    case CMD_SETHOME:
        if (Tab* t = ActiveTab()) { g_homePage = IsStartPageUrl(t->url) ? std::wstring(kStartAlias) : t->url; SaveSettings(); }
        break;
    case CMD_REOPENTAB: ReopenClosedTab(); break;
    case CMD_DEVTOOLS:  ShowWebInspector(); break;
    case CMD_HISTORY:   OpenHistory(); break;
    case CMD_BOOKMARKS: OpenBookmarks(); break;
    case CMD_DOWNLOADS: OpenDownloads(); break;
    case CMD_BOOKMARK_PAGE: BookmarkCurrentPage(); break;
    case CMD_FIND:      FindBarShow(); break;
    case CMD_PRINT:     PrintActivePage(); break;
    case CMD_PASSWORDS: OpenPasswordsSettings(); break;
    case CMD_ADBLOCK_SITE:
        if (Tab* t = ActiveTab()) {
            AdBlockSetAllowedOn(t->url, !AdBlockAllowedOn(t->url));
            WKPageReload(t->page);
        }
        break;
    }
}

void ShowMoreMenu() {
    HMENU m = CreatePopupMenu();
    if (UpdaterReady()) {                // a downloaded update: first in the menu
        const std::wstring label = std::wstring(Tr(L"Restart to Update to ", L"Перезапустить и обновить до ")) + UpdaterReadyVersion();
        AppendMenuW(m, MF_STRING, CMD_UPDATE, label.c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(m, MF_STRING, CMD_NEWTAB,    Tr(L"New Tab\tCtrl+T", L"Новая вкладка\tCtrl+T"));
    AppendMenuW(m, MF_STRING, CMD_NEWPRIVATE, Tr(L"New Private Tab\tCtrl+Shift+N", L"Новая приватная вкладка\tCtrl+Shift+N"));
    AppendMenuW(m, MF_STRING, CMD_CLOSETAB,  Tr(L"Close Tab\tCtrl+W", L"Закрыть вкладку\tCtrl+W"));
    AppendMenuW(m, MF_STRING | (g_closedTabs.empty() ? MF_GRAYED : 0), CMD_REOPENTAB,
                Tr(L"Reopen Closed Tab\tCtrl+Shift+T", L"Вернуть закрытую вкладку\tCtrl+Shift+T"));
    // The last closed tabs, newest first (a click brings that one back).
    constexpr UINT kClosedBase = 6000;
    const int closedShown = std::min<int>((int)g_closedTabs.size(), 10);
    std::vector<std::wstring> closedLabels;
    if (closedShown > 1) {
        HMENU sub = CreatePopupMenu();
        for (int k = 0; k < closedShown; ++k) {
            const ClosedTab& c = g_closedTabs[g_closedTabs.size() - 1 - k];
            std::wstring label = c.title.empty() ? c.url : c.title;
            if (label.size() > 60) label = label.substr(0, 57) + L"\x2026";
            for (auto& ch : label) if (ch == L'&') ch = L'+';
            closedLabels.push_back(label);
            AppendMenuW(sub, MF_STRING, kClosedBase + k, closedLabels.back().c_str());
        }
        AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, Tr(L"Recently Closed", L"Недавно закрытые"));
    }
    AppendMenuW(m, MF_STRING, CMD_HISTORY,   Tr(L"History\tCtrl+H", L"История\tCtrl+H"));
    AppendMenuW(m, MF_STRING, CMD_BOOKMARKS, Tr(L"Bookmarks\tCtrl+Shift+O", L"Закладки\tCtrl+Shift+O"));
    AppendMenuW(m, MF_STRING | (BookmarksBarShown() ? MF_CHECKED : 0), CMD_BOOKMARKSBAR,
                Tr(L"Show Bookmarks Bar\tCtrl+Shift+B", L"Панель закладок\tCtrl+Shift+B"));
    AppendMenuW(m, MF_STRING | (SidebarPanelOpen() ? MF_CHECKED : 0), CMD_SIDEBAR,
                Tr(L"Sidebar\tCtrl+Shift+E", L"Боковая панель\tCtrl+Shift+E"));
    {
        const Tab* t = ActiveTab();
        AppendMenuW(m, MF_STRING | ((t && CanBookmark(t->url)) ? 0 : MF_GRAYED), CMD_READINGLIST,
                    Tr(L"Add to Reading List", L"Добавить в список для чтения"));
    }
    AppendMenuW(m, MF_STRING, CMD_PASSWORDS, Tr(L"Passwords", L"Пароли"));
    AppendMenuW(m, MF_STRING, CMD_DOWNLOADS, Tr(L"Downloads\tCtrl+J", L"Загрузки\tCtrl+J"));
    {
        const Tab* t = ActiveTab();
        AppendMenuW(m, MF_STRING | ((t && CanBookmark(t->url)) ? 0 : MF_GRAYED), CMD_BOOKMARK_PAGE,
                    (t && IsBookmarkedCached(t->url)) ? Tr(L"Edit Bookmark\tCtrl+D", L"Изменить закладку\tCtrl+D")
                                                      : Tr(L"Bookmark This Page\tCtrl+D", L"Добавить в закладки\tCtrl+D"));
    }
    if (g_prefs.adBlock) {
        const Tab* t = ActiveTab();
        bool web = t && (t->url.compare(0, 7, L"http://") == 0 || t->url.compare(0, 8, L"https://") == 0);
        AppendMenuW(m, MF_STRING | (web ? 0 : MF_GRAYED) | (web && AdBlockAllowedOn(t->url) ? MF_CHECKED : 0), CMD_ADBLOCK_SITE,
                    Tr(L"Block Ads on This Site", L"\u0411\u043b\u043e\u043a\u0438\u0440\u043e\u0432\u0430\u0442\u044c \u0440\u0435\u043a\u043b\u0430\u043c\u0443 \u043d\u0430 \u044d\u0442\u043e\u043c \u0441\u0430\u0439\u0442\u0435"));
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_COPYURL,   Tr(L"Copy Address", L"Копировать адрес"));
    AppendMenuW(m, MF_STRING, CMD_SETHOME,   Tr(L"Set Current Page as Home Page", L"Сделать эту страницу домашней"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    {
        Tab* rt = ActiveTab();
        const bool inReader = rt && ReaderIsReaderUrl(rt->url);
        AppendMenuW(m, MF_STRING | (inReader ? MF_CHECKED : 0) | ((rt && (rt->readerable || inReader)) ? 0 : MF_GRAYED), CMD_READER,
                    Tr(L"Reader View\tF9", L"Режим чтения\tF9"));
    }
    {
        Tab* tt = ActiveTab();
        const bool web = tt && (tt->url.compare(0, 7, L"http://") == 0 || tt->url.compare(0, 8, L"https://") == 0);
        const std::wstring label = (tt && tt->translated) ? std::wstring(Tr(L"Show Original", L"Показать оригинал"))
            : std::wstring(Tr(L"Translate to ", L"Перевести: ")) + TranslateTargetName();
        AppendMenuW(m, MF_STRING | (web ? 0 : MF_GRAYED), CMD_TRANSLATE, label.c_str());
    }
    AppendMenuW(m, MF_STRING | (PipActive() ? MF_CHECKED : 0), CMD_PIP, Tr(L"Picture in Picture", L"Картинка в картинке"));
    AppendMenuW(m, MF_STRING | (SplitShown() ? MF_CHECKED : 0), CMD_SPLIT, Tr(L"Split View", L"Разделённый экран"));
    AppendMenuW(m, MF_STRING, CMD_PALETTE,   Tr(L"Command Palette\x2026\tCtrl+K", L"Палитра команд\x2026\tCtrl+K"));
    AppendMenuW(m, MF_STRING, CMD_TABSEARCH, Tr(L"Search Tabs\x2026\tCtrl+Shift+A", L"Поиск по вкладкам\x2026\tCtrl+Shift+A"));
    AppendMenuW(m, MF_STRING, CMD_FIND,      Tr(L"Find on Page\x2026\tCtrl+F", L"Найти на странице\x2026\tCtrl+F"));
    AppendMenuW(m, MF_STRING, CMD_SCREENSHOT, Tr(L"Screenshot\x2026\tCtrl+Shift+S", L"Снимок страницы\x2026\tCtrl+Shift+S"));
    AppendMenuW(m, MF_STRING | (PrintInProgress() ? MF_GRAYED : 0), CMD_PRINT,
                Tr(L"Print\x2026\tCtrl+P", L"Печать\x2026\tCtrl+P"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_ZOOMIN,    Tr(L"Zoom In\tCtrl++", L"Увеличить\tCtrl++"));
    AppendMenuW(m, MF_STRING, CMD_ZOOMOUT,   Tr(L"Zoom Out\tCtrl+-", L"Уменьшить\tCtrl+-"));
    AppendMenuW(m, MF_STRING, CMD_ZOOMRESET, Tr(L"Actual Size\tCtrl+0", L"Исходный размер\tCtrl+0"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    if (g_prefs.devExtras) AppendMenuW(m, MF_STRING, CMD_DEVTOOLS, Tr(L"Developer Tools\tF12", L"Инструменты разработчика\tF12"));
    AppendMenuW(m, MF_STRING | (CustomizeActive() ? MF_CHECKED : 0), CMD_CUSTOMIZE, Tr(L"Customize Toolbar\x2026", L"Настроить интерфейс\x2026"));
    AppendMenuW(m, MF_STRING, CMD_SETTINGS,  Tr(L"Settings\x2026\tCtrl+,", L"Настройки\x2026\tCtrl+,"));
    static const struct { UINT id; const wchar_t* glyph; } icons[] = {
        { CMD_UPDATE, L"\uE72C" }, { CMD_NEWTAB, L"\uE710" }, { CMD_NEWPRIVATE, L"\uED1A" }, { CMD_CLOSETAB, L"\uE711" },
        { CMD_REOPENTAB, L"\uE7A7" }, { CMD_HISTORY, L"\uE81C" }, { CMD_BOOKMARKS, L"\uE728" }, { CMD_PASSWORDS, L"\uE8D7" },
        { CMD_DOWNLOADS, L"\uE896" }, { CMD_BOOKMARK_PAGE, L"\uE734" }, { CMD_ADBLOCK_SITE, L"\uE72E" }, { CMD_COPYURL, L"\uE71B" },
        { CMD_SETHOME, L"\uE80F" }, { CMD_READER, L"\uE736" }, { CMD_TRANSLATE, L"\uE8C1" }, { CMD_PIP, L"\uE8B9" },
        { CMD_SPLIT, L"\uE8A0" }, { CMD_PALETTE, L"\uE765" }, { CMD_TABSEARCH, L"\uE7C4" }, { CMD_SCREENSHOT, L"\uE722" }, { CMD_FIND, L"\uE721" }, { CMD_PRINT, L"\uE749" },
        { CMD_ZOOMIN, L"\uE8A3" }, { CMD_ZOOMOUT, L"\uE71F" }, { CMD_ZOOMRESET, L"\uE71E" }, { CMD_DEVTOOLS, L"\uE943" },
        { CMD_CUSTOMIZE, L"\uE771" }, { CMD_SETTINGS, L"\uE713" },
    };
    for (auto& i : icons) MenuSetIcon(m, i.id, i.glyph);
    const RectF anchor = ChromeMenuAnchor();
    POINT pt = { (LONG)anchor.GetRight(), (LONG)(anchor.GetBottom() + S(4)) };
    ClientToScreen(g_hWnd, &pt);
    int cmd = MenuTrack(m, TPM_RIGHTALIGN | TPM_TOPALIGN, pt.x, pt.y, g_hWnd);
    DestroyMenu(m);
    if (cmd >= (int)kClosedBase && cmd < (int)kClosedBase + closedShown)
        ReopenClosedTab((int)g_closedTabs.size() - 1 - (cmd - (int)kClosedBase));
    else if (cmd) RunCommand(cmd);
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

    // Esc leaves element fullscreen (video players). WebKit leaves this to the app: on Windows
    // nothing else does it, and the page never sees the key as a way out.
    if (!ctrl && !alt && !shift && k == VK_ESCAPE && g_fullScreenView) { ExitFullScreenForTabSwitch(); return true; }
    if (!ctrl && !alt && !shift && k == VK_ESCAPE && CustomizeActive()) { CustomizeEnd(); return true; }

    if (!ctrl && !alt && !shift && k == VK_F9) { RunCommand(CMD_READER); return true; }
    if (ctrl && shift && !alt) {
        if (k == 'T') { ReopenClosedTab(); return true; }
        if (k == 'I') { ShowWebInspector(); return true; }
        if (k == 'O') { RunCommand(CMD_BOOKMARKS); return true; }
        if (k == 'N') { RunCommand(CMD_NEWPRIVATE); return true; }
        if (k == 'A') { RunCommand(CMD_TABSEARCH); return true; }
        if (k == 'S') { RunCommand(CMD_SCREENSHOT); return true; }
        if (k == 'B') { RunCommand(CMD_BOOKMARKSBAR); return true; }
        if (k == 'E') { RunCommand(CMD_SIDEBAR); return true; }
        if (k == VK_DELETE) { OpenClearDataSettings(); return true; }
    }
    if (ctrl && alt && !shift && k >= '1' && k <= '9') { WorkspaceSwitchByIndex((int)(k - '1')); return true; }
    if (ctrl && !alt) {
        switch (k) {
        case 'T': RunCommand(CMD_NEWTAB); return true;
        case 'W': RunCommand(CMD_CLOSETAB); return true;
        case 'L': ShowUrlEdit(); return true;
        case 'H': RunCommand(CMD_HISTORY); return true;
        case 'D': RunCommand(CMD_BOOKMARK_PAGE); return true;
        case 'J': RunCommand(CMD_DOWNLOADS); return true;
        case 'F': RunCommand(CMD_FIND); return true;
        case 'K': CommandPaletteShow(); return true;
        case 'G': FindBarNext(shift); return true;
        case 'P': RunCommand(CMD_PRINT); return true;
        case VK_OEM_COMMA: RunCommand(CMD_SETTINGS); return true;
        case 'R': ReloadActive(false); return true;
        case VK_OEM_PLUS: case VK_ADD:         RunCommand(CMD_ZOOMIN); return true;
        case VK_OEM_MINUS: case VK_SUBTRACT:   RunCommand(CMD_ZOOMOUT); return true;
        case '0': case VK_NUMPAD0:             RunCommand(CMD_ZOOMRESET); return true;
        case VK_TAB: {
            int n = WsTabEnd();
            if (n > 1) SelectTab(((g_active + (shift ? -1 : 1)) % n + n) % n);
            return true;
        }
        }
        if (k >= '1' && k <= '9') {
            int n = WsTabEnd();
            const int want = k == '9' ? n - 1 : (int)(k - '1');
            if (want < n) SelectTab(want);
            return true;
        }
    }
    if (alt && !ctrl) {
        if (k == VK_LEFT)  { GoBack(); return true; }
        if (k == VK_RIGHT) { GoForward(); return true; }
    }
    if (k == VK_F5) { ReloadActive(false); return true; }
    if (k == VK_F3 && !ctrl && !alt) { FindBarNext(shift); return true; }
    if (k == VK_F12) { ShowWebInspector(); return true; }
    return false;
}
