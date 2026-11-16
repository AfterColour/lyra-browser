// Lyra — режим чтения: статья без рекламы и меню, удобный шрифт и тема (Mozilla Readability).
#include "browser/Reader.h"
#include "app/Log.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "core/Localization.h"
#include "core/Settings.h"
#include "core/StringUtils.h"
#include "core/Url.h"
#include "ui/Chrome.h"
#include "ui/Theme.h"

#include <fstream>
#include <iterator>
#include <sstream>

namespace {

std::map<std::wstring, std::wstring> s_sources;     // reader file name -> the article's address
unsigned s_counter = 0;
bool s_busy = false;

std::wstring ReaderDir() { return g_settingsDir + L"\\Reader"; }

std::string ReadResource(const wchar_t* name) {
    std::wstring path = StartPagePath();                 // <Lyra>\resources\start.html
    path = path.substr(0, path.find_last_of(L'\\') + 1) + name;
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Readability wrapped so nothing it defines lands in the page; `tail` is the function body's end.
// `probeOnly`: just isProbablyReaderable (the small part before the LYRA-SPLIT line).
std::string WrappedReadability(const char* tail, bool probeOnly = false) {
    static std::string source, probe;
    if (source.empty()) {
        source = ReadResource(L"readability.js");
        const size_t split = source.find("/* LYRA-SPLIT */");
        probe = split == std::string::npos ? source : source.substr(0, split);
    }
    const std::string& code = probeOnly ? probe : source;
    if (code.empty()) return std::string();
    return "(function () { var module; \n" + code + "\n" + tail + "\n})()";
}

std::wstring FileName(const std::wstring& url) {
    const size_t slash = url.find_last_of(L"/\\");
    std::wstring name = slash == std::wstring::npos ? url : url.substr(slash + 1);
    const size_t q = name.find_first_of(L"?#");
    if (q != std::wstring::npos) name.resize(q);
    return name;
}

Tab* TabById(unsigned long long id) {
    for (auto& t : g_tabs) if (t->id == id && t->state == TabState::Live) return t.get();
    return nullptr;
}

void OnProbe(WKTypeRef result, WKErrorRef, void* ctx) {
    Tab* t = TabById((unsigned long long)(uintptr_t)ctx);
    if (!t) return;
    const bool yes = result && WKGetTypeID(result) == WKStringGetTypeID() && WKStr((WKStringRef)result) == L"1";
    if (yes != t->readerable) { t->readerable = yes; InvalidateChrome(); }
}

std::string JsonForScriptTag(std::string json) {
    // Inside <script type="application/json"> only "</" could end the element early.
    std::string out;
    out.reserve(json.size() + 64);
    for (char c : json) { if (c == '<') out += "\\u003c"; else out += c; }
    return out;
}

void OnArticle(WKTypeRef result, WKErrorRef, void* ctx) {
    s_busy = false;
    Tab* t = TabById((unsigned long long)(uintptr_t)ctx);
    if (!t || !t->page) return;
    std::string json;
    if (result && WKGetTypeID(result) == WKStringGetTypeID()) json = WKStrUtf8((WKStringRef)result);
    if (json.empty()) { Log("reader: no article on tab#%llu", t->id); MessageBeep(MB_ICONWARNING); return; }

    std::string page = ReadResource(L"reader.html");
    if (page.empty()) { Log("reader: resources\\reader.html missing"); return; }
    wchar_t accent[8];
    swprintf(accent, 8, L"%02X%02X%02X", g_th.accent.GetR(), g_th.accent.GetG(), g_th.accent.GetB());
    std::ostringstream prefs;
    prefs << "{\"size\":" << g_prefs.readerSize << ",\"font\":" << g_prefs.readerFont << ",\"theme\":" << g_prefs.readerTheme
          << ",\"width\":" << g_prefs.readerWidth << ",\"dark\":" << (g_dark ? 1 : 0) << ",\"ru\":" << (UiRussian() ? 1 : 0)
          << ",\"accent\":\"" << ToUtf8(accent) << "\"}";
    auto put = [&page](const char* key, const std::string& value) {
        const size_t at = page.find(key);
        if (at != std::string::npos) page.replace(at, strlen(key), value);
    };
    put("%LYRA_ARTICLE%", JsonForScriptTag(json));
    put("%LYRA_PREFS%", prefs.str());

    CreateDirectoryW(ReaderDir().c_str(), nullptr);
    const std::wstring name = L"tab" + std::to_wstring(t->id) + L"-" + std::to_wstring(++s_counter) + L".html";
    const std::wstring path = ReaderDir() + L"\\" + name;
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(page.data(), (std::streamsize)page.size());
    }
    wchar_t url[2048];
    DWORD len = ARRAYSIZE(url);
    if (FAILED(UrlCreateFromPathW(path.c_str(), url, &len, 0))) return;
    s_sources[name] = t->url;
    if (WKURLRef u = WKURLCreateWithUTF8CString(ToUtf8(url).c_str())) {
        WKPageLoadURL(t->page, u);
        WKRelease(u);
    }
    Log("reader: tab#%llu in reader view", t->id);
}

void ExitReader(Tab& t) {
    if (WKPageCanGoBack(t.page)) { WKPageGoBack(t.page); return; }
    const std::wstring source = ReaderShownUrl(t.url);
    if (source == t.url) return;
    if (WKURLRef u = WKURLCreateWithUTF8CString(ToUtf8(source).c_str())) {
        WKPageLoadURL(t.page, u);
        WKRelease(u);
    }
}

} // namespace

