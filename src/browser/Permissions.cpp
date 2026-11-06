// Lyra — разрешения сайтов: уведомления и местоположение (запрос, запоминание, показ).
#include "browser/Permissions.h"
#include "app/Log.h"
#include "browser/GeoLocation.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "browser/WebContext.h"
#include "core/Constants.h"
#include "core/Localization.h"
#include "core/Settings.h"
#include "core/StringUtils.h"
#include "ui/Dialogs.h"
#include "ui/MainWindow.h"

#include <WebKit/WKContext.h>
#include <WebKit/WKGeolocationManager.h>
#include <WebKit/WKGeolocationPermissionRequest.h>
#include <WebKit/WKGeolocationPosition.h>
#include <WebKit/WKMutableArray.h>
#include <WebKit/WKMutableDictionary.h>
#include <WebKit/WKNotification.h>
#include <WebKit/WKNotificationManager.h>
#include <WebKit/WKNotificationPermissionRequest.h>
#include <WebKit/WKNotificationProvider.h>
#include <WebKit/WKNumber.h>

#include <atomic>
#include <fstream>
#include <thread>

namespace {

// ---------------------------------------------------------------------------------------------
//  Remembered answers: "<kind> <origin> allow|deny" per line.
// ---------------------------------------------------------------------------------------------
std::map<std::string, bool> s_answers;          // "notifications https://site" -> allowed

std::wstring File() { return g_settingsDir + L"\\permissions.txt"; }

void Load() {
    s_answers.clear();
    std::ifstream f(File(), std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t a = line.find(' '), b = line.rfind(' ');
        if (a == std::string::npos || b == a) continue;
        s_answers[line.substr(0, b)] = line.substr(b + 1) == "allow";
    }
}

void Save() {
    std::string text;
    for (auto& [key, allowed] : s_answers) text += key + (allowed ? " allow\n" : " deny\n");
    std::ofstream f(File(), std::ios::binary | std::ios::trunc);
    f << text;
}

std::string OriginKey(WKSecurityOriginRef origin) {
    std::string s;
    if (origin) if (WKStringRef o = WKSecurityOriginCopyToString(origin)) { s = ToUtf8(WKStr(o)); WKRelease(o); }
    return s;
}

// -1 not asked yet, 0 denied, 1 allowed
int Lookup(const char* kind, const std::string& origin) {
    auto it = s_answers.find(std::string(kind) + " " + origin);
    return it == s_answers.end() ? -1 : it->second ? 1 : 0;
}

bool Ask(const std::string& origin, const wchar_t* question) {
    std::wstring site = ToWide(origin);
    std::wstring text = site + L"\n\n" + question + L"\n\n" +
        Tr(L"Lyra remembers the answer for this site (Settings > Privacy resets it).",
           L"Lyra запомнит ответ для этого сайта (сбросить — Настройки > Конфиденциальность).");
    ModalScope modal;
    return MessageBoxW(g_hWnd, text.c_str(), kAppName, MB_YESNO | MB_ICONQUESTION) == IDYES;
}

bool Decide(const char* kind, WKSecurityOriginRef originRef, const wchar_t* question) {
    const std::string origin = OriginKey(originRef);
    if (origin.empty() || origin == "null") return false;
    int known = Lookup(kind, origin);
    if (known >= 0) return known == 1;
    const bool allowed = Ask(origin, question);
    s_answers[std::string(kind) + " " + origin] = allowed;
    Save();
    Log("permission %s %s: %s", kind, origin.c_str(), allowed ? "allow" : "deny");
    return allowed;
}

// ---------------------------------------------------------------------------------------------
//  Notifications: a tray icon shows each one as a Windows notification (balloon -> toast).
// ---------------------------------------------------------------------------------------------
constexpr UINT kTrayId = 1;
bool s_trayAdded = false;
struct Shown { uint64_t id = 0; WKNotificationManagerRef manager = nullptr; WKPageRef page = nullptr; };
Shown s_last;                                    // the notification on screen now (one at a time)
std::vector<WKNotificationManagerRef> s_managers;

NOTIFYICONDATAW TrayData() {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hWnd;
    nid.uID = kTrayId;
    return nid;
}

void TrayRemove() {
    if (!s_trayAdded) return;
    NOTIFYICONDATAW nid = TrayData();
    Shell_NotifyIconW(NIM_DELETE, &nid);
    s_trayAdded = false;
}

void OnShow(WKPageRef page, WKNotificationRef n, const void* ci) {
    auto manager = (WKNotificationManagerRef)ci;
    std::wstring title, body;
    if (WKStringRef s = WKNotificationCopyTitle(n)) { title = WKStr(s); WKRelease(s); }
    if (WKStringRef s = WKNotificationCopyBody(n))  { body = WKStr(s);  WKRelease(s); }
    std::wstring site = ToWide(OriginKey(WKNotificationGetSecurityOrigin(n)));
    if (body.empty()) body = site;
    else if (!site.empty()) body += L"\n" + site;

    NOTIFYICONDATAW nid = TrayData();
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_INFO;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = (HICON)SendMessageW(g_hWnd, WM_GETICON, ICON_SMALL2, 0);
    if (!nid.hIcon) nid.hIcon = (HICON)GetClassLongPtrW(g_hWnd, GCLP_HICONSM);
    if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcsncpy_s(nid.szTip, kAppName, _TRUNCATE);
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, body.empty() ? L" " : body.c_str(), _TRUNCATE);
    nid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
    nid.hBalloonIcon = nid.hIcon;
    BOOL ok = Shell_NotifyIconW(s_trayAdded ? NIM_MODIFY : NIM_ADD, &nid);
    if (!s_trayAdded && ok) s_trayAdded = true;
    if (!ok) { Log("notification: Shell_NotifyIcon failed"); return; }

