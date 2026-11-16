// Lyra: pages go through the proxy Windows is set to (Settings > Network > Proxy), as in other browsers.
#pragma once

#include "common/Platform.h"

void SystemProxyInit();                              // after g_dataStore exists: apply, then follow changes
void SystemProxyShutdown();
void SystemProxyApply(WKWebsiteDataStoreRef store);  // a data store made later (private tabs)
void SystemProxyForget(WKWebsiteDataStoreRef store); // before it is released
void SystemProxyChanged(LPARAM pick);                // WM_APP_PROXY on the UI thread
