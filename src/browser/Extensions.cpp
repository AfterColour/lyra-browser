// Lyra — расширения браузера (см. Extensions.h).
#include "browser/Extensions.h"
#include "app/Log.h"
#include "browser/AdBlock.h"
#include "browser/ExtensionStore.h"
#include "browser/ExtensionDnr.h"
#include "core/Constants.h"
#include "core/Json.h"
#include "core/Localization.h"
#include "core/Settings.h"
#include "core/StringUtils.h"
#include "browser/TabManager.h"
#include "browser/WebContext.h"
#include "ui/Chrome.h"
#include "ui/MainWindow.h"
#include "ui/Menu.h"
#include "ui/Theme.h"
#include "ui/settings/SettingsPages.h"
#include "ui/settings/SettingsWindow.h"
#include <WebKit/WKPageFullScreenClient.h>      // WKCompletionListenerComplete
#include <WebKit/WKUserContentControllerRef.h>
#include <WebKit/WKUserScriptRef.h>
#include <WebKit/WKScriptMessageRef.h>
#include <WebKit/WKPageNavigationClient.h>
#include <WebKit/WKFrameInfoRef.h>
#include <WebKit/WKContextPrivate.h>
#include <WebKit/WKSecurityOriginRef.h>
#include <WebKit/WKPageConfigurationRef.h>
#include <WebKit/WKPreferencesRefPrivate.h>
#include <shobjidl.h>
#include <filesystem>
#include <functional>
#include <set>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;
extern HWND g_setHwnd;

