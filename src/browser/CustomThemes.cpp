// Lyra — свои темы: цвета панелей, акцент и обои из тем с themes.vivaldi.net.
#include "browser/CustomThemes.h"
#include "app/Http.h"
#include "app/Log.h"
#include "browser/StartPage.h"
#include "browser/Wallpaper.h"
#include "core/Constants.h"
#include "core/Localization.h"
#include "core/Settings.h"
#include "core/StringUtils.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/settings/SettingsWindow.h"
#include <shobjidl.h>
#include <atomic>
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;
extern HWND g_setHwnd;

namespace {
std::vector<CustomTheme> s_themes;
bool s_loaded = false;
std::atomic<bool> s_busy{false};

std::wstring Root() { return g_settingsDir + L"\\Themes"; }

std::string ReadFileUtf8(const std::wstring& path) {
    std::string data;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return data;
    LARGE_INTEGER size = {};
    GetFileSizeEx(f, &size);
    if (size.QuadPart > 0 && size.QuadPart < (1 << 20)) {
        data.resize((size_t)size.QuadPart);
        DWORD read = 0;
        ReadFile(f, data.data(), (DWORD)data.size(), &read, nullptr);
        data.resize(read);
    }
    CloseHandle(f);
    return data;
}

// settings.json is a flat object: "key": "text" | number | true / false.
std::string Raw(const std::string& j, const char* key) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = j.find(k);
    if (p == std::string::npos) return {};
    p = j.find(':', p + k.size());
    if (p == std::string::npos) return {};
    ++p;
    while (p < j.size() && (j[p] == ' ' || j[p] == '\t' || j[p] == '\r' || j[p] == '\n')) ++p;
    if (p >= j.size()) return {};
    if (j[p] == '"') {
        std::string out;
        for (++p; p < j.size() && j[p] != '"'; ++p) {
            if (j[p] == '\\' && p + 1 < j.size()) {
                ++p;
                if (j[p] == 'u' && p + 4 < j.size()) {
                    const unsigned cp = (unsigned)strtoul(j.substr(p + 1, 4).c_str(), nullptr, 16);
                    p += 4;
                    if (cp < 0x80) out += (char)cp;
                    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 63)); }
                    else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 63)); out += (char)(0x80 | (cp & 63)); }
                    continue;
                }
            }
            out += j[p];
        }
        return out;
    }
    size_t e = p;
    while (e < j.size() && j[e] != ',' && j[e] != '}' && j[e] != '\n' && j[e] != '\r') ++e;
    std::string v = j.substr(p, e - p);
    while (!v.empty() && v.back() == ' ') v.pop_back();
    return v;
}

