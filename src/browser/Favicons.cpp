// Lyra — значки сайтов: загрузка /favicon.ico, кэш в памяти и на диске.
//
// Every site gets one icon, keyed by its host ("github.com"). Lookups go: memory -> disk cache
// (%LOCALAPPDATA%\Lyra\Favicons\<host>.png, refreshed after a week) -> download on a worker
// thread. The worker only fetches bytes; decoding (GDI+) and saving happen on the UI thread.
#include "browser/Favicons.h"
#include "app/Log.h"
#include "core/Constants.h"
#include "core/Settings.h"
#include "core/StringUtils.h"
#include "core/Url.h"
#include "ui/MainWindow.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

struct FetchJob { std::wstring key, scheme, host; INTERNET_PORT port = 0; };
struct FetchResult { std::wstring key; std::string bytes; };

static std::unordered_map<std::wstring, std::unique_ptr<Bitmap>> s_icons;
static std::set<std::wstring> s_failed, s_pending;
// Shared with the worker thread. Deliberately never destroyed: the worker may still be waiting
// for a slow server while the program exits, and must not find these gone.
static std::deque<FetchJob>& s_queue = *new std::deque<FetchJob>();
static std::mutex& s_lock = *new std::mutex();
static std::condition_variable& s_wake = *new std::condition_variable();
static std::thread s_worker;
static bool s_quit = false;

static std::wstring FolderPath() { return g_settingsDir + L"\\Favicons"; }

std::wstring FaviconKey(const std::wstring& pageUrl) {
    if (pageUrl.compare(0, 7, L"http://") != 0 && pageUrl.compare(0, 8, L"https://") != 0) return std::wstring();
    std::wstring key = HostOf(pageUrl);
    if (!key.empty()) CharLowerBuffW(&key[0], (DWORD)key.size());
    for (auto& c : key) if (!(iswalnum(c) || c == L'.' || c == L'-')) c = L'_';
    return key;
}

std::wstring FaviconFolderUrl() {
    wchar_t url[2084] = {};
    DWORD len = ARRAYSIZE(url);
    std::wstring dir = FolderPath() + L"\\";
    return SUCCEEDED(UrlCreateFromPathW(dir.c_str(), url, &len, 0)) ? std::wstring(url) : std::wstring();
}

// ---- download (worker thread) --------------------------------------------------
// Sites that answer only browsers turn away a bare "Lyra" agent, so the icon fetcher says what
// Lyra is: a WebKit browser.
static const wchar_t kAgent[] = L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Safari/605.1.15 Lyra";

