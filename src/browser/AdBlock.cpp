// Lyra — блокировка рекламы и трекеров: списки EasyList, перевод в правила WebKit (Content Blockers).
//
// Filter lists (Adblock Plus syntax) are downloaded to %LOCALAPPDATA%\Lyra\AdBlock\<id>.txt and
// refreshed every few days. A worker thread turns each list into WebKit content blocker rules
// (JSON); the UI thread compiles them with WKUserContentExtensionStore, which keeps the compiled
// lists in AdBlock\Compiled, so the next start only looks them up. All compiled lists sit in one
// WKUserContentController shared by every page.
//
// What carries over from the filter syntax: network rules (||domain^, |anchors|, *, ^, and the
// options third-party, domain=, resource types, match-case), their @@ exceptions, and element
// hiding (##selector, domain##selector, #@# for generic selectors). Scriptlets (##+js), procedural
// selectors (:has-text and the like), regular-expression rules and redirects have no WebKit
// counterpart and are skipped.
#include "browser/AdBlock.h"
#include "app/Log.h"
#include "browser/TabManager.h"
#include "core/Constants.h"
#include "core/Localization.h"
#include "core/Settings.h"
#include "core/StringUtils.h"
#include "core/Url.h"
#include "ui/MainWindow.h"

#include <WebKit/WKUserContentControllerRef.h>
#include <WebKit/WKUserScriptRef.h>
#include <WebKit/WKUserContentExtensionStoreRef.h>

#include <deque>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

namespace {

struct ListDef { const wchar_t* id; const wchar_t* host; const wchar_t* path; };
const ListDef kLists[] = {
    { L"easylist",    L"easylist.to", L"/easylist/easylist.txt" },
    { L"easyprivacy", L"easylist.to", L"/easylist/easyprivacy.txt" },
    { L"ruadlist",    L"easylist-downloads.adblockplus.org", L"/advblock.txt" },
};
constexpr ULONGLONG kRefreshAfter = 4ULL * 24 * 3600 * 10000000ULL;   // 4 days, in FILETIME units

struct Job { std::wstring id; bool download = false; };
struct Built { std::wstring id; std::string json; int rules = 0; };

WKUserContentControllerRef s_controller = nullptr;
WKUserContentExtensionStoreRef s_store = nullptr;
std::map<std::wstring, WKUserContentFilterRef> s_filters;   // compiled and in use
std::map<std::wstring, int> s_ruleCounts;
std::set<std::wstring> s_allowed;                              // hosts where nothing is blocked

// Shared with the worker; never destroyed (the worker may still be busy when the program exits).
std::deque<Job>& s_jobs = *new std::deque<Job>();
std::mutex& s_lock = *new std::mutex();
bool s_workerRunning = false;

std::wstring Dir() { return g_settingsDir + L"\\AdBlock"; }
std::wstring ListFile(const std::wstring& id) { return Dir() + L"\\" + id + L".txt"; }
std::wstring AllowFile() { return Dir() + L"\\allowed-sites.txt"; }
std::string RuleListId(const std::wstring& id) { return "lyra-" + ToUtf8(id); }

ULONGLONG FileAge(const std::wstring& path, bool* exists) {
    WIN32_FILE_ATTRIBUTE_DATA fa {};
    *exists = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow + fa.nFileSizeHigh > 0;
    if (!*exists) return ~0ULL;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a, b;
    a.LowPart = fa.ftLastWriteTime.dwLowDateTime; a.HighPart = fa.ftLastWriteTime.dwHighDateTime;
    b.LowPart = now.dwLowDateTime; b.HighPart = now.dwHighDateTime;
    return b.QuadPart > a.QuadPart ? b.QuadPart - a.QuadPart : 0;
}

bool ReadFileBytes(const std::wstring& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool WriteFileBytes(const std::wstring& path, const std::string& data) {
    std::wstring tmp = path + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(data.data(), (std::streamsize)data.size());
        if (!f) return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

// ---- download (worker thread) ---------------------------------------------------------------
bool HttpsGet(const wchar_t* host, const wchar_t* path, std::string& out) {
    bool ok = false;
    HINTERNET session = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) Lyra",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 10000, 10000, 15000, 30000);
    if (HINTERNET conn = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0)) {
        if (HINTERNET req = WinHttpOpenRequest(conn, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)) {
            DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;
            WinHttpSetOption(req, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof(decompress));
            if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(req, nullptr)) {
                DWORD status = 0, size = sizeof(status);
                WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
                if (status == 200) {
                    char buf[64 * 1024];
                    DWORD got = 0;
                    while (WinHttpReadData(req, buf, sizeof(buf), &got) && got > 0)
                        out.append(buf, got);
                    ok = out.size() > 1000;
                }
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(conn);
    }
    WinHttpCloseHandle(session);
    return ok;
}

// ---- Adblock Plus syntax -> WebKit content blocker rules ---------------------------------------
std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool IsAscii(const std::string& s) {
    for (unsigned char c : s) if (c >= 0x80 || c < 0x20) return false;
    return true;
}

std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        size_t p = s.find(sep, start);
        parts.push_back(s.substr(start, p == std::string::npos ? std::string::npos : p - start));
        if (p == std::string::npos) break;
        start = p + 1;
    }
    return parts;
}

