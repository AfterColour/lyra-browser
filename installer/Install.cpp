// Installing and removing Lyra: files, shortcuts, registry, closing a running Lyra.
#include "Setup.h"
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <algorithm>
#include <set>

namespace {
const wchar_t kUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Lyra";
const wchar_t kAppPathsKey[]  = L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\Lyra.exe";
const wchar_t kClientKey[]    = L"Software\\Clients\\StartMenuInternet\\Lyra";
const wchar_t kCapabilities[] = L"Software\\Clients\\StartMenuInternet\\Lyra\\Capabilities";
const wchar_t kManifest[]     = L"install.files";     // the files of the last install, one per line
const wchar_t* const kUrlSchemes[] = { L"http", L"https" };
const wchar_t* const kFileTypes[]  = { L".htm", L".html", L".shtml", L".xht", L".xhtml", L".svg", L".webp" };

std::wstring Lower(std::wstring s) {
    CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p))) s = p;
    CoTaskMemFree(p);
    return s;
}

bool SetStr(const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    HKEY h;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS)
        return false;
    const LSTATUS s = RegSetValueExW(h, name, 0, REG_SZ, (const BYTE*)value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return s == ERROR_SUCCESS;
}

bool SetDword(const std::wstring& key, const wchar_t* name, DWORD value) {
    HKEY h;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS)
        return false;
    const LSTATUS s = RegSetValueExW(h, name, 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
    RegCloseKey(h);
    return s == ERROR_SUCCESS;
}

std::wstring GetStr(const wchar_t* key, const wchar_t* name) {
    wchar_t buf[2048];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    return buf;
}

void DeleteValue(const std::wstring& key, const wchar_t* name) {
    HKEY h;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_SET_VALUE, &h) == ERROR_SUCCESS) {
        RegDeleteValueW(h, name);
        RegCloseKey(h);
    }
}

std::wstring Quote(const std::wstring& s) { return L"\"" + s + L"\""; }

bool CreateShortcut(const std::wstring& lnk, const std::wstring& target, const std::wstring& dir) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return false;
    link->SetPath(target.c_str());
    link->SetWorkingDirectory(dir.c_str());
    link->SetIconLocation(target.c_str(), 0);
    link->SetDescription(L"Lyra");
    IPersistFile* file = nullptr;
    bool ok = SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)));
    if (ok) {
        ok = SUCCEEDED(file->Save(lnk.c_str(), TRUE));
        file->Release();
    }
    link->Release();
    return ok;
}

std::wstring StartMenuShortcut() { return KnownFolder(FOLDERID_Programs) + L"\\Lyra.lnk"; }
std::wstring DesktopShortcut()   { return KnownFolder(FOLDERID_Desktop) + L"\\Lyra.lnk"; }

