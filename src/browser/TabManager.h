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