void JsonString(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
            else out += (char)c;
        }
    }
    out += '"';
}

void JsonList(std::string& out, const char* key, const std::vector<std::string>& items) {
    out += ",\"";
    out += key;
    out += "\":[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out += ',';
        JsonString(out, items[i]);
    }
    out += ']';
}

// A domain for if-domain / unless-domain: lowercase ASCII, no wildcards ("example.*" is skipped).
bool CleanDomain(std::string d, std::string& out) {
    d = Lower(d);
    while (!d.empty() && (d.back() == ' ' || d.back() == '.')) d.pop_back();
    if (d.empty() || !IsAscii(d)) return false;
    for (char c : d)
        if (!(isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_')) return false;
    out = "*" + d;
    return true;
}

// The URL part of a network filter as a WebKit url-filter (a small regular-expression subset:
// no alternation, no counted repetition; ^ and $ only at the ends).
bool PatternToRegex(std::string p, std::string& re) {
    re.clear();
    if (!IsAscii(p)) return false;
    bool domainAnchor = false, startAnchor = false, endAnchor = false;
    if (p.compare(0, 2, "||") == 0) { domainAnchor = true; p.erase(0, 2); }
    else if (!p.empty() && p[0] == '|') { startAnchor = true; p.erase(0, 1); }
    if (!p.empty() && p.back() == '|') { endAnchor = true; p.pop_back(); }
    if (domainAnchor) re = "^[a-z][a-z0-9.+-]*:/+([^/:]*\\.)?";
    else if (startAnchor) re = "^";
    for (size_t i = 0; i < p.size(); ++i) {
        char c = p[i];
        switch (c) {
        case '*':
            if (re.size() < 2 || re.compare(re.size() - 2, 2, ".*") != 0) re += ".*";
            break;
        case '^':
            if (i + 1 == p.size()) {
                // A separator or the end of the address; there is no "or end" in WebKit's subset.
                // Right after a host name it is always a separator (addresses have a path: "/").
                if (domainAnchor && p.find('/') == std::string::npos) re += "[^a-zA-Z0-9_.%-]";
                break;
            }
            re += "[^a-zA-Z0-9_.%-]";
            break;
        case '.': case '+': case '?': case '(': case ')': case '[': case ']':
        case '{': case '}': case '\\': case '$': case '|':
            re += '\\';
            re += c;
            break;
        default:
            re += c;
        }
    }
    // Leading and trailing ".*" match anyway.
    if (!domainAnchor && !startAnchor && re.compare(0, 2, ".*") == 0) re.erase(0, 2);
    if (re.size() >= 2 && re.compare(re.size() - 2, 2, ".*") == 0 && !endAnchor) re.erase(re.size() - 2);
    if (endAnchor) re += '$';
    if (re.empty() || re == "^" || re == "$") re = ".*";
    return true;
}

bool IsProceduralSelector(const std::string& s) {
    static const char* const kUnsupported[] = {
        ":-abp-", ":has-text(", ":contains(", ":xpath(", ":matches-css", ":upward(", ":remove(",
        ":style(", ":min-text-length(", ":watch-attr(", ":nth-ancestor(", ":others(", ":if(",
        ":if-not(", ":matches-path(", ":matches-attr(", ":matches-prop(", ":remove-attr(",
        ":remove-class(", ":matches-media(", "{", "}",
    };
    for (auto* u : kUnsupported) if (s.find(u) != std::string::npos) return true;
    return false;
}

struct Rule { std::string json; };

class Converter {
public:
    void Line(std::string line) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos) return;
        line.erase(0, first);
        if (line[0] == '!' || line[0] == '[') return;
        if (Cosmetic(line)) return;
        Network(line);
    }

    std::string Finish(int* count) {
        std::string out = "[";
        size_t n = 0;
        constexpr size_t kMaxRules = 149000;
        auto add = [&](const std::string& rule) {
            if (n >= kMaxRules) return;
            if (n) out += ",\n";
            out += rule;
            ++n;
        };
        for (auto& r : m_blocks) add(r);
        for (auto& sel : m_genericHide) {
            if (m_genericHideExceptions.count(sel)) continue;
            std::string r = "{\"trigger\":{\"url-filter\":\".*\"},\"action\":{\"type\":\"css-display-none\",\"selector\":";
            JsonString(r, sel);
            r += "}}";
            add(r);
        }
        for (auto& [domains, selectors] : m_specificHide) {
            // A few selectors per rule: one the engine cannot parse only drops its own rule.
            for (size_t i = 0; i < selectors.size(); i += 20) {
                std::string joined;
                for (size_t j = i; j < selectors.size() && j < i + 20; ++j) {
                    if (!joined.empty()) joined += ", ";
                    joined += selectors[j];
                }
                std::string r = "{\"trigger\":{\"url-filter\":\".*\"";
                r += domains;
                r += "},\"action\":{\"type\":\"css-display-none\",\"selector\":";
                JsonString(r, joined);
                r += "}}";
                add(r);
            }
        }
        // Exceptions last: ignore-previous-rules only undoes rules that come before it.
        for (auto& r : m_exceptions) add(r);
        out += "]";
        *count = (int)n;
        return out;
    }

private:
    bool Cosmetic(const std::string& line) {
        static const char* const kMarkers[] = { "#@?#", "#@$#", "#@%#", "#?#", "#$#", "#%#", "#@#", "##" };
        size_t best = std::string::npos;
        std::string marker;
        for (auto* m : kMarkers) {
            size_t p = line.find(m);
            if (p != std::string::npos && (p < best || (p == best && strlen(m) > marker.size()))) { best = p; marker = m; }
        }
        if (best == std::string::npos) return false;
        if (marker != "##" && marker != "#@#") return true;          // extended syntaxes: skipped
        std::string domains = line.substr(0, best);
        std::string selector = line.substr(best + marker.size());
        if (selector.empty() || selector[0] == '+' || selector[0] == '^' || IsProceduralSelector(selector)) return true;
        if (domains.empty()) {
            if (marker == "##") m_genericHide.insert(selector);
            else m_genericHideExceptions.insert(selector);
            return true;
        }
        if (marker == "#@#") return true;                            // site exceptions: no counterpart
        std::vector<std::string> ifDomains, unlessDomains;
        for (auto& d : Split(domains, ',')) {
            std::string clean;
            if (!d.empty() && d[0] == '~') { if (CleanDomain(d.substr(1), clean)) unlessDomains.push_back(clean); }
            else if (CleanDomain(d, clean)) ifDomains.push_back(clean);
        }
        std::string key;
        if (!ifDomains.empty()) { JsonList(key, "if-domain", ifDomains); }
        else if (!unlessDomains.empty()) { JsonList(key, "unless-domain", unlessDomains); }
        else return true;
        m_specificHide[key].push_back(selector);
        return true;
    }

    void Network(std::string line) {
        bool exception = line.compare(0, 2, "@@") == 0;
        if (exception) line.erase(0, 2);
        // Regular-expression filters: WebKit's subset would reject most of them.
        if (line.size() > 1 && line[0] == '/' && (line.back() == '/' || line.find("/$") != std::string::npos)) return;

        std::string pattern = line, options;
        size_t dollar = line.rfind('$');
        if (dollar != std::string::npos && dollar + 1 < line.size() && line.find('/', dollar) == std::string::npos) {
            pattern = line.substr(0, dollar);
            options = line.substr(dollar + 1);
        }

        std::set<std::string> types, notTypes;
        bool childFrameOnly = false, subdocument = false, document = false;
        std::string loadType;
        bool caseSensitive = false;
        std::vector<std::string> ifDomains, unlessDomains;
        for (auto& raw : Split(options, ',')) {
            if (raw.empty()) continue;
            std::string opt = Lower(raw);
            bool neg = opt[0] == '~';
            if (neg) opt.erase(0, 1);
            std::string value;
            size_t eq = opt.find('=');
            if (eq != std::string::npos) { value = raw.substr(raw.find('=') + 1); opt = opt.substr(0, eq); }

            const char* type = nullptr;
            if (opt == "script") type = "script";
            else if (opt == "image") type = "image";
            else if (opt == "stylesheet" || opt == "css") type = "style-sheet";
            else if (opt == "font") type = "font";
            else if (opt == "media") type = "media";
            else if (opt == "xmlhttprequest" || opt == "xhr") type = "fetch";
            else if (opt == "websocket") type = "websocket";
            else if (opt == "ping" || opt == "beacon") type = "ping";
            else if (opt == "other" || opt == "object" || opt == "object-subrequest") type = "other";
            else if (opt == "popup") type = "popup";
            else if (opt == "subdocument" || opt == "frame") { type = "document"; if (!neg) subdocument = true; }
            if (type) { (neg ? notTypes : types).insert(type); continue; }

            if (opt == "third-party" || opt == "3p") loadType = neg ? "first-party" : "third-party";
            else if (opt == "first-party" || opt == "1p") loadType = neg ? "third-party" : "first-party";
            else if (opt == "match-case") caseSensitive = true;
            else if (opt == "domain" || opt == "from") {
                for (auto& d : Split(value, '|')) {
                    std::string clean;
                    if (!d.empty() && d[0] == '~') { if (CleanDomain(d.substr(1), clean)) unlessDomains.push_back(clean); }
                    else if (CleanDomain(d, clean)) ifDomains.push_back(clean);
                }
            } else if (opt == "document" || opt == "doc") document = true;
            else if (opt == "important" || opt == "all") { }
            else return;   // redirect, csp, removeparam, elemhide, badfilter, ...: no counterpart
        }
        if (document && !exception) return;
        if (types.size() == 1 && subdocument) childFrameOnly = true;
        if (types.empty() && !notTypes.empty()) {
            static const char* const kAll[] = { "document", "image", "style-sheet", "script", "font", "fetch",
                                               "websocket", "other", "media", "popup", "ping", "svg-document" };
            for (auto* t : kAll) if (!notTypes.count(t)) types.insert(t);
            if (types.empty()) return;
        }

        std::string re;
        if (document) {
            // @@||site^$document: nothing is blocked on that site.
            std::string host = pattern;
            if (host.compare(0, 2, "||") != 0) return;
            host.erase(0, 2);
            while (!host.empty() && (host.back() == '^' || host.back() == '/' || host.back() == '|')) host.pop_back();
            std::string clean;
            if (!CleanDomain(host, clean)) return;
            ifDomains.assign(1, clean);
            unlessDomains.clear();
            re = ".*";
        } else {
            if (!PatternToRegex(pattern, re)) return;
            if (re == ".*" && ifDomains.empty() && unlessDomains.empty() && loadType.empty()) return;   // would block everything
        }

        std::string r = "{\"trigger\":{\"url-filter\":";
        JsonString(r, re);
        if (caseSensitive) r += ",\"url-filter-is-case-sensitive\":true";
        if (!document && !types.empty()) JsonList(r, "resource-type", std::vector<std::string>(types.begin(), types.end()));
        if (childFrameOnly) r += ",\"load-context\":[\"child-frame\"]";
        if (!loadType.empty()) r += ",\"load-type\":[\"" + loadType + "\"]";
        if (!ifDomains.empty()) JsonList(r, "if-domain", ifDomains);
        else if (!unlessDomains.empty()) JsonList(r, "unless-domain", unlessDomains);
        r += exception ? "},\"action\":{\"type\":\"ignore-previous-rules\"}}" : "},\"action\":{\"type\":\"block\"}}";
        (exception ? m_exceptions : m_blocks).push_back(std::move(r));
    }

    std::vector<std::string> m_blocks, m_exceptions;
    std::set<std::string> m_genericHide, m_genericHideExceptions;
    std::map<std::string, std::vector<std::string>> m_specificHide;   // key: the domain condition (JSON)
};

