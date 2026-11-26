// Lyra — «Сообщить о проблеме» (см. BugReport.h).
#include "app/BugReport.h"
#include "app/Log.h"
#include "browser/Extensions.h"
#include "browser/TabManager.h"
#include "browser/WebContext.h"
#include "core/Constants.h"
#include "core/Localization.h"
#include "core/Settings.h"
#include "core/StringUtils.h"
#include "ui/MainWindow.h"
#include "LyraVersion.h"

#include <dxgi.h>
#include <shlobj.h>
#include <fstream>
#include <sstream>
#include <vector>

namespace {

constexpr size_t kLogTailBytes = 400 * 1024;   // the end of tabs.log: the last few sessions

std::wstring RegString(HKEY root, const wchar_t* key, const wchar_t* value) {
    wchar_t buf[256] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return L"";
    return buf;
}

DWORD RegDword(HKEY root, const wchar_t* key, const wchar_t* value) {
    DWORD v = 0, size = sizeof(v);
    return RegGetValueW(root, key, value, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS ? v : 0;
}

std::wstring WindowsVersion() {
    const wchar_t* cv = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    const std::wstring build = RegString(HKEY_LOCAL_MACHINE, cv, L"CurrentBuildNumber");
    const DWORD ubr = RegDword(HKEY_LOCAL_MACHINE, cv, L"UBR");
    // ProductName still says "Windows 10" on Windows 11; the build number tells them apart.
    const std::wstring name = _wtoi(build.c_str()) >= 22000 ? L"Windows 11" : L"Windows 10";
    std::wstring s = name + L" " + RegString(HKEY_LOCAL_MACHINE, cv, L"DisplayVersion") + L" (build " + build;
    if (ubr) s += L"." + std::to_wstring(ubr);
    return s + L")";
}

std::wstring Gpus(std::wstring* first) {
    std::wstring out;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) return L"  (DXGI not available)\r\n";
    IDXGIAdapter1* a = nullptr;
    SIZE_T biggest = 0;
    for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d = {};
        a->GetDesc1(&d);
        LARGE_INTEGER drv = {};
        std::wstring driver = L"?";
        if (SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &drv)))
            driver = std::to_wstring(HIWORD(drv.HighPart)) + L"." + std::to_wstring(LOWORD(drv.HighPart)) + L"." +
                     std::to_wstring(HIWORD(drv.LowPart)) + L"." + std::to_wstring(LOWORD(drv.LowPart));
        wchar_t line[400];
        swprintf_s(line, L"  %ls  (vendor %04X, %llu MB, driver %ls)%ls\r\n", d.Description, d.VendorId,
                   (unsigned long long)(d.DedicatedVideoMemory >> 20), driver.c_str(),
                   (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? L" software" : L"");
        if (out.find(line) == std::wstring::npos) out += line;   // (one card can be listed once per output)
        // The issue's line names the card with the most memory of its own (the discrete one in a laptop)
        if (first && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && (first->empty() || d.DedicatedVideoMemory > biggest)) {
            *first = d.Description;
            biggest = d.DedicatedVideoMemory;
        }
        a->Release();
    }
    factory->Release();
    return out;
}

std::wstring Displays(std::wstring* first) {
    std::wstring out;
    DISPLAY_DEVICEW dd = { sizeof(dd) };
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i, dd.cb = sizeof(dd)) {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        DEVMODEW m = { };
        m.dmSize = sizeof(m);
        if (!EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &m)) continue;
        wchar_t line[200];
        swprintf_s(line, L"  %lux%lu @ %lu Hz%ls\r\n", m.dmPelsWidth, m.dmPelsHeight, m.dmDisplayFrequency,
                   (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? L" (main)" : L"");
        out += line;
        if (first && (first->empty() || (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE))) {
            wchar_t s[64];
            swprintf_s(s, L"%lux%lu @ %lu Hz", m.dmPelsWidth, m.dmPelsHeight, m.dmDisplayFrequency);
            *first = s;
        }
    }
    return out;
}

