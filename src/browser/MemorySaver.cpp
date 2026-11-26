// Lyra — сжатие памяти простаивающих процессов WebKit (см. MemorySaver.h).
#include "browser/MemorySaver.h"
#include "app/Log.h"
#include "browser/SplitView.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "browser/WebContext.h"
#include "core/Settings.h"
#include "ui/MainWindow.h"

#include <WebKit/WKPagePrivate.h>
#include <WebKit/WKWebsiteDataStoreRef.h>
#include <psapi.h>
#include <map>
#include <set>
#include <string>
#include <vector>

void TabsListPages(std::vector<std::pair<std::wstring, WKPageRef>>& out);
void SidebarListPages(std::vector<std::pair<std::wstring, WKPageRef>>& out);
void ExtensionsListPages(std::vector<std::pair<std::wstring, WKPageRef>>& out);
bool SidebarPanelOpen();

namespace {

constexpr ULONGLONG kIdleMs = 30000;       // no work for this long: its memory goes
constexpr ULONGLONG kBusyCpuMs = 60;       // more processor time than this since the last look: it works

struct Proc {
    ULONGLONG cpuMs = 0;                   // processor time at the last look
    ULONGLONG activeAt = 0;                // when it last worked (or was seen first)
    bool trimmed = false;                  // given back since it last worked
    bool seen = false;
};
std::map<DWORD, Proc> s_procs;

ULONGLONG CpuMs(HANDLE h) {
    FILETIME c, e, k, u;
    if (!GetProcessTimes(h, &c, &e, &k, &u)) return 0;
    auto ms = [](const FILETIME& f) { return ((ULONGLONG)f.dwHighDateTime << 32 | f.dwLowDateTime) / 10000; };
    return ms(k) + ms(u);
}

}  // namespace

void MemorySaverTick() {
    if (!g_prefs.memorySaver || !g_hWnd) { s_procs.clear(); return; }
    const bool minimized = IsIconic(g_hWnd) != FALSE;
    // What may give its memory back: the processes of pages not on screen. A process also runs
    // the pages on screen when WebKit put them together: those stay.
    std::vector<std::pair<std::wstring, WKPageRef>> pages;
    TabsListPages(pages);
    const size_t tabsEnd = pages.size();
    SidebarListPages(pages);
    const size_t sidebarEnd = pages.size();
    ExtensionsListPages(pages);
    std::set<DWORD> shown, candidates;
    std::map<DWORD, std::wstring> what;
    Tab* act = ActiveTab();
    Tab* partner = act ? SplitPartnerOf(*act) : nullptr;
    std::set<WKPageRef> onScreen;
    if (!minimized) {
        if (act && act->page) onScreen.insert(act->page);
        if (partner && partner->page) onScreen.insert(partner->page);
        for (auto& t : g_tabs) if (t->pipHwnd && t->page) onScreen.insert(t->page);   // picture-in-picture
    }
    for (size_t i = 0; i < pages.size(); ++i) {
        const DWORD pid = (DWORD)WKPageGetProcessIdentifier(pages[i].second);
        if (!pid) continue;
        const bool sidebarShown = i >= tabsEnd && i < sidebarEnd && SidebarPanelOpen() && !minimized;
        if (onScreen.count(pages[i].second) || sidebarShown) shown.insert(pid);
        candidates.insert(pid);
        std::wstring& w = what[pid];
        if (w.size() < 120) w += (w.empty() ? L"" : L", ") + pages[i].first;
    }
    if (!pages.empty()) {
        if (g_dataStore) if (DWORD net = (DWORD)WKWebsiteDataStoreGetNetworkProcessIdentifier(g_dataStore)) { candidates.insert(net); what[net] = L"network process"; }
        if (minimized) if (DWORD gpu = (DWORD)WKPageGetGPUProcessIdentifier(pages.front().second)) { candidates.insert(gpu); what[gpu] = L"GPU process"; }
    }
    const ULONGLONG now = GetTickCount64();
    for (auto& [pid, p] : s_procs) p.seen = false;
    for (DWORD pid : candidates) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_QUOTA, FALSE, pid);
        if (!h) continue;
        Proc& p = s_procs[pid];
        const ULONGLONG cpu = CpuMs(h);
        static const bool verbose = GetEnvironmentVariableW(L"LYRA_PROCS", nullptr, 0) != 0;
        if (verbose && p.activeAt) Log("memory saver: pid %lu (%ls) %llu ms of processor in the last look%s", pid, what[pid].c_str(), cpu - p.cpuMs, shown.count(pid) ? ", on screen" : "");
        if (!p.activeAt || shown.count(pid) || cpu > p.cpuMs + kBusyCpuMs) { p.activeAt = now; p.trimmed = false; }
        p.cpuMs = cpu;
        p.seen = true;
        if (!p.trimmed && now - p.activeAt >= kIdleMs) {
            PROCESS_MEMORY_COUNTERS before = {}, after = {};
            GetProcessMemoryInfo(h, &before, sizeof(before));
            if (EmptyWorkingSet(h)) {
                GetProcessMemoryInfo(h, &after, sizeof(after));
                Log("memory saver: pid %lu (%ls) %zu -> %zu MB", pid, what[pid].c_str(), before.WorkingSetSize >> 20, after.WorkingSetSize >> 20);
            }
            p.trimmed = true;                  // (once per idle spell, also when it failed)
        }
        CloseHandle(h);
    }
    for (auto it = s_procs.begin(); it != s_procs.end();) it = it->second.seen ? std::next(it) : s_procs.erase(it);
}