namespace {

struct ContentScript {
    std::vector<std::string> matches, excludes;     // WebKit user content URL patterns
    std::vector<std::wstring> js, css;              // files, relative to the extension
    bool atStart = false, allFrames = false;
};

struct Ruleset {                         // declarative_net_request.rule_resources
    std::string id;
    std::wstring path;                   // relative, native separators
    bool enabled = false;               // on by default
};

struct Extension {
    ExtensionInfo info;
    std::string manifestText, messagesText = "{}", lang = "en";
    std::vector<ContentScript> scripts;
    bool hasAction = false;
    std::string defaultIcon;             // JSON: the button's default_icon (or the extension's icons)
    std::wstring defaultTitle;
    std::string defaultPopup;            // relative to the extension
    std::string optionsPage;
    std::vector<Ruleset> rulesets;
    std::vector<std::string> war;        // web_accessible_resources (globs, relative)
    bool firefoxBuild = false;           // made for Firefox (browser_specific_settings.gecko)
};

std::vector<Extension> s_exts;
std::vector<ExtensionInfo> s_infos;
std::set<std::wstring> s_worldsWithHandler;
struct HandlerCtx { std::wstring id; bool content = true; bool tabFrame = false; };   // tabFrame: one of its pages inside a tab      // whose script sent a message, and from where
std::map<std::wstring, std::unique_ptr<HandlerCtx>> s_handlerIds;    // context of each world's handler
std::map<std::wstring, std::map<std::string, std::string>> s_session; // storage.session, per extension
std::string s_apiJs;

std::wstring Root() { return g_settingsDir + L"\\Extensions"; }

std::string ReadText(const std::wstring& path, size_t limit = 32u << 20) {
    std::string data;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return data;
    LARGE_INTEGER size = {};
    GetFileSizeEx(f, &size);
    if (size.QuadPart > 0 && (size_t)size.QuadPart <= limit) {
        data.resize((size_t)size.QuadPart);
        DWORD read = 0;
        ReadFile(f, data.data(), (DWORD)data.size(), &read, nullptr);
        data.resize(read);
    }
    CloseHandle(f);
    return data;
}

bool WriteFileAtomic(const std::wstring& path, const std::string& data) {
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

// A path from the manifest, inside the extension's folder (no "..", no drive).
bool SafeRelative(const std::wstring& rel) {
    if (rel.empty() || rel.find(L':') != std::wstring::npos || rel[0] == L'/' || rel[0] == L'\\') return false;
    return rel.find(L"..") == std::wstring::npos;
}

std::wstring Native(std::wstring rel) {
    for (auto& c : rel) if (c == L'/') c = L'\\';
    return rel;
}

// ---- manifest ---------------------------------------------------------------------------------
std::string PickLocale(const std::wstring& dir, const std::string& defaultLocale) {
    std::vector<std::string> wanted;
    if (UiRussian()) wanted.push_back("ru");
    wanted.push_back("en");
    if (!defaultLocale.empty()) wanted.push_back(defaultLocale);
    for (const std::string& l : wanted)
        if (GetFileAttributesW((dir + L"\\_locales\\" + ToWide(l) + L"\\messages.json").c_str()) != INVALID_FILE_ATTRIBUTES) return l;
    return "";
}

// "__MSG_appName__" -> the message
std::string Localize(const std::string& text, const JsonValue& messages) {
    if (text.size() < 9 || text.compare(0, 6, "__MSG_") != 0 || text.compare(text.size() - 2, 2, "__") != 0) return text;
    std::string key = text.substr(6, text.size() - 8);
    for (auto& m : messages.members) {
        if (_stricmp(m.first.c_str(), key.c_str()) == 0) return m.second.str("message", text);
    }
    return text;
}

// Chrome's match patterns -> WebKit's user content URL patterns
void AddPatterns(const JsonValue* list, std::vector<std::string>& out) {
    if (!list || !list->isArray()) return;
    for (auto& v : list->items) {
        if (!v.isString()) continue;
        // (not file://: Chrome needs the user's leave for it, and Lyra's own pages - start page,
        // welcome - are files)
        if (v.s == "<all_urls>") { out.push_back("http://*/*"); out.push_back("https://*/*"); }
        else if (v.s.rfind("file:", 0) != 0) out.push_back(v.s);
    }
}

void AddFiles(const JsonValue* list, std::vector<std::wstring>& out) {
    if (!list || !list->isArray()) return;
    for (auto& v : list->items)
        if (v.isString() && SafeRelative(ToWide(v.s))) out.push_back(ToWide(v.s));
}

std::wstring Slug(const std::string& name) {
    std::wstring s;
    for (unsigned char c : name) {
        if (isalnum(c)) s += (wchar_t)tolower(c);
        else if (!s.empty() && s.back() != L'-') s += L'-';
        if (s.size() >= 32) break;
    }
    while (!s.empty() && s.back() == L'-') s.pop_back();
    unsigned h = 2166136261u;
    for (unsigned char c : name) h = (h ^ c) * 16777619u;
    wchar_t tail[16];
    swprintf(tail, 16, L"%s%06x", s.empty() ? L"" : L"-", h & 0xFFFFFF);
    return (s.empty() ? L"extension" : s) + tail;
}

// The extension's id: the Firefox id when it has one, otherwise one made from its name (stable
// across versions, so installing a newer version replaces the old one).
std::wstring MakeId(const JsonValue& manifest, const std::string& name) {
    for (const char* key : { "browser_specific_settings", "applications" }) {
        if (const JsonValue* b = manifest.get(key))
            if (const JsonValue* g = b->get("gecko")) {
                std::wstring id;
                for (char c : g->str("id")) if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '@') id += (wchar_t)(c == '@' ? '_' : c);
                if (!id.empty() && id.size() < 80) return id;
            }
    }
    return Slug(name);
}

bool LoadExtension(const std::wstring& dir, Extension& ext, std::string* error) {
    ext = Extension();
    ext.manifestText = ReadText(dir + L"\\manifest.json", 4u << 20);
    JsonValue m;
    if (ext.manifestText.empty() || !JsonParse(ext.manifestText, m, error) || !m.isObject()) {
        if (error && error->empty()) *error = "manifest.json is missing";
        return false;
    }
    ExtensionInfo& info = ext.info;
    info.dir = dir;
    info.id = dir.substr(dir.find_last_of(L'\\') + 1);
    info.manifestVersion = m.get("manifest_version") ? (int)m.get("manifest_version")->n : 2;
    ext.lang = PickLocale(dir, m.str("default_locale"));
    JsonValue messages;
    if (!ext.lang.empty()) {
        ext.messagesText = ReadText(dir + L"\\_locales\\" + ToWide(ext.lang) + L"\\messages.json", 4u << 20);
        if (!JsonParse(ext.messagesText, messages) || !messages.isObject()) { ext.messagesText = "{}"; messages = JsonValue(); }
    }
    info.name = ToWide(Localize(m.str("name", "Extension"), messages));
    info.description = ToWide(Localize(m.str("description"), messages));
    info.version = ToWide(m.str("version"));
    // icons: the largest
    int best = 0;
    if (const JsonValue* icons = m.get("icons")) {
        for (auto& i : icons->members) {
            const int size = atoi(i.first.c_str());
            if (i.second.isString() && size > best && SafeRelative(ToWide(i.second.s))) { best = size; info.icon = dir + L"\\" + Native(ToWide(i.second.s)); }
        }
    }
    for (const char* key : { "action", "browser_action", "page_action" }) {
        const JsonValue* a = m.get(key);
        if (!a || !a->isObject()) continue;
        ext.hasAction = true;
        if (const JsonValue* i = a->get("default_icon")) ext.defaultIcon = i->raw;
        ext.defaultTitle = ToWide(Localize(a->str("default_title"), messages));
        ext.defaultPopup = a->str("default_popup");
        break;
    }
    if (ext.hasAction && ext.defaultIcon.empty()) if (const JsonValue* icons = m.get("icons")) ext.defaultIcon = icons->raw;
    if (ext.defaultTitle.empty()) ext.defaultTitle = info.name;
    ext.optionsPage = m.str("options_page");
    if (ext.optionsPage.empty()) if (const JsonValue* ui = m.get("options_ui")) ext.optionsPage = ui->str("page");
    info.hasAction = ext.hasAction;
    info.pinned = GetPrivateProfileIntW(L"Lyra", L"Toolbar", 1, (dir + L"\\lyra.ini").c_str()) != 0;
    for (const char* key : { "permissions", "host_permissions", "optional_permissions" }) {
        if (const JsonValue* p = m.get(key))
            for (auto& v : p->items) if (v.isString()) info.permissions.push_back(ToWide(v.s));
    }
    if (const JsonValue* cs = m.get("content_scripts")) {
        for (auto& entry : cs->items) {
            ContentScript s;
            AddPatterns(entry.get("matches"), s.matches);
            AddPatterns(entry.get("exclude_matches"), s.excludes);
            AddFiles(entry.get("js"), s.js);
            AddFiles(entry.get("css"), s.css);
            s.atStart = entry.str("run_at") == "document_start";
            s.allFrames = entry.flag("all_frames");
            if (!s.matches.empty() && (!s.js.empty() || !s.css.empty())) ext.scripts.push_back(std::move(s));
        }
    }
    info.contentScripts = (int)ext.scripts.size();
    for (const char* key : { "browser_specific_settings", "applications" })
        if (const JsonValue* b = m.get(key)) if (b->get("gecko")) ext.firefoxBuild = true;
    if (const JsonValue* war = m.get("web_accessible_resources"))
        for (auto& w : war->items) {
            if (w.isString()) ext.war.push_back(w.s);
            else if (const JsonValue* res = w.get("resources")) for (auto& r : res->items) if (r.isString()) ext.war.push_back(r.s);
        }
    for (auto& w : ext.war) while (!w.empty() && w[0] == '/') w.erase(0, 1);
    if (const JsonValue* dnr = m.get("declarative_net_request"))
        if (const JsonValue* list = dnr->get("rule_resources"))
            for (auto& r : list->items) {
                const std::string id = r.str("id");
                std::string path = r.str("path");
                while (!path.empty() && path[0] == '/') path.erase(0, 1);      // ("/rulesets/main/easylist.json")
                if (id.empty() || path.empty() || !SafeRelative(ToWide(path))) continue;
                ext.rulesets.push_back({ id, Native(ToWide(path)), r.flag("enabled") });
            }
    info.enabled = GetPrivateProfileIntW(L"Lyra", L"Enabled", 1, (dir + L"\\lyra.ini").c_str()) != 0;
    return true;
}

void LoadAll() {
    s_exts.clear();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((Root() + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            Extension e;
            std::string err;
            if (LoadExtension(Root() + L"\\" + fd.cFileName, e, &err)) s_exts.push_back(std::move(e));
            else Log("extensions: %ls skipped: %s", fd.cFileName, err.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(s_exts.begin(), s_exts.end(), [](const Extension& a, const Extension& b) { return _wcsicmp(a.info.name.c_str(), b.info.name.c_str()) < 0; });
    s_infos.clear();
    for (auto& e : s_exts) s_infos.push_back(e.info);
}

Extension* Find(const std::wstring& id) {
    for (auto& e : s_exts) if (e.info.id == id) return &e;
    return nullptr;
}

// ---- lyra-extension://<id>/<path>: the extension's files ------------------------------------------
// Its pages load from here (so "/js/x.js" means the extension's own folder, as in Chrome), and web
// pages get the files it lets them have (web_accessible_resources, e.g. images its content scripts
// put in), plus its .html pages (opened in a tab). Lyra's own files in the folder are never served.
const wchar_t kExtScheme[] = L"lyra-extension";
enum : intptr_t { kFromExtensionPage = 1, kFromWebPage = 2 };

std::wstring ExtBase(const Extension& e) { return std::wstring(kExtScheme) + L"://" + e.info.id + L"/"; }

bool PrivateFile(std::wstring rel) {
    CharLowerBuffW(rel.data(), (DWORD)rel.size());
    if (rel.size() > 4 && rel.compare(rel.size() - 4, 4, L".tmp") == 0) return true;
    if (rel.find(L'\\') != std::wstring::npos) return false;
    return rel == L"lyra.ini" || rel.rfind(L"storage-", 0) == 0 || rel.rfind(L"dnr-", 0) == 0 || rel == L"registered-scripts.json" || rel == L"menus.json";
}

bool GlobMatch(const char* p, const char* s) {               // '*' matches anything, also "/"
    if (!*p) return !*s;
    if (*p == '*') return GlobMatch(p + 1, s) || (*s && GlobMatch(p, s + 1));
    return *s && *p == *s && GlobMatch(p + 1, s + 1);
}

std::string MimeOf(const std::wstring& path) {
    std::wstring ext = path.substr(path.find_last_of(L'.') == std::wstring::npos ? path.size() : path.find_last_of(L'.'));
    CharLowerBuffW(ext.data(), (DWORD)ext.size());
    static const std::pair<const wchar_t*, const char*> kTypes[] = {
        { L".html", "text/html; charset=utf-8" }, { L".htm", "text/html; charset=utf-8" },
        { L".js", "text/javascript; charset=utf-8" }, { L".mjs", "text/javascript; charset=utf-8" },
        { L".css", "text/css; charset=utf-8" }, { L".json", "application/json; charset=utf-8" },
        { L".txt", "text/plain; charset=utf-8" }, { L".svg", "image/svg+xml" }, { L".xml", "text/xml; charset=utf-8" },
        { L".png", "image/png" }, { L".gif", "image/gif" }, { L".jpg", "image/jpeg" }, { L".jpeg", "image/jpeg" },
        { L".webp", "image/webp" }, { L".ico", "image/x-icon" }, { L".woff", "font/woff" }, { L".woff2", "font/woff2" },
        { L".ttf", "font/ttf" }, { L".otf", "font/otf" }, { L".wasm", "application/wasm" }, { L".mp3", "audio/mpeg" },
        { L".mp4", "video/mp4" }, { L".webm", "video/webm" }, { L".ogg", "audio/ogg" }, { L".wav", "audio/wav" },
    };
    for (auto& [e, m] : kTypes) if (ext == e) return m;
    return "application/octet-stream";
}

void OnExtensionScheme(WKURLSchemeTaskRef task, WKURLRef url, WKStringRef, WKURLRef frameURL, const void* context) {
    auto text = [](WKURLRef u) {
        std::wstring s;
        if (u) if (WKStringRef w = WKURLCopyString(u)) { s = WKStr(w); WKRelease(w); }
        return s;
    };
    auto finish = [&](int status, const std::string& mime, const std::string& body) {
        static const bool logIt = GetEnvironmentVariableW(L"LYRA_EXT_SCHEME_LOG", nullptr, 0) > 0;   // every request, for debugging
        if (logIt) Log("extensions: scheme %d %zu %ls (from %ls)", status, body.size(), text(url).c_str(), text(frameURL).c_str());
        WKStringRef m = WKStringCreateWithUTF8CString(mime.c_str());
        WKURLSchemeTaskFinish(task, status, m, body.data(), body.size());
        WKRelease(m);
    };
    const std::wstring u = text(url), from = text(frameURL);
    const std::wstring prefix = std::wstring(kExtScheme) + L"://";
    if (_wcsnicmp(u.c_str(), prefix.c_str(), prefix.size()) != 0) return finish(404, "text/plain", "");
    const std::wstring rest = u.substr(prefix.size());
    const size_t slash = rest.find(L'/');
    const std::wstring id = rest.substr(0, slash);
    std::string raw = ToUtf8(slash == std::wstring::npos ? L"" : rest.substr(slash + 1));
    raw = raw.substr(0, raw.find_first_of("?#"));
    std::string decoded;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '%' && i + 2 < raw.size() && isxdigit((unsigned char)raw[i + 1]) && isxdigit((unsigned char)raw[i + 2])) {
            decoded += (char)strtol(raw.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else decoded += raw[i];
    }
    const Extension* e = nullptr;
    for (const Extension& x : s_exts) if (_wcsicmp(x.info.id.c_str(), id.c_str()) == 0) e = &x;
    const std::wstring path = ToWide(decoded);
    if (!e || !e->info.enabled || path.empty() || !SafeRelative(path)) return finish(404, "text/plain", "");
    const std::wstring native = Native(path);
    if (PrivateFile(native)) return finish(404, "text/plain", "");
    if ((intptr_t)context == kFromWebPage) {
        const std::wstring own = ExtBase(*e);
        bool allowed = _wcsnicmp(from.c_str(), own.c_str(), own.size()) == 0;     // its own page in a tab
        if (!allowed) allowed = std::any_of(e->war.begin(), e->war.end(), [&](const std::string& g) { return GlobMatch(g.c_str(), decoded.c_str()); });
        if (!allowed) {
            const std::string mime = MimeOf(native);
            allowed = mime.rfind("text/html", 0) == 0;                             // an extension page opened in a tab
        }
        if (!allowed) return finish(404, "text/plain", "");
    }
    const std::wstring file = e->info.dir + L"\\" + native;
    const DWORD attr = GetFileAttributesW(file.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) return finish(404, "text/plain", "");
    finish(200, MimeOf(native), ReadText(file, 512u << 20));
}

void SetSchemeHandler(WKPageConfigurationRef conf, intptr_t from) {
    WKStringRef scheme = WKStringCreateWithUTF8CString(ToUtf8(kExtScheme).c_str());
    WKPageConfigurationSetURLSchemeHandler(conf, scheme, OnExtensionScheme, nullptr, (const void*)from);
    WKRelease(scheme);
}

// ---- storage ----------------------------------------------------------------------------------
using Store = std::map<std::string, std::string>;      // key -> value as JSON text

std::wstring StoreFile(const Extension& e, const std::string& area) { return e.info.dir + L"\\storage-" + ToWide(area) + L".json"; }

// The areas are read from disk once and then kept in memory (an extension may keep megabytes there
// and ask many times a second); changes go to disk from a writer thread, 300 ms later, the last
// version of each file only.
std::map<std::wstring, std::map<std::string, Store>> s_stores;   // extension -> area -> items

std::mutex s_writeMutex;
std::map<std::wstring, std::string> s_pendingWrites;          // file -> its new content
bool s_writerRunning = false;

void QueueWrite(const std::wstring& path, std::string data) {
    std::lock_guard<std::mutex> lock(s_writeMutex);
    s_pendingWrites[path] = std::move(data);
    if (s_writerRunning) return;
    s_writerRunning = true;
    std::thread([] {
        for (;;) {
            Sleep(300);
            std::map<std::wstring, std::string> batch;
            {
                std::lock_guard<std::mutex> lock(s_writeMutex);
                if (s_pendingWrites.empty()) { s_writerRunning = false; return; }
                batch.swap(s_pendingWrites);
            }
            for (auto& [path, data] : batch) WriteFileAtomic(path, data);
        }
    }).detach();
}

void FlushWrites() {                                          // at exit: what is still waiting
    for (int i = 0; i < 100; ++i) {
        std::map<std::wstring, std::string> batch;
        {
            std::lock_guard<std::mutex> lock(s_writeMutex);
            if (!s_writerRunning && s_pendingWrites.empty()) return;
            batch.swap(s_pendingWrites);
        }
        for (auto& [path, data] : batch) WriteFileAtomic(path, data);
        Sleep(20);                                            // (the writer thread may be in the middle of one)
    }
}

Store& StoreOf(const Extension& e, const std::string& area) {
    if (area == "session") return s_session[e.info.id];
    auto& areas = s_stores[e.info.id];
    auto it = areas.find(area);
    if (it != areas.end()) return it->second;
    Store& s = areas[area];
    std::vector<std::pair<std::string, std::string>> members;
    if (JsonObjectMembers(ReadText(StoreFile(e, area)), members))
        for (auto& [k, v] : members) s[k] = std::move(v);
    return s;
}

void SaveStore(const Extension& e, const std::string& area) {
    if (area == "session") return;
    const Store& s = StoreOf(e, area);
    size_t size = 2;
    for (auto& [k, v] : s) size += k.size() + v.size() + 4;
    std::string out;
    out.reserve(size + 16);
    out += "{";
    for (auto& [k, v] : s) { if (out.size() > 1) out += ","; out += JsonQuote(k); out += ":"; out += v; }
    out += "}";
    QueueWrite(StoreFile(e, area), std::move(out));
}

std::string Storage(Extension& e, const JsonValue& msg, const std::string& op) {
    const std::string area = msg.str("area", "local");
    if (area != "local" && area != "sync" && area != "session" && area != "managed") return "{\"error\":\"unknown storage area\"}";
    Store& store = StoreOf(e, area);
    const JsonValue* keys = msg.get("keys");
    if (op == "storage.get") {
        std::string out = "{";
        auto add = [&](const std::string& k, const std::string* fallback) {
            auto it = store.find(k);
            const std::string* v = it != store.end() ? &it->second : fallback;
            if (!v) return;
            if (out.size() > 1) out += ",";
            out += JsonQuote(k) + ":" + *v;
        };
        if (!keys || keys->type == JsonValue::Null) for (auto& [k, v] : store) add(k, nullptr);
        else if (keys->isString()) add(keys->s, nullptr);
        else if (keys->isArray()) { for (auto& k : keys->items) if (k.isString()) add(k.s, nullptr); }
        else if (keys->isObject()) { for (auto& m : keys->members) add(m.first, &m.second.raw); }
        return "{\"value\":" + out + "}}";
    }
    if (area == "managed") return "{\"error\":\"storage.managed is read-only\"}";
    std::string changes = "{";
    auto changed = [&](const std::string& k, const std::string* oldV, const std::string* newV) {
        if (changes.size() > 1) changes += ",";
        changes += JsonQuote(k) + ":{";
        if (oldV) changes += "\"oldValue\":" + *oldV;
        if (newV) changes += std::string(oldV ? "," : "") + "\"newValue\":" + *newV;
        changes += "}";
    };
    if (op == "storage.set") {
        if (const JsonValue* items = msg.get("items"))
            for (auto& m : items->members) {
                auto it = store.find(m.first);
                if (it != store.end() && it->second == m.second.raw) continue;
                changed(m.first, it != store.end() ? &it->second : nullptr, &m.second.raw);
                store[m.first] = m.second.raw;
            }
    } else if (op == "storage.remove") {
        std::vector<std::string> list;
        if (keys && keys->isString()) list.push_back(keys->s);
        else if (keys && keys->isArray()) for (auto& k : keys->items) if (k.isString()) list.push_back(k.s);
        for (auto& k : list) {
            auto it = store.find(k);
            if (it == store.end()) continue;
            changed(k, &it->second, nullptr);
            store.erase(it);
        }
    } else if (op == "storage.clear") {
        for (auto& [k, v] : store) changed(k, &v, nullptr);
        store.clear();
    } else {
        return "{\"error\":\"unknown operation\"}";
    }
    SaveStore(e, area);
    return "{\"value\":" + changes + "}}";
}

// The bridge (window.webkit.messageHandlers.lyraExt): see HandleMessage below.
void OnMessage(WKScriptMessageRef message, WKCompletionListenerRef reply, const void* context);
std::string LoadApi();
std::string ExtJson(const Extension& e, const char* context);

std::wstring World(const Extension& e) { return L"lyra-extension-" + e.info.id; }

WKArrayRef StringArray(const std::vector<std::string>& list) {
    std::vector<WKTypeRef> items;
    for (auto& s : list) items.push_back(WKStringCreateWithUTF8CString(s.c_str()));
    WKArrayRef a = WKArrayCreateAdoptingValues(items.data(), items.size());
    return a;
}

void AddScript(WKUserContentControllerRef controller, const std::string& source, const ContentScript& s, const std::wstring& world) {
    WKStringRef src = WKStringCreateWithUTF8CStringWithLength(source.data(), source.size());
    WKStringRef w = world.empty() ? nullptr : WKStringCreateWithUTF8CString(ToUtf8(world).c_str());   // none: the page's own world
    WKArrayRef allow = StringArray(s.matches), block = StringArray(s.excludes);
    WKUserScriptRef script = WKUserScriptCreateWithSourceInWorld(src, s.atStart ? kWKInjectAtDocumentStart : kWKInjectAtDocumentEnd, !s.allFrames, w, allow, block);
    WKUserContentControllerAddUserScript(controller, script);
    WKRelease(script);
    WKRelease(allow);
    WKRelease(block);
    if (w) WKRelease(w);
    WKRelease(src);
}

const char kCssInjector[] =
    "(function(css){var add=function(){var s=document.createElement('style');s.setAttribute('data-lyra-extension','');"
    "s.textContent=css;(document.head||document.documentElement).appendChild(s);};"
    "if(document.documentElement)add();else new MutationObserver(function(r,o){if(document.documentElement){o.disconnect();add();}})"
    ".observe(document,{childList:true});})";

// ---- scripting.registerContentScripts: content scripts an extension adds while it runs ------------
// They join the manifest's (AddAllScripts), from the next page load on. Those that persist across
// sessions (the default) are kept in registered-scripts.json; world "MAIN" runs in the page's own world.
struct Registered { std::string id, raw; bool persist = true; };
std::map<std::wstring, std::vector<Registered>> s_registered;
std::set<std::wstring> s_registeredLoaded;

std::wstring RegisteredFile(const Extension& e) { return e.info.dir + L"\\registered-scripts.json"; }

std::vector<Registered>& RegisteredScripts(const Extension& e) {
    auto& list = s_registered[e.info.id];
    if (s_registeredLoaded.insert(e.info.id).second) {
        JsonValue v;
        if (JsonParse(ReadText(RegisteredFile(e)), v) && v.isArray())
            for (auto& s : v.items) if (s.isObject() && !s.str("id").empty()) list.push_back({ s.str("id"), s.raw, true });
    }
    return list;
}

void SaveRegistered(const Extension& e) {
    std::string s = "[";
    for (auto& r : s_registered[e.info.id]) if (r.persist) { if (s.size() > 1) s += ","; s += r.raw; }
    WriteFileAtomic(RegisteredFile(e), s + "]");
}

bool RegisteredToContent(const JsonValue& s, ContentScript& c, bool& mainWorld) {
    AddPatterns(s.get("matches"), c.matches);
    AddPatterns(s.get("excludeMatches"), c.excludes);
    auto files = [](const JsonValue* list, std::vector<std::wstring>& out) {
        if (!list || !list->isArray()) return;
        for (auto& v : list->items) {
            if (!v.isString()) continue;
            std::string p = v.s;
            while (!p.empty() && p[0] == '/') p.erase(0, 1);
            if (SafeRelative(ToWide(p))) out.push_back(ToWide(p));
        }
    };
    files(s.get("js"), c.js);
    files(s.get("css"), c.css);
    c.atStart = s.str("runAt") == "document_start";
    c.allFrames = s.flag("allFrames");
    mainWorld = s.str("world") == "MAIN";
    return !c.matches.empty() && (!c.js.empty() || !c.css.empty());
}

std::string ScriptError(const std::string& text) { return "{\"error\":" + JsonQuote(text) + "}"; }

std::string ScriptingRegistered(const Extension& e, const JsonValue& msg, const std::string& op) {
    auto& list = RegisteredScripts(e);
    std::set<std::string> ids;
    const JsonValue* filter = msg.get("ids");
    const bool filtered = filter && filter->isArray();
    if (filtered) for (auto& v : filter->items) if (v.isString()) ids.insert(v.s);
    if (op == "scripting.getRegisteredContentScripts") {
        std::string s = "[";
        for (auto& r : list) if (!filtered || ids.count(r.id)) { if (s.size() > 1) s += ","; s += r.raw; }
        return "{\"value\":" + s + "]}";
    }
    if (op == "scripting.unregisterContentScripts") {
        for (auto& id : ids)
            if (std::none_of(list.begin(), list.end(), [&](const Registered& r) { return r.id == id; }))
                return ScriptError("Nonexistent script ID '" + id + "'");
        list.erase(std::remove_if(list.begin(), list.end(), [&](const Registered& r) { return !filtered || ids.count(r.id); }), list.end());
    } else {
        const JsonValue* scripts = msg.get("scripts");
        if (!scripts || !scripts->isArray()) return ScriptError("scripts must be an array");
        const bool update = op == "scripting.updateContentScripts";
        std::vector<Registered> next = list;
        for (auto& s : scripts->items) {
            const std::string id = s.str("id");
            if (id.empty() || id[0] == '_') return ScriptError("Invalid script ID '" + id + "'");
            auto it = std::find_if(next.begin(), next.end(), [&](const Registered& r) { return r.id == id; });
            if (!update && it != next.end()) return ScriptError("Duplicate script ID '" + id + "'");
            if (update && it == next.end()) return ScriptError("Script with ID '" + id + "' does not exist or is not fully registered");
            std::string raw = s.raw;
            if (update) {                                 // the members given replace the old ones
                JsonValue old;
                JsonParse(it->raw, old);
                std::string merged = "{";
                auto add = [&](const std::string& k, const std::string& v) { if (merged.size() > 1) merged += ","; merged += JsonQuote(k) + ":" + v; };
                for (auto& [k, v] : old.members) if (!s.get(k)) add(k, v.raw);
                for (auto& [k, v] : s.members) add(k, v.raw);
                raw = merged + "}";
            }
            JsonValue parsed;
            JsonParse(raw, parsed);
            ContentScript c;
            bool mainWorld = false;
            if (!RegisteredToContent(parsed, c, mainWorld))
                return ScriptError("Script with ID '" + id + "' must specify both matches and at least one js or css file.");
            Registered r{ id, raw, parsed.flag("persistAcrossSessions", true) };
            if (it != next.end()) *it = r;
            else next.push_back(r);
        }
        list.swap(next);
    }
    SaveRegistered(e);
    AdBlockRefreshUserScripts();                          // pages loaded from now on get them
    return "{\"value\":null}";
}

// Called by AdBlock.cpp whenever it rebuilds the shared list of user scripts.
void AddAllScripts(WKUserContentControllerRef controller) {
    ExtensionStoreAddScript(controller);   // "Add to Lyra" on the stores' pages
    LoadApi();
    if (s_apiJs.empty()) return;
    // Extensions' own pages inside tabs (a password manager's menu in a page's iframe, an extension's
    // page opened in a tab): their full API in the page's world, answered by one handler there.
    static WKUserContentControllerRef s_frameHandlerOn = nullptr;
    static HandlerCtx s_frameCtx{ L"", false, true };
    if (s_frameHandlerOn != controller) {
        s_frameHandlerOn = controller;
        WKStringRef name = WKStringCreateWithUTF8CString("lyraExt");
        WKUserContentControllerAddScriptMessageHandler(controller, name, OnMessage, &s_frameCtx);
        WKRelease(name);
    }
    for (Extension& e : s_exts) {
        if (!e.info.enabled) continue;
        ContentScript frames;
        frames.matches.push_back(ToUtf8(std::wstring(kExtScheme) + L"://" + e.info.id + L"/*"));
        frames.atStart = true;
        frames.allFrames = true;
        AddScript(controller, "(" + s_apiJs + ")(" + ExtJson(e, "extension") + ");", frames, L"");
    }
    for (Extension& e : s_exts) {
        if (!e.info.enabled) continue;
        std::vector<std::pair<ContentScript, bool>> all;     // (script, in the page's own world)
        for (const ContentScript& s : e.scripts) all.push_back({ s, false });
        for (const Registered& r : RegisteredScripts(e)) {
            JsonValue v;
            ContentScript c;
            bool mainWorld = false;
            if (JsonParse(r.raw, v) && RegisteredToContent(v, c, mainWorld)) all.push_back({ std::move(c), mainWorld });
        }
        if (all.empty()) continue;
        const std::wstring world = World(e);
        if (!s_worldsWithHandler.count(world)) {
            auto& ctx = s_handlerIds[world];
            ctx = std::make_unique<HandlerCtx>(HandlerCtx{ e.info.id, true });
            WKStringRef name = WKStringCreateWithUTF8CString("lyraExt");
            WKStringRef w = WKStringCreateWithUTF8CString(ToUtf8(world).c_str());
            WKUserContentControllerAddScriptMessageHandlerInWorld(controller, name, OnMessage, ctx.get(), w);
            WKRelease(w);
            WKRelease(name);
            s_worldsWithHandler.insert(world);
        }
        const std::string ext = ExtJson(e, "content");
        for (const auto& [s, mainWorld] : all) {
            const std::wstring into = mainWorld ? std::wstring() : world;
            // 1: the API (once per page and world) and the styles; then each script on its own,
            // in order, in the same world (as Chrome runs them).
            std::string head = mainWorld ? std::string() : "if (!globalThis.__lyraExtension) { globalThis.__lyraExtension = true; (" + s_apiJs + ")(" + ext + "); }\n";
            for (const std::wstring& css : s.css) head += std::string(kCssInjector) + "(" + JsonQuote(ReadText(e.info.dir + L"\\" + Native(css))) + ");\n";
            if (!head.empty()) AddScript(controller, head, s, into);
            for (const std::wstring& js : s.js) {
                std::string code = ReadText(e.info.dir + L"\\" + Native(js));
                if (code.size() >= 3 && (unsigned char)code[0] == 0xEF) code.erase(0, 3);
                AddScript(controller, code + "\n//# sourceURL=" + ToUtf8(FileUrl(e.info.dir + L"\\" + Native(js))), s, into);
            }
        }
    }
}

void DnrSyncAll();

void Changed() {
    DnrSyncAll();                         // extensions' network rules
    AdBlockRefreshUserScripts();          // pages loaded from now on get the new set
    SettingsRefresh();
    if (g_hWnd) RefreshLayout();          // the toolbar buttons
}

// ---- installing ---------------------------------------------------------------------------------
bool Unzip(const std::wstring& zip, const std::wstring& dir, std::wstring& err) {
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring tar = std::wstring(sys) + L"\\tar.exe";
    std::wstring cmd = L"\"" + tar + L"\" -xf \"" + zip + L"\" -C \"" + dir + L"\"";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { err = L"cannot run tar.exe"; return false; }
    const bool done = WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0;
    DWORD code = 1;
    if (done) GetExitCodeProcess(pi.hProcess, &code); else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (!done || code != 0) { err = Tr(L"not a valid archive", L"это не архив расширения"); return false; }
    return true;
}

// A .crx is a zip with a header in front: "Cr24", version, then (2) key and signature lengths or
// (3) the length of a header.
bool StripCrx(const std::string& data, std::string& zip) {
    if (data.size() < 16 || data.compare(0, 4, "Cr24") != 0) return false;
    auto u32 = [&](size_t at) { return (uint32_t)(unsigned char)data[at] | ((uint32_t)(unsigned char)data[at + 1] << 8) | ((uint32_t)(unsigned char)data[at + 2] << 16) | ((uint32_t)(unsigned char)data[at + 3] << 24); };
    const uint32_t version = u32(4);
    size_t start = version == 3 ? 12 + (size_t)u32(8) : 16 + (size_t)u32(8) + (size_t)u32(12);
    if (start >= data.size()) return false;
    zip = data.substr(start);
    return true;
}


// =================================================================================================
//  Extension pages: the background page (or service worker), options windows; messages between
//  an extension's parts; tabs, scripting and the other APIs that need Lyra.
// =================================================================================================

std::string LoadApi() {
    if (s_apiJs.empty()) {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring path = exe;
        path = path.substr(0, path.find_last_of(L'\\')) + L"\\resources\\extensions\\api.js";
        s_apiJs = ReadText(path);
        const size_t f = s_apiJs.find("function (EXT)");
        if (f != std::string::npos) s_apiJs.erase(0, f);              // (the comment block above it)
    }
    return s_apiJs;
}

std::string ExtJson(const Extension& e, const char* context) {
    return "{\"id\":" + JsonQuote(ToUtf8(e.info.id)) + ",\"base\":" + JsonQuote(ToUtf8(ExtBase(e))) +
           ",\"manifest\":" + e.manifestText + ",\"messages\":" + e.messagesText +
           ",\"lang\":" + JsonQuote(UiRussian() ? "ru" : "en") + ",\"context\":\"" + context + "\"}";
}

struct Host {
    std::wstring id;
    HandlerCtx ctx;                          // the handler's context: this extension, its own pages
    WKUserContentControllerRef ucc = nullptr;
    WKPageConfigurationRef conf = nullptr;
    WKViewRef bgView = nullptr;
    WKPageRef bgPage = nullptr;
    bool bgReady = false;
    std::wstring installReason;              // "install" / "update" when the background page has loaded
    bool atStartup = false;                  // started with the browser: runtime.onStartup
    std::vector<std::string> queued;         // scripts for the background page before it has loaded
    std::vector<HWND> windows;               // options pages and such (ExtWindow)
};
std::map<std::wstring, std::unique_ptr<Host>> s_hosts;
HWND s_hostWnd = nullptr;
WKPreferencesRef s_extPrefs = nullptr;

struct PendingCall { WKCompletionListenerRef reply = nullptr; int waiting = 0; };
std::map<int, PendingCall> s_calls;
int s_nextCall = 1;

void Complete(int callId, const std::string& answer) {
    auto it = s_calls.find(callId);
    if (it == s_calls.end()) return;
    WKStringRef r = WKStringCreateWithUTF8CString(answer.c_str());
    WKCompletionListenerComplete(it->second.reply, r);
    WKRelease(r);
    WKRelease(it->second.reply);
    s_calls.erase(it);
}

int NewCall(WKCompletionListenerRef reply, int waiting) {
    const int id = s_nextCall++;
    WKRetain(reply);
    s_calls[id] = { reply, waiting };
    return id;
}

const char kNoReceiver[] = "{\"error\":\"Could not establish connection. Receiving end does not exist.\"}";

void Eval(WKPageRef page, const std::string& script) {
    if (!page) return;
    WKStringRef s = WKStringCreateWithUTF8CStringWithLength(script.data(), script.size());
    WKPageEvaluateJavaScriptInMainFrame(page, s, nullptr, nullptr);
    WKRelease(s);
}

void EvalInWorld(WKPageRef page, const std::wstring& world, const std::string& script, void* context, WKPageEvaluateJavaScriptFunction callback,
                 WKFrameInfoRef frame = nullptr) {
    if (!page) { if (callback) callback(nullptr, nullptr, context); return; }
    WKStringRef s = WKStringCreateWithUTF8CStringWithLength(script.data(), script.size());
    WKStringRef w = world.empty() ? nullptr : WKStringCreateWithUTF8CString(ToUtf8(world).c_str());   // none: the page's own world
    WKPageEvaluateJavaScriptInFrameInWorld(page, frame, s, w, context, callback);
    if (w) WKRelease(w);
    WKRelease(s);
}

// ---- tabs as the extensions see them --------------------------------------------------------
bool Visible(const Tab& t) { return t.state == TabState::Live && !t.privateTab; }

std::string TabJson(const Tab& t) {
    int index = -1;
    for (int i = 0; i < (int)g_tabs.size(); ++i) if (g_tabs[i].get() == &t) index = i;
    const bool active = index == g_active;
    const std::wstring url = t.lazyPending ? t.lazyUrl : t.url;
    return "{\"id\":" + std::to_string(t.id) + ",\"index\":" + std::to_string(index) + ",\"windowId\":1" +
           ",\"active\":" + (active ? "true" : "false") + ",\"highlighted\":" + (active ? "true" : "false") +
           ",\"selected\":" + (active ? "true" : "false") + ",\"pinned\":" + (t.pinned ? "true" : "false") +
           ",\"audible\":" + (t.audible ? "true" : "false") + ",\"discarded\":" + ((t.sleeping || t.lazyPending) ? "true" : "false") +
           ",\"autoDiscardable\":true,\"incognito\":false,\"mutedInfo\":{\"muted\":" + (t.muted ? "true" : "false") + "}" +
           ",\"url\":" + JsonQuote(ToUtf8(url)) + ",\"pendingUrl\":" + JsonQuote(ToUtf8(url)) + ",\"title\":" + JsonQuote(ToUtf8(t.title)) +
           ",\"status\":\"" + (t.loading ? "loading" : "complete") + "\",\"groupId\":-1}";
}

Tab* TabById(const JsonValue* v) {
    if (!v || v->type == JsonValue::Null) { Tab* t = ActiveTab(); return t && Visible(*t) ? t : nullptr; }
    if (v->type != JsonValue::Number) return nullptr;
    Tab* t = FindTab((unsigned long long)v->n);
    return t && Visible(*t) ? t : nullptr;
}

// A Chrome match pattern (or "<all_urls>") against a URL.
bool Glob(const std::string& pattern, const std::string& text) {
    size_t p = 0, s = 0, star = std::string::npos, mark = 0;
    while (s < text.size()) {
        if (p < pattern.size() && (pattern[p] == text[s] || pattern[p] == '?')) { ++p; ++s; }
        else if (p < pattern.size() && pattern[p] == '*') { star = p++; mark = s; }
        else if (star != std::string::npos) { p = star + 1; s = ++mark; }
        else return false;
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

bool MatchUrl(const std::string& pattern, const std::string& url) {
    if (pattern == "<all_urls>") return url.rfind("http", 0) == 0 || url.rfind("file:", 0) == 0;
    const size_t sep = pattern.find("://");
    if (sep == std::string::npos) return false;
    const std::string scheme = pattern.substr(0, sep);
    const size_t usep = url.find("://");
    if (usep == std::string::npos) return false;
    const std::string uscheme = url.substr(0, usep);
    if (scheme == "*" ? (uscheme != "http" && uscheme != "https") : scheme != uscheme) return false;
    std::string rest = pattern.substr(sep + 3), urest = url.substr(usep + 3);
    const size_t slash = rest.find('/'), uslash = urest.find('/');
    std::string host = rest.substr(0, slash), path = slash == std::string::npos ? "/*" : rest.substr(slash);
    std::string uhost = urest.substr(0, uslash), upath = uslash == std::string::npos ? "/" : urest.substr(uslash);
    const size_t colon = uhost.find(':');
    if (colon != std::string::npos) uhost.resize(colon);
    if (host != "*") {
        if (host.rfind("*.", 0) == 0) {
            const std::string base = host.substr(2);
            if (uhost != base && !(uhost.size() > base.size() && uhost.compare(uhost.size() - base.size() - 1, std::string::npos, "." + base) == 0)) return false;
        } else if (_stricmp(host.c_str(), uhost.c_str()) != 0) return false;
    }
    return Glob(path, upath);
}

std::string TabsQuery(const JsonValue& info) {
    std::string out = "[";
    for (int i = 0; i < (int)g_tabs.size(); ++i) {
        const Tab& t = *g_tabs[i];
        if (!Visible(t)) continue;
        auto wants = [&](const char* key, bool value) { const JsonValue* v = info.get(key); return !v || v->type != JsonValue::Bool || v->b == value; };
        if (!wants("active", i == g_active) || !wants("highlighted", i == g_active) || !wants("pinned", t.pinned) ||
            !wants("audible", t.audible) || !wants("discarded", t.sleeping || t.lazyPending) || !wants("muted", t.muted))
            continue;
        if (const JsonValue* st = info.get("status")) if (st->isString() && st->s != (t.loading ? "loading" : "complete")) continue;
        if (const JsonValue* ix = info.get("index")) if (ix->type == JsonValue::Number && (int)ix->n != i) continue;
        const std::string url = ToUtf8(t.lazyPending ? t.lazyUrl : t.url);
        if (const JsonValue* u = info.get("url")) {
            bool any = false;
            if (u->isString()) any = MatchUrl(u->s, url);
            else for (auto& p : u->items) if (p.isString() && MatchUrl(p.s, url)) any = true;
            if (!any) continue;
        }
        if (const JsonValue* ti = info.get("title")) if (ti->isString() && !Glob(ti->s, ToUtf8(t.title))) continue;
        if (out.size() > 1) out += ",";
        out += TabJson(t);
    }
    return out + "]";
}

std::wstring ResolveExtensionUrl(const Extension& e, const std::string& url) {
    if (url.empty()) return L"";
    if (url.find("://") != std::string::npos || url.rfind("about:", 0) == 0 || url.rfind("data:", 0) == 0) return ToWide(url);
    std::wstring rel = ToWide(url);
    while (!rel.empty() && rel[0] == L'/') rel.erase(0, 1);
    return ExtBase(e) + rel;
}

void LoadUrl(Tab& t, const std::wstring& url) {
    WKURLRef u = WKURLCreateWithUTF8CString(ToUtf8(url).c_str());
    if (u) { WKPageLoadURL(t.page, u); WKRelease(u); }
}

std::string WindowJson() {
    RECT r = {};
    if (g_hWnd) GetWindowRect(g_hWnd, &r);
    const bool max = g_hWnd && IsZoomed(g_hWnd), min = g_hWnd && IsIconic(g_hWnd);
    return "{\"id\":1,\"focused\":" + std::string(GetForegroundWindow() == g_hWnd ? "true" : "false") +
           ",\"top\":" + std::to_string(r.top) + ",\"left\":" + std::to_string(r.left) + ",\"width\":" + std::to_string(r.right - r.left) +
           ",\"height\":" + std::to_string(r.bottom - r.top) + ",\"incognito\":false,\"type\":\"normal\",\"state\":\"" +
           (min ? "minimized" : max ? "maximized" : "normal") + "\",\"alwaysOnTop\":false}";
}

// ---- events for the extension's pages ---------------------------------------------------------
void ToBackground(Host& h, const std::string& script) {
    if (!h.bgPage) return;
    if (!h.bgReady) { h.queued.push_back(script); return; }
    Eval(h.bgPage, script);
}

void ExtensionPagesEvent(Host& h, const std::string& script, WKPageRef except = nullptr) {
    if (h.bgPage != except) ToBackground(h, script);
    for (HWND w : h.windows)
        if (WKViewRef v = (WKViewRef)GetWindowLongPtrW(w, GWLP_USERDATA))
            if (WKViewGetPage(v) != except) Eval(WKViewGetPage(v), script);
}

void Broadcast(const std::string& name, const std::string& argsJson) {
    const std::string script = "globalThis.__lyraEvent && __lyraEvent(" + JsonQuote(name) + "," + argsJson + ")";
    for (auto& [id, h] : s_hosts) ExtensionPagesEvent(*h, script);
}

// ---- context menu items (chrome.contextMenus) -------------------------------------------------
// Kept in menus.json: extensions create them once, in runtime.onInstalled, and expect them back
// after a restart (as Chrome keeps them). An install or update starts them over.
struct MenuItem {
    std::string id, parentId, type = "normal", title;
    bool numeric = false, checked = false, enabled = true, visible = true;
    std::vector<std::string> contexts, documentUrlPatterns, targetUrlPatterns;
};
std::map<std::wstring, std::vector<MenuItem>> s_menus;
std::set<std::wstring> s_menusLoaded;

std::wstring MenusFile(const Extension& e) { return e.info.dir + L"\\menus.json"; }

void StringList(const JsonValue* v, std::vector<std::string>& out) {
    out.clear();
    if (!v) return;
    if (v->isString()) out.push_back(v->s);
    else if (v->isArray()) for (auto& x : v->items) if (x.isString()) out.push_back(x.s);
}

void ApplyMenuProps(MenuItem& m, const JsonValue& p) {
    if (p.get("type")) m.type = p.str("type", "normal");
    if (p.get("title")) m.title = p.str("title");
    if (const JsonValue* v = p.get("parentId")) m.parentId = v->isString() ? v->s : v->raw == "null" ? std::string() : v->raw;
    if (p.get("checked")) m.checked = p.flag("checked");
    if (p.get("enabled")) m.enabled = p.flag("enabled");
    if (p.get("visible")) m.visible = p.flag("visible");
    if (p.get("contexts")) StringList(p.get("contexts"), m.contexts);
    if (p.get("documentUrlPatterns")) StringList(p.get("documentUrlPatterns"), m.documentUrlPatterns);
    if (p.get("targetUrlPatterns")) StringList(p.get("targetUrlPatterns"), m.targetUrlPatterns);
}

std::string JsonStrings(const std::vector<std::string>& v) {
    std::string s = "[";
    for (auto& x : v) { if (s.size() > 1) s += ","; s += JsonQuote(x); }
    return s + "]";
}

std::vector<MenuItem>& Menus(const Extension& e) {
    auto& list = s_menus[e.info.id];
    if (s_menusLoaded.insert(e.info.id).second) {
        JsonValue v;
        if (JsonParse(ReadText(MenusFile(e)), v) && v.isArray())
            for (auto& x : v.items) {
                if (!x.isObject() || x.str("id").empty()) continue;
                MenuItem m;
                m.id = x.str("id");
                m.numeric = x.flag("numeric");
                ApplyMenuProps(m, x);
                list.push_back(std::move(m));
            }
    }
    return list;
}

void SaveMenus(const Extension& e) {
    const bool t = true;
    std::string s = "[";
    for (auto& m : s_menus[e.info.id]) {
        if (s.size() > 1) s += ",";
        s += "{\"id\":" + JsonQuote(m.id) + ",\"numeric\":" + (m.numeric ? "true" : "false") + ",\"parentId\":" + JsonQuote(m.parentId) +
             ",\"type\":" + JsonQuote(m.type) + ",\"title\":" + JsonQuote(m.title) + ",\"checked\":" + (m.checked ? "true" : "false") +
             ",\"enabled\":" + (m.enabled ? "true" : "false") + ",\"visible\":" + (m.visible ? "true" : "false") +
             ",\"contexts\":" + JsonStrings(m.contexts) + ",\"documentUrlPatterns\":" + JsonStrings(m.documentUrlPatterns) +
             ",\"targetUrlPatterns\":" + JsonStrings(m.targetUrlPatterns) + "}";
    }
    (void)t;
    WriteFileAtomic(MenusFile(e), s + "]");
}

void ForgetMenus(const Extension& e) {
    s_menus.erase(e.info.id);
    s_menusLoaded.insert(e.info.id);
    DeleteFileW(MenusFile(e).c_str());
}

std::string MenusOp(const Extension& e, const JsonValue& msg, const std::string& op) {
    auto& list = Menus(e);
    auto find = [&](const std::string& id) -> int {
        for (size_t i = 0; i < list.size(); ++i) if (list[i].id == id) return (int)i;
        return -1;
    };
    const std::string id = msg.str("id");
    const JsonValue* props = msg.get("props");
    auto notFound = [&](const std::string& what) { return "{\"error\":" + JsonQuote("Cannot find menu item with id " + what) + "}"; };
    if (op == "menus.create") {
        if (id.empty() || !props || !props->isObject()) return "{\"error\":\"bad menu item\"}";
        MenuItem m;
        m.id = id;
        m.numeric = msg.flag("numeric");
        ApplyMenuProps(m, *props);
        if (!m.parentId.empty() && find(m.parentId) < 0) return notFound(m.parentId);
        // The same id again (an extension that creates its items at every start): replaced.
        if (int i = find(id); i >= 0) list[i] = std::move(m); else list.push_back(std::move(m));
    } else if (op == "menus.update") {
        const int i = find(id);
        if (i < 0) return notFound(id);
        if (props && props->isObject()) ApplyMenuProps(list[i], *props);
    } else if (op == "menus.remove") {
        if (find(id) < 0) return notFound(id);
        std::set<std::string> gone = { id };
        for (bool more = true; more;) {          // and everything under it
            more = false;
            for (auto& m : list) if (!m.parentId.empty() && gone.count(m.parentId) && gone.insert(m.id).second) more = true;
        }
        list.erase(std::remove_if(list.begin(), list.end(), [&](const MenuItem& m) { return gone.count(m.id) != 0; }), list.end());
    } else if (op == "menus.removeAll") {
        list.clear();
    } else {
        return "{\"error\":\"unknown operation\"}";
    }
    SaveMenus(e);
    return "{\"value\":null}";
}

bool MenuShows(const MenuItem& m, const ExtensionMenuContext& c, bool inherit) {
    if (!m.visible) return false;
    const bool special = !c.link.empty() || !c.image.empty() || !c.media.empty() || c.editable || c.selection;
    bool any = inherit && m.contexts.empty();
    const std::vector<std::string> page = { "page" };
    for (auto& s : m.contexts.empty() ? page : m.contexts) {
        if (c.action) { if (s == "all" || s == "action" || s == "browser_action" || s == "page_action") any = true; continue; }
        if (s == "all") any = true;
        else if (s == "page" && !special) any = true;
        else if (s == "selection" && c.selection) any = true;
        else if (s == "link" && !c.link.empty()) any = true;
        else if (s == "image" && !c.image.empty()) any = true;
        else if ((s == "video" || s == "audio") && !c.media.empty()) any = true;
        else if (s == "editable" && c.editable) any = true;
    }
    if (!any) return false;
    if (!m.documentUrlPatterns.empty()) {
        bool ok = false;
        for (auto& p : m.documentUrlPatterns) if (MatchUrl(p, ToUtf8(c.pageUrl))) ok = true;
        if (!ok) return false;
    }
    const std::wstring& target = !c.link.empty() ? c.link : !c.image.empty() ? c.image : c.media;
    if (!m.targetUrlPatterns.empty() && !target.empty()) {
        bool ok = false;
        for (auto& p : m.targetUrlPatterns) if (MatchUrl(p, ToUtf8(target))) ok = true;
        if (!ok) return false;
    }
    return true;
}

std::wstring MenuTitle(const MenuItem& m, const ExtensionMenuContext& c) {
    std::wstring title = ToWide(m.title);
    const size_t at = title.find(L"%s");
    if (at != std::wstring::npos) {
        std::wstring sel = c.selectionText;
        for (auto& ch : sel) if (ch == L'\n' || ch == L'\r' || ch == L'\t') ch = L' ';
        if (sel.size() > 32) sel = sel.substr(0, 32) + L"\x2026";
        title.replace(at, 2, sel);
    }
    return title;
}

void MenuChildren(const std::vector<MenuItem>& list, const std::string& parent, const ExtensionMenuContext& c,
                  const std::wstring& extId, std::vector<ExtensionMenuEntry>& out, int depth) {
    for (auto& m : list) {
        if (m.parentId != parent || !MenuShows(m, c, !parent.empty())) continue;
        ExtensionMenuEntry entry;
        entry.extId = extId;
        entry.itemId = m.id;
        entry.separator = m.type == "separator";
        entry.checkable = m.type == "checkbox" || m.type == "radio";
        entry.checked = m.checked;
        entry.enabled = m.enabled;
        entry.title = MenuTitle(m, c);
        if (depth < 4) MenuChildren(list, m.id, c, extId, entry.children, depth + 1);
        out.push_back(std::move(entry));
    }
}

// storage.onChanged everywhere the extension runs: its pages and its scripts in every tab.
void StorageChanged(const Extension& e, const std::string& changes, const std::string& area) {
    if (changes == "{}") return;
    const std::string script = "globalThis.__lyraEvent && (__lyraEvent('storage.onChanged',[" + changes + "," + JsonQuote(area) +
                               "]), __lyraEvent('storage." + area + ".onChanged',[" + changes + "]))";
    if (auto it = s_hosts.find(e.info.id); it != s_hosts.end()) ExtensionPagesEvent(*it->second, script);
    if (e.scripts.empty()) return;
    for (auto& t : g_tabs)
        if (Visible(*t) && t->page) EvalInWorld(t->page, World(e), script, nullptr, nullptr);
}

// ---- messages ---------------------------------------------------------------------------------
std::string DeliverScript(int callId, const std::string& message, const std::string& sender) {
    return "globalThis.__lyraDeliverMessage ? __lyraDeliverMessage(" + std::to_string(callId) + "," + message + "," + sender + ") : false";
}

void OnTabDelivered(WKTypeRef result, WKErrorRef, void* context) {
    const int callId = (int)(intptr_t)context;
    const bool ok = result && WKGetTypeID(result) == WKBooleanGetTypeID() && WKBooleanGetValue((WKBooleanRef)result);
    if (!ok) Complete(callId, kNoReceiver);
}

struct ScriptCall { int callId; bool css; };
void OnScriptDone(WKTypeRef result, WKErrorRef, void* context) {
    std::unique_ptr<ScriptCall> c((ScriptCall*)context);
    if (c->css) { Complete(c->callId, "{\"value\":null}"); return; }
    std::string r = result && WKGetTypeID(result) == WKStringGetTypeID() ? ToUtf8(WKStr((WKStringRef)result)) : std::string();
    JsonValue v;
    if (r.empty() || !JsonParse(r, v)) { Complete(c->callId, "{\"error\":\"the script could not run in this tab\"}"); return; }
    if (const JsonValue* err = v.get("e")) { Complete(c->callId, "{\"error\":" + JsonQuote(err->s) + "}"); return; }
    const JsonValue* value = v.get("r");
    Complete(c->callId, "{\"value\":[{\"frameId\":0,\"result\":" + (value ? value->raw : std::string("null")) + "}]}");
}

std::string Files(const Extension& e, const JsonValue* files) {
    std::string code;
    if (!files) return code;
    for (auto& f : files->items)
        if (f.isString() && SafeRelative(ToWide(f.s))) code += ReadText(e.info.dir + L"\\" + Native(ToWide(f.s))) + "\n;\n";
    return code;
}

// ---- long-lived connections: runtime.connect / tabs.connect --------------------------------------
// One side opens a port; the other side is every page of the extension that listens for
// runtime.onConnect (or the extension's scripts in a tab). Messages from the opener go to all the
// receivers, a receiver's go to the opener. A side that goes away (its page navigates, closes,
// or it disconnects) is heard by the other side as onDisconnect.
struct PortEnd {
    WKPageRef page = nullptr;
    WKFrameInfoRef frame = nullptr;      // a content script in a subframe (retained); null: the main frame
    bool content = false;                // the extension's scripts in a web page (else one of its own pages)
    unsigned long long tabId = 0;        // content: the tab
};
struct Port { std::wstring ext; PortEnd opener; std::vector<PortEnd> receivers; };
std::map<int, Port> s_ports;
int s_nextPort = 1;

void ReleaseEnd(PortEnd& end) {
    if (end.frame) WKRelease(end.frame);
    end.frame = nullptr;
}

void ToEnd(const std::wstring& extId, const PortEnd& end, const std::string& script, void* context = nullptr,
           WKPageEvaluateJavaScriptFunction callback = nullptr) {
    Extension* e = Find(extId);
    if (!e || !end.page) { if (callback) callback(nullptr, nullptr, context); return; }
    if (end.content) { EvalInWorld(end.page, World(*e), script, context, callback, end.frame); return; }
    if (end.frame) { EvalInWorld(end.page, L"", script, context, callback, end.frame); return; }   // its page inside a tab
    auto it = s_hosts.find(extId);
    if (it != s_hosts.end() && it->second->bgPage == end.page) ToBackground(*it->second, script);
    else Eval(end.page, script);
}

std::string PortScript(const char* fn, int id, const std::string& args) {
    return std::string("globalThis.") + fn + " ? " + fn + "(" + std::to_string(id) + (args.empty() ? "" : "," + args) + ") : false";
}

// The opener has gone: every receiver hears it.
void ClosePort(int id) {
    auto it = s_ports.find(id);
    if (it == s_ports.end()) return;
    Port p = std::move(it->second);
    s_ports.erase(it);
    for (auto& r : p.receivers) { ToEnd(p.ext, r, PortScript("__lyraPortDisconnect", id, "")); ReleaseEnd(r); }
    ReleaseEnd(p.opener);
}

// A receiver has gone: once none is left the opener hears it.
void DropReceiver(int id, size_t index) {
    auto it = s_ports.find(id);
    if (it == s_ports.end() || index >= it->second.receivers.size()) return;
    Port& p = it->second;
    ReleaseEnd(p.receivers[index]);
    p.receivers.erase(p.receivers.begin() + (ptrdiff_t)index);
    if (!p.receivers.empty()) return;
    ToEnd(p.ext, p.opener, PortScript("__lyraPortDisconnect", id, ""));
    ReleaseEnd(p.opener);
    s_ports.erase(it);
}

// Sides that are no longer there (a page that navigated or closed): the other sides hear it.
void DropPorts(const std::function<bool(const PortEnd&)>& gone) {
    std::vector<int> ids;
    for (auto& [id, p] : s_ports) ids.push_back(id);
    for (int id : ids) {
        auto it = s_ports.find(id);
        if (it == s_ports.end()) continue;
        if (gone(it->second.opener)) { it->second.opener.page = nullptr; ClosePort(id); continue; }
        for (size_t i = it->second.receivers.size(); i-- > 0;) {
            auto jt = s_ports.find(id);
            if (jt == s_ports.end()) break;
            if (gone(jt->second.receivers[i])) { jt->second.receivers[i].page = nullptr; DropReceiver(id, i); }
        }
    }
}

// The extension stops: its ports go without a word (its scripts lose the bridge with it).
void ForgetPorts(const std::wstring& extId) {
    for (auto it = s_ports.begin(); it != s_ports.end();) {
        if (it->second.ext != extId) { ++it; continue; }
        ReleaseEnd(it->second.opener);
        for (auto& r : it->second.receivers) ReleaseEnd(r);
        it = s_ports.erase(it);
    }
}

void OnPortReached(WKTypeRef result, WKErrorRef, void* context) {
    const int id = (int)(intptr_t)context;
    const bool ok = result && WKGetTypeID(result) == WKBooleanGetTypeID() && WKBooleanGetValue((WKBooleanRef)result);
    if (ok) return;
    auto it = s_ports.find(id);
    if (it == s_ports.end()) return;
    for (size_t i = 0; i < it->second.receivers.size(); ++i)
        if (it->second.receivers[i].content) { it->second.receivers[i].page = nullptr; DropReceiver(id, i); return; }
}

Tab* TabOfPage(WKPageRef page) {
    for (auto& t : g_tabs) if (page && t->page == page) return Visible(*t) ? t.get() : nullptr;
    return nullptr;
}

// runtime.MessageSender for a message (or a port) from `fromPage`.
std::string SenderJson(const Extension& e, bool content, WKPageRef fromPage, bool mainFrame) {
    std::string sender = "{\"id\":" + JsonQuote(ToUtf8(e.info.id));
    if (content) {
        if (Tab* tab = TabOfPage(fromPage))
            sender += ",\"tab\":" + TabJson(*tab) + ",\"url\":" + JsonQuote(ToUtf8(tab->url)) + ",\"frameId\":" + (mainFrame ? "0" : "1") +
                      ",\"origin\":" + JsonQuote(ToUtf8(tab->url));
    } else sender += ",\"url\":" + JsonQuote(ToUtf8(ExtBase(e)));
    return sender + "}";
}

std::vector<WKPageRef> ExtensionPages(Host* host, WKPageRef except) {
    std::vector<WKPageRef> pages;
    if (!host) return pages;
    if (host->bgPage && host->bgPage != except) pages.push_back(host->bgPage);
    for (HWND w : host->windows)
        if (WKViewRef v = (WKViewRef)GetWindowLongPtrW(w, GWLP_USERDATA))
            if (WKViewGetPage(v) != except) pages.push_back(WKViewGetPage(v));
    return pages;
}

// ---- the toolbar button's state: badge, title, icon, pop-up, for one tab or for all ----------------
struct ActionState {
    std::map<std::string, std::string> values;   // "badgeText", "badgeText:<tab id>", ... -> JSON
    std::unique_ptr<Bitmap> icon;
    std::string iconFor;                         // what the icon was made from
};
std::map<std::wstring, ActionState> s_actions;

const std::string* ActionValue(ActionState& a, const std::string& what, unsigned long long tab) {
    if (tab) if (auto it = a.values.find(what + ":" + std::to_string(tab)); it != a.values.end()) return &it->second;
    if (auto it = a.values.find(what); it != a.values.end()) return &it->second;
    return nullptr;
}

std::string JsonString(const std::string* raw) {
    JsonValue v;
    return raw && JsonParse(*raw, v) && v.isString() ? v.s : std::string();
}

// A CSS colour (#rgb, #rrggbb, #rrggbbaa, rgb() / rgba(), a few names) or [r, g, b, a].
bool ParseColor(const std::string& raw, Color& out) {
    JsonValue v;
    if (!JsonParse(raw, v)) return false;
    if (v.type == JsonValue::Array && v.items.size() >= 3) {
        auto ch = [&](size_t i, int def) { return i < v.items.size() && v.items[i].type == JsonValue::Number ? std::clamp((int)v.items[i].n, 0, 255) : def; };
        out = Color((BYTE)ch(3, 255), (BYTE)ch(0, 0), (BYTE)ch(1, 0), (BYTE)ch(2, 0));
        return true;
    }
    if (!v.isString()) return false;
    std::string s = v.s;
    for (auto& ch : s) ch = (char)tolower((unsigned char)ch);
    static const std::pair<const char*, const char*> names[] = {
        { "red", "#ff0000" }, { "green", "#008000" }, { "blue", "#0000ff" }, { "black", "#000000" }, { "white", "#ffffff" },
        { "gray", "#808080" }, { "grey", "#808080" }, { "orange", "#ffa500" }, { "yellow", "#ffff00" }, { "purple", "#800080" },
        { "transparent", "#00000000" } };
    for (auto& n : names) if (s == n.first) s = n.second;
    if (s.size() > 1 && s[0] == '#') {
        std::string hex = s.substr(1);
        if (hex.size() == 3 || hex.size() == 4) { std::string x; for (char ch : hex) { x += ch; x += ch; } hex = x; }
        if (hex.size() != 6 && hex.size() != 8) return false;
        const unsigned long n = strtoul(hex.c_str(), nullptr, 16);
        if (hex.size() == 6) out = Color(255, (BYTE)(n >> 16), (BYTE)(n >> 8), (BYTE)n);
        else out = Color((BYTE)n, (BYTE)(n >> 24), (BYTE)(n >> 16), (BYTE)(n >> 8));
        return true;
    }
    int r = 0, g = 0, b = 0;
    float a = 1;
    if (sscanf_s(s.c_str(), "rgba(%d,%d,%d,%f)", &r, &g, &b, &a) >= 3 || sscanf_s(s.c_str(), "rgb(%d,%d,%d)", &r, &g, &b) == 3) {
        out = Color((BYTE)std::clamp((int)(a * 255.f), 0, 255), (BYTE)std::clamp(r, 0, 255), (BYTE)std::clamp(g, 0, 255), (BYTE)std::clamp(b, 0, 255));
        return true;
    }
    return false;
}

std::string ColorJson(const Color& c) {
    return "[" + std::to_string(c.GetR()) + "," + std::to_string(c.GetG()) + "," + std::to_string(c.GetB()) + "," + std::to_string(c.GetA()) + "]";
}

// The pop-up of a toolbar button (one at a time).
struct PopupState {
    HWND wnd = nullptr;
    std::wstring id;
    RectF anchor;                        // its button, in the main window's client coordinates
    bool shown = false;                  // placed and visible (it waits off-screen for its size)
    DWORD closedAt = 0;
    std::wstring closedId;
};
PopupState s_popup;

WKPageRef PopupPage() {
    WKViewRef v = s_popup.wnd ? (WKViewRef)GetWindowLongPtrW(s_popup.wnd, GWLP_USERDATA) : nullptr;
    return v ? WKViewGetPage(v) : nullptr;
}
void PlacePopup(int cssW, int cssH);
void ClosePopup();
void OpenPopup(const Extension& e, const std::wstring& url, const RectF& anchor);
bool ButtonAnchor(const std::wstring& id, RectF& anchor);

HWND OpenExtensionWindow(Host& h, const std::wstring& url, const std::wstring& title);

// ---- declarativeNetRequest: rulesets, dynamic and session rules -> WebKit content blockers ------
// The rules are converted in the background (browser/ExtensionDnr.cpp), then compiled and put in
// front of every page (browser/AdBlock.cpp). State: dnr-state.json (rulesets on, rules off) and
// dnr-dynamic.json in the extension's folder; session rules only in memory.
struct DnrState {
    bool loaded = false;
    std::set<std::string> enabled;                              // static rulesets on
    std::map<std::string, std::set<long long>> disabled;        // per ruleset: rule ids off
    std::vector<std::pair<long long, std::string>> dynamicRules, sessionRules;   // id, rule as JSON
    unsigned changes = 0;
};
std::map<std::wstring, DnrState> s_dnr;
std::map<std::wstring, std::string> s_dnrApplied;             // extension -> what its list was built from
std::map<std::wstring, unsigned> s_dnrGen;
struct DnrJob { std::wstring extId; unsigned gen; DnrResult result; };

std::wstring DnrStateFile(const Extension& e) { return e.info.dir + L"\\dnr-state.json"; }
std::wstring DnrDynamicFile(const Extension& e) { return e.info.dir + L"\\dnr-dynamic.json"; }

void ParseRules(const std::string& text, std::vector<std::pair<long long, std::string>>& out) {
    JsonValue v;
    if (!JsonParse(text, v) || !v.isArray()) return;
    for (auto& r : v.items) if (r.isObject() && r.get("id")) out.push_back({ (long long)r.get("id")->n, r.raw });
}

std::string JoinRules(const std::vector<std::pair<long long, std::string>>& rules) {
    std::string s = "[";
    for (auto& [id, r] : rules) { if (s.size() > 1) s += ","; s += r; }
    return s + "]";
}

DnrState& Dnr(const Extension& e) {
    DnrState& st = s_dnr[e.info.id];
    if (st.loaded) return st;
    st.loaded = true;
    JsonValue v;
    const bool saved = JsonParse(ReadText(DnrStateFile(e)), v) && v.isObject();
    if (saved && v.get("enabled")) { for (auto& id : v.get("enabled")->items) if (id.isString()) st.enabled.insert(id.s); }
    else for (auto& rs : e.rulesets) if (rs.enabled) st.enabled.insert(rs.id);
    if (saved)
        if (const JsonValue* d = v.get("disabled"))
            for (auto& [rs, ids] : d->members) for (auto& id : ids.items) st.disabled[rs].insert((long long)id.n);
    ParseRules(ReadText(DnrDynamicFile(e)), st.dynamicRules);
    return st;
}

void SaveDnrState(const Extension& e, const DnrState& st) {
    std::string s = "{\"enabled\":[";
    bool first = true;
    for (auto& id : st.enabled) { s += (first ? "" : ",") + JsonQuote(id); first = false; }
    s += "],\"disabled\":{";
    first = true;
    for (auto& [rs, ids] : st.disabled) {
        if (ids.empty()) continue;
        s += (first ? "" : ",") + JsonQuote(rs) + ":[";
        bool f = true;
        for (long long id : ids) { s += (f ? "" : ",") + std::to_string(id); f = false; }
        s += "]";
        first = false;
    }
    WriteFileAtomic(DnrStateFile(e), s + "}}");
}

// What the extension's list is built from; when that changed it is converted (in the background)
// and compiled again.
void DnrSync(const Extension& e) {
    DnrState& st = Dnr(e);
    std::vector<DnrSource> src;
    std::string sig;
    if (e.info.enabled) {
        for (auto& rs : e.rulesets)
            if (st.enabled.count(rs.id)) {
                src.push_back({ e.info.dir + L"\\" + rs.path, "", st.disabled[rs.id] });
                sig += rs.id + ",";
            }
        if (!st.dynamicRules.empty()) src.push_back({ L"", JoinRules(st.dynamicRules), {} });
        if (!st.sessionRules.empty()) src.push_back({ L"", JoinRules(st.sessionRules), {} });
    }
    if (!src.empty()) sig += "|" + ToUtf8(e.info.version) + "|" + std::to_string(st.changes);
    auto it = s_dnrApplied.find(e.info.id);
    if (it == s_dnrApplied.end() ? sig.empty() : it->second == sig) return;
    s_dnrApplied[e.info.id] = sig;
    const unsigned gen = ++s_dnrGen[e.info.id];
    if (src.empty()) { AdBlockSetExtensionRules(e.info.id, "", ""); return; }
    const std::wstring id = e.info.id, dir = e.info.dir;
    std::thread([id, dir, src, gen] {
        auto* job = new DnrJob{ id, gen, DnrConvert(src, dir) };
        if (!g_hWnd || !PostMessageW(g_hWnd, WM_APP_DNR, 0, (LPARAM)job)) delete job;
    }).detach();
}

void DnrSyncAll() {
    for (const Extension& e : s_exts) DnrSync(e);
    for (auto it = s_dnrApplied.begin(); it != s_dnrApplied.end();) {      // removed extensions
        if (Find(it->first)) { ++it; continue; }
        ++s_dnrGen[it->first];
        AdBlockSetExtensionRules(it->first, "", "");
        it = s_dnrApplied.erase(it);
    }
}

std::string DnrError(const std::string& text) { return "{\"error\":" + JsonQuote(text) + "}"; }

std::string DnrOp(const Extension& e, const JsonValue& msg, const std::string& op) {
    DnrState& st = Dnr(e);
    if (op == "dnr.isRegexSupported")
        return DnrRegexSupported(msg.str("regex")) ? "{\"value\":{\"isSupported\":true}}"
                                                   : "{\"value\":{\"isSupported\":false,\"reason\":\"syntaxError\"}}";
    if (op == "dnr.getEnabledRulesets") {
        std::string s = "[";
        for (auto& rs : e.rulesets) if (st.enabled.count(rs.id)) { if (s.size() > 1) s += ","; s += JsonQuote(rs.id); }
        return "{\"value\":" + s + "]}";
    }
    if (op == "dnr.updateEnabledRulesets") {
        auto known = [&](const std::string& id) { for (auto& rs : e.rulesets) if (rs.id == id) return true; return false; };
        std::vector<std::string> on, off;
        for (const char* key : { "enable", "disable" })
            if (const JsonValue* list = msg.get(key))
                for (auto& v : list->items) {
                    if (!v.isString() || !known(v.s)) return DnrError("Invalid ruleset id: " + v.s + ".");
                    (key[0] == 'e' ? on : off).push_back(v.s);
                }
        for (auto& id : off) st.enabled.erase(id);
        for (auto& id : on) st.enabled.insert(id);
        ++st.changes;
        SaveDnrState(e, st);
        DnrSync(e);
        return "{\"value\":null}";
    }
    if (op == "dnr.updateStaticRules") {
        std::set<long long>& off = st.disabled[msg.str("rulesetId")];
        if (const JsonValue* list = msg.get("disable")) for (auto& v : list->items) off.insert((long long)v.n);
        if (const JsonValue* list = msg.get("enable")) for (auto& v : list->items) off.erase((long long)v.n);
        ++st.changes;
        SaveDnrState(e, st);
        DnrSync(e);
        return "{\"value\":null}";
    }
    if (op == "dnr.getDisabledRuleIds") {
        std::string s = "[";
        for (long long id : st.disabled[msg.str("rulesetId")]) { if (s.size() > 1) s += ","; s += std::to_string(id); }
        return "{\"value\":" + s + "]}";
    }
    const bool session = msg.flag("session");
    auto& rules = session ? st.sessionRules : st.dynamicRules;
    if (op == "dnr.getRules") {
        std::set<long long> only;
        const JsonValue* ids = msg.get("ids");
        const bool filter = ids && ids->isArray();
        if (filter) for (auto& v : ids->items) only.insert((long long)v.n);
        std::string s = "[";
        for (auto& [id, r] : rules) if (!filter || only.count(id)) { if (s.size() > 1) s += ","; s += r; }
        return "{\"value\":" + s + "]}";
    }
    if (op == "dnr.updateRules") {
        std::set<long long> remove;
        if (const JsonValue* list = msg.get("remove")) for (auto& v : list->items) remove.insert((long long)v.n);
        std::vector<std::pair<long long, std::string>> next;
        std::set<long long> ids;
        for (auto& [id, r] : rules) if (!remove.count(id)) { next.push_back({ id, r }); ids.insert(id); }
        if (const JsonValue* list = msg.get("add"))
            for (auto& r : list->items) {
                const JsonValue* idv = r.isObject() ? r.get("id") : nullptr;
                if (!idv || idv->type != JsonValue::Number || idv->n < 1) return DnrError("Rules must have a positive integer id.");
                const long long id = (long long)idv->n;
                const std::string name = "Rule with id " + std::to_string(id);
                if (!ids.insert(id).second) return DnrError(name + " does not have a unique ID.");
                const JsonValue* a = r.get("action");
                if (!a || a->str("type").empty()) return DnrError(name + " does not specify the action type.");
                if (const JsonValue* c = r.get("condition"))
                    if (!c->str("regexFilter").empty() && !DnrRegexSupported(c->str("regexFilter")))
                        return DnrError(name + " specified a non-supported regexFilter.");
                next.push_back({ id, r.raw });
            }
        rules.swap(next);
        if (!session) WriteFileAtomic(DnrDynamicFile(e), JoinRules(rules));
        ++st.changes;
        DnrSync(e);
        return "{\"value\":null}";
    }
    return "{\"error\":\"unknown operation\"}";
}

void HandleMessage(const HandlerCtx& ctx, WKScriptMessageRef message, WKCompletionListenerRef reply, std::string& answer, bool& deferred) {
    Extension* e = Find(ctx.id);
    WKTypeRef body = WKScriptMessageGetBody(message);
    if (!e || !e->info.enabled) { answer = "{\"error\":\"the extension is off\"}"; return; }
    JsonValue msg;
    // (4 levels: msg.add[i].condition.regexFilter; deeper lists such as requestDomains stay text)
    if (!body || WKGetTypeID(body) != WKStringGetTypeID() || !JsonParseShallow(ToUtf8(WKStr((WKStringRef)body)), msg, 4) || !msg.isObject()) {
        answer = "{\"error\":\"bad message\"}";
        return;
    }
    const std::string op = msg.str("op");
    WKPageRef fromPage = nullptr;
    WKFrameInfoRef fromFrame = WKScriptMessageGetFrameInfo(message);
    bool mainFrame = true;
    if (fromFrame) { fromPage = WKFrameInfoGetPage(fromFrame); mainFrame = WKFrameInfoGetIsMainFrame(fromFrame); }
    Host* host = s_hosts.count(e->info.id) ? s_hosts[e->info.id].get() : nullptr;

    if (op.rfind("storage.", 0) == 0) {
        answer = Storage(*e, msg, op);
        if (op != "storage.get") {
            JsonValue a;
            if (JsonParse(answer, a)) if (const JsonValue* v = a.get("value")) StorageChanged(*e, v->raw, msg.str("area", "local"));
        }
        return;
    }
    const JsonValue* payload = msg.get("message");
    const std::string messageJson = payload ? payload->raw : "null";
    if (op == "runtime.sendMessage") {
        // From a page's scripts: to the background page and the extension's windows. From one
        // of those: to the others.
        const std::string sender = SenderJson(*e, ctx.content, fromPage, mainFrame);
        std::vector<WKPageRef> targets = ExtensionPages(host, fromPage);
        if (targets.empty()) { answer = kNoReceiver; return; }
        const int callId = NewCall(reply, (int)targets.size());
        deferred = true;
        for (WKPageRef p : targets) {
            const std::string script = DeliverScript(callId, messageJson, sender);
            if (p == host->bgPage) ToBackground(*host, script); else Eval(p, script);
        }
        return;
    }
    if (op == "port.connect") {
        PortEnd opener;
        opener.page = fromPage;
        opener.content = ctx.content;
        if (ctx.content) {
            if (Tab* t = TabOfPage(fromPage)) opener.tabId = t->id;
            if (fromFrame && !mainFrame) { opener.frame = fromFrame; WKRetain(fromFrame); }
        } else if (ctx.tabFrame) {                           // its page inside a tab: answered in that frame
            if (Tab* t = TabOfPage(fromPage)) opener.tabId = t->id;
            if (fromFrame) { opener.frame = fromFrame; WKRetain(fromFrame); }
        }
        const JsonValue* tabId = msg.get("tabId");
        const bool toTab = tabId && tabId->type == JsonValue::Number;
        std::vector<PortEnd> receivers;
        if (toTab) {
            Tab* t = ctx.content ? nullptr : TabById(tabId);
            if (t && t->page && !e->scripts.empty()) { PortEnd r; r.page = t->page; r.content = true; r.tabId = t->id; receivers.push_back(r); }
        } else {
            for (WKPageRef p : ExtensionPages(host, fromPage)) { PortEnd r; r.page = p; receivers.push_back(r); }
        }
        const int id = s_nextPort++;
        if (receivers.empty()) {
            ReleaseEnd(opener);
            answer = "{\"value\":{\"id\":" + std::to_string(id) + ",\"alone\":true}}";
            return;
        }
        const std::string sender = toTab ? SenderJson(*e, false, nullptr, true) : SenderJson(*e, ctx.content || ctx.tabFrame, fromPage, mainFrame);
        s_ports[id] = Port{ e->info.id, opener, receivers };
        const std::string script = PortScript("__lyraPortConnect", id, JsonQuote(msg.str("name")) + "," + sender);
        for (const PortEnd& r : receivers) {
            if (r.content) ToEnd(e->info.id, r, script, (void*)(intptr_t)id, OnPortReached);
            else ToEnd(e->info.id, r, script);
        }
        answer = "{\"value\":{\"id\":" + std::to_string(id) + "}}";
        return;
    }
    if (op == "port.post" || op == "port.close") {
        const int id = msg.get("id") ? (int)msg.get("id")->n : 0;
        answer = "{\"value\":null}";
        auto it = s_ports.find(id);
        if (it == s_ports.end()) return;
        Port& p = it->second;
        auto same = [&](const PortEnd& end) { return end.page == fromPage && end.content == ctx.content && (!ctx.tabFrame || end.frame); };
        const bool fromOpener = same(p.opener);
        if (op == "port.post") {
            const std::string script = PortScript("__lyraPortMessage", id, messageJson);
            if (fromOpener) { for (const PortEnd& r : std::vector<PortEnd>(p.receivers)) ToEnd(p.ext, r, script); }
            else ToEnd(p.ext, p.opener, script);
            return;
        }
        if (fromOpener) { p.opener.page = nullptr; ClosePort(id); return; }
        for (size_t i = 0; i < p.receivers.size(); ++i)
            if (same(p.receivers[i])) { p.receivers[i].page = nullptr; DropReceiver(id, i); break; }
        return;
    }
    if (op == "message.reply") {
        const int callId = msg.get("callId") ? (int)msg.get("callId")->n : 0;
        auto it = s_calls.find(callId);
        if (it != s_calls.end()) {
            if (msg.flag("answered")) Complete(callId, "{\"value\":" + (msg.get("value") ? msg.get("value")->raw : std::string("null")) + "}");
            else if (--it->second.waiting <= 0) Complete(callId, "{\"value\":null}");
        }
        answer = "{\"value\":null}";
        return;
    }
    if (ctx.content) { answer = "{\"error\":\"" + op + " is not available to content scripts\"}"; return; }

    // ---- the extension's own pages from here on ----
    if (op.rfind("menus.", 0) == 0) { answer = MenusOp(*e, msg, op); return; }
    if (op == "tabs.query") { answer = "{\"value\":" + TabsQuery(msg.get("info") ? *msg.get("info") : JsonValue()) + "}"; return; }
    if (op == "tabs.get") {
        Tab* t = TabById(msg.get("tabId"));
        answer = t ? "{\"value\":" + TabJson(*t) + "}" : "{\"error\":\"No tab with this id.\"}";
        return;
    }
    if (op == "windows.get") { answer = "{\"value\":" + WindowJson() + "}"; return; }
    if (op == "tabs.create") {
        const JsonValue* props = msg.get("props");
        const std::wstring url = ResolveExtensionUrl(*e, props ? props->str("url") : "");
        const bool active = !props || props->flag("active", true);
        Tab* t = OpenInNewTab(url.empty() ? L"lyra://start" : url, !active, false);
        answer = t ? "{\"value\":" + TabJson(*t) + "}" : "{\"error\":\"the tab could not be opened\"}";
        return;
    }
    if (op == "tabs.update") {
        Tab* t = TabById(msg.get("tabId"));
        if (!t) { answer = "{\"error\":\"No tab with this id.\"}"; return; }
        if (const JsonValue* props = msg.get("props")) {
            const std::string url = props->str("url");
            if (!url.empty()) LoadUrl(*t, ResolveExtensionUrl(*e, url));
            if (props->flag("active") || props->flag("highlighted")) {
                for (int i = 0; i < (int)g_tabs.size(); ++i) if (g_tabs[i].get() == t) SelectTab(i);
            }
        }
        answer = "{\"value\":" + TabJson(*t) + "}";
        return;
    }
    if (op == "tabs.remove") {
        std::vector<int> close;
        if (const JsonValue* ids = msg.get("tabIds"))
            for (auto& v : ids->items) {
                int index = -1;
                if (v.type == JsonValue::Number) if (Tab* t = FindTab((unsigned long long)v.n, &index); t && Visible(*t)) close.push_back(index);
            }
        std::sort(close.rbegin(), close.rend());
        for (int i : close) CloseTab(i);
        answer = "{\"value\":null}";
        return;
    }
    if (op == "tabs.reload") {
        if (Tab* t = TabById(msg.get("tabId"))) WKPageReload(t->page);
        answer = "{\"value\":null}";
        return;
    }
    if (op == "tabs.sendMessage") {
        Tab* t = TabById(msg.get("tabId"));
        if (!t || !t->page || e->scripts.empty()) { answer = kNoReceiver; return; }
        const std::string sender = "{\"id\":" + JsonQuote(ToUtf8(e->info.id)) + ",\"url\":" + JsonQuote(ToUtf8(ExtBase(*e))) + "}";
        const int callId = NewCall(reply, 1);
        deferred = true;
        EvalInWorld(t->page, World(*e), DeliverScript(callId, messageJson, sender), (void*)(intptr_t)callId, OnTabDelivered);
        return;
    }
    if (op == "scripting.executeScript" || op == "scripting.insertCSS") {
        Tab* t = TabById(msg.get("tabId"));
        if (!t || !t->page) { answer = "{\"error\":\"No tab with this id.\"}"; return; }
        const bool css = op == "scripting.insertCSS";
        const bool mainWorld = msg.str("world") == "MAIN";
        std::string script;
        if (css) {
            const std::string text = msg.str("css") + Files(*e, msg.get("files"));
            script = std::string(kCssInjector) + "(" + JsonQuote(text) + ")";
        } else {
            const std::string code = msg.get("code") && msg.get("code")->isString() ? msg.str("code") : Files(*e, msg.get("files"));
            // The extension's API first if no content script of it ran in this page yet (not in the
            // page's own world, world: "MAIN": the page must not get it).
            const std::string api = mainWorld ? std::string() : "if (!globalThis.__lyraExtension) { globalThis.__lyraExtension = true; (" + LoadApi() + ")(" + ExtJson(*e, "content") + "); }\n";
            script = api + "(() => { try { const r = (0, eval)(" + JsonQuote(code) + "); return JSON.stringify({ r: r === undefined ? null : r }); } "
                     "catch (e) { return JSON.stringify({ e: String(e && e.message || e) }); } })()";
        }
        const int callId = NewCall(reply, 1);
        deferred = true;
        EvalInWorld(t->page, mainWorld ? std::wstring() : World(*e), script, new ScriptCall{ callId, css }, OnScriptDone);
        return;
    }
    if (op == "debug.log") {
        Log("extensions: %ls %s %s: %s", e->info.id.c_str(), msg.str("url").c_str(), msg.str("level").c_str(), msg.str("text").substr(0, 600).c_str());
        answer = "{\"value\":null}";
        return;
    }
    if (op.rfind("dnr.", 0) == 0) { answer = DnrOp(*e, msg, op); return; }
    if (op == "scripting.registerContentScripts" || op == "scripting.getRegisteredContentScripts" ||
        op == "scripting.unregisterContentScripts" || op == "scripting.updateContentScripts") {
        answer = ScriptingRegistered(*e, msg, op);
        return;
    }
    if (op == "action.set") {
        const JsonValue* d = msg.get("details");
        const std::string what = msg.str("what");
        if (!what.empty() && d) {
            ActionState& a = s_actions[e->info.id];
            const JsonValue* tab = d->get("tabId");
            const std::string key = what + (tab && tab->type == JsonValue::Number ? ":" + std::to_string((unsigned long long)tab->n) : std::string());
            const JsonValue* v = d->get("value");
            if (!v || v->type == JsonValue::Null) a.values.erase(key);
            else a.values[key] = v->raw;
            InvalidateChrome();
        }
        answer = "{\"value\":null}";
        return;
    }
    if (op == "action.get") {
        const JsonValue* d = msg.get("details");
        const std::string what = msg.str("what");
        const JsonValue* tab = d ? d->get("tabId") : nullptr;
        const std::string* v = ActionValue(s_actions[e->info.id], what, tab && tab->type == JsonValue::Number ? (unsigned long long)tab->n : 0);
        std::string value = "null";
        if (what == "badgeColor" || what == "badgeTextColor") {
            Color col = what == "badgeColor" ? g_th.accent : Color(255, 255, 255, 255);
            if (v) ParseColor(*v, col);
            value = ColorJson(col);
        } else if (what == "popup") {
            const std::string popup = v ? JsonString(v) : e->defaultPopup;
            value = JsonQuote(popup.empty() ? std::string() : ToUtf8(ResolveExtensionUrl(*e, popup)));
        } else if (v) value = *v;
        else if (what == "title") value = JsonQuote(ToUtf8(e->defaultTitle));
        else if (what == "enabled") value = "true";
        else if (what == "badgeText") value = "\"\"";
        answer = "{\"value\":" + value + "}";
        return;
    }
    if (op == "action.openPopup") {
        Tab* t = ActiveTab();
        const std::string* v = ActionValue(s_actions[e->info.id], "popup", t ? t->id : 0);
        const std::string popup = v ? JsonString(v) : e->defaultPopup;
        RectF anchor;
        if (popup.empty() || !ButtonAnchor(e->info.id, anchor)) { answer = "{\"error\":\"The extension has no pop-up on the toolbar.\"}"; return; }
        OpenPopup(*e, ResolveExtensionUrl(*e, popup), anchor);
        answer = "{\"value\":null}";
        return;
    }
    if (op == "popup.size") {
        if (s_popup.wnd && fromPage && PopupPage() == fromPage)
            PlacePopup(msg.get("w") ? (int)msg.get("w")->n : 0, msg.get("h") ? (int)msg.get("h")->n : 0);
        answer = "{\"value\":null}";
        return;
    }
    if (op == "window.close") {
        if (s_popup.wnd && fromPage && PopupPage() == fromPage) ClosePopup();
        else if (host)
            for (HWND w : host->windows)
                if (WKViewRef v = (WKViewRef)GetWindowLongPtrW(w, GWLP_USERDATA)) if (WKViewGetPage(v) == fromPage) PostMessageW(w, WM_CLOSE, 0, 0);
        answer = "{\"value\":null}";
        return;
    }
    if (op == "runtime.openOptionsPage") {
        if (e->optionsPage.empty() || !host) { answer = "{\"error\":\"This extension has no options page.\"}"; return; }
        OpenExtensionWindow(*host, ResolveExtensionUrl(*e, e->optionsPage), e->info.name);
        answer = "{\"value\":null}";
        return;
    }
    if (op == "runtime.reload") { answer = "{\"value\":null}"; PostMessageW(g_hWnd, WM_NULL, 0, 0); return; }
    answer = "{\"error\":\"" + op + " is not supported yet\"}";
}

// An extension's page inside a tab: which extension, from the frame's origin as the engine reports
// it (a site's own frames, which can reach this handler too, are turned away).
bool TabFrameExtension(WKScriptMessageRef message, std::wstring& id) {
    WKFrameInfoRef frame = WKScriptMessageGetFrameInfo(message);
    if (!frame) return false;
    WKSecurityOriginRef origin = WKFrameInfoCopySecurityOrigin(frame);
    if (!origin) return false;
    std::wstring protocol, host;
    if (WKStringRef s = WKSecurityOriginCopyProtocol(origin)) { protocol = WKStr(s); WKRelease(s); }
    if (WKStringRef s = WKSecurityOriginCopyHost(origin)) { host = WKStr(s); WKRelease(s); }
    WKRelease(origin);
    if (_wcsicmp(protocol.c_str(), kExtScheme) != 0 || host.empty()) return false;
    for (const Extension& x : s_exts)
        if (_wcsicmp(x.info.id.c_str(), host.c_str()) == 0) { id = x.info.id; return true; }
    return false;
}

void OnMessage(WKScriptMessageRef message, WKCompletionListenerRef reply, const void* context) {
    const ULONGLONG started = GetTickCount64();
    std::string answer;
    bool deferred = false;
    HandlerCtx frameCtx;
    if (context && ((const HandlerCtx*)context)->tabFrame) {
        frameCtx.content = false;
        frameCtx.tabFrame = true;
        if (!TabFrameExtension(message, frameCtx.id)) context = nullptr;
        else context = &frameCtx;
    }
    if (!context) answer = "{\"error\":\"extension not found\"}";
    else HandleMessage(*(const HandlerCtx*)context, message, reply, answer, deferred);
    const ULONGLONG took = GetTickCount64() - started;
    if (took >= 30) {
        WKTypeRef body = WKScriptMessageGetBody(message);
        const std::string text = body && WKGetTypeID(body) == WKStringGetTypeID() ? ToUtf8(WKStr((WKStringRef)body)) : std::string();
        Log("extensions: slow message (%llu ms, %zu bytes in, %zu out): %.120s", took, text.size(), answer.size(), text.c_str());
    }
    if (deferred) return;
    WKStringRef r = WKStringCreateWithUTF8CStringWithLength(answer.data(), answer.size());
    WKCompletionListenerComplete(reply, r);
    WKRelease(r);
}

// ---- the extension's pages: one page configuration per extension ----------------------------
LRESULT CALLBACK HostWndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

Host& EnsureHost(const Extension& e) {
    auto& slot = s_hosts[e.info.id];
    if (slot) return *slot;
    slot = std::make_unique<Host>();
    Host& h = *slot;
    h.id = e.info.id;
    h.ctx = HandlerCtx{ e.info.id, false };
    if (!s_extPrefs) {
        s_extPrefs = WKPreferencesCreate();
        WKPreferencesSetFileAccessFromFileURLsAllowed(s_extPrefs, true);
        WKPreferencesSetUniversalAccessFromFileURLsAllowed(s_extPrefs, true);
        WKPreferencesSetWebSecurityEnabled(s_extPrefs, false);   // fetch() to the sites it works with, as Chrome lets it
    }
    h.ucc = WKUserContentControllerCreate();
    const std::string source = "(" + LoadApi() + ")(" + ExtJson(e, "extension") + ");";
    WKStringRef src = WKStringCreateWithUTF8CStringWithLength(source.data(), source.size());
    WKUserScriptRef script = WKUserScriptCreateWithSource(src, kWKInjectAtDocumentStart, true);
    WKUserContentControllerAddUserScript(h.ucc, script);
    WKRelease(script);
    WKRelease(src);
    WKStringRef name = WKStringCreateWithUTF8CString("lyraExt");
    WKUserContentControllerAddScriptMessageHandler(h.ucc, name, OnMessage, &h.ctx);
    WKRelease(name);
    h.conf = WKPageConfigurationCreate();
    WKPageConfigurationSetContext(h.conf, g_context);
    WKPageConfigurationSetPreferences(h.conf, s_extPrefs);
    if (g_dataStore) WKPageConfigurationSetWebsiteDataStore(h.conf, g_dataStore);
    WKPageConfigurationSetUserContentController(h.conf, h.ucc);
    SetSchemeHandler(h.conf, kFromExtensionPage);
    return h;
}

// Pages of an extension made for Chrome say they are Chrome: some find out which browser they run
// in from the user agent and stop when it is none they know (Bitwarden: " Chrome/" or Safari's own
// extension API). Those made for Firefox keep WebKit's.
void ExtensionUserAgent(const Host& h, WKPageRef page) {
    const Extension* e = Find(h.id);
    if (!e || e->firefoxBuild || !page) return;
    WKStringRef ua = WKStringCreateWithUTF8CString("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                                                   "(KHTML, like Gecko) Chrome/145.0.0.0 Safari/537.36 Lyra");
    WKPageSetCustomUserAgent(page, ua);
    WKRelease(ua);
}

void OnBackgroundLoaded(WKPageRef, WKNavigationRef, WKTypeRef, const void* context) {
    const std::wstring id = context ? *(const std::wstring*)context : L"";
    auto it = s_hosts.find(id);
    if (it == s_hosts.end() || it->second->bgReady) return;
    Host& h = *it->second;
    h.bgReady = true;
    Log("extensions: background page of %ls is running", id.c_str());
    if (!h.installReason.empty())
        Eval(h.bgPage, "globalThis.__lyraEvent && __lyraEvent('runtime.onInstalled',[{reason:" + JsonQuote(ToUtf8(h.installReason)) + "}])");
    else if (h.atStartup)
        Eval(h.bgPage, "globalThis.__lyraEvent && __lyraEvent('runtime.onStartup',[])");
    for (auto& s : std::exchange(h.queued, {})) Eval(h.bgPage, s);
}

// The page the background runs in: its own page, its scripts, or its service worker in a page.
std::wstring BackgroundUrl(const Extension& e) {
    JsonValue m;
    if (!JsonParse(e.manifestText, m)) return L"";
    const JsonValue* bg = m.get("background");
    if (!bg) return L"";
    const std::string page = bg->str("page");
    if (!page.empty()) return ResolveExtensionUrl(e, page);
    std::string html = "<!doctype html><meta charset=\"utf-8\"><title>background</title>\n";
    const std::string worker = bg->str("service_worker");
    if (!worker.empty()) {
        // A service worker runs here as a page: what it expects of its global object.
        html += "<script>self.importScripts = (...urls) => { for (const u of urls) { const x = new XMLHttpRequest(); "
                "x.open('GET', new URL(u, location.href), false); x.send(); (0, eval)(x.responseText + '\\n//# sourceURL=' + u); } };"
                "self.registration = { scope: location.href, showNotification: () => Promise.resolve(), unregister: () => Promise.resolve(true) };"
                "self.clients = { matchAll: () => Promise.resolve([]), claim: () => Promise.resolve(), openWindow: (u) => chrome.tabs.create({ url: u }) };"
                "self.skipWaiting = () => Promise.resolve();</script>\n";
        html += std::string("<script") + (bg->str("type") == "module" ? " type=\"module\"" : "") + " src=\"" + worker + "\"></script>\n";
    } else if (const JsonValue* scripts = bg->get("scripts")) {
        for (auto& s : scripts->items) if (s.isString()) html += "<script src=\"" + s.s + "\"></script>\n";
    } else return L"";
    const std::wstring file = e.info.dir + L"\\_lyra_background.html";
    WriteFileAtomic(file, html);
    return ExtBase(e) + L"_lyra_background.html";
}

void StopBackground(const std::wstring& id) {
    auto it = s_hosts.find(id);
    if (it == s_hosts.end()) return;
    Host& h = *it->second;
    ForgetPorts(id);
    for (HWND w : std::vector<HWND>(h.windows)) DestroyWindow(w);
    if (h.bgView) {
        HWND v = WKViewGetWindow(h.bgView);
        if (v && IsWindow(v)) DestroyWindow(v);
        WKRelease(h.bgView);
    }
    h.bgView = nullptr;
    h.bgPage = nullptr;
    h.bgReady = false;
    h.queued.clear();
    // (the host itself stays: WebKit may still call its handler with the context it was given)
}

std::map<std::wstring, std::unique_ptr<std::wstring>> s_navIds;

void StartBackground(const Extension& e, const std::wstring& installReason, bool atStartup) {
    if (!e.info.enabled) return;
    const std::wstring url = BackgroundUrl(e);
    Host& h = EnsureHost(e);
    if (url.empty() || h.bgView) return;
    if (!s_hostWnd) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = HostWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"LyraExtensionHost";
        RegisterClassExW(&wc);
        s_hostWnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, -32000, -32000, 800, 600, nullptr, nullptr, wc.hInstance, nullptr);
    }
    RECT r = { 0, 0, 800, 600 };
    h.bgView = WKViewCreate(r, h.conf, s_hostWnd);
    if (!h.bgView) return;
    h.bgPage = WKViewGetPage(h.bgView);
    ExtensionUserAgent(h, h.bgPage);
    h.bgReady = false;
    if (!installReason.empty()) ForgetMenus(e);   // (an install or update creates them again)
    h.installReason = installReason;
    h.atStartup = atStartup;
    auto& navId = s_navIds[e.info.id];
    if (!navId) navId = std::make_unique<std::wstring>(e.info.id);
    WKPageNavigationClientV0 nc = {};
    nc.base.version = 0;
    nc.base.clientInfo = navId.get();
    nc.didFinishNavigation = OnBackgroundLoaded;
    WKPageSetPageNavigationClient(h.bgPage, &nc.base);
    WKURLRef u = WKURLCreateWithUTF8CString(ToUtf8(url).c_str());
    WKPageLoadURL(h.bgPage, u);
    WKRelease(u);
    Log("extensions: starting the background of %ls", e.info.id.c_str());
}

// Whether the extension is new or has a new version since Lyra last ran it.
std::wstring InstallReason(const Extension& e) {
    const std::wstring ini = e.info.dir + L"\\lyra.ini";
    wchar_t seen[64] = {};
    GetPrivateProfileStringW(L"Lyra", L"SeenVersion", L"", seen, 64, ini.c_str());
    if (e.info.version == seen) return L"";
    WritePrivateProfileStringW(L"Lyra", L"SeenVersion", e.info.version.c_str(), ini.c_str());
    return *seen ? L"update" : L"install";
}

// ---- options pages and such: a window of their own -------------------------------------------
void ForgetExtensionWindow(HWND h, WKViewRef view) {
    if (view) { WKPageRef page = WKViewGetPage(view); DropPorts([page](const PortEnd& end) { return !end.content && end.page == page; }); }
    for (auto& [id, host] : s_hosts) host->windows.erase(std::remove(host->windows.begin(), host->windows.end(), h), host->windows.end());
    // The page's own window (a child) is destroyed only after this WM_DESTROY of its frame; it
    // still uses its WebView then, so it goes first and the WebView is let go after (as tabs do).
    if (view) {
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        HWND viewWnd = WKViewGetWindow(view);
        if (viewWnd && IsWindow(viewWnd)) DestroyWindow(viewWnd);
        WKRelease(view);
    }
}

LRESULT CALLBACK ExtWindowProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    WKViewRef view = (WKViewRef)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_SIZE:
        if (view) { RECT r; GetClientRect(h, &r); SetWindowPos(WKViewGetWindow(view), nullptr, 0, 0, r.right, r.bottom, SWP_NOZORDER); }
        return 0;
    case WM_SETFOCUS:
        if (view) SetFocus(WKViewGetWindow(view));
        return 0;
    case WM_DESTROY:
        ForgetExtensionWindow(h, view);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

HWND OpenExtensionWindow(Host& h, const std::wstring& url, const std::wstring& title) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = ExtWindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"LyraExtensionWindow";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.hIcon = (HICON)SendMessageW(g_hWnd, WM_GETICON, ICON_BIG, 0);
        RegisterClassExW(&wc);
        registered = true;
    }
    const int w = (int)S(860), ht = (int)S(680);
    HWND wnd = CreateWindowExW(0, L"LyraExtensionWindow", title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, w, ht,
                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!wnd) return nullptr;
    ApplyDwm(wnd);
    RECT r;
    GetClientRect(wnd, &r);
    WKViewRef view = WKViewCreate(r, h.conf, wnd);
    if (!view) { DestroyWindow(wnd); return nullptr; }
    SetWindowLongPtrW(wnd, GWLP_USERDATA, (LONG_PTR)view);
    WKViewSetIsInWindow(view, true);
    h.windows.push_back(wnd);
    ExtensionUserAgent(h, WKViewGetPage(view));
    WKURLRef u = WKURLCreateWithUTF8CString(ToUtf8(url).c_str());
    WKPageLoadURL(WKViewGetPage(view), u);
    WKRelease(u);
    ShowWindow(wnd, SW_SHOW);
    return wnd;
}

// ---- the toolbar button: its icon and its pop-up ------------------------------------------------
std::string Base64Decode(const std::string& in) {
    std::string out;
    int val = 0, bits = -8;
    for (unsigned char ch : in) {
        int d;
        if (ch >= 'A' && ch <= 'Z') d = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') d = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') d = ch - '0' + 52;
        else if (ch == '+' || ch == '-') d = 62;
        else if (ch == '/' || ch == '_') d = 63;
        else continue;
        val = ((val << 6) | d) & 0xFFFFFF;
        bits += 6;
        if (bits >= 0) { out.push_back((char)((val >> bits) & 0xFF)); bits -= 8; }
    }
    return out;
}

// A PNG (or other picture GDI+ reads) from memory, as a bitmap of its own (not tied to the stream).
std::unique_ptr<Bitmap> ImageFromBytes(const std::string& data) {
    if (data.empty()) return nullptr;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, data.size());
    if (!mem) return nullptr;
    memcpy(GlobalLock(mem), data.data(), data.size());
    GlobalUnlock(mem);
    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(mem, TRUE, &stream))) { GlobalFree(mem); return nullptr; }
    std::unique_ptr<Bitmap> out;
    {
        std::unique_ptr<Bitmap> loaded(Bitmap::FromStream(stream));
        if (loaded && loaded->GetLastStatus() == Ok && loaded->GetWidth() > 0 && loaded->GetHeight() > 0) {
            const int w = (int)loaded->GetWidth(), h = (int)loaded->GetHeight();
            out = std::make_unique<Bitmap>(w, h, PixelFormat32bppPARGB);
            Graphics g(out.get());
            g.DrawImage(loaded.get(), 0, 0, w, h);
        }
    }
    stream->Release();
    return out;
}

