// Lyra — перевод страниц: Google (без ключа) или DeepL (свой ключ API).
#include "browser/Translate.h"
#include "app/Log.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "core/Constants.h"
#include "core/Localization.h"
#include "core/Settings.h"
#include "core/StringUtils.h"
#include "ui/Chrome.h"
#include "ui/Dialogs.h"
#include "ui/MainWindow.h"

#include <winhttp.h>
#include <wincrypt.h>
#include <dpapi.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32")

#include <fstream>
#include <iterator>
#include <thread>

namespace {

// ---------------------------------------------------------------------------------------------
//  The page side. __lyraTr.collect(max) hands out text nodes not seen yet as [[id, text], ...];
//  apply() writes translations back (keeping the original's surrounding spaces); restore() puts
//  every original back. Code, inputs, notranslate / translate="no" parts are left alone.
// ---------------------------------------------------------------------------------------------
const char kInstall[] = R"JS(if (!window.__lyraTr) (function () {
  var nodes = [], orig = [];
  var SKIP = /^(SCRIPT|STYLE|NOSCRIPT|CODE|PRE|TEXTAREA|INPUT|SELECT|OPTION|SVG|MATH|KBD|SAMP|VAR|TEMPLATE|IFRAME)$/;
  function skipped(el) {
    for (var e = el; e && e !== document.documentElement; e = e.parentElement) {
      if (SKIP.test(e.tagName) || e.isContentEditable || e.getAttribute('translate') === 'no' ||
          (e.classList && e.classList.contains('notranslate'))) return true;
    }
    return false;
  }
  Object.defineProperty(window, '__lyraTr', { value: {
    collect: function (max) {
      var out = [], total = 0, w = document.createTreeWalker(document.body || document.documentElement, NodeFilter.SHOW_TEXT), n;
      while ((n = w.nextNode())) {
        if (n.__lyraTr) continue;
        n.__lyraTr = 1;
        var t = n.nodeValue, s = t.trim();
        if (!s || !/\p{L}/u.test(s) || skipped(n.parentElement)) continue;
        nodes.push(n); orig.push(t);
        out.push([nodes.length - 1, s]);
        total += s.length;
        if (total > max) break;
      }
      if (document.title && !document.__lyraTrTitle) { document.__lyraTrTitle = document.title; out.push([-1, document.title]); }
      return JSON.stringify(out);
    },
    apply: function (list) {
      list.forEach(function (p) {
        if (p[0] === -1) { document.title = p[1]; return; }
        var n = nodes[p[0]], t = orig[p[0]];
        if (!n || t == null) return;
        n.nodeValue = t.match(/^\s*/)[0] + p[1] + t.match(/\s*$/)[0];
      });
    },
    restore: function () {
      nodes.forEach(function (n, i) { n.nodeValue = orig[i]; n.__lyraTr = 0; });
      nodes = []; orig = [];
      var w = document.createTreeWalker(document.body || document.documentElement, NodeFilter.SHOW_TEXT), n;
      while ((n = w.nextNode())) n.__lyraTr = 0;
      if (document.__lyraTrTitle) { document.title = document.__lyraTrTitle; document.__lyraTrTitle = null; }
    }
  }});
})();)JS";

// lang attribute | share of Cyrillic letters | share of Latin letters | letters seen
const char kProbe[] = R"JS((function () {
  var text = ((document.body && document.body.innerText) || '').slice(0, 4000), cyr = 0, lat = 0, all = 0;
  for (var i = 0; i < text.length; i++) {
    var c = text[i];
    if (/\p{L}/u.test(c)) { all++; if (/\p{Script=Cyrillic}/u.test(c)) cyr++; else if (/\p{Script=Latin}/u.test(c)) lat++; }
  }
  return (document.documentElement.lang || '').toLowerCase() + '|' + (all ? cyr / all : 0).toFixed(2) + '|' +
         (all ? lat / all : 0).toFixed(2) + '|' + all;
})())JS";

// ---------------------------------------------------------------------------------------------
//  A little JSON reader for the services' answers and the page's lists.
// ---------------------------------------------------------------------------------------------
struct JVal {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    double num = 0;
    std::wstring str;
    std::vector<JVal> arr;
    std::vector<std::pair<std::wstring, JVal>> obj;
    const JVal* get(const wchar_t* key) const {
        for (auto& [k, v] : obj) if (k == key) return &v;
        return nullptr;
    }
};