    s_last = { WKNotificationGetID(n), manager, page };
    WKNotificationManagerProviderDidShowNotification(manager, s_last.id);
}

void OnCancel(WKNotificationRef n, const void*) {
    if (WKNotificationGetID(n) == s_last.id) { TrayRemove(); s_last = {}; }
}

void OnDestroyed(WKNotificationRef n, const void*) {
    if (WKNotificationGetID(n) == s_last.id) s_last.page = nullptr;
}

void OnAddManager(WKNotificationManagerRef m, const void*) { s_managers.push_back(m); }
void OnRemoveManager(WKNotificationManagerRef m, const void*) {
    s_managers.erase(std::remove(s_managers.begin(), s_managers.end(), m), s_managers.end());
}

WKDictionaryRef OnPermissions(const void*) {
    WKMutableDictionaryRef d = WKMutableDictionaryCreate();
    for (auto& [key, allowed] : s_answers) {
        if (key.rfind("notifications ", 0) != 0) continue;
        WKStringRef origin = WKStringCreateWithUTF8CString(key.c_str() + 14);
        WKBooleanRef value = WKBooleanCreate(allowed);
        WKDictionarySetItem(d, origin, value);
        WKRelease(value);
        WKRelease(origin);
    }
    return (WKDictionaryRef)d;
}

void SetNotificationProvider(WKNotificationManagerRef manager) {
    if (!manager) return;
    WKNotificationProviderV0 p = {};
    p.base.version = 0;
    p.base.clientInfo = manager;
    p.show = OnShow;
    p.cancel = OnCancel;
    p.didDestroyNotification = OnDestroyed;
    p.addNotificationManager = OnAddManager;
    p.removeNotificationManager = OnRemoveManager;
    p.notificationPermissions = OnPermissions;
    WKNotificationManagerSetProvider(manager, &p.base);
}

void BringTabForward(WKPageRef page) {
    if (IsIconic(g_hWnd)) ShowWindow(g_hWnd, SW_RESTORE);
    SetForegroundWindow(g_hWnd);
    if (!page) return;
    for (int i = 0; i < (int)g_tabs.size(); ++i)
        if (g_tabs[i]->page == page) { if (i != g_active) SelectTab(i); break; }
}

// ---------------------------------------------------------------------------------------------
//  Geolocation: a worker asks Windows while a page watches the position (every 15 s).
// ---------------------------------------------------------------------------------------------
WKGeolocationManagerRef s_geoManager = nullptr;
std::atomic<unsigned> s_geoGeneration{ 0 };
std::atomic<bool> s_geoHigh{ false };
bool s_geoUpdating = false;

struct GeoMessage { unsigned generation; GeoFix fix; };

void GeoWorker(unsigned generation) {
    while (s_geoGeneration.load() == generation) {
        GeoFix fix = GeoLocateNow(s_geoHigh.load());
        if (s_geoGeneration.load() != generation) break;
        if (!PostMessageW(g_hWnd, WM_APP_GEO, 0, (LPARAM)new GeoMessage{ generation, fix })) break;
        for (int i = 0; i < 150 && s_geoGeneration.load() == generation; ++i) Sleep(100);
    }
}