// The button's icon from what the manifest or setIcon gave: a path, { size: path }, or pixels
// ({ width, height, rgba: base64 }, from api.js).
std::unique_ptr<Bitmap> IconFromValue(const Extension& e, const std::string& raw, int px) {
    JsonValue v;
    if (raw.empty() || !JsonParse(raw, v)) return nullptr;
    if (v.isObject() && v.get("rgba")) {
        const int w = v.get("width") ? (int)v.get("width")->n : 0, h = v.get("height") ? (int)v.get("height")->n : 0;
        const std::string rgba = Base64Decode(v.str("rgba"));
        if (w <= 0 || h <= 0 || w > 512 || h > 512 || rgba.size() < (size_t)w * h * 4) return nullptr;
        auto bmp = std::make_unique<Bitmap>(w, h, PixelFormat32bppARGB);
        Rect r(0, 0, w, h);
        BitmapData d;
        if (bmp->LockBits(&r, ImageLockModeWrite, PixelFormat32bppARGB, &d) != Ok) return nullptr;
        for (int y = 0; y < h; ++y) {
            BYTE* dst = (BYTE*)d.Scan0 + (ptrdiff_t)y * d.Stride;
            const BYTE* src = (const BYTE*)rgba.data() + (size_t)y * w * 4;
            for (int x = 0; x < w; ++x, dst += 4, src += 4) { dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = src[3]; }
        }
        bmp->UnlockBits(&d);
        return bmp;
    }
    std::string rel;
    if (v.isString()) rel = v.s;
    else if (v.isObject()) {
        int bestSize = -1;          // the smallest at least `px`, else the largest
        for (auto& m : v.members) {
            if (!m.second.isString()) continue;
            const int size = atoi(m.first.c_str());
            const bool better = bestSize < 0 || (size >= px && (bestSize < px || size < bestSize)) || (bestSize < px && size > bestSize);
            if (better) { bestSize = size; rel = m.second.s; }
        }
    }
    if (rel.rfind("./", 0) == 0) rel.erase(0, 2);
    while (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
    if (rel.empty() || !SafeRelative(ToWide(rel))) return nullptr;
    return ImageFromBytes(ReadText(e.info.dir + L"\\" + Native(ToWide(rel)), 4u << 20));
}

// Measures the pop-up's page (its natural width, at most 800 x 600 as in Chrome) and tells Lyra.
const char kPopupMeasure[] = R"JS((() => {
  if (window.__lyraPopup) return;
  window.__lyraPopup = true;
  const bridge = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.lyraExt;
  if (!bridge) return;
  let last = "";
  const measure = () => {
    const de = document.documentElement, body = document.body;
    if (!de) return;
    const keep = de.style.width;
    de.style.width = "max-content";
    const r = de.getBoundingClientRect();
    let h = r.height;
    if (document.compatMode === "BackCompat" && body) {
      h = 0;
      for (const ch of body.children) h = Math.max(h, ch.getBoundingClientRect().bottom);
      h += parseFloat(getComputedStyle(body).marginBottom) || 0;
    }
    const w = Math.ceil(r.width);
    de.style.width = keep;
    h = Math.ceil(h);
    const key = w + "x" + h;
    if (key === last) return;
    last = key;
    bridge.postMessage(JSON.stringify({ op: "popup.size", w, h }));
  };
  measure();
  try { const ro = new ResizeObserver(() => measure()); ro.observe(document.documentElement); if (document.body) ro.observe(document.body); } catch (e) {}
  new MutationObserver((records) => {
    if (records.every((x) => x.type === "attributes" && x.target === document.documentElement && x.attributeName === "style")) return;
    measure();
  }).observe(document.documentElement, { childList: true, subtree: true, attributes: true, characterData: true });
  for (const t of [50, 200, 600, 1500]) setTimeout(measure, t);
})())JS";