std::string ConvertList(const std::string& text, int* rules) {
    Converter c;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        c.Line(text.substr(start, end - start));
        start = end + 1;
    }
    return c.Finish(rules);
}

// ---- worker ---------------------------------------------------------------------------------
void WorkerMain() {
    while (true) {
        Job job;
        {
            std::lock_guard<std::mutex> g(s_lock);
            if (s_jobs.empty()) { s_workerRunning = false; return; }
            job = s_jobs.front();
            s_jobs.pop_front();
        }
        const ListDef* def = nullptr;
        for (auto& l : kLists) if (job.id == l.id) def = &l;
        if (!def) continue;

        std::string text;
        if (job.download) {
            if (HttpsGet(def->host, def->path, text) && text.find("[Adblock") != std::string::npos) {
                WriteFileBytes(ListFile(job.id), text);
                Log("adblock: downloaded %ls (%zu bytes)", job.id.c_str(), text.size());
            } else {
                Log("adblock: download of %ls failed", job.id.c_str());
                text.clear();
            }
        }
        if (text.empty() && !ReadFileBytes(ListFile(job.id), text)) continue;
        if (text.empty()) continue;

        auto* built = new Built();
        built->id = job.id;
        DWORD t0 = GetTickCount();
        built->json = ConvertList(text, &built->rules);
        Log("adblock: %ls -> %d rules in %lums", job.id.c_str(), built->rules, GetTickCount() - t0);
        if (!g_hWnd || g_closing || !PostMessageW(g_hWnd, WM_APP_ADBLOCK, 0, (LPARAM)built)) delete built;
    }
}

