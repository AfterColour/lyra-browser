// Lyra — восстановление сессии: вкладки прошлого запуска (и после сбоя).
//
// session.txt (UTF-8):
//   LyraSession 1
//   clean 0|1          1 = written when Lyra closed normally
//   active <index>
//   tab                one block per tab, in order
//   url <address>      Lyra's own pages as their lyra:// alias
//   title <title>
//   state <base64>     WebKit's session state (back/forward list); optional
//   end
#include "browser/Session.h"
#include "app/Log.h"
#include "browser/StartPage.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "browser/WebContext.h"
#include "core/Settings.h"
#include "core/StringUtils.h"
#include "core/Url.h"

#include <WebKit/WKSessionStateRef.h>
#include <fstream>
#include <sstream>

namespace {

std::wstring SessionFile() { return g_settingsDir + L"\\session.txt"; }

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64(const unsigned char* p, size_t n) {
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = p[i] << 16 | (i + 1 < n ? p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        out += kB64[v >> 18 & 63];
        out += kB64[v >> 12 & 63];
        out += i + 1 < n ? kB64[v >> 6 & 63] : '=';
        out += i + 2 < n ? kB64[v & 63] : '=';
    }
    return out;
}

std::string Unbase64(const std::string& s) {
    std::string out;
    unsigned v = 0;
    int bits = 0;
    for (char c : s) {
        const char* p = strchr(kB64, c);
        if (!p || !c) continue;
        v = v << 6 | (unsigned)(p - kB64);
        bits += 6;
        if (bits >= 8) { bits -= 8; out += (char)(v >> bits & 0xFF); }
    }
    return out;
}

std::string OneLine(const std::wstring& s) {
    std::string u = ToUtf8(s);
    for (auto& c : u) if (c == '\r' || c == '\n') c = ' ';
    return u;
}

// Lyra's own pages are generated files whose addresses change; they are saved as their alias.
std::wstring SavedUrl(const std::wstring& url) {
    if (IsStartPageUrl(url)) return kStartAlias;
    if (IsHistoryPageUrl(url)) return kHistoryAlias;
    if (IsBookmarksPageUrl(url)) return kBookmarksAlias;
    if (IsDownloadsPageUrl(url)) return kDownloadsAlias;
    return url;
}

bool AllowAll(WKPageRef, WKStringRef, WKTypeRef, void*) { return true; }

std::string PageState(WKPageRef page) {
    std::string out;
    // With an even `context` WebKit returns the state already encoded, as a WKDataRef (the same
    // encoding WKSessionStateCreateFromData reads back).
    WKTypeRef state = WKPageCopySessionState(page, nullptr, AllowAll);
    if (!state) return out;
    if (WKGetTypeID(state) == WKDataGetTypeID()) {
        WKDataRef data = (WKDataRef)state;
        out = Base64(WKDataGetBytes(data), WKDataGetSize(data));
    } else if (WKGetTypeID(state) == WKSessionStateGetTypeID()) {
        if (WKDataRef data = WKSessionStateCopyData((WKSessionStateRef)state)) {
            out = Base64(WKDataGetBytes(data), WKDataGetSize(data));
            WKRelease(data);
        }
    }
    WKRelease(state);
    return out;
}

struct SavedTab { std::wstring url, title; std::string state; };

std::string s_lastSignature;
DWORD s_lastSave = 0;

std::string Build(bool clean, std::string* signature) {
    std::ostringstream out, sig;
    // Private tabs are never saved; the active index counts only the saved ones.
    int activeSaved = 0, saved = 0;
    for (int i = 0; i < (int)g_tabs.size(); ++i) {
        if (g_tabs[i]->state != TabState::Live || g_tabs[i]->privateTab) continue;
        if (i <= g_active) activeSaved = saved;
        ++saved;
    }
    out << "LyraSession 1\nclean " << (clean ? 1 : 0) << "\nactive " << activeSaved << "\n";
    sig << g_active << '|';
    for (auto& tp : g_tabs) {
        const Tab& t = *tp;
        if (t.state != TabState::Live || t.privateTab) continue;
        std::wstring url = t.lazyPending ? t.lazyUrl : SavedUrl(t.url);
        if (url.empty()) url = t.url;
        std::string state;
        if (t.lazyPending) state = t.lazyState;
        else if (t.page && !IsInternalPageUrl(t.url) && !t.url.empty()) state = PageState(t.page);
        out << "tab\nurl " << OneLine(url) << "\ntitle " << OneLine(t.title) << "\n";
        if (!state.empty()) out << "state " << state << "\n";
        out << "end\n";
        sig << OneLine(url) << '|' << state.size() << '|' << (t.canBack ? 1 : 0) << (t.canFwd ? 1 : 0) << '\n';
    }
    if (signature) *signature = sig.str();
    return out.str();
}

void Write(const std::string& text) {
    CreateDirectoryW(g_settingsDir.c_str(), nullptr);
    const std::wstring path = SessionFile(), tmp = path + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f.write(text.data(), (std::streamsize)text.size());
        if (!f) return;
    }
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

} // namespace