struct JParser {
    const std::wstring& s;
    size_t i = 0;
    bool ok = true;
    void ws() { while (i < s.size() && (s[i] == L' ' || s[i] == L'\n' || s[i] == L'\r' || s[i] == L'\t')) ++i; }
    JVal value() {
        ws();
        JVal v;
        if (i >= s.size()) { ok = false; return v; }
        const wchar_t c = s[i];
        if (c == L'"') { v.type = JVal::Str; v.str = string(); }
        else if (c == L'[') {
            v.type = JVal::Arr; ++i; ws();
            if (i < s.size() && s[i] == L']') { ++i; return v; }
            while (ok) {
                v.arr.push_back(value()); ws();
                if (i < s.size() && s[i] == L',') { ++i; continue; }
                if (i < s.size() && s[i] == L']') { ++i; break; }
                ok = false;
            }
        } else if (c == L'{') {
            v.type = JVal::Obj; ++i; ws();
            if (i < s.size() && s[i] == L'}') { ++i; return v; }
            while (ok) {
                ws();
                if (i >= s.size() || s[i] != L'"') { ok = false; break; }
                std::wstring k = string(); ws();
                if (i >= s.size() || s[i] != L':') { ok = false; break; }
                ++i;
                v.obj.emplace_back(std::move(k), value()); ws();
                if (i < s.size() && s[i] == L',') { ++i; continue; }
                if (i < s.size() && s[i] == L'}') { ++i; break; }
                ok = false;
            }
        } else if (s.compare(i, 4, L"true") == 0) { v.type = JVal::Bool; v.num = 1; i += 4; }
        else if (s.compare(i, 5, L"false") == 0) { v.type = JVal::Bool; i += 5; }
        else if (s.compare(i, 4, L"null") == 0) { i += 4; }
        else {
            size_t start = i;
            while (i < s.size() && (iswdigit(s[i]) || s[i] == L'-' || s[i] == L'+' || s[i] == L'.' || s[i] == L'e' || s[i] == L'E')) ++i;
            if (start == i) { ok = false; return v; }
            v.type = JVal::Num; v.num = _wtof(s.substr(start, i - start).c_str());
        }
        return v;
    }
    std::wstring string() {
        std::wstring out;
        ++i;                                             // opening quote
        while (i < s.size() && s[i] != L'"') {
            wchar_t c = s[i++];
            if (c != L'\\') { out += c; continue; }
            if (i >= s.size()) break;
            const wchar_t e = s[i++];
            switch (e) {
            case L'n': out += L'\n'; break;
            case L't': out += L'\t'; break;
            case L'r': out += L'\r'; break;
            case L'b': out += L'\b'; break;
            case L'f': out += L'\f'; break;
            case L'u':
                if (i + 4 <= s.size()) { out += (wchar_t)wcstoul(s.substr(i, 4).c_str(), nullptr, 16); i += 4; }
                break;
            default: out += e;
            }
        }
        ++i;                                             // closing quote
        return out;
    }
};

bool ParseJson(const std::wstring& text, JVal& out) {
    JParser p{ text };
    out = p.value();
    return p.ok;
}

// ---------------------------------------------------------------------------------------------
//  Services (worker thread)
// ---------------------------------------------------------------------------------------------
std::string HttpPost(const wchar_t* host, const std::wstring& path, const std::string& body,
                     const std::wstring& headers, DWORD& status) {
    status = 0;
    std::string out;
    HINTERNET session = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) Lyra",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return out;
    WinHttpSetTimeouts(session, 10000, 10000, 15000, 30000);
    HINTERNET connect = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    std::wstring h = L"Content-Type: application/x-www-form-urlencoded\r\n" + headers;
    if (request && WinHttpSendRequest(request, h.c_str(), (DWORD)-1L, (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(request, &avail) && avail) {
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, &chunk[0], avail, &read) || !read) break;
            out.append(chunk.data(), read);
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return out;
}

struct Batch {
    unsigned long long tabId = 0;
    unsigned gen = 0;
    std::vector<int> ids;
    std::vector<std::wstring> texts;
};

struct Result {
    unsigned long long tabId = 0;
    unsigned gen = 0;
    std::vector<int> ids;
    std::vector<std::wstring> texts;       // same order; empty when the batch failed
    std::wstring error;
};

std::wstring s_deeplKey;                    // decrypted copy (Settings)