std::wstring FileDate(const std::wstring& path, ULONGLONG* size = nullptr) {
    WIN32_FILE_ATTRIBUTE_DATA a = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return L"none";
    if (size) *size = ((ULONGLONG)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    SYSTEMTIME st;
    FileTimeToSystemTime(&a.ftLastWriteTime, &st);
    wchar_t s[64];
    swprintf_s(s, L"%04u-%02u-%02u %02u:%02u UTC", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return s;
}

std::string ReadTail(const std::wstring& path, size_t bytes) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return "(no log)\n";
    const std::streamoff len = f.tellg();
    const std::streamoff from = len > (std::streamoff)bytes ? len - (std::streamoff)bytes : 0;
    f.seekg(from);
    std::string s((size_t)(len - from), '\0');
    f.read(s.data(), (std::streamsize)s.size());
    if (from > 0) { size_t nl = s.find('\n'); if (nl != std::string::npos) s.erase(0, nl + 1); }   // (from a whole line)
    return s;
}

void ReplaceAll(std::wstring& s, const std::wstring& what, const std::wstring& with) {
    if (what.empty()) return;
    for (size_t at = 0; (at = s.find(what, at)) != std::wstring::npos; at += with.size()) s.replace(at, what.size(), with);
}

// What is not needed to find a bug: the Windows user name (in paths) and what Lyra's own pages
// carry in their addresses (the start page's settings, a weather city).
std::wstring Scrub(std::wstring s) {
    wchar_t profile[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH)) {
        ReplaceAll(s, profile, L"%USERPROFILE%");
        std::wstring fwd = profile;
        for (auto& c : fwd) if (c == L'\\') c = L'/';
        ReplaceAll(s, fwd, L"%USERPROFILE%");
    }
    for (size_t at = 0; (at = s.find(L"lyra://", at)) != std::wstring::npos;) {
        size_t end = s.find_first_of(L" \r\n\"'", at);
        if (end == std::wstring::npos) end = s.size();
        const size_t q = s.find(L'?', at);
        if (q != std::wstring::npos && q < end) { s.replace(q, end - q, L"?\x2026"); end = q + 2; }
        at = end;
    }
    return s;
}

std::wstring Now(const wchar_t* fmt) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t s[64];
    swprintf_s(s, fmt, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return s;
}

struct Summary { std::wstring windows, gpu, display; };

std::wstring Build(Summary& sum) {
    std::wstringstream o;
    o << L"Lyra problem report, " << Now(L"%04u-%02u-%02u %02u:%02u:%02u") << L"\r\n";
    o << L"Nothing in this file was sent anywhere. Read it before you attach it to an issue.\r\n\r\n";

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    sum.windows = WindowsVersion();
    o << L"== Lyra\r\n  version " << LYRA_VERSION_WSTR << L"\r\n  " << exe << L"\r\n";
    o << L"  language: " << (UiRussian() ? L"Russian" : L"English") << L"\r\n\r\n";

    o << L"== Computer\r\n  " << sum.windows << L"\r\n";
    o << L"  " << TrimWs(RegString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString")) << L"\r\n";
    MEMORYSTATUSEX ms = { sizeof(ms) };
    GlobalMemoryStatusEx(&ms);
    o << L"  memory: " << (ms.ullTotalPhys >> 20) << L" MB, " << (ms.ullAvailPhys >> 20) << L" MB free\r\n";
    o << L"  power: " << (OnBatteryPower() ? L"battery" : L"mains") << L"\r\n";
    o << L"  graphics:\r\n" << Gpus(&sum.gpu);
    o << L"  displays:\r\n" << Displays(&sum.display);
    if (g_hWnd) o << L"  window scale: " << (GetDpiForWindow(g_hWnd) * 100 / 96) << L" %\r\n";
    o << L"\r\n";

    ULONGLONG regSize = 0;
    const std::wstring reg = FileDate(g_settingsDir + L"\\gstreamer-registry.bin", &regSize);
    o << L"== Media\r\n  GStreamer plugin cache: " << reg;
    if (regSize) o << L", " << (regSize >> 10) << L" KB";
    o << L"\r\n\r\n";

    o << L"== Tabs\r\n  " << g_tabs.size() << L" open\r\n\r\n";

    o << L"== Extensions\r\n";
    const auto exts = ExtensionsList();
    if (exts.empty()) o << L"  none\r\n";
    for (const auto& e : exts) o << L"  " << e.name << L" " << e.version << (e.enabled ? L"" : L" (off)") << L"\r\n";
    o << L"\r\n";

    o << L"== settings.ini\r\n";
    {
        std::ifstream f(g_settingsFile, std::ios::binary);
        std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::wstring text;
        if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE)   // (UTF-16, as WritePrivateProfileString keeps it)
            text.assign((const wchar_t*)(raw.data() + 2), (raw.size() - 2) / 2);
        else
            text = ToWide(raw);
        o << text << L"\r\n\r\n";
    }

    o << L"== tabs.log (the end; it names the sites that were open)\r\n";
    o << ToWide(ReadTail(g_settingsDir + L"\\tabs.log", kLogTailBytes));
    return Scrub(o.str());
}

}  // namespace

