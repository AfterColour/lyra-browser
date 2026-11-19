// Lyra — контекст WebKit, профиль (cookies, хранилище) и применение настроек к движку.
#include "browser/WebContext.h"
#include "app/Log.h"
#include "core/Settings.h"
#include "core/StringUtils.h"

WKContextRef          g_context = nullptr;

WKPreferencesRef      g_preferences = nullptr;

WKWebsiteDataStoreRef g_dataStore = nullptr;   // cookies, storage, cache under %LOCALAPPDATA%\Lyra

// Without an explicit data store WebKit keeps cookies, localStorage, IndexedDB and the
// HTTP cache in %LOCALAPPDATA%\Apple Computer\WebKit - shared with MiniBrowser and any
// other WebKit app. Give the browser its own profile folder instead.
WKWebsiteDataStoreRef CreateWebsiteDataStore() {
    const std::wstring root = g_settingsDir + L"\\WebsiteData";
    CreateDirectoryW(g_settingsDir.c_str(), nullptr);
    CreateDirectoryW(root.c_str(), nullptr);

    WKWebsiteDataStoreConfigurationRef conf = WKWebsiteDataStoreConfigurationCreate();
    auto set = [&](void (*setter)(WKWebsiteDataStoreConfigurationRef, WKStringRef), const wchar_t* sub) {
        std::wstring path = root + L"\\" + sub;
        WKStringRef s = WKStringCreateWithUTF8CString(ToUtf8(path).c_str());
        setter(conf, s);
        WKRelease(s);
    };
    set(WKWebsiteDataStoreConfigurationSetCookieStorageFile,                L"Cookies.db");
    set(WKWebsiteDataStoreConfigurationSetNetworkCacheDirectory,            L"Cache");
    set(WKWebsiteDataStoreConfigurationSetLocalStorageDirectory,            L"LocalStorage");
    set(WKWebsiteDataStoreConfigurationSetIndexedDBDatabaseDirectory,       L"IndexedDB");
    set(WKWebsiteDataStoreConfigurationSetWebSQLDatabaseDirectory,          L"WebSQL");
    set(WKWebsiteDataStoreConfigurationSetCacheStorageDirectory,            L"CacheStorage");
    set(WKWebsiteDataStoreConfigurationSetGeneralStorageDirectory,          L"Storage");
    set(WKWebsiteDataStoreConfigurationSetServiceWorkerRegistrationDirectory, L"ServiceWorkers");
    set(WKWebsiteDataStoreConfigurationSetMediaKeysStorageDirectory,        L"MediaKeys");
    set(WKWebsiteDataStoreConfigurationSetResourceLoadStatisticsDirectory,  L"ResourceLoadStatistics");
    WKWebsiteDataStoreRef store = WKWebsiteDataStoreCreateWithConfiguration(conf);
    WKRelease(conf);
    Log("website data store: %ls (%p)", root.c_str(), (void*)store);
    return store;
}

// ============================================================================
//  Web preferences + Settings window
//  (Vivaldi-style: category sidebar, searchable, scrollable pages with
//   toggles / dropdowns / sliders / text fields; everything is persisted)
// ============================================================================
void ApplyWebPrefs() {
    if (!g_preferences) return;
    WKPreferencesSetJavaScriptEnabled(g_preferences, g_prefs.js);
    // Spelling: pasted text and whole fields are checked in the background (WebCore asks for the
    // whole field after a paste only with asynchronous checking; typing checks the word either way).
    WKPreferencesSetAsynchronousSpellCheckingEnabled(g_preferences, true);
    WKPreferencesSetLoadsImagesAutomatically(g_preferences, g_prefs.images);
    WKPreferencesSetLocalStorageEnabled(g_preferences, g_prefs.localStorage);
    WKPreferencesSetDatabasesEnabled(g_preferences, g_prefs.databases);
    WKPreferencesSetDeveloperExtrasEnabled(g_preferences, g_prefs.devExtras);
    WKPreferencesSetFullScreenEnabled(g_preferences, g_prefs.fullscreen);
    WKPreferencesSetWebAudioEnabled(g_preferences, g_prefs.webAudio);
    WKPreferencesSetDefaultFontSize(g_preferences, (uint32_t)g_prefs.defaultFont);
    WKPreferencesSetMinimumFontSize(g_preferences, (uint32_t)g_prefs.minFont);
    // Camera and microphone (navigator.mediaDevices), on secure sites only, as in other browsers.
    WKPreferencesSetMediaDevicesEnabled(g_preferences, true);
    WKPreferencesSetMediaCaptureRequiresSecureConnection(g_preferences, true);
    // Tests: WebKit's pretend camera and microphone instead of the real ones.
    wchar_t mock[4] = {};
    if (GetEnvironmentVariableW(L"LYRA_MOCK_CAPTURE", mock, 4) && mock[0] == L'1') WKPreferencesSetMockCaptureDevicesEnabled(g_preferences, true);

    // Canvas 2D follows the page tiles: both on the GPU (Skia on ANGLE in the GPU process), or both
    // on the CPU with LYRA_DISABLE_GPU_RASTER=1. A GPU canvas under CPU-painted tiles would be
    // read back from the GPU for every tile it covers, every frame.
    wchar_t noGpuRaster[4] = {};
    bool cpuRaster = GetEnvironmentVariableW(L"MYBROWSER_DISABLE_GPU_RASTER", noGpuRaster, 4) && noGpuRaster[0] == L'1';
    WKPreferencesSetCanvasUsesAcceleratedDrawing(g_preferences, !cpuRaster);

    // Animated mouse-wheel scrolling, as in other browsers (WebKit's Windows port has it off and
    // there is no C API for it; the preference key is the generated one).
    WKStringRef scrollAnimatorKey = WKStringCreateWithUTF8CString("ScrollAnimatorEnabled");
    WKPreferencesSetBoolValueForKeyForTesting(g_preferences, true, scrollAnimatorKey);
    WKRelease(scrollAnimatorKey);
    // Render at the display's refresh rate (120/144/240 Hz), not the nearest 60 fps divisor.
    WKStringRef near60Key = WKStringCreateWithUTF8CString("PreferPageRenderingUpdatesNear60FPSEnabled");
    WKPreferencesSetBoolValueForKeyForTesting(g_preferences, false, near60Key);
    WKRelease(near60Key);
}