void Enqueue(const std::wstring& id, bool download) {
    std::lock_guard<std::mutex> g(s_lock);
    for (auto& j : s_jobs) if (j.id == id) { j.download = j.download || download; return; }
    s_jobs.push_back({ id, download });
    if (!s_workerRunning) {
        s_workerRunning = true;
        std::thread(WorkerMain).detach();
    }
}

void RefreshController() {
    if (!s_controller) return;
    WKUserContentControllerRemoveAllUserContentFilters(s_controller);
    if (!g_prefs.adBlock) return;
    for (auto& [id, filter] : s_filters) WKUserContentControllerAddUserContentFilter(s_controller, filter);
}

void SetFilter(const std::wstring& id, WKUserContentFilterRef filter) {
    auto it = s_filters.find(id);
    if (it != s_filters.end()) WKRelease(it->second);
    WKRetain(filter);
    s_filters[id] = filter;
    RefreshController();
}

// The lists that are missing or old get downloaded again; lists that are on disk but not
// compiled (a new WebKit, or a compile that failed) are converted again.
void ScheduleUpdates(bool force) {
    for (auto& l : kLists) {
        bool exists = false;
        ULONGLONG age = FileAge(ListFile(l.id), &exists);
        if (force || !exists || age > kRefreshAfter) Enqueue(l.id, true);
    }
}

