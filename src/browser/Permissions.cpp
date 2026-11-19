// Lyra — разрешения сайтов: уведомления и местоположение (запрос, запоминание, показ).
#include "browser/Permissions.h"
#include <functional>
#include "app/Log.h"
#include "browser/GeoLocation.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "browser/WebContext.h"
#include "core/Constants.h"
#include "core/Localization.h"
#include "core/Settings.h"
#include "core/StringUtils.h"
#include "ui/MainWindow.h"
#include "ui/PermissionPrompt.h"

#include <WebKit/WKContext.h>
#include <WebKit/WKGeolocationManager.h>
#include <WebKit/WKGeolocationPermissionRequest.h>
#include <WebKit/WKGeolocationPosition.h>
#include <WebKit/WKUserMediaPermissionRequest.h>
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
    if (origin) if (WKStringRef o = WKSecurityOriginCopyToString(origin)) { s = WKStrUtf8(o); WKRelease(o); }
    return s;
}

// -1 not asked yet, 0 denied, 1 allowed. "camera+microphone": both remembered on their own -
// allowed when both are, denied when either is.
int Lookup(const char* kind, const std::string& origin) {
    if (std::string(kind) == "camera+microphone") {
        const int c = Lookup("camera", origin), m = Lookup("microphone", origin);
        if (c == 0 || m == 0) return 0;
        return c == 1 && m == 1 ? 1 : -1;
    }
    auto it = s_answers.find(std::string(kind) + " " + origin);
    return it == s_answers.end() ? -1 : it->second ? 1 : 0;
}

void Remember(const std::string& kind, const std::string& origin, bool allowed) {
    if (kind == "camera+microphone") { Remember("camera", origin, allowed); Remember("microphone", origin, allowed); return; }
    s_answers[kind + " " + origin] = allowed;
}

// The first device of a kind WebKit offers (its list starts with the system's default one).
WKStringRef FirstDevice(WKArrayRef list) {
    if (!list || !WKArrayGetSize(list)) return nullptr;
    WKTypeRef v = WKArrayGetItemAtIndex(list, 0);
    return v && WKGetTypeID(v) == WKStringGetTypeID() ? (WKStringRef)v : nullptr;
}

// ---------------------------------------------------------------------------------------------
//  Questions waiting for an answer (one bubble per tab, see PermissionPrompt.cpp).
// ---------------------------------------------------------------------------------------------
struct Pending { PermissionAsk ask; WKTypeRef request = nullptr; };
std::vector<Pending> s_pending;
unsigned long long s_nextSeq = 1;

void Resolve(const Pending& p, bool allowed) {
    if (p.ask.kind == "geolocation") {
        if (allowed) WKGeolocationPermissionRequestAllow((WKGeolocationPermissionRequestRef)p.request);
        else WKGeolocationPermissionRequestDeny((WKGeolocationPermissionRequestRef)p.request);
    } else if (p.ask.kind == "camera" || p.ask.kind == "microphone" || p.ask.kind == "camera+microphone") {
        auto r = (WKUserMediaPermissionRequestRef)p.request;
        WKArrayRef audio = WKUserMediaPermissionRequestAudioDeviceUIDs(r), video = WKUserMediaPermissionRequestVideoDeviceUIDs(r);
        WKStringRef a = WKUserMediaPermissionRequestRequiresMicrophoneCapture(r) ? FirstDevice(audio) : nullptr;
        WKStringRef v = WKUserMediaPermissionRequestRequiresCameraCapture(r) ? FirstDevice(video) : nullptr;
        if (allowed && (a || v)) WKUserMediaPermissionRequestAllow(r, a, v);
        else WKUserMediaPermissionRequestDeny(r, allowed ? kWKNoCaptureDevices : kWKPermissionDenied);
        Log("permission %s %s: %s (audio %s, video %s)", p.ask.kind.c_str(), p.ask.origin.c_str(), allowed ? "allowed" : "denied",
            a ? WKStrUtf8(a).c_str() : "-", v ? WKStrUtf8(v).c_str() : "-");
        if (audio) WKRelease(audio);
        if (video) WKRelease(video);
    } else {
        if (allowed) WKNotificationPermissionRequestAllow((WKNotificationPermissionRequestRef)p.request);
        else WKNotificationPermissionRequestDeny((WKNotificationPermissionRequestRef)p.request);
    }
    WKRelease(p.request);
}