bool TranslateGoogle(const Batch& b, const std::string& target, std::vector<std::wstring>& out, std::wstring& error) {
    std::string body;
    for (auto& t : b.texts) body += (body.empty() ? "q=" : "&q=") + UrlEncode(ToUtf8(t));
    DWORD status = 0;
    const std::string answer = HttpPost(L"translate.googleapis.com",
        L"/translate_a/t?client=gtx&sl=auto&tl=" + ToWide(target) + L"&format=text", body, L"", status);
    if (status != 200) { error = L"Google: HTTP " + std::to_wstring(status); return false; }
    JVal v;
    if (!ParseJson(ToWide(answer), v) || v.type != JVal::Arr) { error = L"Google: unexpected answer"; return false; }
    if (b.texts.size() == 1) {                           // one text: ["translation", "lang"]
        if (!v.arr.empty() && v.arr[0].type == JVal::Str) { out.push_back(v.arr[0].str); return true; }
    }
    for (auto& item : v.arr) {                           // several: [["translation", "lang"], ...] or ["t1", "t2"]
        if (item.type == JVal::Str) out.push_back(item.str);
        else if (item.type == JVal::Arr && !item.arr.empty() && item.arr[0].type == JVal::Str) out.push_back(item.arr[0].str);
        else out.push_back(std::wstring());
    }
    if (out.size() != b.texts.size()) { error = L"Google: unexpected answer"; return false; }
    return true;
}

bool TranslateDeepL(const Batch& b, const std::string& target, const std::wstring& key, std::vector<std::wstring>& out, std::wstring& error) {
    if (key.empty()) { error = Tr(L"DeepL: no API key (Settings > General > Translation)", L"DeepL: не указан ключ API (Настройки > Общие > Перевод)"); return false; }
    std::string body = "target_lang=" + target;
    for (auto& t : b.texts) body += "&text=" + UrlEncode(ToUtf8(t));
    const bool free = key.size() > 3 && key.compare(key.size() - 3, 3, L":fx") == 0;
    DWORD status = 0;
    const std::string answer = HttpPost(free ? L"api-free.deepl.com" : L"api.deepl.com", L"/v2/translate", body,
                                        L"Authorization: DeepL-Auth-Key " + key + L"\r\n", status);
    if (status == 403) { error = Tr(L"DeepL did not accept the API key.", L"DeepL не принял ключ API."); return false; }
    if (status == 456) { error = Tr(L"The DeepL character limit for this month is used up.", L"Лимит символов DeepL на этот месяц исчерпан."); return false; }
    if (status != 200) { error = L"DeepL: HTTP " + std::to_wstring(status); return false; }
    JVal v;
    const JVal* list = nullptr;
    if (ParseJson(ToWide(answer), v)) list = v.get(L"translations");
    if (!list || list->type != JVal::Arr || list->arr.size() != b.texts.size()) { error = L"DeepL: unexpected answer"; return false; }
    for (auto& item : list->arr) {
        const JVal* text = item.get(L"text");
        out.push_back(text ? text->str : std::wstring());
    }
    return true;
}

void Worker(std::vector<Batch> batches, int service, std::string target, std::wstring key) {
    for (auto& b : batches) {
        auto* r = new Result;
        r->tabId = b.tabId;
        r->gen = b.gen;
        r->ids = b.ids;
        const bool ok = service == 1 ? TranslateDeepL(b, target, key, r->texts, r->error)
                                     : TranslateGoogle(b, target, r->texts, r->error);
        if (!ok) r->texts.clear();
        if (!PostMessageW(g_hWnd, WM_APP_TRANSLATE, 0, (LPARAM)r)) { delete r; return; }
        if (!ok) return;                                  // the rest would fail the same way
    }
}

// ---------------------------------------------------------------------------------------------
//  Main thread
// ---------------------------------------------------------------------------------------------
const std::vector<TranslateLanguage> kLanguages = {
    { "", "", L"" },                                     // Lyra's language (filled in by TranslateTarget)
    { "ru", "RU", L"Русский" }, { "en", "EN-US", L"English" },
    { "uk", "UK", L"Українська" }, { "de", "DE", L"Deutsch" },
    { "fr", "FR", L"Français" }, { "es", "ES", L"Español" }, { "it", "IT", L"Italiano" }, { "pl", "PL", L"Polski" },
    { "pt", "PT-BR", L"Português" }, { "tr", "TR", L"Türkçe" }, { "ja", "JA", L"日本語" },
    { "zh-CN", "ZH", L"中文" }, { "ko", "KO", L"한국어" },
};

const TranslateLanguage& Target() {
    int i = g_prefs.translateTarget;
    if (i <= 0 || i >= (int)kLanguages.size()) i = UiRussian() ? 1 : 2;
    return kLanguages[i];
}

std::string Primary(const std::string& lang) {
    const size_t dash = lang.find_first_of("-_");
    return dash == std::string::npos ? lang : lang.substr(0, dash);
}