void SessionSave(bool clean) {
    if (g_tabs.empty()) return;          // closing: the tabs are already gone, keep the last save
    std::string sig;
    Write(Build(clean, &sig));
    s_lastSignature = sig;
    s_lastSave = GetTickCount();
}

void SessionTick() {
    static DWORD lastCheck = 0;
    const DWORD now = GetTickCount();
    if (now - lastCheck < 1000) return;
    lastCheck = now;
    if (g_tabs.empty()) return;
    std::string sig;
    std::string text = Build(false, &sig);
    // The signature misses some changes (moves inside the back/forward list keep its size):
    // write anyway every half minute.
    if (sig == s_lastSignature && now - s_lastSave < 30000) return;
    Write(text);
    s_lastSignature = sig;
    s_lastSave = now;
}

bool SessionRestore() {
    std::ifstream f(SessionFile(), std::ios::binary);
    if (!f) return false;
    std::vector<SavedTab> tabs;
    bool clean = false;
    int active = 0;
    std::string line;
    SavedTab cur;
    bool inTab = false;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto value = [&](const char* key) -> const char* {
            size_t n = strlen(key);
            return line.compare(0, n, key) == 0 && line.size() > n && line[n] == ' ' ? line.c_str() + n + 1 : nullptr;
        };
        if (line == "tab") { cur = SavedTab(); inTab = true; }
        else if (line == "end") { if (inTab && !cur.url.empty()) tabs.push_back(cur); inTab = false; }
        else if (const char* v = value("clean")) clean = atoi(v) != 0;
        else if (const char* v = value("active")) active = atoi(v);
        else if (!inTab) continue;
        else if (const char* v = value("url")) cur.url = FromUtf8(v);
        else if (const char* v = value("title")) cur.title = FromUtf8(v);
        else if (const char* v = value("state")) cur.state = v;
    }
    // Closed before any tab starts WebKit's processes: they inherit open CRT files, and an
    // inherited handle kept session.txt locked, so later saves could not replace it.
    f.close();
    if (tabs.empty()) return false;
    if (clean && !g_prefs.restoreSession) return false;
    // A single empty start page is not worth restoring.
    if (tabs.size() == 1 && tabs[0].url == kStartAlias) return false;
    Log("session: restoring %zu tab(s) (previous exit %s)", tabs.size(), clean ? "normal" : "NOT clean");

    for (auto& s : tabs) {
        WKPageConfigurationRef conf = CreatePageConfigurationForTab();
        Tab* t = CreateTabWithConfiguration(conf, false, false);
        WKRelease(conf);
        if (!t) break;
        t->lazyPending = true;
        t->lazyUrl = s.url;
        t->lazyState = Unbase64(s.state);
        t->url = s.url;
        t->title = s.title.empty() ? HostOf(s.url) : s.title;
    }
    if (g_tabs.empty()) return false;
    SelectTab(std::min(std::max(active, 0), (int)g_tabs.size() - 1));
    return true;
}

void SessionLoadLazyTab(Tab& t) {
    if (!t.lazyPending || !t.page) return;
    t.lazyPending = false;
    const std::wstring url = std::move(t.lazyUrl);
    const std::string state = std::move(t.lazyState);
    if (!state.empty() && url.compare(0, 7, L"lyra://") != 0) {
        WKDataRef data = WKDataCreate((const unsigned char*)state.data(), state.size());
        WKSessionStateRef session = WKSessionStateCreateFromData(data);
        WKRelease(data);
        if (session) {
            WKPageRestoreFromSessionState(t.page, session);
            WKRelease(session);
            Log("tab#%llu restored with its history", t.id);
            return;
        }
    }
    const std::string target = ResolveNavigation(url);
    if (target.empty()) return;
    if (WKURLRef u = WKURLCreateWithUTF8CString(target.c_str())) {
        WKPageLoadURL(t.page, u);
        WKRelease(u);
    }
}