void OnPopupLoaded(WKPageRef page, WKNavigationRef, WKTypeRef, const void*) { Eval(page, kPopupMeasure); }

LRESULT CALLBACK PopupProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    WKViewRef view = (WKViewRef)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_SIZE:
        if (view) { RECT r; GetClientRect(h, &r); SetWindowPos(WKViewGetWindow(view), nullptr, 0, 0, r.right, r.bottom, SWP_NOZORDER); }
        return 0;
    case WM_SETFOCUS:
        if (view) SetFocus(WKViewGetWindow(view));
        return 0;
    case WM_ACTIVATE:
        // Another window of Lyra or another app came forward: the pop-up goes (not for windows
        // of its own, such as a <select>'s list).
        if (LOWORD(w) == WA_INACTIVE && s_popup.shown && h == s_popup.wnd) {
            for (HWND to = (HWND)l; to; to = GetWindow(to, GW_OWNER)) if (to == h) return 0;
            PostMessageW(h, WM_CLOSE, 0, 0);
        }
        return 0;
    case WM_TIMER:                       // no size came: a usual one
        KillTimer(h, 1);
        if (h == s_popup.wnd && !s_popup.shown) PlacePopup(360, 420);
        return 0;
    case WM_CLOSE:
        if (h == s_popup.wnd) ClosePopup(); else DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        if (h == s_popup.wnd) {
            s_popup.closedAt = GetTickCount();
            s_popup.closedId = s_popup.id;
            s_popup.wnd = nullptr;
            s_popup.shown = false;
            InvalidateChrome();
        }
        ForgetExtensionWindow(h, view);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void OpenPopup(const Extension& e, const std::wstring& url, const RectF& anchor) {
    if (s_popup.wnd) ClosePopup();
    Host& h = EnsureHost(e);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = PopupProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"LyraExtensionPopup";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        RegisterClassExW(&wc);
        registered = true;
    }
    // It lays out at the largest size off-screen until the page says how big it is.
    HWND wnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"LyraExtensionPopup", e.info.name.c_str(), WS_POPUP, -32000, -32000,
                               (int)S(800), (int)S(600), g_hWnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!wnd) return;
    const DWORD round = 2;                               // DWMWCP_ROUND (Windows 11)
    DwmSetWindowAttribute(wnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
    const BOOL dark = g_dark;
    DwmSetWindowAttribute(wnd, kDwmDarkMode, &dark, sizeof(dark));
    RECT r;
    GetClientRect(wnd, &r);
    WKViewRef view = WKViewCreate(r, h.conf, wnd);
    if (!view) { DestroyWindow(wnd); return; }
    SetWindowLongPtrW(wnd, GWLP_USERDATA, (LONG_PTR)view);
    WKViewSetIsInWindow(view, true);
    h.windows.push_back(wnd);
    s_popup.wnd = wnd;
    s_popup.id = e.info.id;
    s_popup.anchor = anchor;
    s_popup.shown = false;
    WKPageRef page = WKViewGetPage(view);
    ExtensionUserAgent(h, page);
    WKPageNavigationClientV0 nc = {};
    nc.base.version = 0;
    nc.didFinishNavigation = OnPopupLoaded;
    WKPageSetPageNavigationClient(page, &nc.base);
    WKURLRef u = WKURLCreateWithUTF8CString(ToUtf8(url).c_str());
    WKPageLoadURL(page, u);
    WKRelease(u);
    ShowWindow(wnd, SW_SHOWNOACTIVATE);
    SetTimer(wnd, 1, 1500, nullptr);
    InvalidateChrome();
    Log("extensions: pop-up of %ls", e.info.id.c_str());
}

