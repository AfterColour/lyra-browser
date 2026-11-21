// Lyra — контекст WebKit, профиль (cookies, хранилище) и применение настроек к движку.
#pragma once

#include "common/Platform.h"

extern WKContextRef g_context;
extern WKPreferencesRef g_preferences;
extern WKWebsiteDataStoreRef g_dataStore;

WKWebsiteDataStoreRef CreateWebsiteDataStore();
void ApplyWebPrefs();
// Settings > Performance > Frame rate, and the power source: pages draw at the display's rate or
// at about 60 fps (on battery with "Auto"). Called again when the power source changes.
void ApplyFrameRate();
// A page's own set of preferences (not the tabs' ones) follows that frame rate too.
void FollowFrameRate(WKPreferencesRef prefs);
bool OnBatteryPower();
