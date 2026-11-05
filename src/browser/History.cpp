// Lyra — история посещений: запись, поиск, часто посещаемые сайты, сохранение на диск.
#include "browser/History.h"
#include "app/Log.h"
#include "core/Settings.h"
#include "core/StringUtils.h"
#include "core/Url.h"

#include <unordered_map>

static std::unordered_map<std::wstring, HistoryEntry> s_entries;
static bool s_loaded = false, s_dirty = false;
static int  s_saveCountdown = 0;
constexpr size_t kMaxEntries = 10000;
constexpr long long kDayMs = 24LL * 60 * 60 * 1000;
constexpr long long kKeepMs = 180 * kDayMs;          // forget addresses not visited for half a year

long long NowUnixMs() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (long long)(u.QuadPart / 10000ULL) - 11644473600000LL;   // 1601 -> 1970
}

static std::wstring HistoryPath() { return g_settingsDir + L"\\history.tsv"; }

// Titles of Lyra's own empty pages that older builds could record for a site by mistake.
static bool PlaceholderTitle(const std::wstring& t) {
    return t == L"New Tab" || t == L"\x041D\x043E\x0432\x0430\x044F \x0432\x043A\x043B\x0430\x0434\x043A\x0430";   // "Новая вкладка"
}

static std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}

// "https://www.example.com/a" -> "example.com/a"
static std::wstring Bare(const std::wstring& url) {
    std::wstring s = url;
    size_t p = s.find(L"://");
    if (p != std::wstring::npos) s = s.substr(p + 3);
    if (s.compare(0, 4, L"www.") == 0) s = s.substr(4);
    if (!s.empty() && s.back() == L'/') s.pop_back();
    return Lower(s);
}

bool HistoryRecordable(const std::wstring& url) {
    if (!g_prefs.recordHistory) return false;
    if (url.compare(0, 7, L"http://") != 0 && url.compare(0, 8, L"https://") != 0) return false;
    return url.size() < 2048;
}

static void Prune() {
    const long long cutoff = NowUnixMs() - kKeepMs;
    for (auto it = s_entries.begin(); it != s_entries.end();) {
        if (it->second.lastVisit < cutoff) it = s_entries.erase(it);
        else ++it;
    }
    if (s_entries.size() <= kMaxEntries) return;
    std::vector<std::pair<long long, std::wstring>> byAge;
    for (auto& [url, e] : s_entries) byAge.emplace_back(e.lastVisit, url);
    std::sort(byAge.begin(), byAge.end());
    for (size_t i = 0; i + kMaxEntries < byAge.size(); ++i) s_entries.erase(byAge[i].second);
}

void HistoryLoad() {
    if (s_loaded) return;
    s_loaded = true;
    HANDLE f = CreateFileW(HistoryPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size = {};
    GetFileSizeEx(f, &size);
    std::string data((size_t)std::min<long long>(size.QuadPart, 64LL << 20), '\0');
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
        // lastVisit \t visits \t url \t title
        size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1),
               c = b == std::string::npos ? b : line.find('\t', b + 1);
        if (c == std::string::npos) continue;
        HistoryEntry e;
        e.lastVisit = _atoi64(line.substr(0, a).c_str());
        e.visits = atoi(line.substr(a + 1, b - a - 1).c_str());
        e.url = ToWide(line.substr(b + 1, c - b - 1));
        e.title = ToWide(line.substr(c + 1));
        if (PlaceholderTitle(e.title)) e.title.clear();
        if (!e.url.empty() && e.visits > 0) s_entries[e.url] = std::move(e);
    }
    Prune();
    Log("history: %d entries", (int)s_entries.size());
}

void HistorySave() {
    if (!s_dirty) return;
    s_dirty = false;
    std::string out;
    out.reserve(s_entries.size() * 120);
    for (auto& [url, e] : s_entries) {
        std::wstring title = e.title;
        for (auto& ch : title) if (ch == L'\t' || ch == L'\r' || ch == L'\n') ch = L' ';
        out += std::to_string(e.lastVisit) + "\t" + std::to_string(e.visits) + "\t" + ToUtf8(url) + "\t" + ToUtf8(title) + "\n";
    }
    // Write a new file and swap it in, so a crash never leaves a half-written history.
    const std::wstring path = HistoryPath(), tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { s_dirty = true; return; }
    DWORD written = 0;
    const BOOL ok = out.empty() || WriteFile(f, out.data(), (DWORD)out.size(), &written, nullptr);
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        s_dirty = true;
    }
}

void HistorySaveSoon() {
    if (!s_dirty) { s_saveCountdown = 0; return; }
    if (++s_saveCountdown >= 1000) {          // TIMER_UI runs every 30 ms -> ~30 s
        s_saveCountdown = 0;
        HistorySave();
    }
}