struct LookupContext { std::wstring id; };

void OnLookup(WKUserContentFilterRef filter, WKUserContentExtensionStoreResult result, void* context) {
    std::unique_ptr<LookupContext> ctx((LookupContext*)context);
    if (result == kWKUserContentExtensionStoreSuccess && filter) {
        SetFilter(ctx->id, filter);
        WKRelease(filter);
        return;
    }
    if (filter) WKRelease(filter);
    Log("adblock: no compiled %ls (result %u)", ctx->id.c_str(), (unsigned)result);
    bool exists = false;
    FileAge(ListFile(ctx->id), &exists);
    Enqueue(ctx->id, !exists);
}

void OnCompiled(WKUserContentFilterRef filter, WKUserContentExtensionStoreResult result, void* context) {
    std::unique_ptr<LookupContext> ctx((LookupContext*)context);
    if (result == kWKUserContentExtensionStoreSuccess && filter) {
        Log("adblock: compiled %ls", ctx->id.c_str());
        SetFilter(ctx->id, filter);
        WKRelease(filter);
        return;
    }
    if (filter) WKRelease(filter);
    Log("adblock: compiling %ls FAILED (result %u)", ctx->id.c_str(), (unsigned)result);
}

void LoadAllowed() {
    std::string text;
    if (!ReadFileBytes(AllowFile(), text)) return;
    for (auto& line : Split(text, '\n')) {
        std::string h = Lower(line);
        while (!h.empty() && (h.back() == '\r' || h.back() == ' ')) h.pop_back();
        if (!h.empty()) s_allowed.insert(FromUtf8(h));
    }
}

