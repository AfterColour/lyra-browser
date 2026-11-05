// Lyra — содержимое разделов настроек и сброс к значениям по умолчанию.
#include "ui/settings/SettingsPages.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "browser/WebContext.h"
#include "core/Constants.h"
#include "core/Settings.h"
#include "core/StringUtils.h"
#include "ui/Chrome.h"
#include "ui/Commands.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/settings/SettingsModel.h"
#include "ui/settings/SettingsWindow.h"

static void ResetAllSettings();

// ---- pages -----------------------------------------------------------------
static void ResetAllSettings() {
    g_prefs = Prefs();
    g_homePage = L"https://example.com";
    g_searchEngine = L"https://duckduckgo.com/?q=";
    ApplyWebPrefs();
    SetZoom(1.0);                         // also saves
    ApplyTheme(ResolveDark());
    RefreshLayout();
    SaveSettings();
    if (g_setHwnd) InvalidateRect(g_setHwnd, nullptr, FALSE);
}

void BuildPages() {
    g_pages.clear();
    const std::function<void()> save    = []() { SaveSettings(); };
    const std::function<void()> web     = []() { ApplyWebPrefs(); SaveSettings(); };
    const std::function<void()> theme   = []() { ApplyTheme(ResolveDark()); SaveSettings(); };
    const std::function<void()> layout  = []() { RefreshLayout(); SaveSettings(); };
    const std::function<void()> repaint = []() { InvalidateChrome(); SaveSettings(); };

    {   // General
        SPage p; p.name = L"General"; p.icon = L"\uE713";
        p.rows.push_back(RHeader(L"Startup"));
        p.rows.push_back(RText(L"Home page", L"Opened when the browser starts.",
            []() { return g_homePage; },
            [](const std::wstring& s) { std::wstring t = TrimWs(s); if (!t.empty()) { g_homePage = t; SaveSettings(); } }));
        p.rows.push_back(RButton(L"Use current page", L"Make the page in the active tab your home page.", L"Set",
            []() { if (Tab* t = ActiveTab()) if (!t->url.empty() && t->url != L"about:blank") { g_homePage = t->url; SaveSettings(); } }));
        p.rows.push_back(RHeader(L"Page zoom"));
        p.rows.push_back(RSlider(L"Default zoom", L"Applied to every page. Shortcuts: Ctrl +, Ctrl -, Ctrl 0.", 25, 300, 5,
            []() { return (int)std::lround(g_zoom * 100.0); },
            [](int v) { SetZoom(v / 100.0); },
            [](int v) { return std::to_wstring(v) + L"%"; }));
        g_pages.push_back(std::move(p));
    }
    {   // Appearance
        SPage p; p.name = L"Appearance"; p.icon = L"\uE790";
        p.rows.push_back(RHeader(L"Theme"));
        p.rows.push_back(RChoice(L"Color scheme", L"Follow Windows or force a light or dark browser.",
            { L"Use system setting", L"Light", L"Dark" },
            []() { return g_prefs.themeMode; },
            [theme](int i) { g_prefs.themeMode = i; theme(); }));
        std::vector<std::wstring> accents;
        for (int i = 0; i < kAccentCount; ++i) accents.push_back(kAccents[i].name);
        p.rows.push_back(RChoice(L"Accent color", L"Used for focus rings, progress and toggles.", accents,
            []() { return g_prefs.accent; },
            [theme](int i) { g_prefs.accent = i; theme(); }));
        p.rows.push_back(RHeader(L"Toolbar"));
        p.rows.push_back(RToggle(L"Compact layout", L"Smaller toolbar and tab bar to leave more room for pages.",
            &g_prefs.compact, layout));
        g_pages.push_back(std::move(p));
    }
    {   // Tabs
        SPage p; p.name = L"Tabs"; p.icon = L"\uE7C4";
        p.rows.push_back(RHeader(L"New tabs"));
        p.rows.push_back(RChoice(L"New tab opens", L"What you see when you press Ctrl+T or the + button.",
            { L"Home page", L"Blank page" },
            []() { return g_prefs.newTabMode; },
            [save](int i) { g_prefs.newTabMode = i; save(); }));
        p.rows.push_back(RHeader(L"Tab bar"));
        p.rows.push_back(RToggle(L"Show close button on every tab", L"Otherwise it appears on the active and hovered tab.",
            &g_prefs.closeAlways, repaint));
        p.rows.push_back(RInfo(L"Maximum open tabs", std::to_wstring(metrics::kMaxTabs)));
        p.rows.push_back(RInfo(L"Closing the last tab", L"Closes the window"));
        g_pages.push_back(std::move(p));
    }
    {   // Search
        SPage p; p.name = L"Search"; p.icon = L"\uE721";
        p.rows.push_back(RHeader(L"Address bar"));
        p.rows.push_back(RToggle(L"Show full address", L"Show the complete URL instead of just the domain.",
            &g_prefs.showFullUrl, repaint));
        p.rows.push_back(RHeader(L"Search engine"));
        std::vector<std::wstring> names;
        for (int i = 0; i < kEngineCount; ++i) names.push_back(kEngines[i].name);
        names.push_back(L"Custom");
        p.rows.push_back(RChoice(L"Default search engine", L"Used for anything typed in the address bar that is not a website.",
            names,
            []() { for (int i = 0; i < kEngineCount; ++i) if (g_searchEngine == kEngines[i].url) return i; return kEngineCount; },
            [save](int i) { if (i < kEngineCount) { g_searchEngine = kEngines[i].url; save(); } }));
        p.rows.push_back(RText(L"Search URL", L"The search query is appended to this address.",
            []() { return g_searchEngine; },
            [](const std::wstring& s) { std::wstring t = TrimWs(s); if (!t.empty()) { g_searchEngine = t; SaveSettings(); } }));
        g_pages.push_back(std::move(p));
    }
    {   // Webpages
        SPage p; p.name = L"Webpages"; p.icon = L"\uE774";
        p.rows.push_back(RHeader(L"Content"));
        p.rows.push_back(RToggle(L"Enable JavaScript", L"Turning this off breaks most modern websites.", &g_prefs.js, web));
        p.rows.push_back(RToggle(L"Load images automatically", L"Disable to save data on slow connections.", &g_prefs.images, web));
        p.rows.push_back(RToggle(L"Allow full screen", L"Let pages such as video players use the whole screen.", &g_prefs.fullscreen, web));
        p.rows.push_back(RToggle(L"Enable Web Audio", L"Has no effect yet: Web Audio is disabled in the current WebKit build.", &g_prefs.webAudio, web));
        p.rows.push_back(RHeader(L"Fonts"));
        p.rows.push_back(RSlider(L"Default font size", L"Base size for pages that do not set their own.", 9, 32, 1,
            []() { return g_prefs.defaultFont; },
            [web](int v) { g_prefs.defaultFont = v; web(); },
            [](int v) { return std::to_wstring(v) + L" px"; }));
        p.rows.push_back(RSlider(L"Minimum font size", L"Text smaller than this is enlarged. 0 turns it off.", 0, 24, 1,
            []() { return g_prefs.minFont; },
            [web](int v) { g_prefs.minFont = v; web(); },
            [](int v) { return v == 0 ? std::wstring(L"Off") : std::to_wstring(v) + L" px"; }));
        p.rows.push_back(RHeader(L"Developer"));
        p.rows.push_back(RToggle(L"Developer tools", L"Adds Inspect Element to the page context menu.", &g_prefs.devExtras, web));
        g_pages.push_back(std::move(p));
    }
    {   // Privacy
        SPage p; p.name = L"Privacy"; p.icon = L"\uE72E";
        p.rows.push_back(RHeader(L"Website data"));
        p.rows.push_back(RToggle(L"Allow local storage", L"Lets sites remember settings and sign-ins in this browser.", &g_prefs.localStorage, web));
        p.rows.push_back(RToggle(L"Allow web databases", L"Used by some offline-capable web apps.", &g_prefs.databases, web));
        p.rows.push_back(RInfo(L"Note", L"Changes apply to pages loaded afterwards"));
        g_pages.push_back(std::move(p));
    }
    {   // Keyboard
        SPage p; p.name = L"Keyboard"; p.icon = L"\uE765";
        p.rows.push_back(RHeader(L"Tabs"));
        p.rows.push_back(RInfo(L"New tab", L"Ctrl+T", true));
        p.rows.push_back(RInfo(L"Close tab", L"Ctrl+W", true));
        p.rows.push_back(RInfo(L"Next tab", L"Ctrl+Tab", true));
        p.rows.push_back(RInfo(L"Previous tab", L"Ctrl+Shift+Tab", true));
        p.rows.push_back(RInfo(L"Go to tab 1-8", L"Ctrl+1 ... 8", true));
        p.rows.push_back(RInfo(L"Go to last tab", L"Ctrl+9", true));
        p.rows.push_back(RInfo(L"Reopen closed tab", L"Ctrl+Shift+T", true));
        p.rows.push_back(RHeader(L"Navigation"));
        p.rows.push_back(RInfo(L"Focus address bar", L"Ctrl+L", true));
        p.rows.push_back(RInfo(L"Reload page", L"Ctrl+R  /  F5", true));
        p.rows.push_back(RInfo(L"Back", L"Alt+Left", true));
        p.rows.push_back(RInfo(L"Forward", L"Alt+Right", true));
        p.rows.push_back(RHeader(L"View"));
        p.rows.push_back(RInfo(L"Zoom in", L"Ctrl++", true));
        p.rows.push_back(RInfo(L"Zoom out", L"Ctrl+-", true));
        p.rows.push_back(RInfo(L"Actual size", L"Ctrl+0", true));
        p.rows.push_back(RHeader(L"Browser"));
        p.rows.push_back(RInfo(L"Settings", L"Ctrl+,", true));
        p.rows.push_back(RInfo(L"Developer tools", L"F12  /  Ctrl+Shift+I", true));
        g_pages.push_back(std::move(p));
    }
    {   // About
        SPage p; p.name = L"About"; p.icon = L"\uE946";
        p.rows.push_back(RHeader(kAppName));
        p.rows.push_back(RInfo(L"Engine", L"WebKit"));
        p.rows.push_back(RInfo(L"Version", L"1.0"));
        p.rows.push_back(RInfo(L"Settings file", g_settingsFile));
        p.rows.push_back(RButton(L"Settings folder", L"", L"Open",
            []() { ShellExecuteW(nullptr, L"open", g_settingsDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }));
        p.rows.push_back(RHeader(L"Reset"));
        p.rows.push_back(RButton(L"Reset all settings", L"Restore every option to its default value.", L"Reset...",
            []() {
                if (MessageBoxW(g_setHwnd, L"Reset all settings to their defaults?", L"Settings",
                                MB_YESNO | MB_ICONQUESTION) == IDYES)
                    ResetAllSettings();
            }));
        g_pages.push_back(std::move(p));
    }
}
