// Lyra - the start page's layout file and wallpaper pictures (see Wallpaper.h).
#include "browser/Wallpaper.h"
#include "app/Http.h"
#include "app/Log.h"
#include "core/Constants.h"
#include "core/Localization.h"
#include "core/Settings.h"
#include "core/StringUtils.h"
#include "ui/MainWindow.h"
#include <shobjidl.h>
#include <atomic>
#include <thread>

namespace {
std::atomic<bool> s_bingBusy{false};

std::wstring ConfigPath() { return g_settingsDir + L"\\start.json"; }
std::wstring Dir() { return g_settingsDir + L"\\Wallpapers"; }

std::string ReadAll(const std::wstring& path) {
    std::string data;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return data;
    LARGE_INTEGER size = {};
    GetFileSizeEx(f, &size);
    if (size.QuadPart > 0 && size.QuadPart < (16 << 20)) {
        data.resize((size_t)size.QuadPart);
        DWORD read = 0;
        ReadFile(f, data.data(), (DWORD)data.size(), &read, nullptr);
        data.resize(read);
    }
    CloseHandle(f);
    return data;
}

bool WriteAll(const std::wstring& path, const std::string& data) {
    const std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const bool ok = WriteFile(f, data.data(), (DWORD)data.size(), &w, nullptr) && w == data.size();
    CloseHandle(f);
    return ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::wstring FileUrl(const std::wstring& path) {
    wchar_t url[2084] = {};
    DWORD len = ARRAYSIZE(url);
    return SUCCEEDED(UrlCreateFromPathW(path.c_str(), url, &len, 0)) ? std::wstring(url) : std::wstring();
}

std::wstring Today() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[16];
    swprintf(buf, 16, L"%04u-%02u-%02u", st.wYear, st.wMonth, st.wDay);
    return buf;
}

// custom.jpg / custom.png ... (the extension of the picture chosen)
std::wstring CustomName() {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((Dir() + L"\\custom.*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    FindClose(h);
    return fd.cFileName;
}

std::wstring ThemeName() {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((Dir() + L"\\theme.*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    FindClose(h);
    return fd.cFileName;
}

long long WriteTime(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return 0;
    return (long long)(((ULONGLONG)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime) / 10000000LL;
}

std::string JsonField(const std::string& j, const char* key) {
    const std::string k = std::string("\"") + key + "\":\"";
    size_t p = j.find(k);
    if (p == std::string::npos) return {};
    std::string out;
    for (p += k.size(); p < j.size() && j[p] != '"'; ++p) {
        if (j[p] == '\\' && p + 1 < j.size()) {
            ++p;
            if (j[p] == 'u' && p + 4 < j.size()) {           // é -> UTF-8
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
}  // namespace

std::wstring StartConfigJson() { return ToWide(ReadAll(ConfigPath())); }

void StartConfigSave(const std::string& utf8Json) {
    if (utf8Json.size() > (4 << 20)) return;               // a page gone wrong; keep the last good one
    if (!WriteAll(ConfigPath(), utf8Json)) Log("start page: cannot write %ls", ConfigPath().c_str());
}

std::wstring WallpaperParams() {
    std::wstring p = L"&wpdir=" + ToWide(UrlEncode(ToUtf8(FileUrl(Dir() + L"\\"))));
    const std::wstring custom = CustomName();
    if (!custom.empty()) p += L"&custom=" + ToWide(UrlEncode(ToUtf8(custom))) + L"&cv=" + std::to_wstring(WriteTime(Dir() + L"\\" + custom));
    const std::wstring theme = ThemeName();
    if (!theme.empty()) p += L"&themewp=" + ToWide(UrlEncode(ToUtf8(theme))) + L"&tv=" + std::to_wstring(WriteTime(Dir() + L"\\" + theme));
    const std::wstring bing = Dir() + L"\\bing.jpg";
    if (GetFileAttributesW(bing.c_str()) != INVALID_FILE_ATTRIBUTES) {
        const std::string info = ReadAll(Dir() + L"\\bing.txt");            // date \n copyright \n link
        const size_t nl = info.find('\n');
        p += L"&bing=1&bv=" + std::to_wstring(WriteTime(bing));
        if (nl != std::string::npos) {
            std::string rest = info.substr(nl + 1);
            const size_t nl2 = rest.find('\n');
            p += L"&bingInfo=" + ToWide(UrlEncode(rest.substr(0, nl2)));
            if (nl2 != std::string::npos) p += L"&bingLink=" + ToWide(UrlEncode(rest.substr(nl2 + 1)));
        }
    }
    return p;
}

void WallpaperBingEnsure() {
    const std::string cfg = ReadAll(ConfigPath());
    if (cfg.find("\"kind\":\"bing\"") == std::string::npos) return;
    const std::string info = ReadAll(Dir() + L"\\bing.txt");
    if (info.compare(0, 10, ToUtf8(Today())) == 0 && GetFileAttributesW((Dir() + L"\\bing.jpg").c_str()) != INVALID_FILE_ATTRIBUTES) return;
    if (s_bingBusy.exchange(true)) return;
    const bool ru = g_prefs.language == 2 || (g_prefs.language == 0 && PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN);
    std::thread([ru] {
        std::wstring err;
        std::string json;
        const std::wstring api = std::wstring(L"https://www.bing.com/HPImageArchive.aspx?format=js&idx=0&n=1&mkt=") + (ru ? L"ru-RU" : L"en-US");
        bool ok = HttpFetch(api, &json, L"", 0, nullptr, err);
        const std::string url = JsonField(json, "url"), copyright = JsonField(json, "copyright"), link = JsonField(json, "copyrightlink");
        if (ok && !url.empty()) {
            CreateDirectoryW(Dir().c_str(), nullptr);
            const std::wstring part = Dir() + L"\\bing.jpg.part";
            ok = HttpFetch(L"https://www.bing.com" + ToWide(url), nullptr, part, 0, nullptr, err) &&
                 MoveFileExW(part.c_str(), (Dir() + L"\\bing.jpg").c_str(), MOVEFILE_REPLACE_EXISTING);
            if (ok) WriteAll(Dir() + L"\\bing.txt", ToUtf8(Today()) + "\n" + copyright + "\n" + link);
            else DeleteFileW(part.c_str());
        }
        Log("start page: picture of the day %s%ls", ok ? "updated" : "failed: ", ok ? L"" : err.c_str());
        s_bingBusy = false;
        if (ok && g_hWnd) PostMessageW(g_hWnd, WM_APP_WALLPAPER, 0, 0);
    }).detach();
}

void WallpaperSetThemePicture(const std::wstring& src) {
    CreateDirectoryW(Dir().c_str(), nullptr);
    for (std::wstring old = ThemeName(); !old.empty(); old = ThemeName())
        if (!DeleteFileW((Dir() + L"\\" + old).c_str())) break;
    if (src.empty()) return;
    std::wstring ext = src.substr(src.find_last_of(L'.') + 1);
    CharLowerBuffW(ext.data(), (DWORD)ext.size());
    CopyFileW(src.c_str(), (Dir() + L"\\theme." + ext).c_str(), FALSE);
}

std::string StartWallpaperKind() {
    const std::string cfg = ReadAll(ConfigPath());
    const size_t w = cfg.find("\"wallpaper\":{");
    if (w == std::string::npos) return "builtin";
    const std::string obj = cfg.substr(w, cfg.find('}', w) - w);
    const std::string k = JsonField(obj, "kind");
    return k.empty() ? "builtin" : k;
}

void StartWallpaperSet(const std::string& kind, const std::string& id) {
    std::string cfg = ReadAll(ConfigPath());
    int dim = 0;
    const size_t w = cfg.find("\"wallpaper\":{");
    if (w != std::string::npos) {
        const size_t e = cfg.find('}', w);
        const size_t d = cfg.find("\"dim\":", w);
        if (d != std::string::npos && d < e) dim = atoi(cfg.c_str() + d + 6);
    }
    std::string obj = "{\"kind\":\"" + kind + "\"" + (id.empty() ? std::string() : ",\"id\":\"" + id + "\"") +
                      ",\"dim\":" + std::to_string(std::max(0, std::min(70, dim))) + "}";
    if (cfg.empty() || cfg.find('{') == std::string::npos) cfg = "{\"v\":1,\"wallpaper\":" + obj + "}";
    else if (w != std::string::npos) cfg.replace(w, cfg.find('}', w) - w + 1, "\"wallpaper\":" + obj);
    else cfg.insert(cfg.find('{') + 1, "\"wallpaper\":" + obj + ",");
    StartConfigSave(cfg);
}

bool WallpaperPickFile() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    const COMDLG_FILTERSPEC types[] = { { Tr(L"Pictures", L"Изображения"), L"*.jpg;*.jpeg;*.png;*.webp;*.bmp;*.gif" } };
    dlg->SetFileTypes(1, types);
    dlg->SetTitle(Tr(L"Wallpaper for the start page", L"Обои для стартовой страницы"));
    bool picked = false;
    if (SUCCEEDED(dlg->Show(g_hWnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                std::wstring src = path, ext = src.substr(src.find_last_of(L'.') + 1);
                CharLowerBuffW(ext.data(), (DWORD)ext.size());
                CreateDirectoryW(Dir().c_str(), nullptr);
                for (std::wstring old = CustomName(); !old.empty(); old = CustomName())
                    if (!DeleteFileW((Dir() + L"\\" + old).c_str())) break;
                picked = CopyFileW(src.c_str(), (Dir() + L"\\custom." + ext).c_str(), FALSE) != 0;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return picked;
}
