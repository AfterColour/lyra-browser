// Lyra — загрузки файлов в папку «Загрузки».
#include "browser/Downloads.h"
#include "core/Localization.h"
#include "app/Log.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "core/Constants.h"
#include "core/StringUtils.h"
#include "ui/Dialogs.h"
#include "ui/MainWindow.h"

static std::wstring DownloadsFolder();
static std::wstring SanitizeFileName(std::wstring name);
static std::wstring UniquePath(const std::wstring& dir, const std::wstring& name);
static WKStringRef OnDownloadDecideDestination(WKDownloadRef download, WKURLResponseRef response, WKStringRef suggestedFilename, const void*);
static void OnDownloadFinished(WKDownloadRef download, const void*);
static void OnDownloadFailed(WKDownloadRef download, WKErrorRef error, WKDataRef, const void*);
static void AttachDownloadClient(WKDownloadRef download);
static void CloseTabIfEmptyPopup(const void* ci);

static std::map<WKDownloadRef, std::wstring> g_downloadPaths;   // destination of each running download

// --- downloads (saved straight to the user's Downloads folder) ------------------
static std::wstring DownloadsFolder() {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &p)) && p) dir = p;
    if (p) CoTaskMemFree(p);
    if (dir.empty()) {
        wchar_t buf[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH)) dir = std::wstring(buf) + L"\\Downloads";
    }
    return dir;
}

static std::wstring SanitizeFileName(std::wstring name) {
    for (wchar_t& c : name)
        if (c < 0x20 || wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    while (!name.empty() && (name.back() == L'.' || name.back() == L' ')) name.pop_back();
    while (!name.empty() && name.front() == L' ') name.erase(name.begin());
    if (name.size() > 180) name = name.substr(name.size() - 180);
    return name.empty() ? std::wstring(L"download") : name;
}

// "file.zip" -> "file (1).zip", "file (2).zip", ... until the name is free.
static std::wstring UniquePath(const std::wstring& dir, const std::wstring& name) {
    std::wstring path = dir + L"\\" + name;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return path;
    size_t dot = name.rfind(L'.');
    std::wstring stem = dot == std::wstring::npos || dot == 0 ? name : name.substr(0, dot);
    std::wstring ext  = dot == std::wstring::npos || dot == 0 ? L"" : name.substr(dot);
    for (int i = 1; i < 10000; ++i) {
        path = dir + L"\\" + stem + L" (" + std::to_wstring(i) + L")" + ext;
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return path;
    }
    return dir + L"\\" + stem + L" (" + std::to_wstring(GetTickCount()) + L")" + ext;
}

static WKStringRef OnDownloadDecideDestination(WKDownloadRef download, WKURLResponseRef response,
                                               WKStringRef suggestedFilename, const void*) {
    std::wstring name = WKStr(suggestedFilename);
    if (name.empty() && response) {
        if (WKURLRef u = WKURLResponseCopyURL(response)) {
            if (WKStringRef s = WKURLCopyLastPathComponent(u)) { name = WKStr(s); WKRelease(s); }
            if (name.empty() || name == L"/")
                if (WKStringRef s = WKURLCopyHostName(u)) { name = WKStr(s); WKRelease(s); }
            WKRelease(u);
        }
    }
    std::wstring dir = DownloadsFolder();
    if (dir.empty()) { Log("download: no Downloads folder"); return nullptr; }
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring path = UniquePath(dir, SanitizeFileName(name));
    g_downloadPaths[download] = path;
    Log("download: -> %ls", path.c_str());
    return WKStringCreateWithUTF8CString(ToUtf8(path).c_str());   // +1, owned by WebKit
}

static void OnDownloadFinished(WKDownloadRef download, const void*) {
    std::wstring path;
    auto it = g_downloadPaths.find(download);
    if (it != g_downloadPaths.end()) { path = it->second; g_downloadPaths.erase(it); }
    Log("download finished: %ls", path.c_str());
    if (path.empty() || g_closing || !g_hWnd) return;
    const wchar_t* base = wcsrchr(path.c_str(), L'\\');
    std::wstring msg = std::wstring(Tr(L"Downloaded \x201C", L"Файл \x00AB")) + (base ? base + 1 : path.c_str()) +
                       Tr(L"\x201D to your Downloads folder.\n\nShow it in the folder?",
                          L"\x00BB сохранён в папку \x00ABЗагрузки\x00BB.\n\nПоказать его в папке?");
    int r;
    { ModalScope modal; r = MessageBoxW(g_hWnd, msg.c_str(), kAppName, MB_YESNO | MB_ICONINFORMATION); }
    if (r == IDYES) {
        std::wstring args = L"/select,\"" + path + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
}

static void OnDownloadFailed(WKDownloadRef download, WKErrorRef error, WKDataRef, const void*) {
    g_downloadPaths.erase(download);
    int code = error ? WKErrorGetErrorCode(error) : 0;
    std::wstring text;
    if (error) if (WKStringRef s = WKErrorCopyLocalizedDescription(error)) { text = WKStr(s); WKRelease(s); }
    Log("download failed: code=%d %ls", code, text.c_str());
    if (code == 302 || code == 400 || g_closing || !g_hWnd) return;   // cancelled
    std::wstring msg = std::wstring(Tr(L"The download failed.\n\n", L"Не удалось скачать файл.\n\n")) + text;
    ModalScope modal;
    MessageBoxW(g_hWnd, msg.c_str(), kAppName, MB_OK | MB_ICONWARNING);
}

static void AttachDownloadClient(WKDownloadRef download) {
    WKDownloadClientV0 dc = {};
    dc.base.version = 0;
    dc.base.clientInfo = nullptr;
    dc.decideDestinationWithResponse = OnDownloadDecideDestination;
    dc.didFinish = OnDownloadFinished;
    dc.didFailWithError = OnDownloadFailed;
    WKDownloadSetClient(download, &dc.base);
}

// A link opened in a new tab that turns out to be a file leaves an empty tab behind.
static void CloseTabIfEmptyPopup(const void* ci) {
    Tab* t = ResolveTab(ci, "download");
    if (t && t->openedByPage && (t->url.empty() || t->url == L"about:blank"))
        PostMessageW(g_hWnd, WM_APP_CLOSE_TAB, (WPARAM)t->id, 0);
}

void OnActionBecameDownload(WKPageRef, WKNavigationActionRef, WKDownloadRef d, const void* ci) {
    AttachDownloadClient(d);
    CloseTabIfEmptyPopup(ci);
}

void OnResponseBecameDownload(WKPageRef, WKNavigationResponseRef, WKDownloadRef d, const void* ci) {
    AttachDownloadClient(d);
    CloseTabIfEmptyPopup(ci);
}

void OnContextMenuDownload(WKPageRef, WKDownloadRef d, const void*) {
    AttachDownloadClient(d);
}