std::wstring SiteName(const std::string& origin) {
    std::string s = origin;
    const size_t scheme = s.find("://");
    if (scheme != std::string::npos) s = s.substr(scheme + 3);
    if (s.rfind("www.", 0) == 0) s = s.substr(4);
    return ToWide(s);
}

Tab* TabOfPage(WKPageRef page) {
    for (auto& t : g_tabs) if (t->page == page) return t.get();
    return nullptr;
}

// Answers at once when the site was answered before, otherwise queues the question.
void Decide(const char* kind, WKPageRef page, WKSecurityOriginRef originRef, WKTypeRef request) {
    Pending p;
    p.ask.kind = kind;
    p.ask.origin = OriginKey(originRef);
    p.request = WKRetain(request);
    Tab* t = TabOfPage(page);
    const int known = p.ask.origin.empty() || p.ask.origin == "null" || !t ? 0 : Lookup(kind, p.ask.origin);
    if (known >= 0) { Resolve(p, known == 1); return; }
    p.ask.tabId = t->id;
    p.ask.site = SiteName(p.ask.origin);
    p.ask.seq = s_nextSeq++;
    s_pending.push_back(std::move(p));
    PermissionPromptUpdate();
}

// Questions of closed tabs are declined.
void Prune() {
    for (size_t i = 0; i < s_pending.size();) {
        bool live = false;
        for (auto& t : g_tabs) if (t->id == s_pending[i].ask.tabId && t->state == TabState::Live) { live = true; break; }
        if (live) { ++i; continue; }
        Resolve(s_pending[i], false);
        s_pending.erase(s_pending.begin() + i);
    }
}

// ---------------------------------------------------------------------------------------------
//  Notifications: a tray icon shows each one as a Windows notification (balloon -> toast).
// ---------------------------------------------------------------------------------------------
constexpr UINT kTrayId = 1;
bool s_trayAdded = false;
struct Shown {
    uint64_t id = 0; WKNotificationManagerRef manager = nullptr; WKPageRef page = nullptr;   // a site's
    std::function<void()> click; std::function<void(bool)> closed;                        // Lyra's own (extensions)
};
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

// The one notification on screen goes: an extension's is told it closed.
void ReplaceShown() {
    if (s_last.closed) { auto closed = std::move(s_last.closed); s_last = {}; closed(false); }
    s_last = {};
}

bool ShowBalloon(const std::wstring& title, const std::wstring& body) {
    NOTIFYICONDATAW nid = TrayData();
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_INFO;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = (HICON)SendMessageW(g_hWnd, WM_GETICON, ICON_SMALL2, 0);
    if (!nid.hIcon) nid.hIcon = (HICON)GetClassLongPtrW(g_hWnd, GCLP_HICONSM);
    if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcsncpy_s(nid.szTip, kAppName, _TRUNCATE);
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, body.empty() ? L" " : body.c_str(), _TRUNCATE);
    // The large picture in the notification needs Lyra's big icon: with the small one Windows refused
    // the whole notification (ERROR_TIMEOUT) and nothing was shown.
    HICON big = (HICON)SendMessageW(g_hWnd, WM_GETICON, ICON_BIG, 0);
    if (!big) big = (HICON)GetClassLongPtrW(g_hWnd, GCLP_HICON);
    nid.dwInfoFlags = big ? (NIIF_USER | NIIF_LARGE_ICON) : NIIF_INFO;
    nid.hBalloonIcon = big;
    BOOL ok = Shell_NotifyIconW(s_trayAdded ? NIM_MODIFY : NIM_ADD, &nid);
    if (!ok) {                                     // then a plain one
        nid.dwInfoFlags = NIIF_INFO;
        nid.hBalloonIcon = nullptr;
        ok = Shell_NotifyIconW(s_trayAdded ? NIM_MODIFY : NIM_ADD, &nid);
    }
    if (!s_trayAdded && ok) s_trayAdded = true;
    if (!ok) Log("notification: Shell_NotifyIcon failed (error %lu)", GetLastError());
    return ok != FALSE;
}