bool ParseColor(const std::string& s, Color& c) {
    if (s.size() < 4 || s[0] != '#') return false;
    std::string h = s.substr(1);
    if (h.size() == 3) h = { h[0], h[0], h[1], h[1], h[2], h[2] };
    if (h.size() != 6 || h.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return false;
    const unsigned v = (unsigned)strtoul(h.c_str(), nullptr, 16);
    c = Color((BYTE)(v >> 16), (BYTE)(v >> 8), (BYTE)v);
    return true;
}

float Luminance(const Color& c) { return (0.2126f * c.GetR() + 0.7152f * c.GetG() + 0.0722f * c.GetB()) / 255.f; }

std::wstring OptionsFile(const std::wstring& dir) { return dir + L"\\lyra.ini"; }

// Reads the theme in `dir`; false if it is not a usable theme.
bool Parse(const std::wstring& dir, CustomTheme& t) {
    const std::string j = ReadFileUtf8(dir + L"\\settings.json");
    if (j.empty()) return false;
    t = CustomTheme();
    if (!ParseColor(Raw(j, "colorBg"), t.bg) || !ParseColor(Raw(j, "colorFg"), t.fg)) return false;
    if (!ParseColor(Raw(j, "colorHighlightBg"), t.highlight)) t.highlight = t.fg;
    if (!ParseColor(Raw(j, "colorAccentBg"), t.accent)) t.accent = t.bg;
    t.hasWindow = ParseColor(Raw(j, "colorWindowBg"), t.window);
    t.accentOnWindow = Raw(j, "accentOnWindow") == "true";
    t.dark = Luminance(t.bg) < 0.5f;
    t.name = ToWide(Raw(j, "name"));
    if (t.name.empty()) t.name = Tr(L"Theme", L"Тема");
    std::wstring pic = ToWide(Raw(j, "backgroundImage"));
    if (!pic.empty() && pic.find_first_of(L"\\/:") == std::wstring::npos &&
        GetFileAttributesW((dir + L"\\" + pic).c_str()) != INVALID_FILE_ATTRIBUTES)
        t.picture = pic;
    t.dir = dir;
    t.id = dir.substr(dir.find_last_of(L'\\') + 1);
    const std::wstring ini = OptionsFile(dir);
    t.useColors = GetPrivateProfileIntW(L"Lyra", L"Colors", 1, ini.c_str()) != 0;
    t.useAccent = GetPrivateProfileIntW(L"Lyra", L"Accent", 1, ini.c_str()) != 0;
    t.useWallpaper = GetPrivateProfileIntW(L"Lyra", L"Wallpaper", 1, ini.c_str()) != 0;
    return true;
}

long long Created(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return 0;
    return (long long)(((ULONGLONG)a.ftCreationTime.dwHighDateTime << 32) | a.ftCreationTime.dwLowDateTime);
}

void Load() {
    if (s_loaded) return;
    s_loaded = true;
    s_themes.clear();
    // The first version kept a single theme in Lyra\Theme (on: VivaldiTheme=1).
    const std::wstring old = g_settingsDir + L"\\Theme";
    if (GetFileAttributesW(old.c_str()) != INVALID_FILE_ATTRIBUTES) {
        CreateDirectoryW(Root().c_str(), nullptr);
        const std::wstring to = Root() + L"\\theme-1";
        if (MoveFileExW(old.c_str(), to.c_str(), 0) &&
            GetPrivateProfileIntW(L"Browser", L"VivaldiTheme", 0, g_settingsFile.c_str()))
            g_activeTheme = L"theme-1";
        WritePrivateProfileStringW(L"Browser", L"VivaldiTheme", nullptr, g_settingsFile.c_str());
        for (const wchar_t* ext : { L"png", L"jpg", L"jpeg", L"webp", L"gif", L"bmp" })   // its copy among the wallpapers
            DeleteFileW((g_settingsDir + L"\\Wallpapers\\theme." + ext).c_str());
        SaveSettings();
    }
    std::vector<std::pair<long long, CustomTheme>> found;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((Root() + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            const std::wstring dir = Root() + L"\\" + fd.cFileName;
            CustomTheme t;
            if (Parse(dir, t)) found.push_back({ Created(dir), t });
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& f : found) s_themes.push_back(std::move(f.second));
}

CustomTheme* Find(const std::wstring& id) {
    Load();
    for (auto& t : s_themes) if (t.id == id) return &t;
    return nullptr;
}

// A folder name from the theme's own id (a UUID on themes.vivaldi.net): the same theme installed
// again replaces itself.
std::wstring FolderId(const std::string& themeId) {
    std::wstring id;
    for (char c : themeId) if (isalnum((unsigned char)c) || c == '-') id += (wchar_t)c;
    if (id.size() > 48) id.resize(48);
    if (id.empty()) id = L"theme-" + std::to_wstring(GetTickCount64());
    return id;
}

// Windows 10 1803 and later have tar.exe, which also reads .zip files.
bool Unzip(const std::wstring& zip, const std::wstring& dir, std::wstring& err) {
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring tar = std::wstring(sys) + L"\\tar.exe";
    if (GetFileAttributesW(tar.c_str()) == INVALID_FILE_ATTRIBUTES) { err = L"tar.exe is missing"; return false; }
    std::wstring cmd = L"\"" + tar + L"\" -xf \"" + zip + L"\" -C \"" + dir + L"\"";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        err = L"cannot run tar.exe";
        return false;
    }
    const bool done = WaitForSingleObject(pi.hProcess, 20000) == WAIT_OBJECT_0;
    DWORD code = 1;
    if (done) GetExitCodeProcess(pi.hProcess, &code); else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (!done || code != 0) { err = Tr(L"not a valid .zip", L"это не .zip-архив"); return false; }
    return true;
}

void ApplyNow() {
    ApplyTheme(ResolveDark());
    RefreshStartPages();
    SettingsRefresh();
}

bool IsThemeWallpaper(const std::wstring& id) {
    return StartWallpaperKind() == "theme" && ToWide(StartWallpaperId()) == id;
}

bool Install(const std::wstring& zip, std::wstring& err) {
    Load();
    const std::wstring tmp = g_settingsDir + L"\\Themes.new";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    CreateDirectoryW(tmp.c_str(), nullptr);
    if (!Unzip(zip, tmp, err)) { fs::remove_all(tmp, ec); return false; }
    CustomTheme probe;
    if (!Parse(tmp, probe)) {
        err = Tr(L"there is no theme in this file", L"в этом файле нет темы");
        fs::remove_all(tmp, ec);
        return false;
    }
    const std::wstring id = FolderId(Raw(ReadFileUtf8(tmp + L"\\settings.json"), "id"));
    const std::wstring dir = Root() + L"\\" + id;
    CreateDirectoryW(Root().c_str(), nullptr);
    const std::wstring keepIni = ReadFileUtf8(OptionsFile(dir)).empty() ? L"" : OptionsFile(dir);
    if (!keepIni.empty()) CopyFileW(keepIni.c_str(), OptionsFile(tmp).c_str(), FALSE);   // reinstalled: keep its choices
    fs::remove_all(dir, ec);
    if (!MoveFileExW(tmp.c_str(), dir.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        err = L"cannot write " + dir;
        fs::remove_all(tmp, ec);
        return false;
    }
    s_loaded = false;
    Load();
    const CustomTheme* t = Find(id);
    Log("theme: installed \"%ls\" (%ls)%ls", t ? t->name.c_str() : L"?", id.c_str(), (t && !t->picture.empty()) ? L" with a picture" : L"");
    g_activeTheme.clear();                  // so that Activate also puts its picture on the start page
    CustomThemeActivate(id);
    return true;
}

void ShowError(const std::wstring& err) {
    const std::wstring msg = std::wstring(Tr(L"The theme could not be installed: ", L"Не удалось установить тему: ")) + err;
    MessageBoxW(g_setHwnd ? g_setHwnd : g_hWnd, msg.c_str(), kAppName, MB_OK | MB_ICONWARNING);
}
}  // namespace

const std::vector<CustomTheme>& CustomThemes() { Load(); return s_themes; }

const CustomTheme* CustomThemeActive() { return g_activeTheme.empty() ? nullptr : Find(g_activeTheme); }
bool CustomThemeColorsOn() { const CustomTheme* t = CustomThemeActive(); return t && t->useColors; }
bool CustomThemeAccentOn() { const CustomTheme* t = CustomThemeActive(); return t && t->useAccent; }

void CustomThemeActivate(const std::wstring& id) {
    Load();
    const CustomTheme* next = id.empty() ? nullptr : Find(id);
    const std::wstring prev = g_activeTheme;
    if (prev == (next ? next->id : L"")) { ApplyNow(); return; }
    if (const CustomTheme* old = Find(prev))
        if (old->useWallpaper && IsThemeWallpaper(old->id) && !(next && next->useWallpaper && !next->picture.empty()))
            StartWallpaperSet("builtin", "night");
    g_activeTheme = next ? next->id : L"";
    SaveSettings();
    if (next && next->useWallpaper && !next->picture.empty()) StartWallpaperSet("theme", ToUtf8(next->id));
    ApplyNow();
}

void CustomThemeSetOption(const std::wstring& id, CustomThemeOption what, bool on) {
    CustomTheme* t = Find(id);
    if (!t) return;
    const wchar_t* key = what == kThemeColors ? L"Colors" : what == kThemeAccent ? L"Accent" : L"Wallpaper";
    (what == kThemeColors ? t->useColors : what == kThemeAccent ? t->useAccent : t->useWallpaper) = on;
    WritePrivateProfileStringW(L"Lyra", key, on ? L"1" : L"0", OptionsFile(t->dir).c_str());
    if (what == kThemeWallpaper && g_activeTheme == id && !t->picture.empty()) {
        if (on) StartWallpaperSet("theme", ToUtf8(id));
        else if (IsThemeWallpaper(id)) StartWallpaperSet("builtin", "night");
    }
    ApplyNow();
}

void CustomThemeDelete(const std::wstring& id) {
    const CustomTheme* t = Find(id);
    if (!t) return;
    const std::wstring dir = t->dir;
    if (IsThemeWallpaper(id)) StartWallpaperSet("builtin", "night");
    if (g_activeTheme == id) { g_activeTheme.clear(); SaveSettings(); }
    std::error_code ec;
    fs::remove_all(dir, ec);
    Log("theme: deleted %ls", id.c_str());
    s_loaded = false;
    Load();
    ApplyNow();
}

std::wstring CustomThemeWallpaperParams() {
    Load();
    std::wstring list;
    for (const CustomTheme& t : s_themes) {
        if (t.picture.empty()) continue;
        WIN32_FILE_ATTRIBUTE_DATA a = {};
        GetFileAttributesExW((t.dir + L"\\" + t.picture).c_str(), GetFileExInfoStandard, &a);
        const long long v = (long long)(((ULONGLONG)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime) / 10000000LL;
        if (!list.empty()) list += L",";
        list += L"{\"id\":" + JsString(t.id) + L",\"f\":" + JsString(t.picture) + L",\"n\":" + JsString(t.name) +
                L",\"v\":" + std::to_wstring(v) + L"}";
    }
    if (list.empty()) return L"";
    wchar_t url[2084] = {};
    DWORD len = ARRAYSIZE(url);
    const std::wstring root = Root() + L"\\";
    if (FAILED(UrlCreateFromPathW(root.c_str(), url, &len, 0))) return L"";
    return L"&thdir=" + ToWide(UrlEncode(ToUtf8(url))) + L"&themes=" + ToWide(UrlEncode(ToUtf8(L"[" + list + L"]")));
}

bool CustomThemePickFile() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    const COMDLG_FILTERSPEC types[] = { { Tr(L"Theme", L"Тема"), L"*.zip" } };
    dlg->SetFileTypes(1, types);
    dlg->SetTitle(Tr(L"Install a theme", L"Установить тему"));
    std::wstring path;
    if (SUCCEEDED(dlg->Show(g_setHwnd ? g_setHwnd : g_hWnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) { path = p; CoTaskMemFree(p); }
            item->Release();
        }
    }
    dlg->Release();
    if (path.empty()) return false;
    std::wstring err;
    if (!Install(path, err)) { ShowError(err); return false; }
    return true;
}

bool CustomThemeIsInstallUrl(const std::wstring& url) {
    return _wcsnicmp(url.c_str(), L"https://themes.vivaldi.net/install/", 35) == 0;
}

void CustomThemeDownload(const std::wstring& url) {
    if (s_busy.exchange(true)) return;
    std::thread([url] {
        wchar_t tmp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tmp);
        auto* path = new std::wstring(std::wstring(tmp) + L"lyra-theme.zip");
        std::wstring err;
        const bool ok = HttpFetch(url, nullptr, *path, 0, nullptr, err);
        Log("theme: download %ls %s%ls", url.c_str(), ok ? "done" : "failed: ", ok ? L"" : err.c_str());
        s_busy = false;
        if (!g_hWnd || !PostMessageW(g_hWnd, WM_APP_THEME, ok ? 1 : 0, (LPARAM)path)) delete path;
    }).detach();
}

void CustomThemeDownloaded(WPARAM ok, LPARAM lp) {
    std::unique_ptr<std::wstring> path((std::wstring*)lp);
    std::wstring err;
    if (!ok) err = Tr(L"the download failed", L"не удалось скачать");
    else if (Install(*path, err)) { DeleteFileW(path->c_str()); return; }
    DeleteFileW(path->c_str());
    ShowError(err);
}
