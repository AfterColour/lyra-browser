// Lyra — контекст WebKit, профиль (cookies, хранилище) и применение настроек к движку.
#pragma once

#include "common/Platform.h"

extern WKContextRef g_context;
extern WKPreferencesRef g_preferences;
extern WKWebsiteDataStoreRef g_dataStore;

WKWebsiteDataStoreRef CreateWebsiteDataStore();
void ApplyWebPrefs();
