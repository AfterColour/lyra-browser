// Lyra — контекст WebKit, профиль (cookies, хранилище) и применение настроек к движку.
#include "browser/WebContext.h"
#include "app/Log.h"
#include "core/Settings.h"
#include "core/StringUtils.h"

#include <algorithm>
#include <vector>

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

    // Canvas 2D is drawn by the CPU. A GPU canvas (Skia on ANGLE in the GPU process) never reached
    // the GPU-painted page tiles: canvases stayed empty on screen - PDF pages (PDF.js draws them on
    // canvases), charts, games - while a snapshot of the page showed them. MYBROWSER_GPU_CANVAS=1
    // turns the GPU canvas back on (for working on that); it also needs the GPU-painted tiles.
    wchar_t noGpuRaster[4] = {}, gpuCanvas[4] = {};
    const bool cpuRaster = GetEnvironmentVariableW(L"MYBROWSER_DISABLE_GPU_RASTER", noGpuRaster, 4) && noGpuRaster[0] == L'1';
    const bool wantGpuCanvas = GetEnvironmentVariableW(L"MYBROWSER_GPU_CANVAS", gpuCanvas, 4) && gpuCanvas[0] == L'1';
    WKPreferencesSetCanvasUsesAcceleratedDrawing(g_preferences, wantGpuCanvas && !cpuRaster);

    // Animated mouse-wheel scrolling, as in other browsers (WebKit's Windows port has it off and
    // there is no C API for it; the preference key is the generated one).
    WKStringRef scrollAnimatorKey = WKStringCreateWithUTF8CString("ScrollAnimatorEnabled");
    WKPreferencesSetBoolValueForKeyForTesting(g_preferences, true, scrollAnimatorKey);
    WKRelease(scrollAnimatorKey);
    ApplyFrameRate();
}

bool OnBatteryPower() {
    SYSTEM_POWER_STATUS ps = {};
    return GetSystemPowerStatus(&ps) && ps.ACLineStatus == 0;
}

// Pages render at the display's refresh rate (120/144/240 Hz), or at the nearest 60 fps divisor to
// spare the GPU and the battery (the engine reads the preference in DrawingAreaWC).
// Pages with preferences of their own (the settings window, extension pages, the sidebar's page)
// follow the same frame rate: the preference is ON by default in WebKit, so a fresh set of
// preferences left those pages at 60 fps on a 240 Hz display.
static std::vector<WKPreferencesRef>& FollowingPrefs() {
    static std::vector<WKPreferencesRef> list;
    return list;
}

static bool Near60() { return g_prefs.frameRate == 1 || (g_prefs.frameRate == 2 && OnBatteryPower()); }

static void SetNear60(WKPreferencesRef p, bool near60) {
    WKStringRef near60Key = WKStringCreateWithUTF8CString("PreferPageRenderingUpdatesNear60FPSEnabled");
    WKPreferencesSetBoolValueForKeyForTesting(p, near60, near60Key);
    WKRelease(near60Key);
}

void FollowFrameRate(WKPreferencesRef prefs) {
    if (!prefs) return;
    auto& list = FollowingPrefs();
    if (std::find(list.begin(), list.end(), prefs) == list.end()) list.push_back((WKPreferencesRef)WKRetain(prefs));
    SetNear60(prefs, Near60());
}

void ApplyFrameRate() {
    if (!g_preferences) return;
    const bool near60 = Near60();
    static int applied = -1;
    if (applied == (int)near60) return;
    applied = near60;
    SetNear60(g_preferences, near60);
    for (WKPreferencesRef p : FollowingPrefs()) SetNear60(p, near60);
    Log("frame rate: %s", near60 ? "about 60 fps" : "the display's");
}