void SaveAllowed() {
    std::string text;
    for (auto& h : s_allowed) text += ToUtf8(h) + "\n";
    WriteFileBytes(AllowFile(), text);
}

std::wstring SiteKey(const std::wstring& url) {
    std::wstring host = HostOf(url);
    if (host.empty()) return host;
    CharLowerBuffW(&host[0], (DWORD)host.size());
    if (host.compare(0, 4, L"www.") == 0) host.erase(0, 4);
    return host;
}

} // namespace

void AdBlockInit() {
    CreateDirectoryW(g_settingsDir.c_str(), nullptr);
    CreateDirectoryW(Dir().c_str(), nullptr);
    std::wstring compiled = Dir() + L"\\Compiled";
    CreateDirectoryW(compiled.c_str(), nullptr);
    LoadAllowed();

    s_controller = WKUserContentControllerCreate();
    WKStringRef path = WKStringCreateWithUTF8CString(ToUtf8(compiled).c_str());
    s_store = WKUserContentExtensionStoreCreate(path);
    WKRelease(path);

    for (auto& l : kLists) {
        WKStringRef id = WKStringCreateWithUTF8CString(RuleListId(l.id).c_str());
        WKUserContentExtensionStoreLookup(s_store, id, new LookupContext { l.id }, OnLookup);
        WKRelease(id);
    }
    if (g_prefs.adBlock) ScheduleUpdates(false);
}

void AdBlockShutdown() {
    for (auto& [id, filter] : s_filters) WKRelease(filter);
    s_filters.clear();
    if (s_controller) { WKRelease(s_controller); s_controller = nullptr; }
    if (s_store) { WKRelease(s_store); s_store = nullptr; }
}

WKUserContentControllerRef AdBlockUserContentController() { return s_controller; }