// Lyra in Windows: Settings > Apps (uninstall entry), Default apps (a browser for http/https and
// web pages), Run / start "lyra" (App Paths).
void Register(const Options& o, const std::wstring& version, uint64_t bytes) {
    const std::wstring exe = o.dir + L"\\Lyra.exe", icon = exe + L",0";
    const std::wstring uninst = Quote(o.dir + L"\\Uninstall.exe") + L" /uninstall";
    const std::wstring u = kUninstallKey;
    SetStr(u, L"DisplayName", L"Lyra");
    SetStr(u, L"DisplayVersion", version);
    SetStr(u, L"DisplayIcon", icon);
    SetStr(u, L"Publisher", L"AfterColour");
    SetStr(u, L"InstallLocation", o.dir);
    SetStr(u, L"UninstallString", uninst);
    SetStr(u, L"QuietUninstallString", uninst + L" /S");
    SetStr(u, L"URLInfoAbout", L"https://github.com/AfterColour/lyra-browser");
    SetDword(u, L"EstimatedSize", (DWORD)(bytes / 1024));
    SetDword(u, L"NoModify", 1);
    SetDword(u, L"NoRepair", 1);
    wchar_t date[16];
    SYSTEMTIME st;
    GetLocalTime(&st);
    wsprintfW(date, L"%04u%02u%02u", st.wYear, st.wMonth, st.wDay);
    SetStr(u, L"InstallDate", date);

    SetStr(kAppPathsKey, nullptr, exe);
    SetStr(kAppPathsKey, L"Path", o.dir);

    const std::wstring open = Quote(exe) + L" " + Quote(L"%1");
    for (const wchar_t* progId : { L"LyraHTML", L"LyraURL" }) {
        const std::wstring k = std::wstring(L"Software\\Classes\\") + progId;
        SetStr(k, nullptr, progId == std::wstring(L"LyraURL") ? L"Lyra URL" : L"Lyra HTML Document");
        SetStr(k + L"\\DefaultIcon", nullptr, icon);
        SetStr(k + L"\\shell\\open\\command", nullptr, open);
        SetStr(k + L"\\Application", L"ApplicationName", L"Lyra");
        SetStr(k + L"\\Application", L"ApplicationIcon", icon);
        if (progId == std::wstring(L"LyraURL")) SetStr(k, L"URL Protocol", L"");
    }
    SetStr(kClientKey, nullptr, L"Lyra");
    SetStr(std::wstring(kClientKey) + L"\\DefaultIcon", nullptr, icon);
    SetStr(std::wstring(kClientKey) + L"\\shell\\open\\command", nullptr, Quote(exe));
    const std::wstring caps = kCapabilities;
    SetStr(caps, L"ApplicationName", L"Lyra");
    SetStr(caps, L"ApplicationIcon", icon);
    SetStr(caps, L"ApplicationDescription", Tr(L"A fast, quiet web browser.", L"Быстрый и спокойный браузер."));
    SetStr(caps + L"\\StartMenu", L"StartMenuInternet", L"Lyra");
    for (const wchar_t* s : kUrlSchemes) SetStr(caps + L"\\URLAssociations", s, L"LyraURL");
    for (const wchar_t* t : kFileTypes) {
        SetStr(caps + L"\\FileAssociations", t, L"LyraHTML");
        SetStr(std::wstring(L"Software\\Classes\\") + t + L"\\OpenWithProgids", L"LyraHTML", L"");
    }
    SetStr(L"Software\\RegisteredApplications", L"Lyra", caps);

    CreateShortcut(StartMenuShortcut(), exe, o.dir);
    if (o.desktopShortcut) CreateShortcut(DesktopShortcut(), exe, o.dir);
    else DeleteFileW(DesktopShortcut().c_str());
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

void Unregister() {
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
    RegDeleteTreeW(HKEY_CURRENT_USER, kAppPathsKey);
    RegDeleteTreeW(HKEY_CURRENT_USER, kClientKey);
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\LyraHTML");
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\LyraURL");
    DeleteValue(L"Software\\RegisteredApplications", L"Lyra");
    for (const wchar_t* t : kFileTypes)
        DeleteValue(std::wstring(L"Software\\Classes\\") + t + L"\\OpenWithProgids", L"LyraHTML");
    DeleteFileW(StartMenuShortcut().c_str());
    DeleteFileW(DesktopShortcut().c_str());
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

std::vector<std::wstring> ReadManifest(const std::wstring& dir) {
    std::vector<std::wstring> out;
    HANDLE h = CreateFileW((dir + L"\\" + kManifest).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    std::string bytes(GetFileSize(h, nullptr), '\0');
    DWORD got = 0;
    ReadFile(h, bytes.data(), (DWORD)bytes.size(), &got, nullptr);
    CloseHandle(h);
    size_t start = 0;
    while (start < bytes.size()) {
        size_t end = bytes.find('\n', start);
        if (end == std::string::npos) end = bytes.size();
        std::string line = bytes.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) {
            std::wstring w(MultiByteToWideChar(CP_UTF8, 0, line.data(), (int)line.size(), nullptr, 0), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, line.data(), (int)line.size(), w.data(), (int)w.size());
            if (w.find(L"..") == std::wstring::npos && w.find(L':') == std::wstring::npos) out.push_back(w);
        }
        start = end + 1;
    }
    return out;
}

void WriteManifest(const std::wstring& dir, const std::vector<std::wstring>& files) {
    std::string bytes;
    for (const auto& f : files) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, f.c_str(), (int)f.size(), nullptr, 0, nullptr, nullptr);
        std::string s(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, f.c_str(), (int)f.size(), s.data(), n, nullptr, nullptr);
        bytes += s + "\r\n";
    }
    HANDLE h = CreateFileW((dir + L"\\" + kManifest).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, bytes.data(), (DWORD)bytes.size(), &w, nullptr);
    CloseHandle(h);
}