std::wstring BugReportText() {
    Summary s;
    return Build(s);
}

void ReportProblem(HWND owner) {
    const int answer = MessageBoxW(owner,
        Tr(L"Lyra will save a text file with what helps to find the problem: Lyra's version, Windows, the graphics card and "
           L"displays, your settings, extensions and Lyra's log. The log names the sites that were open.\n\n"
           L"Nothing is sent. The file opens in Explorer and a new issue on GitHub opens in a tab: describe what happened "
           L"and drag the file into it.",
           L"Lyra сохранит текстовый файл со сведениями, которые помогают найти ошибку: версия Lyra, Windows, видеокарта и "
           L"мониторы, ваши настройки, расширения и журнал Lyra. В журнале есть названия сайтов, которые были открыты.\n\n"
           L"Ничего не отправляется. Файл откроется в Проводнике, а во вкладке откроется новое обращение на GitHub: опишите, "
           L"что случилось, и перетащите туда файл."),
        Tr(L"Report a problem", L"Сообщить о проблеме"), MB_OKCANCEL | MB_ICONINFORMATION);
    if (answer != IDOK) return;

    Summary sum;
    const std::wstring text = Build(sum);
    const std::wstring dir = g_settingsDir + L"\\Reports";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + L"\\lyra-report-" + Now(L"%04u%02u%02u-%02u%02u%02u") + L".txt";
    const std::string utf8 = "\xEF\xBB\xBF" + ToUtf8(text);       // (BOM: Notepad and GitHub read it as UTF-8)
    {
        std::ofstream f(path, std::ios::binary);
        f.write(utf8.data(), (std::streamsize)utf8.size());
        if (!f) {
            MessageBoxW(owner, (Tr(L"The report could not be saved: ", L"Не удалось сохранить отчёт: ") + path).c_str(),
                        Tr(L"Report a problem", L"Сообщить о проблеме"), MB_OK | MB_ICONWARNING);
            return;
        }
    }
    Log("report: saved %ls (%zu KB)", path.c_str(), utf8.size() / 1024);

    // A new issue, with the system line filled in
    const wchar_t* name = path.c_str() + path.find_last_of(L'\\') + 1;
    std::wstring body =
        Tr(L"**What happened:**\n\n\n**What you did before it (steps):**\n\n\n", L"**Что случилось:**\n\n\n**Что вы делали перед этим (шаги):**\n\n\n");
    body += L"---\nLyra " LYRA_VERSION_WSTR L" \x00B7 " + sum.windows;
    if (!sum.gpu.empty()) body += L" \x00B7 " + sum.gpu;
    if (!sum.display.empty()) body += L" \x00B7 " + sum.display;
    body += L"\n\n";
    body += Tr(L"Report file: drag ", L"Файл отчёта: перетащите ");
    body += name;
    body += Tr(L" from the Explorer window into this box.", L" из окна Проводника в это поле.");
    const std::wstring url = std::wstring(kIssuesUrl) + L"/new?body=" + ToWide(UrlEncode(ToUtf8(body)));
    CreateTab(url);
    if (g_hWnd) SetForegroundWindow(g_hWnd);

    // Then Explorer over it, with the file selected, to drag it into the issue
    if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str())) {
        SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
    }
}
