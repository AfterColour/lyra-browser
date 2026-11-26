// Lyra — инструмент разработчика: замеры на странице (LYRA_PROBE=<файл .js>), результат в tabs.log.
#include "browser/DevProbe.h"
#include "app/Log.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "core/StringUtils.h"

#include <WebKit/WKMutableDictionary.h>
#include <WebKit/WKPagePrivate.h>
#include <psapi.h>
#include <fstream>
#include <map>
#include <vector>
#include <iterator>

// LYRA_PROBE_SNAPSHOT=<file.png>: after the probe, a picture of the page (WKPageTakeSnapshot) goes there.
static void OnSnapshot(const void* pixels, uint32_t w, uint32_t h, uint32_t stride, void*) {
    wchar_t path[MAX_PATH] = {};
    if (!pixels || !GetEnvironmentVariableW(L"LYRA_PROBE_SNAPSHOT", path, MAX_PATH)) { Log("probe: no snapshot"); return; }
    Gdiplus::Bitmap bmp((INT)w, (INT)h, (INT)stride, PixelFormat32bppPARGB, (BYTE*)pixels);
    UINT n = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&n, &size);
    std::vector<BYTE> buf(size);
    auto* codecs = (Gdiplus::ImageCodecInfo*)buf.data();
    if (size && Gdiplus::GetImageEncoders(n, size, codecs) == Gdiplus::Ok)
        for (UINT i = 0; i < n; ++i)
            if (wcscmp(codecs[i].MimeType, L"image/png") == 0) bmp.Save(path, &codecs[i].Clsid);
    Log("probe: snapshot %ux%u saved", w, h);
}

static void OnProbeDone(WKTypeRef result, WKErrorRef error, void* ctx) {
    std::string s;
    if (result && WKGetTypeID(result) == WKStringGetTypeID()) s = WKStrUtf8((WKStringRef)result);
    else if (error) s = "(error)";
    else s = "(no result)";
    Log("probe tab#%llu: %s", (unsigned long long)(uintptr_t)ctx, s.c_str());
    if (GetEnvironmentVariableW(L"LYRA_PROBE_SNAPSHOT", nullptr, 0))
        if (Tab* t = FindTab((unsigned long long)(uintptr_t)ctx); t && t->page) WKPageTakeSnapshot(t->page, 1600, 1200, OnSnapshot, nullptr);
}

void DevProbePageLoaded(Tab& t) {
    static int state = 0;                    // 0 unknown, 1 on, 2 off
    static std::string source;
    if (state == 0) {
        wchar_t path[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"LYRA_PROBE", path, MAX_PATH)) {
            std::ifstream f(path, std::ios::binary);
            source.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        }
        state = source.empty() ? 2 : 1;
        if (state == 1) Log("probe: running %ls on every page", path);
    }
    if (state != 1 || !t.page) return;
    if (t.url.compare(0, 4, L"http") != 0 && t.url.compare(0, 5, L"file:") != 0) return;
    WKStringRef js = WKStringCreateWithUTF8CString(source.c_str());
    WKMutableDictionaryRef args = WKMutableDictionaryCreate();
    WKPageCallAsyncJavaScript(t.page, js, (WKDictionaryRef)args, nullptr, (void*)(uintptr_t)t.id, OnProbeDone);
    WKRelease(args);
    WKRelease(js);
}

// LYRA_PROCS=<seconds> (developer): that long after start, and again every 30 s, which page runs
// in which process and how much memory each process takes (tabs.log).
void TabsListPages(std::vector<std::pair<std::wstring, WKPageRef>>& out);
void SidebarListPages(std::vector<std::pair<std::wstring, WKPageRef>>& out);
void ExtensionsListPages(std::vector<std::pair<std::wstring, WKPageRef>>& out);

static void LogProcesses() {
    std::vector<std::pair<std::wstring, WKPageRef>> pages;
    TabsListPages(pages);
    SidebarListPages(pages);
    ExtensionsListPages(pages);
    std::map<WKProcessID, std::vector<std::wstring>> byPid;
    for (auto& [what, page] : pages) byPid[WKPageGetProcessIdentifier(page)].push_back(what);
    if (!pages.empty()) {
        if (WKProcessID gpu = WKPageGetGPUProcessIdentifier(pages.front().second)) byPid[gpu].push_back(L"(GPU process)");
    }
    size_t total = 0;
    for (auto& [pid, whats] : byPid) {
        size_t ws = 0, priv = 0;
        if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid)) {
            PROCESS_MEMORY_COUNTERS_EX pmc = {};
            if (GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) { ws = pmc.WorkingSetSize; priv = pmc.PrivateUsage; }
            CloseHandle(h);
        }
        total += ws;
        std::wstring list;
        for (auto& w : whats) list += (list.empty() ? L"" : L", ") + w;
        Log("procs: pid %lu %zu MB (private %zu MB): %ls", (unsigned long)pid, ws >> 20, priv >> 20, list.c_str());
    }
    Log("procs: %zu MB in these processes", total >> 20);
}

void DevProcessesStart() {
    wchar_t v[8] = {};
    if (!GetEnvironmentVariableW(L"LYRA_PROCS", v, 8) || _wtoi(v) <= 0) return;
    SetTimer(nullptr, 0, (UINT)_wtoi(v) * 1000, [](HWND, UINT, UINT_PTR id, DWORD) {
        KillTimer(nullptr, id);
        LogProcesses();
        SetTimer(nullptr, 0, 30000, [](HWND, UINT, UINT_PTR, DWORD) { LogProcesses(); });
    });
}