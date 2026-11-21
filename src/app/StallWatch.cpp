// Lyra — сторож микрофризов (см. StallWatch.h).
#include "app/StallWatch.h"
#include "app/Log.h"
#include <psapi.h>
#include <atomic>
#include <thread>

namespace {

constexpr DWORD kPingEveryMs = 50;
constexpr DWORD kStallMs = 100;           // reported from here on
constexpr DWORD kFirstSampleMs = 90;      // where the main thread is, early in the stall
constexpr DWORD kSecondSampleMs = 400;    // ... and again if it goes on
constexpr size_t kStackCopy = 32 * 1024;
constexpr int kMaxFrames = 18;
constexpr UINT WM_STALL_PING = WM_APP + 1;

HWND s_wnd = nullptr;
HANDLE s_mainThread = nullptr;
std::thread s_thread;
std::atomic<bool> s_quit{false};
std::atomic<ULONGLONG> s_pong{0};          // answered ping number

struct Module { uintptr_t base, end; std::wstring name; };
std::vector<Module> s_modules;

void RefreshModules() {
    s_modules.clear();
    HMODULE mods[512];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return;
    for (DWORD i = 0; i < std::min<DWORD>(needed / sizeof(HMODULE), 512); ++i) {
        MODULEINFO mi = {};
        wchar_t path[MAX_PATH] = {};
        if (!GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi))) continue;
        GetModuleFileNameW(mods[i], path, MAX_PATH);
        const wchar_t* base = wcsrchr(path, L'\\');
        s_modules.push_back({ (uintptr_t)mi.lpBaseOfDll, (uintptr_t)mi.lpBaseOfDll + mi.SizeOfImage, base ? base + 1 : path });
    }
}

const Module* ModuleOf(uintptr_t a) {
    for (const Module& m : s_modules) if (a >= m.base && a < m.end) return &m;
    return nullptr;
}

// Does `ret` follow a call instruction? (The usual forms: call rel32, call [rip+disp32],
// call reg, call [reg+disp8 / disp32], with or without a REX prefix.)
bool FollowsCall(uintptr_t ret) {
    unsigned char b[8] = {};
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (const void*)(ret - 8), b, 8, &got) || got != 8) return false;
    if (b[3] == 0xE8) return true;                                         // E8 rel32
    if (b[2] == 0xFF && (b[3] & 0x38) == 0x10 && ((b[3] & 0xC7) == 0x05 || (b[3] & 0xC0) == 0x80)) return true;   // FF /2 [rip/reg+disp32]
    if (b[5] == 0xFF && (b[6] & 0xF8) == 0x50) return true;                // FF /2 [reg+disp8]
    if (b[6] == 0xFF && (b[7] & 0xF8) == 0xD0) return true;                // FF /2 reg
    if (b[1] == 0xFF && (b[2] & 0x38) == 0x10 && (b[2] & 0xC0) == 0x80) return true;  // REX FF /2 [reg+disp32]
    return false;
}

struct Sample {
    bool ok = false;
    uintptr_t rip = 0, rsp = 0;
    std::vector<unsigned char> stack;
};

// Stops the main thread just long enough to read where it is and copy the top of its stack.
Sample TakeSample() {
    Sample s;
    if (SuspendThread(s_mainThread) == (DWORD)-1) return s;
    CONTEXT c = {};
    c.ContextFlags = CONTEXT_CONTROL;
    if (GetThreadContext(s_mainThread, &c)) {
        s.rip = (uintptr_t)c.Rip;
        s.rsp = (uintptr_t)c.Rsp;
        s.stack.resize(kStackCopy);
        // Page by page up to the end of the committed stack (ReadProcessMemory fails instead of
        // faulting there, and fails as a whole for a range that crosses it).
        size_t got = 0;
        while (got < kStackCopy) {
            const uintptr_t at = s.rsp + got;
            const size_t chunk = std::min<size_t>(kStackCopy - got, 4096 - (at & 4095));
            SIZE_T n = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), (const void*)at, s.stack.data() + got, chunk, &n) || n != chunk) break;
            got += chunk;
        }
        s.stack.resize(got);
        s.ok = true;
    }
    ResumeThread(s_mainThread);
    return s;
}

std::string Describe(uintptr_t a) {
    char buf[300];
    if (const Module* m = ModuleOf(a)) snprintf(buf, sizeof(buf), "%ls+0x%llX", m->name.c_str(), (unsigned long long)(a - m->base));
    else snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)a);
    return buf;
}

std::string Frames(const Sample& s) {
    if (!s.ok) return "(no snapshot)";
    std::string out = Describe(s.rip);
    int n = 1;
    for (size_t i = 0; i + sizeof(uintptr_t) <= s.stack.size() && n < kMaxFrames; i += sizeof(uintptr_t)) {
        uintptr_t v;
        memcpy(&v, s.stack.data() + i, sizeof(v));
        if (!ModuleOf(v) || !FollowsCall(v)) continue;
        out += " < " + Describe(v);
        ++n;
    }
    return out;
}

void Watch() {
    ULONGLONG sent = 0;
    while (!s_quit) {
        ++sent;
        const ULONGLONG t0 = GetTickCount64();
        if (!PostMessageW(s_wnd, WM_STALL_PING, 0, (LPARAM)sent)) break;
        Sample first, second;
        bool late = false;
        while (!s_quit && s_pong.load() < sent) {
            Sleep(5);
            const ULONGLONG waited = GetTickCount64() - t0;
            if (waited >= kFirstSampleMs && !first.ok) first = TakeSample();
            if (waited >= kSecondSampleMs && !second.ok) second = TakeSample();
            late = waited >= kStallMs;
        }
        if (s_quit) break;
        {   // Developer check: LYRA_SAMPLE_MAIN=1 logs where the main thread is once a second, busy or not
            static const bool sampleAlways = GetEnvironmentVariableW(L"LYRA_SAMPLE_MAIN", nullptr, 0) != 0;
            static ULONGLONG lastSample = 0;
            if (sampleAlways && GetTickCount64() - lastSample > 1000) {
                lastSample = GetTickCount64();
                RefreshModules();
                Log("sample: main thread at %s", Frames(TakeSample()).c_str());
            }
        }
        const ULONGLONG took = GetTickCount64() - t0;
        if (late && took >= kStallMs) {
            RefreshModules();
            Log("stall: the window did not respond for %llu ms", took);
            Log("stall:   at %s", Frames(first).c_str());
            if (second.ok) Log("stall:   later %s", Frames(second).c_str());
        }
        Sleep(kPingEveryMs);
    }
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_STALL_PING) { s_pong = (ULONGLONG)l; return 0; }
    return DefWindowProcW(h, msg, w, l);
}

}  // namespace

void StallWatchStart() {
    if (s_wnd) return;
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"LyraStallWatch";
    RegisterClassExW(&wc);
    s_wnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!s_wnd) return;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_mainThread,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    RefreshModules();
    s_quit = false;
    s_thread = std::thread(Watch);
}

void StallWatchStop() {
    s_quit = true;
    if (s_thread.joinable()) s_thread.join();
    if (s_wnd) { DestroyWindow(s_wnd); s_wnd = nullptr; }
    if (s_mainThread) { CloseHandle(s_mainThread); s_mainThread = nullptr; }
}
