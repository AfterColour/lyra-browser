// Lyra — диагностический журнал tabs.log.
#include "app/Log.h"
#include "core/Settings.h"

// ----------------------------------------------------------------------------
//  Diagnostic log (tab lifecycle only; never written from paint code).
//  File: %LOCALAPPDATA%\Lyra\tabs.log  - appended across runs, every line is
//  written straight to the file so it survives a crash.
// ----------------------------------------------------------------------------
static HANDLE g_logFile = INVALID_HANDLE_VALUE;

void Log(const char* fmt, ...) {
    if (g_logFile == INVALID_HANDLE_VALUE) return;
    char buf[768];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%02d:%02d:%02d.%03d [t%lu] ",
                        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentThreadId());
    if (n < 0) n = 0;
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE, fmt, ap);
    va_end(ap);
    size_t len = strlen(buf);
    if (len > sizeof(buf) - 3) len = sizeof(buf) - 3;
    buf[len++] = '\r'; buf[len++] = '\n';
    DWORD wr = 0;
    WriteFile(g_logFile, buf, (DWORD)len, &wr, nullptr);
}

void LogOpen() {
    if (g_logFile != INVALID_HANDLE_VALUE || g_settingsDir.empty()) return;
    CreateDirectoryW(g_settingsDir.c_str(), nullptr);
    std::wstring path = g_settingsDir + L"\\tabs.log";
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz = {};
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > (1 << 20)) {      // keep the file small: restart it
        CloseHandle(h);
        h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
    }
    g_logFile = h;
    Log("==== session start, pid=%lu ====", GetCurrentProcessId());
}

void LogAddress(const char* label, const void* addr) {
    HMODULE mod = nullptr;
    wchar_t path[MAX_PATH] = {};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)addr, &mod) && mod && GetModuleFileNameW(mod, path, MAX_PATH)) {
        const wchar_t* base = wcsrchr(path, L'\\');
        Log("%s %p  %ls+0x%llX", label, addr, base ? base + 1 : path,
            (unsigned long long)((const char*)addr - (const char*)mod));
    } else {
        Log("%s %p  (unknown module)", label, addr);
    }
}