Tab* TabById(unsigned long long id) {
    for (auto& t : g_tabs) if (t->id == id && t->state == TabState::Live) return t.get();
    return nullptr;
}

bool WebPage(const Tab& t) {
    return t.url.compare(0, 7, L"http://") == 0 || t.url.compare(0, 8, L"https://") == 0;
}

void OnProbe(WKTypeRef result, WKErrorRef, void* ctx) {
    Tab* t = TabById((unsigned long long)(uintptr_t)ctx);
    if (!t || !result || WKGetTypeID(result) != WKStringGetTypeID()) return;
    std::string s = ToUtf8(WKStr((WKStringRef)result));
    std::string f[4];
    for (int k = 0, p = 0; k < 4; ++k) {
        size_t e = s.find('|', p);
        f[k] = s.substr(p, e == std::string::npos ? std::string::npos : e - p);
        p = e == std::string::npos ? (int)s.size() : (int)e + 1;
    }
    const std::string lang = Primary(f[0]), target = Primary(Target().google);
    const double cyr = atof(f[1].c_str()), lat = atof(f[2].c_str());
    const int letters = atoi(f[3].c_str());
    const bool cyrTarget = target == "ru" || target == "uk";
    const char* script = cyr > 0.4 ? "cyrl" : lat > 0.4 ? "latn" : "other";
    bool offer = false;
    if (letters >= 80) {
        if (cyrTarget) offer = strcmp(script, "cyrl") != 0 || (!lang.empty() && (lang == "ru" || lang == "uk") && lang != target);
        else offer = strcmp(script, "latn") != 0 || (!lang.empty() && lang != target);
    }
    if (offer != t->translatable) { t->translatable = offer; InvalidateChrome(); }
    if (offer && t->autoTranslate && !t->translated) {
        t->translated = true;
        InvalidateChrome();
    }
}

void OnCollected(WKTypeRef result, WKErrorRef, void* ctx);

void Collect(Tab& t, int maxChars) {
    if (!t.page || t.translateCollecting) return;
    t.translateCollecting = true;
    const std::string js = std::string(kInstall) + "\n__lyraTr.collect(" + std::to_string(maxChars) + ");";
    WKStringRef s = WKStringCreateWithUTF8CString(js.c_str());
    WKPageEvaluateJavaScriptInMainFrame(t.page, s, (void*)(uintptr_t)((t.id << 16) | (t.translateGen & 0xFFFF)), OnCollected);
    WKRelease(s);
}

void OnCollected(WKTypeRef result, WKErrorRef, void* ctx) {
    const uintptr_t v = (uintptr_t)ctx;
    Tab* t = TabById(v >> 16);
    if (!t) return;
    t->translateCollecting = false;
    if ((t->translateGen & 0xFFFF) != (v & 0xFFFF) || !t->translated) return;
    if (!result || WKGetTypeID(result) != WKStringGetTypeID()) return;
    JVal list;
    if (!ParseJson(WKStr((WKStringRef)result), list) || list.type != JVal::Arr || list.arr.empty()) return;

    // Batches of up to 60 texts / about 3000 characters each.
    std::vector<Batch> batches;
    Batch cur;
    size_t chars = 0;
    for (auto& item : list.arr) {
        if (item.type != JVal::Arr || item.arr.size() < 2) continue;
        std::wstring text = item.arr[1].str;
        if (text.size() > 4000) text.resize(4000);
        if (!cur.texts.empty() && (cur.texts.size() >= 60 || chars + text.size() > 3000)) {
            batches.push_back(std::move(cur));
            cur = Batch();
            chars = 0;
        }
        cur.tabId = t->id;
        cur.gen = t->translateGen;
        cur.ids.push_back((int)item.arr[0].num);
        cur.texts.push_back(std::move(text));
        chars += cur.texts.back().size();
    }
    if (!cur.texts.empty()) batches.push_back(std::move(cur));
    if (batches.empty()) return;
    t->translatePending += (int)batches.size();
    const int service = g_prefs.translateService;
    const TranslateLanguage& target = Target();
    std::thread(Worker, std::move(batches), service, std::string(service == 1 ? target.deepl : target.google), s_deeplKey).detach();
}

void StartTranslating(Tab& t) {
    t.translated = true;
    t.autoTranslate = true;
    ++t.translateGen;
    t.translateErrorShown = false;
    InvalidateChrome();
    Collect(t, 30000);
}