// ---- user scripts ------------------------------------------------------------------------
// YouTube serves its ads from the same servers as the videos, so filter rules cannot remove
// them; this script takes them out of the player's data, and skips any that still starts.
static const char kYouTubeAds[] = R"JS((function () {
  var host = location.hostname;
  if (!/(^|\.)youtube(-nocookie)?\.com$/.test(host)) return;
  var allowed = %ALLOWED%;
  for (var i = 0; i < allowed.length; i++) if (host === allowed[i] || host.endsWith('.' + allowed[i])) return;
  if (window.__lyraYtAds) return;
  window.__lyraYtAds = 1;
  // The player is told there are no ads (what uBlock's json-prune does).
  var KEYS = ['adPlacements', 'playerAds', 'adSlots', 'adBreakHeartbeatParams'];
  function prune(o) {
    try {
      if (!o || typeof o !== 'object') return o;
      for (var k = 0; k < KEYS.length; k++) if (KEYS[k] in o) delete o[KEYS[k]];
      if (o.playerResponse && typeof o.playerResponse === 'object') prune(o.playerResponse);
      if (Array.isArray(o)) for (var j = 0; j < o.length; j++) if (o[j] && o[j].playerResponse) prune(o[j].playerResponse);
    } catch (e) {}
    return o;
  }
  var parse = JSON.parse;
  JSON.parse = function () { return prune(parse.apply(this, arguments)); };
  var json = Response.prototype.json;
  Response.prototype.json = function () { return json.apply(this, arguments).then(prune); };
  var initial;
  try {
    Object.defineProperty(window, 'ytInitialPlayerResponse', { configurable: true,
      get: function () { return initial; }, set: function (v) { initial = prune(v); } });
  } catch (e) {}
  // An ad that still starts is muted, run to its end and skipped.
  var wasMuted = null;
  setInterval(function () {
    var p = document.querySelector('#movie_player') || document.querySelector('.html5-video-player');
    var v = p && p.querySelector('video');
    if (!v) return;
    if (p.classList.contains('ad-showing') || p.classList.contains('ad-interrupting')) {
      if (wasMuted === null) wasMuted = v.muted;
      v.muted = true;
      if (isFinite(v.duration) && v.duration > 0 && v.currentTime < v.duration - 0.3) v.currentTime = v.duration - 0.1;
      var b = p.querySelector('.ytp-skip-ad-button, .ytp-ad-skip-button, .ytp-ad-skip-button-modern, .ytp-ad-skip-button-slot button');
      if (b) b.click();
    } else if (wasMuted !== null) {
      v.muted = wasMuted;
      wasMuted = null;
    }
  }, 300);
})();)JS";

static std::vector<std::pair<std::string, bool>> s_userScripts;
static void (*s_extraUserScripts)(WKUserContentControllerRef) = nullptr;

static void AddScript(const std::string& source, bool mainFrameOnly) {
    WKStringRef s = WKStringCreateWithUTF8CString(source.c_str());
    WKUserScriptRef script = WKUserScriptCreateWithSource(s, kWKInjectAtDocumentStart, mainFrameOnly);
    WKUserContentControllerAddUserScript(s_controller, script);
    WKRelease(script);
    WKRelease(s);
}

// Pages loaded from now on get the new set.
static void ApplyUserScripts() {
    if (!s_controller) return;
    WKUserContentControllerRemoveAllUserScripts(s_controller);
    for (auto& [source, mainOnly] : s_userScripts) AddScript(source, mainOnly);
    if (s_extraUserScripts) s_extraUserScripts(s_controller);
    if (!g_prefs.adBlock) return;
    std::string allowed = "[";
    for (auto& h : s_allowed) {
        if (allowed.size() > 1) allowed += ",";
        allowed += "\"";
        for (char c : ToUtf8(h)) if (c != '"' && c != '\\') allowed += c;
        allowed += "\"";
    }
    allowed += "]";
    std::string yt = kYouTubeAds;
    const size_t at = yt.find("%ALLOWED%");
    if (at != std::string::npos) yt.replace(at, 9, allowed);
    AddScript(yt, false);
}

