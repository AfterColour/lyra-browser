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
Tab* CreateTabWithConfiguration(WKPageConfigurationRef conf, bool openedByPage);
void CreateTab(const std::wstring& input);
void ReopenClosedTab();
void CloseTab(int i);
void CloseAllTabs();
void HandleTabCrashed(unsigned long long id, int reason);

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
