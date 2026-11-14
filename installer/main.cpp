// LyraSetup entry point: picks install or uninstall, with or without the window (see Setup.h).
#include "Setup.h"
#include <objbase.h>
#include <shellapi.h>
#include <objidl.h>
#include <algorithm>
namespace Gdiplus { using std::min; using std::max; }   // gdiplus.h needs them (NOMINMAX)
#include <gdiplus.h>

bool UiRussian() {
    static const bool ru = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
    return ru;
}

namespace {
struct Args {
    bool silent = false, update = false, uninstall = false, fromTemp = false, noRegister = false, noDesktop = false, noRestart = false;
    std::wstring dir;
};

Args ParseArgs() {
    Args a;
    int n = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &n);
    for (int i = 1; i < n; i++) {
        std::wstring s = argv[i];
        if (!s.empty() && s[0] == L'-') s[0] = L'/';
        if (_wcsicmp(s.c_str(), L"/S") == 0 || _wcsicmp(s.c_str(), L"/silent") == 0) a.silent = true;
        else if (_wcsicmp(s.c_str(), L"/update") == 0) a.update = a.silent = true;
        else if (_wcsicmp(s.c_str(), L"/uninstall") == 0) a.uninstall = true;
        else if (_wcsicmp(s.c_str(), L"/fromtemp") == 0) a.fromTemp = true;
        else if (_wcsicmp(s.c_str(), L"/noregister") == 0) a.noRegister = true;
        else if (_wcsicmp(s.c_str(), L"/nodesktop") == 0) a.noDesktop = true;
        else if (_wcsicmp(s.c_str(), L"/norestart") == 0) a.noRestart = true;
        else if (_wcsnicmp(s.c_str(), L"/dir=", 5) == 0) a.dir = s.substr(5);
    }
    LocalFree(argv);
    while (a.dir.size() > 3 && (a.dir.back() == L'\\' || a.dir.back() == L'/')) a.dir.pop_back();
    return a;
}

// Uninstall.exe cannot delete itself while it runs from the folder it removes: it runs a copy
// of itself from %TEMP% instead, which deletes itself when done.
bool RelaunchFromTemp(const Args& a, const std::wstring& dir) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring copy = std::wstring(tmp) + L"LyraUninstall-" + std::to_wstring(GetCurrentProcessId()) + L".exe";
    if (!CopyFileW(ExePath().c_str(), copy.c_str(), FALSE)) return false;
    std::wstring cmd = L"\"" + copy + L"\" /uninstall /fromtemp \"/dir=" + dir + L"\"";
    if (a.silent) cmd += L" /S";
    if (a.noRegister) cmd += L" /noregister";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, tmp, &si, &pi)) return false;
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void DeleteSelfLater() {
    // Tries for ~20 s: the file stays locked until this process has ended, and a virus scanner
    // may hold it a little longer.
    const std::wstring self = L"\"" + ExePath() + L"\"";
    std::wstring cmd = L"cmd.exe /c for /l %i in (1,1,10) do (ping -n 3 127.0.0.1 >nul & del /f /q " + self +
                       L" >nul 2>&1 & if not exist " + self + L" exit)";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, tmp, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}
}  // namespace

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gdip = 0;
    Gdiplus::GdiplusStartup(&gdip, &gsi, nullptr);

    const Args a = ParseArgs();
    Payload payload;
    const bool carriesFiles = PayloadOpen(payload);
    Options o;
    o.registerApp = !a.noRegister;
    o.desktopShortcut = !a.noDesktop;
    int rc = 0;

    if (a.uninstall || !carriesFiles) {
        o.dir = !a.dir.empty() ? a.dir : InstalledDir();
        if (o.dir.empty()) {                 // no uninstall entry: the folder this exe is in
            o.dir = ExePath();
            o.dir.resize(o.dir.find_last_of(L'\\'));
        }
        if (!a.fromTemp && PathInside(ExePath(), o.dir)) return RelaunchFromTemp(a, o.dir) ? 0 : 1;
        if (a.silent) {
            Progress pr;
            std::wstring err;
            rc = Uninstall(o, pr, err) ? 0 : 1;
        } else {
            rc = RunUi(hi, Mode::Uninstall, nullptr, o);
        }
        if (a.fromTemp) DeleteSelfLater();
    } else {
        // One setup at a time.
        HANDLE mutex = CreateMutexW(nullptr, TRUE, L"LyraSetupRunning");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            if (HWND w = FindWindowW(L"LyraSetupWindow", nullptr)) SetForegroundWindow(w);
            return 0;
        }
        o.dir = !a.dir.empty() ? a.dir : InstalledDir();
        if (o.dir.empty()) o.dir = DefaultInstallDir();
        if (a.silent) {
            Progress pr;
            std::wstring err;
            rc = Install(payload, o, pr, err) ? 0 : 1;
            if (rc == 0 && a.update && !a.noRestart) LaunchLyra(o.dir);
        } else {
            rc = RunUi(hi, Mode::Install, &payload, o);
        }
        if (mutex) CloseHandle(mutex);
    }
    Gdiplus::GdiplusShutdown(gdip);
    CoUninitialize();
    return rc;
}