void PlacePopup(int cssW, int cssH) {
    if (!s_popup.wnd || !g_hWnd) return;
    const int w = (int)std::ceil(S((float)std::clamp(cssW, 25, 800))), h = (int)std::ceil(S((float)std::clamp(cssH, 25, 600)));
    POINT a = { (LONG)s_popup.anchor.GetRight(), (LONG)(s_popup.anchor.GetBottom() + S(6)) };
    ClientToScreen(g_hWnd, &a);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(g_hWnd, MONITOR_DEFAULTTONEAREST), &mi);
    int x = a.x + (int)S(4) - w;
    x = std::max((int)mi.rcWork.left + 4, std::min(x, (int)mi.rcWork.right - w - 4));
    const int y = std::max((int)mi.rcWork.top, std::min((int)a.y, (int)mi.rcWork.bottom - h - 4));
    SetWindowPos(s_popup.wnd, HWND_TOP, x, y, w, h, SWP_NOACTIVATE);
    if (!s_popup.shown) {
        s_popup.shown = true;
        KillTimer(s_popup.wnd, 1);
        SetForegroundWindow(s_popup.wnd);
        SetFocus(s_popup.wnd);
    }
}

void ClosePopup() {
    if (s_popup.wnd) DestroyWindow(s_popup.wnd);       // (WM_DESTROY forgets it)
}

