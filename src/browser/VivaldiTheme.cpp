// Lyra — темы Vivaldi (themes.vivaldi.net): цвета панелей и обои стартовой страницы.
#include "browser/VivaldiTheme.h"
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

namespace {
VivaldiTheme s_theme;
bool s_loaded = false;
std::atomic<bool> s_busy{false};

std::wstring ThemeDir() { return g_settingsDir + L"\\Theme"; }

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

// Reads the theme in `dir`; false if it is not a usable Vivaldi theme.
bool Parse(const std::wstring& dir, VivaldiTheme& t) {
    const std::string j = ReadFileUtf8(dir + L"\\settings.json");
    if (j.empty()) return false;
    t = VivaldiTheme();
    if (!ParseColor(Raw(j, "colorBg"), t.bg) || !ParseColor(Raw(j, "colorFg"), t.fg)) return false;
    if (!ParseColor(Raw(j, "colorHighlightBg"), t.highlight)) t.highlight = t.fg;
    if (!ParseColor(Raw(j, "colorAccentBg"), t.accent)) t.accent = t.bg;
    t.hasWindow = ParseColor(Raw(j, "colorWindowBg"), t.window);
    t.accentOnWindow = Raw(j, "accentOnWindow") == "true";
    t.dark = Luminance(t.bg) < 0.5f;
    t.name = ToWide(Raw(j, "name"));
    if (t.name.empty()) t.name = Tr(L"Vivaldi theme", L"Тема Vivaldi");
    std::wstring pic = ToWide(Raw(j, "backgroundImage"));
    if (!pic.empty() && pic.find_first_of(L"\\/:") == std::wstring::npos &&
        GetFileAttributesW((dir + L"\\" + pic).c_str()) != INVALID_FILE_ATTRIBUTES)
        t.picture = pic;
    t.valid = true;
    return true;
}

void Load() {
    if (s_loaded) return;
    s_loaded = true;
    if (!Parse(ThemeDir(), s_theme)) s_theme = VivaldiTheme();
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
    if (!done || code != 0) { err = L"not a valid .zip"; return false; }
    return true;
}

void ApplyNow() {
    ApplyTheme(ResolveDark());
    RefreshStartPages();
    SettingsRefresh();
}

bool Install(const std::wstring& zip, std::wstring& err) {
    const std::wstring tmp = g_settingsDir + L"\\Theme.new";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    CreateDirectoryW(tmp.c_str(), nullptr);
    if (!Unzip(zip, tmp, err)) { fs::remove_all(tmp, ec); return false; }
    VivaldiTheme t;
    if (!Parse(tmp, t)) {
        err = Tr(L"this is not a Vivaldi theme", L"это не тема Vivaldi");
        fs::remove_all(tmp, ec);
        return false;
    }
    fs::remove_all(ThemeDir(), ec);
    if (!MoveFileExW(tmp.c_str(), ThemeDir().c_str(), MOVEFILE_REPLACE_EXISTING)) {
        err = L"cannot write " + ThemeDir();
        fs::remove_all(tmp, ec);
        return false;
    }
    s_theme = t;
    s_loaded = true;
    WallpaperSetThemePicture(t.picture.empty() ? L"" : ThemeDir() + L"\\" + t.picture);
    Log("theme: installed \"%ls\"%ls", t.name.c_str(), t.picture.empty() ? L"" : L" with a picture");
    g_prefs.vivaldiTheme = false;           // so that SetOn(true) also picks the picture
    VivaldiThemeSetOn(true);
    return true;
}

void ShowError(const std::wstring& err) {
    const std::wstring msg = std::wstring(Tr(L"The theme could not be installed: ", L"Не удалось установить тему: ")) + err;
    MessageBoxW(g_hWnd, msg.c_str(), kAppName, MB_OK | MB_ICONWARNING);
}
}  // namespace

const VivaldiTheme& VivaldiThemeGet() { Load(); return s_theme; }

bool VivaldiThemeOn() { return g_prefs.vivaldiTheme && VivaldiThemeGet().valid; }

void VivaldiThemeSetOn(bool on) {
    Load();
    if (on && !s_theme.valid) on = false;
    const bool was = g_prefs.vivaldiTheme;
    g_prefs.vivaldiTheme = on;
    SaveSettings();
    if (on && !was && !s_theme.picture.empty()) StartWallpaperSet("theme");
    if (!on && StartWallpaperKind() == "theme") StartWallpaperSet("builtin", "night");
    ApplyNow();
}

bool VivaldiThemePickFile() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    const COMDLG_FILTERSPEC types[] = { { Tr(L"Vivaldi theme", L"Тема Vivaldi"), L"*.zip" } };
    dlg->SetFileTypes(1, types);
    dlg->SetTitle(Tr(L"Install a Vivaldi theme", L"Установить тему Vivaldi"));
    HWND owner = g_setHwnd ? g_setHwnd : g_hWnd;
    std::wstring path;
    if (SUCCEEDED(dlg->Show(owner))) {
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

bool VivaldiThemeIsInstallUrl(const std::wstring& url) {
    return _wcsnicmp(url.c_str(), L"https://themes.vivaldi.net/install/", 35) == 0;
}

void VivaldiThemeDownload(const std::wstring& url) {
    if (s_busy.exchange(true)) return;
    std::thread([url] {
        wchar_t tmp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tmp);
        auto* path = new std::wstring(std::wstring(tmp) + L"lyra-vivaldi-theme.zip");
        std::wstring err;
        const bool ok = HttpFetch(url, nullptr, *path, 0, nullptr, err);
        Log("theme: download %ls %s%ls", url.c_str(), ok ? "done" : "failed: ", ok ? L"" : err.c_str());
        s_busy = false;
        if (!g_hWnd || !PostMessageW(g_hWnd, WM_APP_THEME, ok ? 1 : 0, (LPARAM)path)) delete path;
    }).detach();
}

void VivaldiThemeDownloaded(WPARAM ok, LPARAM lp) {
    std::unique_ptr<std::wstring> path((std::wstring*)lp);
    std::wstring err;
    if (!ok) err = Tr(L"the download failed", L"не удалось скачать");
    else if (Install(*path, err)) { DeleteFileW(path->c_str()); return; }
    DeleteFileW(path->c_str());
    ShowError(err);
}