void AdBlockSetExtraUserScripts(void (*add)(WKUserContentControllerRef)) {
    s_extraUserScripts = add;
    ApplyUserScripts();
}

void AdBlockRefreshUserScripts() { ApplyUserScripts(); }

void AdBlockAddUserScript(const char* source, bool mainFrameOnly) {
    s_userScripts.emplace_back(source, mainFrameOnly);
    ApplyUserScripts();
}

void AdBlockSetEnabled(bool enabled) {
    g_prefs.adBlock = enabled;
    RefreshController();
    ApplyUserScripts();
    if (enabled) ScheduleUpdates(false);
}

void AdBlockUpdateNow() { ScheduleUpdates(true); }

void AdBlockListsReady(LPARAM lParam) {
    std::unique_ptr<Built> built((Built*)lParam);
    if (!built || !s_store || g_closing) return;
    s_ruleCounts[built->id] = built->rules;
    WKStringRef id = WKStringCreateWithUTF8CString(RuleListId(built->id).c_str());
    WKStringRef json = WKStringCreateWithUTF8CStringWithLength(built->json.data(), built->json.size());
    WKUserContentExtensionStoreCompile(s_store, id, json, new LookupContext { built->id }, OnCompiled);
    WKRelease(json);
    WKRelease(id);
}

bool AdBlockAllowedOn(const std::wstring& url) {
    if (!g_prefs.adBlock) return false;
    std::wstring host = SiteKey(url);
    while (!host.empty()) {
        if (s_allowed.count(host)) return false;
        size_t dot = host.find(L'.');
        if (dot == std::wstring::npos) break;
        host.erase(0, dot + 1);
    }
    return true;
}

void AdBlockSetAllowedOn(const std::wstring& url, bool blocking) {
    std::wstring host = SiteKey(url);
    if (host.empty()) return;
    if (blocking) {
        // Also drop a parent domain entry that covers this site.
        for (auto it = s_allowed.begin(); it != s_allowed.end();) {
            const std::wstring& h = *it;
            bool covers = h == host || (host.size() > h.size() && host.compare(host.size() - h.size(), h.size(), h) == 0
                                        && host[host.size() - h.size() - 1] == L'.');
            it = covers ? s_allowed.erase(it) : std::next(it);
        }
    } else
        s_allowed.insert(host);
    SaveAllowed();
    ApplyUserScripts();
}

std::wstring AdBlockStatusText() {
    if (!g_prefs.adBlock) return Tr(L"Off", L"Выключено");
    ULONGLONG newest = ~0ULL;
    for (auto& l : kLists) {
        bool exists = false;
        ULONGLONG age = FileAge(ListFile(l.id), &exists);
        if (exists) newest = std::min(newest, age);
    }
    wchar_t buf[200];
    if (s_filters.empty())
        return newest == ~0ULL ? Tr(L"Downloading filter lists\x2026", L"Загрузка списков фильтров\x2026")
                               : Tr(L"Preparing filter lists\x2026", L"Подготовка списков фильтров\x2026");
    unsigned hours = newest == ~0ULL ? 0 : (unsigned)(newest / (3600ULL * 10000000ULL));
    if (hours < 1) swprintf(buf, 200, Tr(L"%zu lists active, updated less than an hour ago", L"Списков: %zu, обновлены меньше часа назад"), s_filters.size());
    else if (hours < 48) swprintf(buf, 200, Tr(L"%zu lists active, updated %u h ago", L"Списков: %zu, обновлены %u ч назад"), s_filters.size(), hours);
    else swprintf(buf, 200, Tr(L"%zu lists active, updated %u days ago", L"Списков: %zu, обновлены %u дн. назад"), s_filters.size(), hours / 24);
    return buf;
}