void OnGeoStart(WKGeolocationManagerRef, const void*) {
    s_geoUpdating = true;
    const unsigned generation = ++s_geoGeneration;
    std::thread(GeoWorker, generation).detach();
}

void OnGeoStop(WKGeolocationManagerRef, const void*) {
    s_geoUpdating = false;
    ++s_geoGeneration;
}

void OnGeoHighAccuracy(WKGeolocationManagerRef, bool enabled, const void*) { s_geoHigh = enabled; }

} // namespace

void PermissionsInit() {
    Load();
    SetNotificationProvider(WKContextGetNotificationManager(g_context));
    SetNotificationProvider(WKNotificationManagerGetSharedServiceWorkerNotificationManager());

    s_geoManager = WKContextGetGeolocationManager(g_context);
    if (s_geoManager) {
        WKGeolocationProviderV1 g = {};
        g.base.version = 1;
        g.startUpdating = OnGeoStart;
        g.stopUpdating = OnGeoStop;
        g.setEnableHighAccuracy = OnGeoHighAccuracy;
        WKGeolocationManagerSetProvider(s_geoManager, &g.base);
    }
}

void PermissionsShutdown() {
    ++s_geoGeneration;
    TrayRemove();
}

void PermissionsDecideNotifications(WKPageRef, WKSecurityOriginRef origin, WKNotificationPermissionRequestRef request) {
    const bool allowed = Decide("notifications", origin,
        Tr(L"This site wants to show notifications. Allow?", L"Сайт хочет показывать уведомления. Разрешить?"));
    if (allowed) WKNotificationPermissionRequestAllow(request);
    else WKNotificationPermissionRequestDeny(request);
}

void PermissionsDecideGeolocation(WKPageRef, WKSecurityOriginRef origin, WKGeolocationPermissionRequestRef request) {
    const bool allowed = Decide("geolocation", origin,
        Tr(L"This site wants to know your location. Allow?", L"Сайт хочет узнать ваше местоположение. Разрешить?"));
    if (allowed) WKGeolocationPermissionRequestAllow(request);
    else WKGeolocationPermissionRequestDeny(request);
}

void PermissionsTrayMessage(WPARAM, LPARAM lParam) {
    switch (LOWORD(lParam)) {
    case NIN_BALLOONUSERCLICK:
    case WM_LBUTTONUP: {
        Shown shown = s_last;
        BringTabForward(shown.page);
        if (shown.manager && shown.id) WKNotificationManagerProviderDidClickNotification(shown.manager, shown.id);
        TrayRemove();
        s_last = {};
        break;
    }
    case NIN_BALLOONTIMEOUT:
        TrayRemove();
        break;
    }
}

void PermissionsGeolocationResult(LPARAM lParam) {
    std::unique_ptr<GeoMessage> m((GeoMessage*)lParam);
    if (!s_geoManager || !s_geoUpdating || m->generation != s_geoGeneration.load()) return;
    if (m->fix.ok) {
        WKGeolocationPositionRef p = WKGeolocationPositionCreate(m->fix.timestamp, m->fix.latitude,
                                                                 m->fix.longitude, m->fix.accuracy);
        WKGeolocationManagerProviderDidChangePosition(s_geoManager, p);
        WKRelease(p);
    } else {
        WKStringRef msg = WKStringCreateWithUTF8CString(
            "Location is unavailable (Windows Settings > Privacy > Location)");
        WKGeolocationManagerProviderDidFailToDeterminePositionWithErrorMessage(s_geoManager, msg);
        WKRelease(msg);
    }
}

void PermissionsReset() {
    std::vector<std::string> notified;
    for (auto& [key, allowed] : s_answers)
        if (key.rfind("notifications ", 0) == 0) notified.push_back(key.substr(14));
    s_answers.clear();
    Save();
    if (notified.empty()) return;
    WKMutableArrayRef origins = WKMutableArrayCreate();
    for (auto& o : notified) {
        WKStringRef s = WKStringCreateWithUTF8CString(o.c_str());
        WKSecurityOriginRef origin = WKSecurityOriginCreateFromString(s);
        WKArrayAppendItem(origins, origin);
        WKRelease(origin);
        WKRelease(s);
    }
    WKNotificationManagerRef managers[] = { WKContextGetNotificationManager(g_context),
                                            WKNotificationManagerGetSharedServiceWorkerNotificationManager() };
    for (auto m : managers) if (m) WKNotificationManagerProviderDidRemoveNotificationPolicies(m, (WKArrayRef)origins);
    WKRelease(origins);
}

size_t PermissionsCount() { return s_answers.size(); }