bool ButtonAnchor(const std::wstring& id, RectF& anchor) {
    const std::vector<ExtensionButton> buttons = ExtensionButtons();
    for (size_t i = 0; i < buttons.size() && i < g_L.ext.size(); ++i)
        if (buttons[i].id == id) { anchor = g_L.ext[i]; return true; }
    return false;
}

// ---- tab events ---------------------------------------------------------------------------------
struct TabSeen { std::wstring url, title; bool loading = false; };
std::map<unsigned long long, TabSeen> s_tabSeen;

}  // namespace

void ExtensionsSetupPageConfiguration(WKPageConfigurationRef conf) { SetSchemeHandler(conf, kFromWebPage); }

void ExtensionsInit() {
    CreateDirectoryW(Root().c_str(), nullptr);
    // Extensions' files are as safe as the page's own (no mixed-content block) and not held back by a site's CSP.
    WKStringRef scheme = WKStringCreateWithUTF8CString(ToUtf8(kExtScheme).c_str());
    WKContextRegisterURLSchemeAsSecure(g_context, scheme);
    WKContextRegisterURLSchemeAsBypassingContentSecurityPolicy(g_context, scheme);
    WKRelease(scheme);
    LoadAll();
    Log("extensions: %zu installed", s_exts.size());
    AdBlockSetExtraUserScripts(AddAllScripts);
    for (const Extension& e : s_exts) StartBackground(e, InstallReason(e), true);
    DnrSyncAll();
}