void HistoryVisit(const std::wstring& url, const std::wstring& title) {
    if (!HistoryRecordable(url)) return;
    HistoryLoad();
    HistoryEntry& e = s_entries[url];
    if (e.url.empty()) e.url = url;
    e.visits += 1;
    e.lastVisit = NowUnixMs();
    if (!title.empty()) e.title = title;
    s_dirty = true;
    if (s_entries.size() > kMaxEntries + 500) Prune();
}

void HistorySetTitle(const std::wstring& url, const std::wstring& title) {
    if (title.empty() || PlaceholderTitle(title)) return;
    auto it = s_entries.find(url);
    if (it == s_entries.end() || it->second.title == title) return;
    it->second.title = title;
    s_dirty = true;
}

void HistoryRemove(const std::wstring& url) {
    HistoryLoad();
    if (s_entries.erase(url)) s_dirty = true;
}

void HistoryClearSince(long long sinceMs) {
    HistoryLoad();
    for (auto it = s_entries.begin(); it != s_entries.end();) {
        if (it->second.lastVisit >= sinceMs) it = s_entries.erase(it);
        else ++it;
    }
    s_dirty = true;
    HistorySave();
}

std::vector<HistoryEntry> HistoryNewest(size_t max) {
    HistoryLoad();
    std::vector<HistoryEntry> v;
    v.reserve(s_entries.size());
    for (auto& [url, e] : s_entries) v.push_back(e);
    std::sort(v.begin(), v.end(), [](const HistoryEntry& a, const HistoryEntry& b) { return a.lastVisit > b.lastVisit; });
    if (v.size() > max) v.resize(max);
    return v;
}

// Visits weighted by how recent they are ("frecency").
static double Frecency(const HistoryEntry& e, long long now) {
    const double ageDays = std::max(0.0, (double)(now - e.lastVisit) / (double)kDayMs);
    return (double)e.visits / (1.0 + ageDays / 7.0);
}

std::vector<HistoryEntry> HistoryMatch(const std::wstring& text, size_t max) {
    HistoryLoad();
    const std::wstring q = Lower(TrimWs(text));
    if (q.empty()) return {};
    // Words of the query must all appear in the address or the title.
    std::vector<std::wstring> words;
    for (size_t p = 0; p < q.size();) {
        size_t e = q.find(L' ', p);
        if (e == std::wstring::npos) e = q.size();
        if (e > p) words.push_back(q.substr(p, e - p));
        p = e + 1;
    }
    const long long now = NowUnixMs();
    std::vector<std::pair<double, const HistoryEntry*>> scored;
    for (auto& [url, e] : s_entries) {
        const std::wstring bare = Bare(url), title = Lower(e.title);
        double score = 0;
        if (bare.compare(0, q.size(), q) == 0) score = 1000;                  // typed the start of the address
        else {
            bool all = true;
            for (auto& w : words)
                if (bare.find(w) == std::wstring::npos && title.find(w) == std::wstring::npos) { all = false; break; }
            if (!all) continue;
            score = 100;
        }
        score += Frecency(e, now) * 10.0;
        if (bare.size() < 40) score += (40 - (double)bare.size()) * 0.5;      // prefer short, "home" addresses
        scored.emplace_back(score, &e);
    }
    std::sort(scored.begin(), scored.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::vector<HistoryEntry> out;
    for (auto& s : scored) {
        if (out.size() >= max) break;
        out.push_back(*s.second);
    }
    return out;
}

std::vector<HistoryEntry> HistoryTopSites(size_t max) {
    HistoryLoad();
    struct Site { double score = 0; HistoryEntry best; double bestScore = -1; bool haveRoot = false; };
    std::unordered_map<std::wstring, Site> sites;
    const long long now = NowUnixMs();
    for (auto& [url, e] : s_entries) {
        const std::wstring host = HostOf(url);
        if (host.empty()) continue;
        Site& s = sites[host];
        const double f = Frecency(e, now);
        s.score += f;
        // Prefer the site's front page as its tile; otherwise its most visited page.
        const size_t slash = url.find(L'/', url.find(L"://") + 3);
        const bool root = slash == std::wstring::npos || slash + 1 >= url.size();
        if ((root && !s.haveRoot) || (root == s.haveRoot && f > s.bestScore)) {
            s.best = e;
            s.bestScore = f;
            s.haveRoot = s.haveRoot || root;
        }
    }
    std::vector<std::pair<double, HistoryEntry>> v;
    for (auto& [host, s] : sites) {
        HistoryEntry e = s.best;
        if (!s.haveRoot) {                       // show the site, not one of its pages
            const size_t slash = e.url.find(L'/', e.url.find(L"://") + 3);
            if (slash != std::wstring::npos) e.url.resize(slash + 1);
            e.title = host;
        }
        v.emplace_back(s.score, e);
    }
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::vector<HistoryEntry> out;
    for (auto& p : v) {
        if (out.size() >= max) break;
        out.push_back(p.second);
    }
    return out;
}