// Removes the folders of `files` that are empty now, deepest first (never `dir` itself here).
void RemoveEmptyDirs(const std::wstring& dir, const std::vector<std::wstring>& files) {
    std::set<std::wstring> dirs;
    for (const auto& f : files)
        for (size_t cut = f.find_last_of(L'\\'); cut != std::wstring::npos && cut > 0; cut = f.find_last_of(L'\\', cut - 1))
            dirs.insert(f.substr(0, cut));
    std::vector<std::wstring> sorted(dirs.begin(), dirs.end());
    std::sort(sorted.begin(), sorted.end(), [](const std::wstring& a, const std::wstring& b) { return a.size() > b.size(); });
    for (const auto& d : sorted) RemoveDirectoryW((dir + L"\\" + d).c_str());
}

// Old copies moved aside because they were in use (see PayloadExtract).
void RemoveLeftovers(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.old").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
    while (FindNextFileW(h, &fd));
    FindClose(h);
}

bool DeleteTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring p = dir + L"\\" + name;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) DeleteTree(p);
            else {
                SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(p.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(dir.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
}

struct Proc { DWORD pid; std::wstring name; };
std::vector<Proc> ProcessesIn(const std::wstring& dir) {
    std::vector<Proc> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe = { sizeof(pe) };
    const DWORD self = GetCurrentProcessId();
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == self || pe.th32ProcessID == 0) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;
        wchar_t path[MAX_PATH * 2];
        DWORD n = MAX_PATH * 2;
        if (QueryFullProcessImageNameW(h, 0, path, &n) && PathInside(path, dir)) out.push_back({ pe.th32ProcessID, pe.szExeFile });
        CloseHandle(h);
    }
    CloseHandle(snap);
    return out;
}
}  // namespace

std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    return buf;
}

std::wstring DefaultInstallDir() { return KnownFolder(FOLDERID_UserProgramFiles) + L"\\Lyra"; }
std::wstring InstalledDir() { return GetStr(kUninstallKey, L"InstallLocation"); }
std::wstring InstalledVersion() { return GetStr(kUninstallKey, L"DisplayVersion"); }
std::wstring UserDataDir() { return KnownFolder(FOLDERID_LocalAppData) + L"\\Lyra"; }

bool PathInside(const std::wstring& path, const std::wstring& dir) {
    if (dir.empty()) return false;
    std::wstring d = Lower(dir), p = Lower(path);
    if (d.back() != L'\\') d += L'\\';
    return p.compare(0, d.size(), d) == 0;
}

bool LyraRunning(const std::wstring& dir) { return !ProcessesIn(dir).empty(); }

bool CloseLyra(const std::wstring& dir, DWORD timeoutMs) {
    auto procs = ProcessesIn(dir);
    if (procs.empty()) return true;
    // Ask the browser window to close: Lyra saves its tabs and stops its helper processes.
    for (const auto& p : procs) {
        if (_wcsicmp(p.name.c_str(), L"Lyra.exe") != 0) continue;
        struct Ctx { DWORD pid; } ctx{ p.pid };
        EnumWindows([](HWND w, LPARAM l) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            wchar_t cls[64];
            if (pid == ((Ctx*)l)->pid && GetClassNameW(w, cls, 64) && wcscmp(cls, L"LyraWindowClass") == 0)
                PostMessageW(w, WM_CLOSE, 0, 0);
            return TRUE;
        }, (LPARAM)&ctx);
    }
    const ULONGLONG until = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < until) {
        if (ProcessesIn(dir).empty()) return true;
        Sleep(150);
    }
    // Still running (a page dialog left open, a hung helper): end them.
    for (const auto& p : ProcessesIn(dir)) {
        if (HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, p.pid)) {
            TerminateProcess(h, 1);
            WaitForSingleObject(h, 3000);
            CloseHandle(h);
        }
    }
    return ProcessesIn(dir).empty();
}