void ExtensionsDnrReady(LPARAM lParam) {
    std::unique_ptr<DnrJob> job((DnrJob*)lParam);   // (its strings are moved on: 20 MB for a big blocker)
    if (s_dnrGen[job->extId] != job->gen) return;          // rules changed again meanwhile
    DnrResult& r = job->result;
    Log("extensions: network rules of %ls: %d, converted %d, skipped %d -> %zu WebKit rules, %llu ms", job->extId.c_str(),
        r.rules, r.converted, r.skipped, r.webkitRules, r.ms);
    AdBlockSetExtensionRules(job->extId, std::move(r.json), std::move(r.fallback), r.hash);
}

void ExtensionsOnTabCreated(Tab* t) {
    if (!t || !Visible(*t) || s_hosts.empty()) return;
    Broadcast("tabs.onCreated", "[" + TabJson(*t) + "]");
}

void ExtensionsOnTabUpdated(Tab* t) {
    if (!t || !Visible(*t) || s_hosts.empty()) return;
    TabSeen& seen = s_tabSeen[t->id];
    std::string change;
    auto add = [&](const std::string& kv) { change += (change.empty() ? "" : ",") + kv; };
    if (t->loading != seen.loading) add(std::string("\"status\":\"") + (t->loading ? "loading" : "complete") + "\"");
    if (t->url != seen.url) add("\"url\":" + JsonQuote(ToUtf8(t->url)));
    if (t->title != seen.title) add("\"title\":" + JsonQuote(ToUtf8(t->title)));
    seen = { t->url, t->title, t->loading };
    if (change.empty()) return;
    Broadcast("tabs.onUpdated", "[" + std::to_string(t->id) + ",{" + change + "}," + TabJson(*t) + "]");
}

void ExtensionsOnTabActivated(Tab* t) {
    if (!t || !Visible(*t) || s_hosts.empty()) return;
    Broadcast("tabs.onActivated", "[{\"tabId\":" + std::to_string(t->id) + ",\"windowId\":1}]");
}

void ExtensionsOnTabCommitted(Tab* t) {
    if (!t || s_ports.empty()) return;
    const unsigned long long id = t->id;
    DropPorts([id](const PortEnd& end) { return (end.content || end.frame) && end.tabId == id; });
}

void ExtensionsOnTabRemoved(unsigned long long id) {
    s_tabSeen.erase(id);
    const std::string suffix = ":" + std::to_string(id);    // the buttons' settings for that tab
    for (auto& [eid, a] : s_actions)
        for (auto it = a.values.begin(); it != a.values.end();) {
            const std::string& k = it->first;
            if (k.size() > suffix.size() && k.compare(k.size() - suffix.size(), std::string::npos, suffix) == 0) it = a.values.erase(it);
            else ++it;
        }
    if (!s_ports.empty()) DropPorts([id](const PortEnd& end) { return (end.content || end.frame) && end.tabId == id; });
    if (s_hosts.empty()) return;
    Broadcast("tabs.onRemoved", "[" + std::to_string(id) + ",{\"windowId\":1,\"isWindowClosing\":false}]");
}

void ExtensionsShutdown() {
    FlushWrites();
    for (auto& [id, h] : s_hosts) StopBackground(id);
    if (s_hostWnd) { DestroyWindow(s_hostWnd); s_hostWnd = nullptr; }
}

const std::vector<ExtensionInfo>& ExtensionsList() { return s_infos; }

void ExtensionSetEnabled(const std::wstring& id, bool enabled) {
    for (auto& i : s_infos) if (i.id == id) i.enabled = enabled;
    if (Extension* e = Find(id)) e->info.enabled = enabled;
    if (!enabled) StopBackground(id);
    else if (Extension* e = Find(id)) StartBackground(*e, L"", false);
    if (Extension* e = Find(id)) WritePrivateProfileStringW(L"Lyra", L"Enabled", enabled ? L"1" : L"0", (e->info.dir + L"\\lyra.ini").c_str());
    Changed();
}

void ExtensionRemove(const std::wstring& id) {
    Extension* e = Find(id);
    if (!e) return;
    StopBackground(id);
    s_dnr.erase(id);
    s_stores.erase(id);
    s_registered.erase(id);
    s_menus.erase(id);
    s_menusLoaded.erase(id);
    s_registeredLoaded.erase(id);
    std::error_code ec;
    fs::remove_all(e->info.dir, ec);
    Log("extensions: removed %ls", id.c_str());
    LoadAll();
    Changed();
}