void OnShow(WKPageRef page, WKNotificationRef n, const void* ci) {
    auto manager = (WKNotificationManagerRef)ci;
    std::wstring title, body;
    if (WKStringRef s = WKNotificationCopyTitle(n)) { title = WKStr(s); WKRelease(s); }
    if (WKStringRef s = WKNotificationCopyBody(n))  { body = WKStr(s);  WKRelease(s); }
    std::wstring site = ToWide(OriginKey(WKNotificationGetSecurityOrigin(n)));
    if (body.empty()) body = site;
    else if (!site.empty()) body += L"\n" + site;
    ReplaceShown();
    if (!ShowBalloon(title, body)) return;

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
    PermissionPromptClose();
    for (auto& p : s_pending) Resolve(p, false);
    s_pending.clear();
    ++s_geoGeneration;
    TrayRemove();
}

void PermissionsDecideNotifications(WKPageRef page, WKSecurityOriginRef origin, WKNotificationPermissionRequestRef request) {
    Decide("notifications", page, origin, request);
}

void PermissionsDecideGeolocation(WKPageRef page, WKSecurityOriginRef origin, WKGeolocationPermissionRequestRef request) {
    Decide("geolocation", page, origin, request);
}

void PermissionsDecideUserMedia(WKPageRef page, WKSecurityOriginRef origin, WKUserMediaPermissionRequestRef request) {
    // Sharing the screen needs a picker of screens and windows: not yet.
    if (WKUserMediaPermissionRequestRequiresDisplayCapture(request)) {
        Log("permission: screen sharing asked - not supported yet");
        WKUserMediaPermissionRequestDeny(request, kWKNoCaptureDevices);
        return;
    }
    const bool cam = WKUserMediaPermissionRequestRequiresCameraCapture(request);
    const bool mic = WKUserMediaPermissionRequestRequiresMicrophoneCapture(request);
    Decide(cam && mic ? "camera+microphone" : cam ? "camera" : "microphone", page, origin, request);
}

const PermissionAsk* PermissionsPendingForTab(unsigned long long tabId) {
    Prune();
    for (auto& p : s_pending) if (p.ask.tabId == tabId) return &p.ask;
    return nullptr;
}

void PermissionsAnswer(unsigned long long seq, int answer) {
    auto it = std::find_if(s_pending.begin(), s_pending.end(), [seq](const Pending& p) { return p.ask.seq == seq; });
    if (it == s_pending.end()) return;
    const PermissionAsk ask = it->ask;
    if (answer >= 0) {
        Remember(ask.kind, ask.origin, answer == 1);
        Save();
        if (ask.kind == "notifications") {
            WKStringRef s = WKStringCreateWithUTF8CString(ask.origin.c_str());
            WKSecurityOriginRef origin = WKSecurityOriginCreateFromString(s);
            if (WKNotificationManagerRef m = WKContextGetNotificationManager(g_context))
                WKNotificationManagerProviderDidUpdateNotificationPolicy(m, origin, answer == 1);
            WKRelease(origin);
            WKRelease(s);
        }
    }
    Log("permission %s %s: %s", ask.kind.c_str(), ask.origin.c_str(), answer == 1 ? "allow" : answer == 0 ? "block" : "not now");
    // The same question asked again by the page (or by its other frames) gets the same answer.
    for (size_t i = 0; i < s_pending.size();) {
        const PermissionAsk& a = s_pending[i].ask;
        if (a.tabId == ask.tabId && a.kind == ask.kind && a.origin == ask.origin) {
            Resolve(s_pending[i], answer == 1);
            s_pending.erase(s_pending.begin() + i);
        } else ++i;
    }
    PermissionPromptUpdate();
}