void ReaderInit() {
    // Reader pages of the last run: their tabs are gone (a restored one shows the original page).
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((ReaderDir() + L"\\*.html").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do { DeleteFileW((ReaderDir() + L"\\" + fd.cFileName).c_str()); } while (FindNextFileW(h, &fd));
    FindClose(h);
}

bool ReaderIsReaderUrl(const std::wstring& url) {
    if (url.compare(0, 5, L"file:") != 0) return false;
    return s_sources.count(FileName(url)) != 0;
}

std::wstring ReaderShownUrl(const std::wstring& url) {
    if (url.compare(0, 5, L"file:") != 0) return url;
    auto it = s_sources.find(FileName(url));
    return it == s_sources.end() ? url : it->second;
}

void ReaderPageLoaded(Tab& t) {
    if (!t.page) return;
    if (t.url.compare(0, 7, L"http://") != 0 && t.url.compare(0, 8, L"https://") != 0) {
        if (t.readerable) { t.readerable = false; InvalidateChrome(); }
        return;
    }
    const std::string js = WrappedReadability("return isProbablyReaderable(document) ? '1' : '';", true);
    if (js.empty()) return;
    WKStringRef s = WKStringCreateWithUTF8CString(js.c_str());
    WKPageEvaluateJavaScriptInMainFrame(t.page, s, (void*)(uintptr_t)t.id, OnProbe);
    WKRelease(s);
}

void ReaderToggle() {
    Tab* t = ActiveTab();
    if (!t || !t->page || s_busy) return;
    if (ReaderIsReaderUrl(t->url)) { ExitReader(*t); return; }
    if (t->url.compare(0, 7, L"http://") != 0 && t->url.compare(0, 8, L"https://") != 0) { MessageBeep(MB_ICONWARNING); return; }
    const std::string js = WrappedReadability(
        "var a = new Readability(document.cloneNode(true), { charThreshold: 300 }).parse();"
        "if (!a || !a.content) return '';"
        "return JSON.stringify({ title: a.title, byline: a.byline, site: a.siteName, content: a.content, length: a.length,"
        "  lang: a.lang || document.documentElement.lang, dir: a.dir, url: location.href });");
    if (js.empty()) return;
    s_busy = true;
    WKStringRef s = WKStringCreateWithUTF8CString(js.c_str());
    WKPageEvaluateJavaScriptInMainFrame(t->page, s, (void*)(uintptr_t)t->id, OnArticle);
    WKRelease(s);
}

void ReaderLyraUrl(unsigned long long tabId, const std::wstring& command, const std::wstring& query) {
    if (command == L"lyra://reader-exit") {
        if (Tab* t = TabById(tabId)) ExitReader(*t);
        return;
    }
    if (command == L"lyra://reader-prefs") {             // the reader page's Aa panel
        size_t p = 0;
        while (p < query.size()) {
            size_t e = query.find(L'&', p);
            if (e == std::wstring::npos) e = query.size();
            const std::wstring pair = query.substr(p, e - p);
            const size_t eq = pair.find(L'=');
            if (eq != std::wstring::npos) {
                const std::wstring k = pair.substr(0, eq);
                const int v = _wtoi(pair.c_str() + eq + 1);
                if (k == L"size") g_prefs.readerSize = std::min(30, std::max(14, v));
                else if (k == L"font") g_prefs.readerFont = v ? 1 : 0;
                else if (k == L"theme") g_prefs.readerTheme = std::min(3, std::max(0, v));
                else if (k == L"width") g_prefs.readerWidth = std::min(2, std::max(0, v));
            }
            p = e + 1;
        }
        SaveSettings();
    }
}
