// Lyra — загрузки: сохранение файлов, прогресс, список загрузок, команды «открыть / в папке / отменить».
#include "browser/Downloads.h"
#include "app/Log.h"
#include "browser/History.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "core/Constants.h"
#include "core/Localization.h"
#include "core/Settings.h"
#include "core/StringUtils.h"
#include "ui/Dialogs.h"
#include "ui/MainWindow.h"

#include <shobjidl.h>

void (*g_downloadsChanged)(bool listChanged) = nullptr;

static std::vector<DownloadItem> s_items;                 // newest first
static std::map<WKDownloadRef, long long> s_running;      // WebKit download -> item id
static long long s_nextId = 1;
static bool s_loaded = false, s_listDirty = false, s_progressDirty = false;
static DWORD s_lastNotify = 0, s_finishedAt = 0;
constexpr size_t kMaxItems = 300;

static std::wstring ListPath() { return g_settingsDir + L"\\downloads.tsv"; }

static DownloadItem* Find(long long id) {
    for (auto& d : s_items) if (d.id == id) return &d;
    return nullptr;
}

static DownloadItem* FindByRef(WKDownloadRef ref) {
    auto it = s_running.find(ref);
    return it == s_running.end() ? nullptr : Find(it->second);
}

static void ListChanged() {
    s_listDirty = true;
    if (g_downloadsChanged) g_downloadsChanged(true);
}

// ----------------------------------------------------------------------------
//  The list on disk
// ----------------------------------------------------------------------------
static std::wstring Clean(std::wstring s) {
    for (auto& ch : s) if (ch == L'\t' || ch == L'\r' || ch == L'\n') ch = L' ';
    return s;
}

void DownloadsSave() {
    if (!s_loaded) return;
    std::string out = "LyraDownloads\t1\n";
    for (const DownloadItem& d : s_items) {
        // A download still running when Lyra closes cannot be continued: it is saved as failed.
        const int state = d.state == DownloadItem::Running ? (int)DownloadItem::Failed : (int)d.state;
        out += std::to_string(d.id) + "\t" + std::to_string(state) + "\t" + std::to_string(d.received) + "\t" +
               std::to_string(d.total) + "\t" + std::to_string(d.started) + "\t" + std::to_string(d.finished) + "\t" +
               ToUtf8(Clean(d.url)) + "\t" + ToUtf8(Clean(d.path)) + "\t" + ToUtf8(Clean(d.name)) + "\t" +
               ToUtf8(Clean(d.state == DownloadItem::Running ? std::wstring(L"interrupted") : d.error)) + "\n";
    }
    const std::wstring path = ListPath(), tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    const BOOL ok = WriteFile(f, out.data(), (DWORD)out.size(), &written, nullptr);
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str());
    s_listDirty = false;
}