void NotificationShow(const std::wstring& title, const std::wstring& body, std::function<void()> click, std::function<void(bool)> closed) {
    ReplaceShown();
    if (!ShowBalloon(title, body)) { if (closed) closed(false); return; }
    s_last.click = std::move(click);
    s_last.closed = std::move(closed);
}

void NotificationHide() {
    if (!s_last.click) return;          // (only Lyra's own)
    TrayRemove();
    s_last = {};
}

void PermissionsTrayMessage(WPARAM, LPARAM lParam) {
    switch (LOWORD(lParam)) {
    case NIN_BALLOONUSERCLICK:
    case WM_LBUTTONUP: {
        Shown shown = std::move(s_last);
        s_last = {};
        if (shown.click) {
            TrayRemove();
            shown.click();
            if (shown.closed) shown.closed(true);
            break;
        }
        BringTabForward(shown.page);
        if (shown.manager && shown.id) WKNotificationManagerProviderDidClickNotification(shown.manager, shown.id);
        TrayRemove();
        s_last = {};
        break;
    }
    case NIN_BALLOONTIMEOUT:
        TrayRemove();
        if (s_last.closed) { auto closed = std::move(s_last.closed); s_last = {}; closed(false); }
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

std::vector<SitePermissions> PermissionsList() {
    std::vector<SitePermissions> out;
    std::map<std::string, size_t> at;                    // origin -> index in out
    for (auto& [key, allowed] : s_answers) {
        const size_t space = key.find(' ');
        if (space == std::string::npos) continue;
        const std::string kind = key.substr(0, space), origin = key.substr(space + 1);
        auto it = at.find(origin);
        if (it == at.end()) {
            SitePermissions s;
            s.origin = origin;
            std::string host = origin;
            const size_t scheme = host.find("://");
            if (scheme != std::string::npos) host = host.substr(scheme + 3);
            s.site = ToWide(host);
            it = at.emplace(origin, out.size()).first;
            out.push_back(std::move(s));
        }
        out[it->second].answers.push_back({ kind, allowed });
    }
    std::sort(out.begin(), out.end(), [](const SitePermissions& a, const SitePermissions& b) { return a.site < b.site; });
    return out;
}

void PermissionsForgetSite(const std::string& origin) {
    bool notifications = false;
    for (auto it = s_answers.begin(); it != s_answers.end();) {
        const size_t space = it->first.find(' ');
        if (space != std::string::npos && it->first.substr(space + 1) == origin) {
            if (it->first.compare(0, space, "notifications") == 0) notifications = true;
            it = s_answers.erase(it);
        } else ++it;
    }
    Save();
    Log("permissions: %s forgotten", origin.c_str());
    if (!notifications) return;
    // WebKit keeps its own copy of the notification policy: drop that site's too.
    WKMutableArrayRef origins = WKMutableArrayCreate();
    WKStringRef s = WKStringCreateWithUTF8CString(origin.c_str());
    WKSecurityOriginRef o = WKSecurityOriginCreateFromString(s);
    WKArrayAppendItem(origins, o);
    WKRelease(o);
    WKRelease(s);
    WKNotificationManagerRef managers[] = { WKContextGetNotificationManager(g_context),
                                            WKNotificationManagerGetSharedServiceWorkerNotificationManager() };
    for (auto m : managers) if (m) WKNotificationManagerProviderDidRemoveNotificationPolicies(m, (WKArrayRef)origins);
    WKRelease(origins);
}