bool Install(const Payload& p, const Options& o, Progress& pr, std::wstring& err) {
    pr.total = p.totalSize ? p.totalSize : 1;
    pr.done = 0;
    if (!CloseLyra(o.dir, 8000)) {
        err = Tr(L"Lyra could not be closed. Close it and try again.", L"Не удалось закрыть Lyra. Закройте её и попробуйте снова.");
        return false;
    }
    if (!CreateDirectoryW(o.dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        if (SHCreateDirectoryExW(nullptr, o.dir.c_str(), nullptr) != ERROR_SUCCESS) {
            err = Tr(L"Cannot create the folder ", L"Не удаётся создать папку ") + o.dir;
            return false;
        }
    }
    const std::vector<std::wstring> before = ReadManifest(o.dir);
    std::vector<std::wstring> now;
    for (const auto& f : p.files) {
        if (!PayloadExtract(p, f, o.dir + L"\\" + f.path, err)) return false;
        now.push_back(f.path);
        pr.done += f.size;
    }
    // Files of the previous version that this one no longer has.
    std::set<std::wstring> keep;
    for (const auto& f : now) keep.insert(Lower(f));
    std::vector<std::wstring> gone;
    for (const auto& f : before)
        if (!keep.count(Lower(f))) {
            DeleteFileW((o.dir + L"\\" + f).c_str());
            gone.push_back(f);
        }
    RemoveEmptyDirs(o.dir, gone);
    RemoveLeftovers(o.dir);
    WriteManifest(o.dir, now);
    if (o.registerApp) Register(o, p.version, p.totalSize);
    BuildMediaCache(o.dir);
    pr.done = pr.total.load();
    return true;
}

// GStreamer's plugin cache, built now: after an update every plugin file is new, and without
// this the first page with video (YouTube) scanned all of them itself and froze meanwhile.
// Lyra.exe --gst-registry does it and exits; a failure only means the page does it as before.
void BuildMediaCache(const std::wstring& dir) {
    const std::wstring exe = dir + L"\\Lyra.exe";
    std::wstring cmd = L"\"" + exe + L"\" --gst-registry";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi))
        return;
    if (WaitForSingleObject(pi.hProcess, 180000) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

bool Uninstall(const Options& o, Progress& pr, std::wstring& err) {
    if (!CloseLyra(o.dir, 8000)) {
        err = Tr(L"Lyra could not be closed. Close it and try again.", L"Не удалось закрыть Lyra. Закройте её и попробуйте снова.");
        return false;
    }
    const std::vector<std::wstring> files = ReadManifest(o.dir);
    pr.total = files.size() + 2;
    pr.done = 0;
    for (const auto& f : files) {
        DeleteFileW((o.dir + L"\\" + f).c_str());
        pr.done++;
    }
    RemoveEmptyDirs(o.dir, files);
    RemoveLeftovers(o.dir);
    DeleteFileW((o.dir + L"\\" + kManifest).c_str());
    RemoveDirectoryW(o.dir.c_str());            // only if nothing else is left in it
    if (o.registerApp) Unregister();
    pr.done++;
    if (o.removeData) DeleteTree(UserDataDir());
    pr.done = pr.total.load();
    return true;
}

void LaunchLyra(const std::wstring& dir) {
    ShellExecuteW(nullptr, L"open", (dir + L"\\Lyra.exe").c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
}

void OpenDefaultAppsSettings() {
    ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps?registeredAppUser=Lyra", nullptr, nullptr, SW_SHOWNORMAL);
}