void ShowOriginal(Tab& t) {
    t.translated = false;
    t.autoTranslate = false;
    ++t.translateGen;                                    // late results are dropped
    t.translatePending = 0;
    InvalidateChrome();
    if (!t.page) return;
    WKStringRef s = WKStringCreateWithUTF8CString("window.__lyraTr && __lyraTr.restore();");
    WKPageEvaluateJavaScriptInMainFrame(t.page, s, nullptr, nullptr);
    WKRelease(s);
}

std::wstring KeyFile() { return g_settingsDir + L"\\deepl.key"; }
const char kEntropy[] = "Lyra DeepL key";

} // namespace

const std::vector<TranslateLanguage>& TranslateLanguages() { return kLanguages; }
std::wstring TranslateTargetName() { return Target().name; }

std::wstring TranslateGetDeepLKey() { return s_deeplKey; }

void TranslateSetDeepLKey(const std::wstring& key) {
    s_deeplKey = TrimWs(key);
    if (s_deeplKey.empty()) { DeleteFileW(KeyFile().c_str()); return; }
    const std::string plain = ToUtf8(s_deeplKey);
    DATA_BLOB in = { (DWORD)plain.size(), (BYTE*)plain.data() }, out = {};
    DATA_BLOB entropy = { (DWORD)sizeof(kEntropy), (BYTE*)kEntropy };
    if (!CryptProtectData(&in, L"Lyra", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return;
    std::ofstream f(KeyFile(), std::ios::binary | std::ios::trunc);
    f.write((const char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
}

void TranslateInit() {
    std::ifstream f(KeyFile(), std::ios::binary);
    if (!f) return;
    std::string blob((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    DATA_BLOB in = { (DWORD)blob.size(), (BYTE*)blob.data() }, out = {};
    DATA_BLOB entropy = { (DWORD)sizeof(kEntropy), (BYTE*)kEntropy };
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return;
    s_deeplKey = ToWide(std::string((const char*)out.pbData, out.cbData));
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
}

void TranslatePageCommitted(Tab& t) {
    t.translatable = false;
    ++t.translateGen;
    t.translatePending = 0;
    t.translateCollecting = false;
    if (t.translated) { t.translated = false; InvalidateChrome(); }
}

void TranslatePageLoaded(Tab& t) {
    if (!t.page || !WebPage(t)) return;
    WKStringRef s = WKStringCreateWithUTF8CString(kProbe);
    WKPageEvaluateJavaScriptInMainFrame(t.page, s, (void*)(uintptr_t)t.id, OnProbe);
    WKRelease(s);
}

void TranslateToggle() {
    Tab* t = ActiveTab();
    if (!t || !t->page) return;
    if (t->translated) { ShowOriginal(*t); return; }
    if (!WebPage(*t)) { MessageBeep(MB_ICONWARNING); return; }
    StartTranslating(*t);
}

void TranslateTick() {
    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (now - last < 1500) return;
    last = now;
    for (auto& tp : g_tabs) {
        Tab& t = *tp;
        if (t.state != TabState::Live || !t.translated || !t.page) continue;
        // A page translated by "keep translating" starts here; later text arrives the same way.
        if (t.translatePending == 0) Collect(t, 20000);
    }
}

void TranslateResult(LPARAM lParam) {
    std::unique_ptr<Result> r((Result*)lParam);
    Tab* t = TabById(r->tabId);
    if (!t || r->gen != t->translateGen || !t->translated) return;
    if (t->translatePending > 0) --t->translatePending;
    if (r->texts.empty()) {
        t->translatePending = 0;
        Log("translate: %s", ToUtf8(r->error).c_str());
        if (!t->translateErrorShown) {
            t->translateErrorShown = true;
            t->translated = false;                         // the icon offers it again
            t->autoTranslate = false;
            InvalidateChrome();
            std::wstring msg = Tr(L"The page could not be translated.\n\n", L"Не удалось перевести страницу.\n\n") + r->error;
            ModalScope modal;
            MessageBoxW(g_hWnd, msg.c_str(), kAppName, MB_OK | MB_ICONWARNING);
        }
        return;
    }
    std::wstring js = L"window.__lyraTr && __lyraTr.apply([";
    for (size_t i = 0; i < r->ids.size() && i < r->texts.size(); ++i) {
        if (r->texts[i].empty()) continue;
        if (js.back() != L'[') js += L",";
        js += L"[" + std::to_wstring(r->ids[i]) + L"," + JsString(r->texts[i]) + L"]";
    }
    js += L"]);";
    WKStringRef s = WKStringCreateWithUTF8CString(ToUtf8(js).c_str());
    WKPageEvaluateJavaScriptInMainFrame(t->page, s, nullptr, nullptr);
    WKRelease(s);
}
