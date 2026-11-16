// Lyra: pages go through the proxy Windows is set to (Settings > Network > Proxy), as in other browsers.
//
// WebKit's network process (libcurl) only knows the http_proxy / https_proxy environment
// variables; the Windows proxy - what VPN clients such as v2rayN, Hiddify or Nekoray set in
// "system proxy" mode - it never looks at, so pages went straight to the network.
// Here the Windows setting is read (a fixed proxy, or what a proxy script / auto-detection
// gives for an ordinary https site) and handed to the website data stores; the setting is
// watched in the registry, so turning the VPN on or off applies without a restart.
// LYRA_PROXY=<url> | direct overrides it (testing).
#include "browser/SystemProxy.h"
#include "app/Log.h"
#include "browser/WebContext.h"
#include "core/Constants.h"
#include "core/StringUtils.h"
#include "browser/TabManager.h"
#include "ui/MainWindow.h"
#include <WebKit/WKWebsiteDataStoreRefCurl.h>
#include <winhttp.h>
#include <atomic>
#include <thread>

namespace {

struct Pick {
    std::string url;       // empty: no proxy
    std::string noProxy;   // libcurl's NOPROXY list
    bool operator==(const Pick& o) const { return url == o.url && noProxy == o.noProxy; }
};

Pick s_current;
bool s_haveCurrent = false;
std::vector<WKWebsiteDataStoreRef> s_stores;   // not owned
std::thread s_watcher;
HANDLE s_stop = nullptr;

std::wstring Lower(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }

// "host:port", "http://host:port", "socks=host:port" ... -> a proxy URL libcurl takes.
std::string ProxyUrl(std::wstring entry, const wchar_t* defaultScheme) {
    entry = TrimWs(entry);
    if (entry.empty()) return {};
    if (entry.find(L"://") == std::wstring::npos) entry = std::wstring(defaultScheme) + L"://" + entry;
    // socks / socks4 / socks5: let the proxy look up host names (sites blocked by DNS still open).
    std::wstring l = Lower(entry);
    if (l.rfind(L"socks://", 0) == 0) entry = L"socks5h://" + entry.substr(8);
    else if (l.rfind(L"socks5://", 0) == 0) entry = L"socks5h://" + entry.substr(9);
    return ToUtf8(entry);
}

// Windows' list: "host:port" for every protocol, or "http=a:1;https=b:2;socks=c:3".
std::string ParseProxyList(const std::wstring& list) {
    std::wstring all, https, http, socks;
    size_t i = 0;
    while (i <= list.size()) {
        size_t j = list.find_first_of(L"; ", i);
        if (j == std::wstring::npos) j = list.size();
        std::wstring item = list.substr(i, j - i);
        i = j + 1;
        if (item.empty()) continue;
        size_t eq = item.find(L'=');
        if (eq == std::wstring::npos) { if (all.empty()) all = item; continue; }
        std::wstring key = Lower(item.substr(0, eq)), val = item.substr(eq + 1);
        if (key == L"https" && https.empty()) https = val;
        else if (key == L"http" && http.empty()) http = val;
        else if (key == L"socks" && socks.empty()) socks = val;
    }
    if (!all.empty())   return ProxyUrl(all, L"http");
    if (!https.empty()) return ProxyUrl(https, L"http");
    if (!socks.empty()) return ProxyUrl(socks, L"socks5h");
    if (!http.empty())  return ProxyUrl(http, L"http");
    return {};
}

// Windows' bypass list ("<local>;*.corp;10.*;192.168.*") -> libcurl's ("localhost,.corp,10.0.0.0/8").
std::string ParseBypass(const std::wstring& list) {
    std::vector<std::string> out = { "localhost", "127.0.0.1", "::1" };
    auto add = [&](const std::string& s) {
        if (!s.empty() && std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
    };
    size_t i = 0;
    while (i <= list.size()) {
        size_t j = list.find_first_of(L";, ", i);
        if (j == std::wstring::npos) j = list.size();
        std::string item = ToUtf8(Lower(TrimWs(list.substr(i, j - i))));
        i = j + 1;
        if (item.empty() || item == "<local>") continue;
        if (item.rfind("*.", 0) == 0) { add(item.substr(1)); continue; }          // *.corp -> .corp
        if (item.find('*') == std::string::npos) { add(item); continue; }
        // 10.*  172.16.*  192.168.1.* : address ranges.
        std::vector<int> parts;
        size_t k = 0;
        bool ok = true;
        while (k < item.size()) {
            size_t d = item.find('.', k);
            std::string p = item.substr(k, d == std::string::npos ? std::string::npos : d - k);
            k = d == std::string::npos ? item.size() : d + 1;
            if (p == "*") { if (k < item.size()) ok = false; break; }
            if (p.empty() || p.size() > 3 || p.find_first_not_of("0123456789") != std::string::npos) { ok = false; break; }
            parts.push_back(atoi(p.c_str()));
        }
        if (!ok || parts.empty() || parts.size() > 3) continue;
        std::string cidr;
        for (size_t n = 0; n < 4; ++n) cidr += (n ? "." : "") + std::to_string(n < parts.size() ? parts[n] : 0);
        add(cidr + "/" + std::to_string(parts.size() * 8));
    }
    std::string s;
    for (auto& o : out) s += (s.empty() ? "" : ",") + o;
    return s;
}

// What a proxy script / auto-detection says for an ordinary https site. Can take seconds
// (auto-detection on a network without one), so only on the watcher thread.
std::string ScriptProxy(bool autoDetect, const std::wstring& script) {
    if (script.empty() && !autoDetect) return {};
    HINTERNET ses = WinHttpOpen(L"Lyra", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return {};
    WinHttpSetTimeouts(ses, 3000, 3000, 3000, 3000);
    WINHTTP_AUTOPROXY_OPTIONS opt = {};
    if (!script.empty()) {
        opt.dwFlags = WINHTTP_AUTOPROXY_CONFIG_URL;
        opt.lpszAutoConfigUrl = script.c_str();
    } else {
        opt.dwFlags = WINHTTP_AUTOPROXY_AUTO_DETECT;
        opt.dwAutoDetectFlags = WINHTTP_AUTO_DETECT_TYPE_DHCP | WINHTTP_AUTO_DETECT_TYPE_DNS_A;
    }
    opt.fAutoLogonIfChallenged = TRUE;
    WINHTTP_PROXY_INFO info = {};
    std::string url;
    if (WinHttpGetProxyForUrl(ses, L"https://www.google.com/", &opt, &info)) {
        if (info.dwAccessType == WINHTTP_ACCESS_TYPE_NAMED_PROXY && info.lpszProxy) url = ParseProxyList(info.lpszProxy);
        if (info.lpszProxy) GlobalFree(info.lpszProxy);
        if (info.lpszProxyBypass) GlobalFree(info.lpszProxyBypass);
    }
    WinHttpCloseHandle(ses);
    return url;
}

// What VPN clients and Windows Settings set, as WinINet keeps it (InternetSetOption).
// WinHttpGetIEProxyConfigForCurrentUser was not used for this: on a Windows 11 test machine it
// kept answering an old copy of the setting long after it changed. <wininet.h> clashes with
// <winhttp.h>, so its few declarations are here and the function is looked up.
struct IeConfig { bool autoDetect = false; std::wstring script, proxy, bypass; };

bool ReadIeConfig(IeConfig& c) {
    struct Option { DWORD dwOption; union { DWORD dwValue; LPWSTR pszValue; FILETIME ftValue; } Value; };
    struct OptionList { DWORD dwSize; LPWSTR pszConnection; DWORD dwOptionCount; DWORD dwOptionError; Option* pOptions; };
    using QueryFn = BOOL(WINAPI*)(void*, DWORD, void*, DWORD*);
    static QueryFn query = [] {
        HMODULE m = LoadLibraryExW(L"wininet.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return m ? (QueryFn)GetProcAddress(m, "InternetQueryOptionW") : nullptr;
    }();
    if (!query) return false;
    Option o[4] = {};
    o[0].dwOption = 1;   // INTERNET_PER_CONN_FLAGS
    o[1].dwOption = 2;   // INTERNET_PER_CONN_PROXY_SERVER
    o[2].dwOption = 3;   // INTERNET_PER_CONN_PROXY_BYPASS
    o[3].dwOption = 4;   // INTERNET_PER_CONN_AUTOCONFIG_URL
    OptionList list = { sizeof(OptionList), nullptr, 4, 0, o };
    DWORD size = sizeof(list);
    if (!query(nullptr, 75 /* INTERNET_OPTION_PER_CONNECTION_OPTION */, &list, &size)) return false;
    const DWORD flags = o[0].Value.dwValue;
    auto take = [](LPWSTR s, bool use) { std::wstring r = (use && s) ? s : L""; if (s) GlobalFree(s); return r; };
    c.proxy = take(o[1].Value.pszValue, flags & 0x2);    // PROXY_TYPE_PROXY
    c.bypass = take(o[2].Value.pszValue, flags & 0x2);
    c.script = take(o[3].Value.pszValue, flags & 0x4);   // PROXY_TYPE_AUTO_PROXY_URL
    c.autoDetect = (flags & 0x8) != 0;                   // PROXY_TYPE_AUTO_DETECT
    return true;
}

Pick ReadPick(bool withScript) {
    Pick p;
    wchar_t env[512] = {};
    if (GetEnvironmentVariableW(L"LYRA_PROXY", env, 512)) {
        std::wstring v = TrimWs(env);
        if (Lower(v) != L"direct") p.url = ProxyUrl(v, L"http");
        p.noProxy = ParseBypass(L"");
        return p;
    }
    IeConfig c;
    if (!ReadIeConfig(c)) return p;
    // Windows' order: auto-detection / the script first, the fixed proxy when they give nothing.
    if (withScript) p.url = ScriptProxy(c.autoDetect, c.script);
    if (p.url.empty() && !c.proxy.empty()) p.url = ParseProxyList(c.proxy);
    p.noProxy = ParseBypass(c.bypass);
    return p;
}

void ApplyTo(WKWebsiteDataStoreRef store, const Pick& p) {
    if (!store) return;
    if (p.url.empty()) {   // none: libcurl's own default (the environment variables, if any)
        WKWebsiteDataStoreEnableDefaultNetworkProxySettings(store);
        return;
    }
    WKURLRef url = WKURLCreateWithUTF8CString(p.url.c_str());
    WKStringRef hosts = WKStringCreateWithUTF8CString(p.noProxy.c_str());
    WKWebsiteDataStoreEnableCustomNetworkProxySettings(store, url, hosts);
    WKRelease(hosts);
    WKRelease(url);
}

void Use(const Pick& p) {
    if (s_haveCurrent && p == s_current) return;
    s_current = p;
    s_haveCurrent = true;
    Log("proxy: %s%s%s", p.url.empty() ? "none" : p.url.c_str(), p.url.empty() ? "" : "  bypass ", p.url.empty() ? "" : p.noProxy.c_str());
    for (auto store : s_stores) ApplyTo(store, p);
}

// Some Windows builds keep the setting outside the registry (no change notice comes), so it is
// also looked at every 2 seconds - a cheap in-process query; the slow part, a proxy script or
// auto-detection, runs only when the setting itself changed.
void Watch() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
            0, KEY_NOTIFY, &key) != ERROR_SUCCESS) key = nullptr;
    HANDLE changed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    IeConfig last;
    bool first = true;
    for (;;) {
        if (key) RegNotifyChangeKeyValue(key, TRUE, REG_NOTIFY_CHANGE_LAST_SET, changed, TRUE);
        if (!first) {
            HANDLE h[2] = { s_stop, changed };
            DWORD w = WaitForMultipleObjects(key ? 2 : 1, h, FALSE, 2000);
            if (w != WAIT_OBJECT_0 + 1 && w != WAIT_TIMEOUT) break;
            if (w == WAIT_OBJECT_0 + 1)   // a VPN client writes several values in a row: settle first
                while (WaitForSingleObject(changed, 300) == WAIT_OBJECT_0)
                    RegNotifyChangeKeyValue(key, TRUE, REG_NOTIFY_CHANGE_LAST_SET, changed, TRUE);
            if (WaitForSingleObject(s_stop, 0) == WAIT_OBJECT_0) break;
        }
        IeConfig now;
        ReadIeConfig(now);
        if (!first && now.autoDetect == last.autoDetect && now.script == last.script
                && now.proxy == last.proxy && now.bypass == last.bypass) continue;
        first = false;
        last = now;
        Pick* p = new Pick(ReadPick(true));
        if (!g_hWnd || g_closing || !PostMessageW(g_hWnd, WM_APP_PROXY, 0, (LPARAM)p)) delete p;
    }
    CloseHandle(changed);
    if (key) RegCloseKey(key);
}

} // namespace

void SystemProxyApply(WKWebsiteDataStoreRef store) {
    if (!store || std::find(s_stores.begin(), s_stores.end(), store) != s_stores.end()) return;
    s_stores.push_back(store);
    if (s_haveCurrent) ApplyTo(store, s_current);
}

void SystemProxyForget(WKWebsiteDataStoreRef store) {
    s_stores.erase(std::remove(s_stores.begin(), s_stores.end(), store), s_stores.end());
}

void SystemProxyInit() {
    if (g_dataStore) s_stores.push_back(g_dataStore);
    Use(ReadPick(false));   // the fixed proxy now, before the first page; a script's answer follows
    s_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    s_watcher = std::thread(Watch);
}

void SystemProxyChanged(LPARAM lParam) {
    std::unique_ptr<Pick> p((Pick*)lParam);
    if (!p || g_closing) return;
    if (s_haveCurrent && *p == s_current) Log("proxy: the Windows setting changed, still %s", p->url.empty() ? "none" : p->url.c_str());
    Use(*p);
}

void SystemProxyShutdown() {
    if (s_stop) SetEvent(s_stop);
    if (s_watcher.joinable()) s_watcher.detach();   // may be inside a proxy script lookup; it only posts
    s_stores.clear();
}