void DownloadsLoad() {
    if (s_loaded) return;
    s_loaded = true;
    HANDLE f = CreateFileW(ListPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size = {};
    GetFileSizeEx(f, &size);
    std::string data((size_t)std::min<long long>(size.QuadPart, 8LL << 20), '\0');
    DWORD read = 0;
    if (!data.empty()) ReadFile(f, &data[0], (DWORD)data.size(), &read, nullptr);
    CloseHandle(f);
    data.resize(read);
    size_t pos = 0;
    while (pos < data.size()) {
        size_t end = data.find('\n', pos);
        if (end == std::string::npos) end = data.size();
        std::string line = data.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> c;
        for (size_t p = 0;;) {
            size_t t = line.find('\t', p);
            c.push_back(line.substr(p, t == std::string::npos ? std::string::npos : t - p));
            if (t == std::string::npos) break;
            p = t + 1;
        }
        if (c.size() < 10 || c[0] == "LyraDownloads") continue;
        DownloadItem d;
        d.id = _atoi64(c[0].c_str());
        const int st = atoi(c[1].c_str());
        d.state = (st >= 1 && st <= 3) ? (DownloadItem::State)st : DownloadItem::Failed;
        d.received = _atoi64(c[2].c_str());
        d.total = _atoi64(c[3].c_str());
        d.started = _atoi64(c[4].c_str());
        d.finished = _atoi64(c[5].c_str());
        d.url = ToWide(c[6]);
        d.path = ToWide(c[7]);
        d.name = ToWide(c[8]);
        d.error = ToWide(c[9]);
        if (d.id <= 0 || d.name.empty()) continue;
        s_nextId = std::max(s_nextId, d.id + 1);
        s_items.push_back(std::move(d));
        if (s_items.size() >= kMaxItems) break;
    }
}

const std::vector<DownloadItem>& DownloadsList() {
    DownloadsLoad();
    return s_items;
}

const DownloadItem* DownloadGet(long long id) { return Find(id); }

int DownloadsRunning() {
    int n = 0;
    for (auto& d : s_items) if (d.state == DownloadItem::Running) ++n;
    return n;
}

float DownloadsProgress() {
    long long got = 0, total = 0;
    bool any = false;
    for (auto& d : s_items) {
        if (d.state != DownloadItem::Running) continue;
        any = true;
        if (d.total <= 0) return -1.f;
        got += d.received;
        total += d.total;
    }
    if (!any || total <= 0) return -1.f;
    return std::min(1.f, (float)((double)got / (double)total));
}

bool DownloadsJustFinished() { return s_finishedAt && GetTickCount() - s_finishedAt < 4000; }

bool DownloadFileExists(const DownloadItem& d) {
    if (d.path.empty()) return false;
    const DWORD a = GetFileAttributesW(d.path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// ----------------------------------------------------------------------------
//  Where files go
// ----------------------------------------------------------------------------
static std::wstring WindowsDownloadsFolder() {
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

std::wstring DownloadsFolder() {
    if (!g_downloadDir.empty()) {
        const DWORD a = GetFileAttributesW(g_downloadDir.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return g_downloadDir;
    }
    return WindowsDownloadsFolder();
}

void OpenDownloadsFolder() {
    const std::wstring dir = DownloadsFolder();
    if (!dir.empty()) ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool ChooseDownloadsFolder(HWND owner) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool chosen = false;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dlg->SetTitle(Tr(L"Save downloads to", L"Сохранять загрузки в"));
        IShellItem* start = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(DownloadsFolder().c_str(), nullptr, IID_PPV_ARGS(&start)))) {
            dlg->SetFolder(start);
            start->Release();
        }
        ModalScope modal;
        if (SUCCEEDED(dlg->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR p = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                    g_downloadDir = p;
                    if (_wcsicmp(g_downloadDir.c_str(), WindowsDownloadsFolder().c_str()) == 0) g_downloadDir.clear();
                    SaveSettings();
                    chosen = true;
                    CoTaskMemFree(p);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return chosen;
}

static std::wstring SanitizeFileName(std::wstring name) {
    for (wchar_t& c : name)
        if (c < 0x20 || wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    while (!name.empty() && (name.back() == L'.' || name.back() == L' ')) name.pop_back();
    while (!name.empty() && name.front() == L' ') name.erase(name.begin());
    if (name.size() > 180) name = name.substr(name.size() - 180);
    return name.empty() ? std::wstring(L"download") : name;
}

// "file.zip" -> "file (1).zip", "file (2).zip", ... until the name is free (also among the
// downloads still running, whose files may not exist yet).
static std::wstring UniquePath(const std::wstring& dir, const std::wstring& name) {
    auto taken = [](const std::wstring& path) {
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
        for (auto& d : s_items)
            if (d.state == DownloadItem::Running && _wcsicmp(d.path.c_str(), path.c_str()) == 0) return true;
        return false;
    };
    std::wstring path = dir + L"\\" + name;
    if (!taken(path)) return path;
    size_t dot = name.rfind(L'.');
    std::wstring stem = dot == std::wstring::npos || dot == 0 ? name : name.substr(0, dot);
    std::wstring ext  = dot == std::wstring::npos || dot == 0 ? L"" : name.substr(dot);
    for (int i = 1; i < 10000; ++i) {
        path = dir + L"\\" + stem + L" (" + std::to_wstring(i) + L")" + ext;
        if (!taken(path)) return path;
    }
    return dir + L"\\" + stem + L" (" + std::to_wstring(GetTickCount()) + L")" + ext;
}

// Settings > Downloads > "Ask where to save": the Save As dialog. Empty if cancelled.
static std::wstring AskSavePath(const std::wstring& dir, const std::wstring& name) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::wstring result;
    IFileSaveDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);
        dlg->SetFileName(name.c_str());
        const size_t dot = name.rfind(L'.');
        if (dot != std::wstring::npos && dot + 1 < name.size()) {
            const std::wstring ext = name.substr(dot + 1), spec = L"*." + ext;
            const std::wstring label = ext + Tr(L" file", L"-файл");
            COMDLG_FILTERSPEC types[] = { { label.c_str(), spec.c_str() }, { Tr(L"All files", L"Все файлы"), L"*.*" } };
            dlg->SetFileTypes(2, types);
            dlg->SetDefaultExtension(ext.c_str());
        }
        IShellItem* start = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(&start)))) {
            dlg->SetFolder(start);
            start->Release();
        }
        ModalScope modal;
        if (SUCCEEDED(dlg->Show(g_hWnd))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR p = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                    result = p;
                    CoTaskMemFree(p);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    if (SUCCEEDED(co)) CoUninitialize();
    // Overwriting was confirmed in the dialog: the old file goes so WebKit can write the new one.
    if (!result.empty() && GetFileAttributesW(result.c_str()) != INVALID_FILE_ATTRIBUTES) DeleteFileW(result.c_str());
    return result;
}

// ----------------------------------------------------------------------------
//  WebKit download client
// ----------------------------------------------------------------------------
static WKStringRef OnDecideDestination(WKDownloadRef download, WKURLResponseRef response,
                                       WKStringRef suggestedFilename, const void*) {
    DownloadItem* d = FindByRef(download);
    std::wstring name = WKStr(suggestedFilename);
    if (name.empty() && response) {
        if (WKURLRef u = WKURLResponseCopyURL(response)) {
            if (WKStringRef s = WKURLCopyLastPathComponent(u)) { name = WKStr(s); WKRelease(s); }
            if (name.empty() || name == L"/")
                if (WKStringRef s = WKURLCopyHostName(u)) { name = WKStr(s); WKRelease(s); }
            WKRelease(u);
        }
    }
    if (d && response) {
        const uint32_t len = WKURLResponseGetExpectedContentLength(response);
        if (len != 0 && len != 0xFFFFFFFFu) d->total = (long long)len;
    }
    const std::wstring dir = DownloadsFolder();
    if (dir.empty()) { Log("download: no Downloads folder"); return nullptr; }
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring path;
    if (g_prefs.askDownload && !g_closing) {
        path = AskSavePath(dir, SanitizeFileName(name));
        d = FindByRef(download);                         // the list may have changed meanwhile
        if (path.empty()) {                              // cancelled: forget it quietly
            if (d) {
                const long long id = d->id;
                s_items.erase(std::remove_if(s_items.begin(), s_items.end(), [id](const DownloadItem& x) { return x.id == id; }), s_items.end());
            }
            s_running.erase(download);
            ListChanged();
            return nullptr;
        }
    } else {
        path = UniquePath(dir, SanitizeFileName(name));
    }
    if (d) {
        d->path = path;
        const size_t slash = path.find_last_of(L"\\/");
        d->name = slash == std::wstring::npos ? path : path.substr(slash + 1);
        ListChanged();
    }
    Log("download: -> %ls", path.c_str());
    return WKStringCreateWithUTF8CString(ToUtf8(path).c_str());   // +1, owned by WebKit
}

static void OnDidWriteData(WKDownloadRef download, long long written, long long totalWritten, long long expected, const void*) {
    DownloadItem* d = FindByRef(download);
    if (!d) return;
    // The curl network backend reports only the bytes of each piece (total 0): add them up.
    if (totalWritten <= 0) totalWritten = d->received + std::max(0LL, written);
    const long long now = NowUnixMs();
    // Smoothed speed from the bytes since the last update.
    static std::map<long long, std::pair<long long, long long>> last;   // id -> (time, bytes)
    auto& l = last[d->id];
    if (l.first == 0) l = { now, totalWritten };
    else if (now - l.first >= 400) {
        const double inst = (double)(totalWritten - l.second) * 1000.0 / (double)(now - l.first);
        d->speed = d->speed <= 0 ? inst : d->speed * 0.7 + inst * 0.3;
        l = { now, totalWritten };
    }
    d->received = totalWritten;
    if (expected > 0) d->total = expected;
    s_progressDirty = true;
}

static void Finish(WKDownloadRef download, DownloadItem::State state, const std::wstring& error) {
    DownloadItem* d = FindByRef(download);
    s_running.erase(download);
    if (!d) return;
    d->state = state;
    d->error = error;
    d->finished = NowUnixMs();
    d->speed = 0;
    if (state == DownloadItem::Done) {
        if (d->total < d->received) d->total = d->received;
        d->received = std::max(d->received, d->total);
        s_finishedAt = GetTickCount();
        if (!s_finishedAt) s_finishedAt = 1;
    }
    ListChanged();
    DownloadsSave();
}

static void OnDidFinish(WKDownloadRef download, const void*) {
    if (DownloadItem* d = FindByRef(download)) {
        WIN32_FILE_ATTRIBUTE_DATA fa = {};
        if (GetFileAttributesExW(d->path.c_str(), GetFileExInfoStandard, &fa))
            d->received = ((long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
        Log("download finished: %ls", d->path.c_str());
    }
    Finish(download, DownloadItem::Done, L"");
}

static void OnDidFail(WKDownloadRef download, WKErrorRef error, WKDataRef, const void*) {
    const int code = error ? WKErrorGetErrorCode(error) : 0;
    std::wstring text;
    if (error) if (WKStringRef s = WKErrorCopyLocalizedDescription(error)) { text = WKStr(s); WKRelease(s); }
    Log("download failed: code=%d %ls", code, text.c_str());
    DownloadItem* d = FindByRef(download);
    const bool cancelled = code == 302 || code == 400 || (d && d->state == DownloadItem::Cancelled);
    if (d && !cancelled && !d->path.empty()) DeleteFileW(d->path.c_str());     // a partial file is no use
    Finish(download, cancelled ? DownloadItem::Cancelled : DownloadItem::Failed, cancelled ? L"" : text);
}

static void Track(WKDownloadRef download) {
    DownloadsLoad();
    if (s_running.count(download)) return;
    DownloadItem d;
    d.id = s_nextId++;
    d.started = NowUnixMs();
    d.name = Tr(L"Starting\x2026", L"Начинается\x2026");
    if (WKURLRequestRef req = WKDownloadCopyRequest(download)) {
        if (WKURLRef u = WKURLRequestCopyURL(req)) {
            if (WKStringRef s = WKURLCopyString(u)) { d.url = WKStr(s); WKRelease(s); }
            WKRelease(u);
        }
        WKRelease(req);
    }
    s_items.insert(s_items.begin(), d);
    if (s_items.size() > kMaxItems) {
        // Drop the oldest entries that are not running.
        for (size_t i = s_items.size(); i-- > 0 && s_items.size() > kMaxItems;)
            if (s_items[i].state != DownloadItem::Running) s_items.erase(s_items.begin() + i);
    }
    s_running[download] = d.id;

    WKDownloadClientV0 dc = {};
    dc.base.version = 0;
    dc.base.clientInfo = nullptr;
    dc.decideDestinationWithResponse = OnDecideDestination;
    dc.didWriteData = OnDidWriteData;
    dc.didFinish = OnDidFinish;
    dc.didFailWithError = OnDidFail;
    WKDownloadSetClient(download, &dc.base);
    ListChanged();
}

// A link opened in a new tab that turns out to be a file leaves an empty tab behind.
static void CloseTabIfEmptyPopup(const void* ci) {
    Tab* t = ResolveTab(ci, "download");
    if (t && t->openedByPage && (t->url.empty() || t->url == L"about:blank"))
        PostMessageW(g_hWnd, WM_APP_CLOSE_TAB, (WPARAM)t->id, 0);
}

void OnActionBecameDownload(WKPageRef, WKNavigationActionRef, WKDownloadRef d, const void* ci) {
    Track(d);
    CloseTabIfEmptyPopup(ci);
}

void OnResponseBecameDownload(WKPageRef, WKNavigationResponseRef, WKDownloadRef d, const void* ci) {
    Track(d);
    CloseTabIfEmptyPopup(ci);
}

void OnContextMenuDownload(WKPageRef, WKDownloadRef d, const void*) {
    Track(d);
}

// ----------------------------------------------------------------------------
//  Commands
// ----------------------------------------------------------------------------
static void CancelDone(WKDataRef, const void*) {}

void DownloadCancel(long long id) {
    for (auto& [ref, rid] : s_running) {
        if (rid != id) continue;
        if (DownloadItem* d = Find(id)) d->state = DownloadItem::Cancelled;     // OnDidFail sees it
        WKDownloadCancel(ref, nullptr, CancelDone);
        ListChanged();
        return;
    }
}

void DownloadRetry(long long id) {
    DownloadItem* d = Find(id);
    if (!d || d->state == DownloadItem::Running || d->url.empty()) return;
    const std::wstring url = d->url;
    DownloadRemove(id);
    // Loading the address again makes it a new download; the page in the tab stays where it is.
    Tab* t = ActiveTab();
    if (!t || !t->page) return;
    if (WKURLRef u = WKURLCreateWithUTF8CString(ToUtf8(url).c_str())) {
        WKPageLoadURL(t->page, u);
        WKRelease(u);
    }
}

void DownloadOpen(long long id) {
    const DownloadItem* d = Find(id);
    if (!d || d->state != DownloadItem::Done || !DownloadFileExists(*d)) return;
    ShellExecuteW(g_hWnd, L"open", d->path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void DownloadShowInFolder(long long id) {
    const DownloadItem* d = Find(id);
    if (!d) return;
    if (DownloadFileExists(*d)) {
        // Selects the file in a new Explorer window.
        if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(d->path.c_str())) {
            SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
            ILFree(pidl);
            return;
        }
    }
    OpenDownloadsFolder();
}

void DownloadRemove(long long id) {
    DownloadItem* d = Find(id);
    if (!d) return;
    if (d->state == DownloadItem::Running) { DownloadCancel(id); return; }
    s_items.erase(std::remove_if(s_items.begin(), s_items.end(), [id](const DownloadItem& x) { return x.id == id; }), s_items.end());
    ListChanged();
    DownloadsSave();
}

void DownloadsClear() {
    s_items.erase(std::remove_if(s_items.begin(), s_items.end(),
                                 [](const DownloadItem& x) { return x.state != DownloadItem::Running; }), s_items.end());
    ListChanged();
    DownloadsSave();
}

void DownloadsTick() {
    if (!s_progressDirty) return;
    const DWORD now = GetTickCount();
    if (now - s_lastNotify < 200) return;               // five times a second is plenty
    s_lastNotify = now;
    s_progressDirty = false;
    if (g_downloadsChanged) g_downloadsChanged(false);
}
