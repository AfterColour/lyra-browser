// Lyra — разбор адресов: что ввёл пользователь -> URL или поиск, отображение адреса.
#include "core/Url.h"
#include "core/Settings.h"
#include "core/StringUtils.h"

static bool HasUrlScheme(const std::wstring& s);

std::wstring StartPagePath() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash);
    return dir + L"\\resources\\start.html";
}

std::wstring StartPageFileUrl() {
    static std::wstring cached;
    if (cached.empty()) {
        wchar_t url[2084] = {};
        DWORD len = ARRAYSIZE(url);
        if (SUCCEEDED(UrlCreateFromPathW(StartPagePath().c_str(), url, &len, 0))) cached = url;
    }
    return cached;
}

static std::wstring LowerCopy(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}

// WebKit may spell a file URL slightly differently (case, file:/// vs file://): compare the part
// after the scheme, ignoring case and the parameters.
static std::wstring FileUrlTail(std::wstring u) {
    size_t q = u.find_first_of(L"?#");
    if (q != std::wstring::npos) u.resize(q);
    size_t p = 5;
    while (p < u.size() && u[p] == L'/') ++p;
    return LowerCopy(u.substr(std::min(p, u.size())));
}

static std::wstring FileUrlOf(const std::wstring& path) {
    wchar_t url[2084] = {};
    DWORD len = ARRAYSIZE(url);
    return SUCCEEDED(UrlCreateFromPathW(path.c_str(), url, &len, 0)) ? std::wstring(url) : std::wstring();
}

bool IsStartPageUrl(const std::wstring& url) {
    if (url.compare(0, 5, L"file:") != 0) return false;
    static const std::wstring mine = FileUrlTail(StartPageFileUrl());
    return !mine.empty() && FileUrlTail(url) == mine;
}

std::wstring HistoryPagePath() { return g_settingsDir + L"\\history.html"; }

bool IsHistoryPageUrl(const std::wstring& url) {
    if (url.compare(0, 5, L"file:") != 0) return false;
    const std::wstring mine = FileUrlTail(FileUrlOf(HistoryPagePath()));
    return !mine.empty() && FileUrlTail(url) == mine;
}

// "https://www.example.com/a?b" -> "example.com"
std::wstring HostOf(const std::wstring& url) {
    if (url.empty() || url == L"about:blank" || IsInternalPageUrl(url)) return std::wstring();
    size_t p = url.find(L"://");
    if (p == std::wstring::npos) return url;
    std::wstring scheme = url.substr(0, p);
    if (scheme != L"http" && scheme != L"https") return url;
    std::wstring rest = url.substr(p + 3);
    size_t e = rest.find_first_of(L"/?#");
    if (e != std::wstring::npos) rest.resize(e);
    size_t at = rest.rfind(L'@');
    if (at != std::wstring::npos) rest = rest.substr(at + 1);
    if (rest.compare(0, 4, L"www.") == 0) rest = rest.substr(4);
    return rest;
}

// "https://example.com/path" -> "example.com/path"
std::wstring DisplayUrl(const std::wstring& url) {
    if (url.empty() || url == L"about:blank" || IsInternalPageUrl(url)) return std::wstring();
    std::wstring s = url;
    if (s.compare(0, 8, L"https://") == 0) s = s.substr(8);
    else if (s.compare(0, 7, L"http://") == 0) s = s.substr(7);
    if (!s.empty() && s.back() == L'/' && s.find(L'/') == s.size() - 1) s.pop_back();
    return s;
}

// "https://...", "file:///...", "ftp://..." -> true. Only a real scheme counts, so a search
// such as "what does :// mean" is not mistaken for an address.
static bool HasUrlScheme(const std::wstring& s) {
    size_t p = s.find(L"://");
    if (p == std::wstring::npos || p == 0) return false;
    if (!iswalpha(s[0])) return false;
    for (size_t i = 1; i < p; ++i)
        if (!(iswalnum(s[i]) || s[i] == L'+' || s[i] == L'-' || s[i] == L'.')) return false;
    return true;
}

// Turn whatever the user typed into a URL (or a search query).
std::string ResolveInput(const std::wstring& input) {
    std::wstring s = TrimWs(input);
    if (s.empty()) return std::string();
    std::string u = ToUtf8(s);
    const bool hasSpace = s.find(L' ') != std::wstring::npos;

    if (!hasSpace && HasUrlScheme(s)) return u;
    if (s.compare(0, 6, L"about:") == 0 || s.compare(0, 5, L"data:") == 0) return u;
    // "javascript:" typed into the address bar is a classic self-XSS trick: search instead.

    // A local file path: C:\folder\page.html or \\server\share\page.html
    if ((s.size() > 2 && iswalpha(s[0]) && s[1] == L':' && (s[2] == L'\\' || s[2] == L'/')) ||
        s.compare(0, 2, L"\\\\") == 0) {
        wchar_t url[2084] = {};
        DWORD len = ARRAYSIZE(url);
        if (SUCCEEDED(UrlCreateFromPathW(s.c_str(), url, &len, 0))) return ToUtf8(url);
    }

    if (!hasSpace) {
        bool local = s.compare(0, 9, L"localhost") == 0;
        bool ipLike = s.find_first_not_of(L"0123456789.:/") == std::wstring::npos &&
                      s.find(L'.') != std::wstring::npos;
        if (local || ipLike) return "http://" + u;
        if (s.find(L'.') != std::wstring::npos && s[0] != L'.' && s.back() != L'.') return "https://" + u;
    }
    return ToUtf8(g_searchEngine) + UrlEncode(u);
}
