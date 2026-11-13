// Lyra — список вкладок: создание, закрытие, переключение, навигация, восстановление.
#pragma once

#include "common/Platform.h"
#include "browser/Tab.h"

extern std::vector<std::wstring> g_closedUrls;

extern std::vector<std::unique_ptr<Tab>> g_tabs;
extern int g_active;

extern std::vector<std::unique_ptr<TabCtx>> g_ctxs;
extern bool g_closing;
extern bool g_tabOp;
extern bool g_closeRequested;
struct TabOpGuard {
    TabOpGuard()  { g_tabOp = true; }
    ~TabOpGuard() { g_tabOp = false; }
};

Tab* ActiveTab();
bool SyncTab(Tab& t);
void SelectTab(int i);
Tab* ResolveTab(const void* clientInfo, const char* what);
Tab* FindTab(unsigned long long id, int* index = nullptr);
void NavigateActive(const std::wstring& input);
// `select` = false: the tab is added hidden, the active tab stays (session restore).
Tab* CreateTabWithConfiguration(WKPageConfigurationRef conf, bool openedByPage, bool select = true);
WKPageConfigurationRef CreatePageConfigurationForTab();   // the settings every tab gets (caller releases)
void CreateTab(const std::wstring& input);
void CreatePrivateTab();             // Ctrl+Shift+N: a private (incognito) tab
// The page's context menu: `input` (an address or a search) in a new tab, in the background (the
// open tab stays) or in front, and private or not.
Tab* OpenInNewTab(const std::wstring& input, bool background, bool privateTab);
void ReopenClosedTab();
// Moves the tab at `from` so that it ends up at index `to` (the others keep their order).
void MoveTab(int from, int to);
// Pinned tabs stay at the start of the list, narrow (only the site icon), and come back after a
// restart. Pinning moves the tab after the pinned ones, unpinning right after them.
void SetTabPinned(int index, bool pinned);
int PinnedTabCount();
void CloseTab(int i);
void CloseAllTabs();
void HandleTabCrashed(unsigned long long id, int reason);
// Unloads a background tab (browser/TabSleep.cpp): its page and web process go away, a new empty
// page takes their place and loads `state` / its address when the tab is shown again, like a
// restored tab. False if it cannot be done now.
bool UnloadTab(Tab& t, const std::string& state);

// Tabs used recently stay rendered off-screen (Settings > Tabs > "Ready tabs"), so switching
// back to them is instant: only the active view is in place, the "ready" ones are parked
// outside the window (WebKit still sees them as visible), the rest are hidden.
// LayoutWebViews calls this after placing the active view; w x h is the page area.
void ParkBackgroundViews(int w, int h);
// A parked tab is kept ready for 20 s, then hidden like the rest (main timer, once a second).
void ExpireReadyTabs();

// A start page loaded in the background for the next new tab (Settings > Tabs), so Ctrl+T
// shows it at once. PrepareSpareTab() is run by TIMER_SPARE_TAB; DiscardSpareTab() drops it.
void PrepareSpareTab();
void DiscardSpareTab();
void ScheduleSpareTab(UINT delayMs = 1500);