bool ExtensionInstall(const std::wstring& path, std::wstring& err, const std::function<bool(const ExtensionInfo&)>& confirm) {
    FlushWrites();                        // (a newer version takes over the storage files)
    const std::wstring tmp = g_settingsDir + L"\\Extensions.new";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    const DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) { err = L"not found"; return false; }
    std::wstring src = tmp;
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        fs::copy(path, tmp, fs::copy_options::recursive, ec);
        if (ec) { err = ToWide(ec.message()); fs::remove_all(tmp, ec); return false; }
    } else {
        CreateDirectoryW(tmp.c_str(), nullptr);
        std::string data = ReadText(path, 256u << 20), zip;
        std::wstring zipPath = path;
        if (StripCrx(data, zip)) {
            zipPath = tmp + L".zip";
            WriteFileAtomic(zipPath, zip);
        }
        const bool ok = Unzip(zipPath, tmp, err);
        if (zipPath != path) DeleteFileW(zipPath.c_str());
        if (!ok) { fs::remove_all(tmp, ec); return false; }
        // (a zip of the folder itself: the manifest one level down)
        if (GetFileAttributesW((tmp + L"\\manifest.json").c_str()) == INVALID_FILE_ATTRIBUTES)
            for (auto& d : fs::directory_iterator(tmp, ec))
                if (d.is_directory() && fs::exists(d.path() / "manifest.json")) { src = d.path().wstring(); break; }
    }
    Extension probe;
    std::string perr;
    if (!LoadExtension(src, probe, &perr)) {
        err = Tr(L"no usable manifest.json", L"нет подходящего manifest.json");
        if (!perr.empty()) err += L" (" + ToWide(perr) + L")";
        fs::remove_all(tmp, ec);
        return false;
    }
    JsonValue m;
    JsonParse(probe.manifestText, m);
    // The id comes from the name in the extension's default language, the same whatever Lyra's.
    JsonValue defaultMessages;
    if (!m.str("default_locale").empty())
        JsonParse(ReadText(src + L"\\_locales\\" + ToWide(m.str("default_locale")) + L"\\messages.json", 4u << 20), defaultMessages);
    const std::wstring id = MakeId(m, Localize(m.str("name", "Extension"), defaultMessages));
    const std::wstring dir = Root() + L"\\" + id;
    if (confirm) {
        probe.info.id = id;
        if (!confirm(probe.info)) { err.clear(); fs::remove_all(tmp, ec); return false; }
    }
    // a newer version replaces the old one but keeps its data and whether it is on
    for (const wchar_t* keep : { L"lyra.ini", L"storage-local.json", L"storage-sync.json", L"dnr-state.json", L"dnr-dynamic.json" })
        if (GetFileAttributesW((dir + L"\\" + keep).c_str()) != INVALID_FILE_ATTRIBUTES)
            CopyFileW((dir + L"\\" + keep).c_str(), (src + L"\\" + keep).c_str(), FALSE);
    StopBackground(id);
    fs::remove_all(dir, ec);
    CreateDirectoryW(Root().c_str(), nullptr);
    if (!MoveFileExW(src.c_str(), dir.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        err = L"cannot write " + dir;
        fs::remove_all(tmp, ec);
        return false;
    }
    fs::remove_all(tmp, ec);
    Log("extensions: installed %ls %ls (%ls)", probe.info.name.c_str(), probe.info.version.c_str(), id.c_str());
    s_dnr.erase(id);                      // read again from the files kept
    s_stores.erase(id);
    s_registered.erase(id);               // (registered scripts do not survive an update)
    s_registeredLoaded.erase(id);
    LoadAll();
    if (Extension* e = Find(id)) StartBackground(*e, InstallReason(*e), false);
    Changed();
    return true;
}

static std::wstring PickPath(bool folder) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return L"";
    if (folder) {
        DWORD o = 0;
        dlg->GetOptions(&o);
        dlg->SetOptions(o | FOS_PICKFOLDERS);
        dlg->SetTitle(Tr(L"Folder of an unpacked extension (with manifest.json)", L"Папка распакованного расширения (с manifest.json)"));
    } else {
        const COMDLG_FILTERSPEC types[] = { { Tr(L"Extensions", L"Расширения"), L"*.crx;*.zip;*.xpi" } };
        dlg->SetFileTypes(1, types);
        dlg->SetTitle(Tr(L"Install an extension", L"Установить расширение"));
    }
    std::wstring path;
    if (SUCCEEDED(dlg->Show(g_setHwnd ? g_setHwnd : g_hWnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) { path = p; CoTaskMemFree(p); }
            item->Release();
        }
    }
    dlg->Release();
    return path;
}

static bool InstallPicked(bool folder) {
    const std::wstring path = PickPath(folder);
    if (path.empty()) return false;
    std::wstring err;
    if (ExtensionInstall(path, err)) return true;
    const std::wstring msg = std::wstring(Tr(L"The extension could not be installed: ", L"Не удалось установить расширение: ")) + err;
    MessageBoxW(g_setHwnd ? g_setHwnd : g_hWnd, msg.c_str(), kAppName, MB_OK | MB_ICONWARNING);
    return false;
}

bool ExtensionInstallFromFile() { return InstallPicked(false); }
bool ExtensionInstallFromFolder() { return InstallPicked(true); }

// ---- the toolbar buttons -------------------------------------------------------------------------
std::vector<ExtensionButton> ExtensionButtons() {
    std::vector<ExtensionButton> out;
    Tab* t = ActiveTab();
    const unsigned long long tab = t ? t->id : 0;
    const int px = std::max(16, (int)std::lround(S(16)));
    for (Extension& e : s_exts) {
        if (!e.info.enabled || !e.hasAction || !e.info.pinned) continue;
        ActionState& a = s_actions[e.info.id];
        ExtensionButton b;
        b.id = e.info.id;
        const std::string* icon = ActionValue(a, "icon", tab);
        const std::string iconRaw = icon ? *icon : e.defaultIcon;
        const std::string key = iconRaw + "@" + std::to_string(px);
        if (a.iconFor != key) {
            a.icon = IconFromValue(e, iconRaw, px);
            if (!a.icon && !e.info.icon.empty()) a.icon = ImageFromBytes(ReadText(e.info.icon, 4u << 20));
            a.iconFor = key;
        }
        b.icon = a.icon.get();
        const std::string* title = ActionValue(a, "title", tab);
        b.title = title ? ToWide(JsonString(title)) : e.defaultTitle;
        if (b.title.empty()) b.title = e.info.name;
        b.badge = ToWide(JsonString(ActionValue(a, "badgeText", tab)));
        if (b.badge.size() > 4) b.badge.resize(4);
        if (const std::string* c = ActionValue(a, "badgeColor", tab)) ParseColor(*c, b.badgeColor);
        if (const std::string* c = ActionValue(a, "badgeTextColor", tab)) ParseColor(*c, b.badgeTextColor);
        if (const std::string* en = ActionValue(a, "enabled", tab)) b.enabled = *en != "false";
        b.open = s_popup.wnd && s_popup.id == e.info.id;
        out.push_back(std::move(b));
    }
    return out;
}

void ExtensionButtonClick(int index) {
    const std::vector<ExtensionButton> buttons = ExtensionButtons();
    if (index < 0 || index >= (int)buttons.size() || index >= (int)g_L.ext.size()) return;
    Extension* e = Find(buttons[index].id);
    if (!e || !buttons[index].enabled) return;
    if (s_popup.wnd) {
        const bool same = s_popup.id == e->info.id;
        ClosePopup();
        if (same) return;
    } else if (s_popup.closedId == e->info.id && GetTickCount() - s_popup.closedAt < 300) {
        return;                          // this click already closed it (the main window came forward)
    }
    Tab* t = ActiveTab();
    const std::string* v = ActionValue(s_actions[e->info.id], "popup", t ? t->id : 0);
    const std::string popup = v ? JsonString(v) : e->defaultPopup;
    if (!popup.empty()) { OpenPopup(*e, ResolveExtensionUrl(*e, popup), g_L.ext[index]); return; }
    auto it = s_hosts.find(e->info.id);
    if (it == s_hosts.end() || !it->second->bgPage) return;
    const std::string tabJson = t && Visible(*t) ? TabJson(*t) : "null";
    ToBackground(*it->second, "globalThis.__lyraEvent && __lyraEvent('action.onClicked',[" + tabJson + "])");
}

void ExtensionButtonMenu(int index) {
    const std::vector<ExtensionButton> buttons = ExtensionButtons();
    if (index < 0 || index >= (int)buttons.size() || index >= (int)g_L.ext.size()) return;
    Extension* e = Find(buttons[index].id);
    if (!e) return;
    const std::wstring id = e->info.id;
    enum { kName = 1, kOptions, kHide, kManage, kRemove };
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_GRAYED, kName, e->info.name.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    if (!e->optionsPage.empty()) AppendMenuW(m, MF_STRING, kOptions, Tr(L"Options", L"Параметры"));
    AppendMenuW(m, MF_STRING, kHide, Tr(L"Hide from the toolbar", L"Убрать с панели"));
    AppendMenuW(m, MF_STRING, kManage, Tr(L"Manage extension", L"Управление расширением"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kRemove, Tr(L"Remove from Lyra\x2026", L"Удалить из Lyra\x2026"));
    MenuSetIcon(m, kOptions, L"\uE713");
    MenuSetIcon(m, kHide, L"\uE77A");
    MenuSetIcon(m, kManage, L"\uE943");
    MenuSetIcon(m, kRemove, L"\uE74D");
    // The extension's own items for its button (contexts "action", "browser_action", "page_action",
    // "all"), first, as Chrome shows them (at most 6 at the top level).
    enum { kItem = 100 };
    ExtensionMenuContext ac;
    ac.action = true;
    Tab* active = ActiveTab();
    if (active) ac.pageUrl = active->url;
    std::vector<ExtensionMenuEntry> own;
    MenuChildren(Menus(*e), "", ac, id, own, 0);
    while (!own.empty() && own.back().separator) own.pop_back();
    if (own.size() > 6) own.resize(6);
    std::vector<std::string> itemIds;
    std::function<void(HMENU, const ExtensionMenuEntry&)> add = [&](HMENU parent, const ExtensionMenuEntry& x) {
        if (x.separator) { AppendMenuW(parent, MF_SEPARATOR, 0, nullptr); return; }
        if (!x.children.empty()) {
            HMENU sub = CreatePopupMenu();
            for (auto& c : x.children) add(sub, c);
            AppendMenuW(parent, MF_POPUP | (x.enabled ? 0 : MF_GRAYED), (UINT_PTR)sub, x.title.c_str());
            return;
        }
        itemIds.push_back(x.itemId);
        AppendMenuW(parent, MF_STRING | (x.enabled ? 0 : MF_GRAYED) | (x.checkable && x.checked ? MF_CHECKED : 0),
                    kItem + (UINT)itemIds.size() - 1, x.title.c_str());
    };
    if (!own.empty()) {
        HMENU withItems = CreatePopupMenu();
        AppendMenuW(withItems, MF_STRING | MF_GRAYED, kName, e->info.name.c_str());
        AppendMenuW(withItems, MF_SEPARATOR, 0, nullptr);
        for (auto& x : own) add(withItems, x);
        AppendMenuW(withItems, MF_SEPARATOR, 0, nullptr);
        // then Lyra's items (from the menu built above, after its name and separator)
        const int n = GetMenuItemCount(m);
        for (int k = 2; k < n; ++k) {
            wchar_t text[256] = {};
            MENUITEMINFOW mi = { sizeof(mi) };
            mi.fMask = MIIM_FTYPE | MIIM_ID | MIIM_STRING | MIIM_STATE;
            mi.dwTypeData = text;
            mi.cch = 255;
            if (!GetMenuItemInfoW(m, k, TRUE, &mi)) continue;
            if (mi.fType & MFT_SEPARATOR) AppendMenuW(withItems, MF_SEPARATOR, 0, nullptr);
            else AppendMenuW(withItems, MF_STRING, mi.wID, text);
        }
        DestroyMenu(m);
        m = withItems;
        MenuSetIcon(m, kOptions, L"\uE713");
        MenuSetIcon(m, kHide, L"\uE77A");
        MenuSetIcon(m, kManage, L"\uE943");
        MenuSetIcon(m, kRemove, L"\uE74D");
    }
    const RectF& r = g_L.ext[index];
    POINT pt = { (LONG)r.GetRight(), (LONG)(r.GetBottom() + S(4)) };
    ClientToScreen(g_hWnd, &pt);
    const int cmd = MenuTrack(m, TPM_RIGHTALIGN | TPM_TOPALIGN, pt.x, pt.y, g_hWnd);
    DestroyMenu(m);
    e = Find(id);
    if (!e) return;
    if (cmd >= kItem && cmd - kItem < (int)itemIds.size()) {
        Tab* t = ActiveTab();
        ExtensionMenuClicked(id, itemIds[cmd - kItem], ac, t ? t->id : 0);
        return;
    }
    switch (cmd) {
    case kOptions: OpenExtensionWindow(EnsureHost(*e), ResolveExtensionUrl(*e, e->optionsPage), e->info.name); break;
    case kHide: ExtensionSetPinned(id, false); break;
    case kManage:
        for (size_t k = 0; k < s_infos.size(); ++k)
            if (s_infos[k].id == id) SettingsOpenPage(SettingsExtensionPage(k));
        break;
    case kRemove:
        if (MessageBoxW(g_hWnd, (std::wstring(Tr(L"Remove the extension \x201C", L"Удалить расширение \x00AB")) + e->info.name +
                                 Tr(L"\x201D? Its settings are deleted too.", L"\x00BB? Его настройки тоже удалятся.")).c_str(),
                        kAppName, MB_YESNO | MB_ICONQUESTION) == IDYES)
            ExtensionRemove(id);
        break;
    }
}

void ExtensionSetPinned(const std::wstring& id, bool pinned) {
    for (auto& i : s_infos) if (i.id == id) i.pinned = pinned;
    Extension* e = Find(id);
    if (!e) return;
    e->info.pinned = pinned;
    WritePrivateProfileStringW(L"Lyra", L"Toolbar", pinned ? L"1" : L"0", (e->info.dir + L"\\lyra.ini").c_str());
    SettingsRefresh();
    if (g_hWnd) RefreshLayout();
}

// ---- context menu items --------------------------------------------------------------------------
bool ExtensionsHaveMenus() {
    for (auto& e : s_exts) if (e.info.enabled && !Menus(e).empty()) return true;
    return false;
}

std::vector<ExtensionMenuGroup> ExtensionMenus(const ExtensionMenuContext& c) {
    std::vector<ExtensionMenuGroup> groups;
    for (auto& e : s_exts) {
        if (!e.info.enabled) continue;
        auto& list = Menus(e);
        if (list.empty()) continue;
        ExtensionMenuGroup g;
        g.extId = e.info.id;
        g.name = e.info.name;
        MenuChildren(list, "", c, e.info.id, g.items, 0);
        while (!g.items.empty() && g.items.back().separator) g.items.pop_back();
        while (!g.items.empty() && g.items.front().separator) g.items.erase(g.items.begin());
        if (!g.items.empty()) groups.push_back(std::move(g));
    }
    return groups;
}

void ExtensionMenuClicked(const std::wstring& extId, const std::string& itemId, const ExtensionMenuContext& c, unsigned long long tabId) {
    Extension* e = Find(extId);
    if (!e || !e->info.enabled) return;
    auto& list = Menus(*e);
    auto it = std::find_if(list.begin(), list.end(), [&](const MenuItem& m) { return m.id == itemId; });
    if (it == list.end()) return;
    MenuItem& m = *it;
    const bool was = m.checked;
    if (m.type == "checkbox") m.checked = !m.checked;
    else if (m.type == "radio") {
        // the radio items next to it (same parent, no other kind in between) are its group
        const size_t at = it - list.begin();
        for (size_t i = at; i-- > 0;) { if (list[i].parentId != m.parentId) continue; if (list[i].type != "radio") break; list[i].checked = false; }
        for (size_t i = at + 1; i < list.size(); ++i) { if (list[i].parentId != m.parentId) continue; if (list[i].type != "radio") break; list[i].checked = false; }
        m.checked = true;
    }
    if (m.type == "checkbox" || m.type == "radio") SaveMenus(*e);
    auto idJson = [&](const MenuItem* x, const std::string& id) { return x && x->numeric ? id : JsonQuote(id); };
    const MenuItem* parent = nullptr;
    for (auto& x : list) if (x.id == m.parentId) parent = &x;
    std::string info = "{\"menuItemId\":" + idJson(&m, m.id) + ",\"editable\":" + (c.editable ? "true" : "false") +
                       ",\"pageUrl\":" + JsonQuote(ToUtf8(c.pageUrl)) + ",\"frameId\":0";
    if (!m.parentId.empty()) info += ",\"parentMenuItemId\":" + idJson(parent, m.parentId);
    if (!c.link.empty()) info += ",\"linkUrl\":" + JsonQuote(ToUtf8(c.link));
    if (!c.image.empty()) info += ",\"srcUrl\":" + JsonQuote(ToUtf8(c.image)) + ",\"mediaType\":\"image\"";
    else if (!c.media.empty()) info += ",\"srcUrl\":" + JsonQuote(ToUtf8(c.media)) + ",\"mediaType\":\"video\"";
    if (!c.selectionText.empty()) info += ",\"selectionText\":" + JsonQuote(ToUtf8(c.selectionText));
    if (m.type == "checkbox" || m.type == "radio")
        info += std::string(",\"wasChecked\":") + (was ? "true" : "false") + ",\"checked\":" + (m.checked ? "true" : "false");
    info += "}";
    Tab* t = FindTab(tabId);
    const std::string tab = t ? TabJson(*t) : "null";
    Log("extensions: menu item %s of %ls chosen", itemId.c_str(), extId.c_str());
    auto h = s_hosts.find(extId);
    if (h == s_hosts.end()) return;
    ExtensionPagesEvent(*h->second, "globalThis.__lyraEvent && __lyraEvent('contextMenus.onClicked',[" + info + "," + tab + "])");
}