// GET `url`; the body if the answer is 200 and not larger than `limit`.
static std::string Get(const std::wstring& url, size_t limit) {
    std::string data;
    URL_COMPONENTS uc = { sizeof(uc) };
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = ARRAYSIZE(host);
    uc.lpszUrlPath = path; uc.dwUrlPathLength = ARRAYSIZE(path);
    wchar_t extra[1024] = {};
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = ARRAYSIZE(extra);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return data;
    const std::wstring object = std::wstring(path) + extra;
    HINTERNET session = WinHttpOpen(kAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return data;
    WinHttpSetTimeouts(session, 4000, 4000, 4000, 6000);
    if (HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0)) {
        const DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
        if (HINTERNET req = WinHttpOpenRequest(conn, L"GET", object.empty() ? L"/" : object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES, flags)) {
            if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(req, nullptr)) {
                DWORD status = 0, size = sizeof(status);
                WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
                if (status == 200) {
                    char buf[8192];
                    DWORD got = 0;
                    while (WinHttpReadData(req, buf, sizeof(buf), &got) && got > 0) {
                        data.append(buf, got);
                        if (data.size() > limit) { data.clear(); break; }
                    }
                }
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(conn);
    }
    WinHttpCloseHandle(session);
    return data;
}

// A format GDI+ can decode (ICO, PNG, GIF, JPEG, BMP); an HTML error page is not.
static bool LooksLikeImage(const std::string& b) {
    if (b.size() < 8) return false;
    const unsigned char* u = (const unsigned char*)b.data();
    return (u[0] == 0 && u[1] == 0 && u[2] == 1 && u[3] == 0) || (u[0] == 0x89 && u[1] == 'P' && u[2] == 'N' && u[3] == 'G') ||
           (u[0] == 'G' && u[1] == 'I' && u[2] == 'F') || (u[0] == 0xFF && u[1] == 0xD8) || (u[0] == 'B' && u[1] == 'M');
}

// The icon a page names in <link rel="... icon ..." href="...">: a large PNG (apple-touch-icon) is
// best, then any icon that is not an SVG (GDI+ cannot draw those). Returns an absolute address.
static std::wstring IconFromPage(const std::string& html, const std::wstring& origin) {
    std::string lower = html;
    for (auto& c : lower) c = (char)tolower((unsigned char)c);
    std::string best;
    int bestScore = -1;
    size_t at = 0;
    while ((at = lower.find("<link", at)) != std::string::npos) {
        const size_t end = lower.find('>', at);
        if (end == std::string::npos) break;
        const std::string tag = lower.substr(at, end - at), orig = html.substr(at, end - at);
        at = end;
        const size_t rel = tag.find("rel=");
        if (rel == std::string::npos || tag.find("icon", rel) == std::string::npos) continue;
        size_t h = tag.find("href=");
        if (h == std::string::npos) continue;
        h += 5;
        const char quote = tag[h] == '"' || tag[h] == '\'' ? tag[h] : 0;
        if (quote) ++h;
        size_t e = quote ? tag.find(quote, h) : tag.find_first_of(" \t\r\n", h);
        if (e == std::string::npos) e = tag.size();
        std::string href = orig.substr(h, e - h);
        const std::string hl = tag.substr(h, e - h);
        if (href.empty() || hl.find(".svg") != std::string::npos || hl.rfind("data:", 0) == 0) continue;
        int score = 1;
        if (tag.find("apple-touch-icon") != std::string::npos) score = 3;
        else if (hl.find(".png") != std::string::npos) score = 2;
        if (score > bestScore) { bestScore = score; best = href; }
    }
    if (best.empty()) return std::wstring();
    for (size_t i; (i = best.find("&amp;")) != std::string::npos;) best.replace(i, 5, "&");
    const std::wstring href = ToWide(best);
    if (href.rfind(L"http://", 0) == 0 || href.rfind(L"https://", 0) == 0) return href;
    if (href.rfind(L"//", 0) == 0) return L"https:" + href;
    if (!href.empty() && href[0] == L'/') return origin + href;
    return origin + L"/" + href;
}

static std::string Fetch(const FetchJob& job) {
    const std::wstring origin = job.scheme + L"://" + job.host +
        ((job.port == 443 && job.scheme == L"https") || (job.port == 80 && job.scheme == L"http") ? L"" : L":" + std::to_wstring(job.port));
    std::string data = Get(origin + L"/favicon.ico", 512u << 10);
    if (LooksLikeImage(data)) return data;
    // No /favicon.ico (or not an image): the icon the start page itself names.
    const std::wstring icon = IconFromPage(Get(origin + L"/", 2u << 20), origin);
    if (icon.empty()) return std::string();
    data = Get(icon, 512u << 10);
    return LooksLikeImage(data) ? data : std::string();
}

static void WorkerLoop() {
    for (;;) {
        FetchJob job;
        {
            std::unique_lock<std::mutex> lock(s_lock);
            s_wake.wait(lock, [] { return s_quit || !s_queue.empty(); });
            if (s_quit) return;
            job = s_queue.front();
            s_queue.pop_front();
        }
        auto* result = new FetchResult{ job.key, Fetch(job) };
        if (!g_hWnd || !PostMessageW(g_hWnd, WM_APP_FAVICON, 0, (LPARAM)result)) delete result;
    }
}

// ---- decoding / cache (UI thread) ---------------------------------------------
static bool PngEncoder(CLSID* clsid) {
    UINT n = 0, size = 0;
    GetImageEncodersSize(&n, &size);
    if (!size) return false;
    std::vector<BYTE> buf(size);
    ImageCodecInfo* codecs = (ImageCodecInfo*)buf.data();
    GetImageEncoders(n, size, codecs);
    for (UINT i = 0; i < n; ++i)
        if (wcscmp(codecs[i].MimeType, L"image/png") == 0) { *clsid = codecs[i].Clsid; return true; }
    return false;
}

// Decoded image -> 32x32 premultiplied copy (crisp when drawn at 14-18 px)
static std::unique_ptr<Bitmap> Normalize(Bitmap& src) {
    if (src.GetLastStatus() != Ok || src.GetWidth() == 0 || src.GetHeight() == 0) return nullptr;
    auto out = std::make_unique<Bitmap>(32, 32, PixelFormat32bppPARGB);
    Graphics g(out.get());
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.Clear(Color(0, 0, 0, 0));
    g.DrawImage(&src, Rect(0, 0, 32, 32));
    return out;
}

static std::unique_ptr<Bitmap> DecodeBytes(const std::string& bytes) {
    if (bytes.size() < 16) return nullptr;
    IStream* stream = SHCreateMemStream((const BYTE*)bytes.data(), (UINT)bytes.size());
    if (!stream) return nullptr;
    std::unique_ptr<Bitmap> result;
    {
        Bitmap decoded(stream);
        result = Normalize(decoded);
    }
    stream->Release();
    return result;
}

static std::unique_ptr<Bitmap> LoadCached(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return nullptr;
    LARGE_INTEGER size = {};
    GetFileSizeEx(f, &size);
    std::string data((size_t)std::min<long long>(size.QuadPart, 1 << 20), '\0');
    DWORD read = 0;
    if (!data.empty()) ReadFile(f, &data[0], (DWORD)data.size(), &read, nullptr);
    CloseHandle(f);
    data.resize(read);
    return DecodeBytes(data);                       // copies the pixels: the file is not kept open
}

static bool CacheIsOld(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return true;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER t1, t2;
    t1.LowPart = a.ftLastWriteTime.dwLowDateTime; t1.HighPart = a.ftLastWriteTime.dwHighDateTime;
    t2.LowPart = now.dwLowDateTime; t2.HighPart = now.dwHighDateTime;
    return t2.QuadPart > t1.QuadPart && (t2.QuadPart - t1.QuadPart) / 10000000ULL > 7ULL * 24 * 3600;
}

static void StartFetch(const std::wstring& key, const std::wstring& pageUrl) {
    if (s_pending.count(key)) return;
    URL_COMPONENTSW uc = { sizeof(uc) };
    wchar_t scheme[16] = {}, host[256] = {};
    uc.lpszScheme = scheme; uc.dwSchemeLength = ARRAYSIZE(scheme);
    uc.lpszHostName = host; uc.dwHostNameLength = ARRAYSIZE(host);
    if (!WinHttpCrackUrl(pageUrl.c_str(), 0, 0, &uc)) return;
    s_pending.insert(key);
    {
        std::lock_guard<std::mutex> lock(s_lock);
        if (s_quit) return;
        s_queue.push_back(FetchJob{ key, scheme, host, uc.nPort });
        if (!s_worker.joinable()) s_worker = std::thread(WorkerLoop);
    }
    s_wake.notify_one();
}

Bitmap* FaviconFor(const std::wstring& pageUrl) {
    const std::wstring key = FaviconKey(pageUrl);
    if (key.empty()) return nullptr;
    auto it = s_icons.find(key);
    if (it != s_icons.end()) return it->second.get();
    if (s_failed.count(key)) return nullptr;
    const std::wstring path = FolderPath() + L"\\" + key + L".png";
    if (auto cached = LoadCached(path)) {
        Bitmap* raw = cached.get();
        s_icons[key] = std::move(cached);
        if (CacheIsOld(path)) StartFetch(key, pageUrl);   // keep showing the old one meanwhile
        return raw;
    }
    StartFetch(key, pageUrl);
    return nullptr;
}

void FaviconArrived(LPARAM lParam) {
    std::unique_ptr<FetchResult> r((FetchResult*)lParam);
    if (!r) return;
    s_pending.erase(r->key);
    std::unique_ptr<Bitmap> icon = DecodeBytes(r->bytes);
    if (!icon) {
        if (!s_icons.count(r->key)) s_failed.insert(r->key);   // letter badge for this session
        return;
    }
    CreateDirectoryW(FolderPath().c_str(), nullptr);
    CLSID png;
    if (PngEncoder(&png)) icon->Save((FolderPath() + L"\\" + r->key + L".png").c_str(), &png, nullptr);
    s_icons[r->key] = std::move(icon);
}

void FaviconsShutdown() {
    {
        std::lock_guard<std::mutex> lock(s_lock);
        s_quit = true;
        s_queue.clear();
    }
    s_wake.notify_one();
    // A download can take a few seconds to time out: do not hold up closing the browser.
    if (s_worker.joinable()) s_worker.detach();
    s_icons.clear();                                 // GDI+ objects go before GdiplusShutdown
}
