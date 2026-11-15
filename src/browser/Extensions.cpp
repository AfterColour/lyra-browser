// Lyra — расширения браузера (см. Extensions.h).
#include "browser/Extensions.h"
#include "app/Log.h"
#include "browser/AdBlock.h"
#include "core/Constants.h"
#include "core/Json.h"
#include "core/Localization.h"
#include "core/Settings.h"
#include "core/StringUtils.h"
#include "browser/TabManager.h"
#include "browser/WebContext.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/settings/SettingsWindow.h"
#include <WebKit/WKPageFullScreenClient.h>      // WKCompletionListenerComplete
#include <WebKit/WKUserContentControllerRef.h>
#include <WebKit/WKUserScriptRef.h>
#include <WebKit/WKScriptMessageRef.h>
#include <WebKit/WKPageNavigationClient.h>
#include <WebKit/WKFrameInfoRef.h>
#include <shobjidl.h>
#include <filesystem>
#include <set>

namespace fs = std::filesystem;
extern HWND g_setHwnd;

namespace {

struct ContentScript {
    std::vector<std::string> matches, excludes;     // WebKit user content URL patterns
    std::vector<std::wstring> js, css;              // files, relative to the extension
    bool atStart = false, allFrames = false;
};

struct Extension {
    ExtensionInfo info;
    std::string manifestText, messagesText = "{}", lang = "en";
    std::vector<ContentScript> scripts;
};

std::vector<Extension> s_exts;
std::vector<ExtensionInfo> s_infos;
std::set<std::wstring> s_worldsWithHandler;
struct HandlerCtx { std::wstring id; bool content = true; };      // whose script sent a message, and from where
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
        if (v.s == "<all_urls>") { out.push_back("http://*/*"); out.push_back("https://*/*"); out.push_back("file:///*"); }
        else out.push_back(v.s);
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

// ---- storage ----------------------------------------------------------------------------------
using Store = std::map<std::string, std::string>;      // key -> value as JSON text

std::wstring StoreFile(const Extension& e, const std::string& area) { return e.info.dir + L"\\storage-" + ToWide(area) + L".json"; }

Store LoadStore(const Extension& e, const std::string& area) {
    Store s;
    if (area == "session") return s_session[e.info.id];
    JsonValue v;
    if (JsonParse(ReadText(StoreFile(e, area)), v) && v.isObject())
        for (auto& m : v.members) s[m.first] = m.second.raw;
    return s;
}

void SaveStore(const Extension& e, const std::string& area, const Store& s) {
    if (area == "session") { s_session[e.info.id] = s; return; }
    std::string out = "{";
    for (auto& [k, v] : s) { if (out.size() > 1) out += ","; out += JsonQuote(k) + ":" + v; }
    WriteFileAtomic(StoreFile(e, area), out + "}");
}

std::string Storage(Extension& e, const JsonValue& msg, const std::string& op) {
    const std::string area = msg.str("area", "local");
    if (area != "local" && area != "sync" && area != "session" && area != "managed") return "{\"error\":\"unknown storage area\"}";
    Store store = LoadStore(e, area);
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
    SaveStore(e, area, store);
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
    WKStringRef w = WKStringCreateWithUTF8CString(ToUtf8(world).c_str());
    WKArrayRef allow = StringArray(s.matches), block = StringArray(s.excludes);
    WKUserScriptRef script = WKUserScriptCreateWithSourceInWorld(src, s.atStart ? kWKInjectAtDocumentStart : kWKInjectAtDocumentEnd, !s.allFrames, w, allow, block);
    WKUserContentControllerAddUserScript(controller, script);
    WKRelease(script);
    WKRelease(allow);
    WKRelease(block);
    WKRelease(w);
    WKRelease(src);
}

const char kCssInjector[] =
    "(function(css){var add=function(){var s=document.createElement('style');s.setAttribute('data-lyra-extension','');"
    "s.textContent=css;(document.head||document.documentElement).appendChild(s);};"
    "if(document.documentElement)add();else new MutationObserver(function(r,o){if(document.documentElement){o.disconnect();add();}})"
    ".observe(document,{childList:true});})";

// Called by AdBlock.cpp whenever it rebuilds the shared list of user scripts.
void AddAllScripts(WKUserContentControllerRef controller) {
    LoadApi();
    if (s_apiJs.empty()) return;
    for (Extension& e : s_exts) {
        if (!e.info.enabled || e.scripts.empty()) continue;
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
        for (const ContentScript& s : e.scripts) {
            // 1: the API (once per page and world) and the styles; then each script on its own,
            // in order, in the same world (as Chrome runs them).
            std::string head = "if (!globalThis.__lyraExtension) { globalThis.__lyraExtension = true; (" + s_apiJs + ")(" + ext + "); }\n";
            for (const std::wstring& css : s.css) head += std::string(kCssInjector) + "(" + JsonQuote(ReadText(e.info.dir + L"\\" + Native(css))) + ");\n";
            AddScript(controller, head, s, world);
            for (const std::wstring& js : s.js) {
                std::string code = ReadText(e.info.dir + L"\\" + Native(js));
                if (code.size() >= 3 && (unsigned char)code[0] == 0xEF) code.erase(0, 3);
                AddScript(controller, code + "\n//# sourceURL=" + ToUtf8(FileUrl(e.info.dir + L"\\" + Native(js))), s, world);
            }
        }
    }
}

void Changed() {
    AdBlockRefreshUserScripts();          // pages loaded from now on get the new set
    SettingsRefresh();
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
    return "{\"id\":" + JsonQuote(ToUtf8(e.info.id)) + ",\"base\":" + JsonQuote(ToUtf8(FileUrl(e.info.dir + L"\\"))) +
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
    std::map<std::string, std::string> action;   // badge text, colour, title, pop-up set by the extension
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

void EvalInWorld(WKPageRef page, const std::wstring& world, const std::string& script, void* context, WKPageEvaluateJavaScriptFunction callback) {
    if (!page) { if (callback) callback(nullptr, nullptr, context); return; }
    WKStringRef s = WKStringCreateWithUTF8CStringWithLength(script.data(), script.size());
    WKStringRef w = WKStringCreateWithUTF8CString(ToUtf8(world).c_str());
    WKPageEvaluateJavaScriptInFrameInWorld(page, nullptr, s, w, context, callback);
    WKRelease(w);
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
    return FileUrl(e.info.dir + L"\\") + rel;
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

HWND OpenExtensionWindow(Host& h, const std::wstring& url, const std::wstring& title);

void HandleMessage(const HandlerCtx& ctx, WKScriptMessageRef message, WKCompletionListenerRef reply, std::string& answer, bool& deferred) {
    Extension* e = Find(ctx.id);
    WKTypeRef body = WKScriptMessageGetBody(message);
    if (!e || !e->info.enabled) { answer = "{\"error\":\"the extension is off\"}"; return; }
    JsonValue msg;
    if (!body || WKGetTypeID(body) != WKStringGetTypeID() || !JsonParse(ToUtf8(WKStr((WKStringRef)body)), msg) || !msg.isObject()) {
        answer = "{\"error\":\"bad message\"}";
        return;
    }
    const std::string op = msg.str("op");
    WKPageRef fromPage = nullptr;
    bool mainFrame = true;
    if (WKFrameInfoRef f = WKScriptMessageGetFrameInfo(message)) { fromPage = WKFrameInfoGetPage(f); mainFrame = WKFrameInfoGetIsMainFrame(f); }
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
        std::string sender = "{\"id\":" + JsonQuote(ToUtf8(e->info.id));
        if (ctx.content) {
            Tab* tab = nullptr;
            for (auto& t : g_tabs) if (t->page == fromPage) tab = t.get();
            if (tab && Visible(*tab)) sender += ",\"tab\":" + TabJson(*tab) + ",\"url\":" + JsonQuote(ToUtf8(tab->url)) + ",\"frameId\":" + (mainFrame ? "0" : "1");
        } else sender += ",\"url\":" + JsonQuote(ToUtf8(FileUrl(e->info.dir + L"\\")));
        sender += "}";
        std::vector<WKPageRef> targets;
        if (host) {
            if (host->bgPage && host->bgPage != fromPage) targets.push_back(host->bgPage);
            for (HWND w : host->windows)
                if (WKViewRef v = (WKViewRef)GetWindowLongPtrW(w, GWLP_USERDATA))
                    if (WKViewGetPage(v) != fromPage) targets.push_back(WKViewGetPage(v));
        }
        if (targets.empty()) { answer = kNoReceiver; return; }
        const int callId = NewCall(reply, (int)targets.size());
        deferred = true;
        for (WKPageRef p : targets) {
            const std::string script = DeliverScript(callId, messageJson, sender);
            if (p == host->bgPage) ToBackground(*host, script); else Eval(p, script);
        }
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
        const std::string sender = "{\"id\":" + JsonQuote(ToUtf8(e->info.id)) + ",\"url\":" + JsonQuote(ToUtf8(FileUrl(e->info.dir + L"\\"))) + "}";
        const int callId = NewCall(reply, 1);
        deferred = true;
        EvalInWorld(t->page, World(*e), DeliverScript(callId, messageJson, sender), (void*)(intptr_t)callId, OnTabDelivered);
        return;
    }
    if (op == "scripting.executeScript" || op == "scripting.insertCSS") {
        Tab* t = TabById(msg.get("tabId"));
        if (!t || !t->page) { answer = "{\"error\":\"No tab with this id.\"}"; return; }
        const bool css = op == "scripting.insertCSS";
        std::string script;
        if (css) {
            const std::string text = msg.str("css") + Files(*e, msg.get("files"));
            script = std::string(kCssInjector) + "(" + JsonQuote(text) + ")";
        } else {
            const std::string code = msg.get("code") && msg.get("code")->isString() ? msg.str("code") : Files(*e, msg.get("files"));
            // The extension's API first if no content script of it ran in this page yet.
            const std::string api = "if (!globalThis.__lyraExtension) { globalThis.__lyraExtension = true; (" + LoadApi() + ")(" + ExtJson(*e, "content") + "); }\n";
            script = api + "(() => { try { const r = (0, eval)(" + JsonQuote(code) + "); return JSON.stringify({ r: r === undefined ? null : r }); } "
                     "catch (e) { return JSON.stringify({ e: String(e && e.message || e) }); } })()";
        }
        const int callId = NewCall(reply, 1);
        deferred = true;
        EvalInWorld(t->page, World(*e), script, new ScriptCall{ callId, css }, OnScriptDone);
        return;
    }
    if (op == "action.set") {
        if (host) {
            const JsonValue* d = msg.get("details");
            const std::string what = msg.str("what");
            if (d) for (const char* key : { "text", "color", "title", "popup" }) if (const JsonValue* v = d->get(key)) host->action[what] = v->raw;
        }
        answer = "{\"value\":null}";
        return;
    }
    if (op == "runtime.openOptionsPage") {
        JsonValue m;
        JsonParse(e->manifestText, m);
        std::string page = m.str("options_page");
        if (const JsonValue* ui = m.get("options_ui")) if (page.empty()) page = ui->str("page");
        if (page.empty() || !host) { answer = "{\"error\":\"This extension has no options page.\"}"; return; }
        OpenExtensionWindow(*host, ResolveExtensionUrl(*e, page), e->info.name);
        answer = "{\"value\":null}";
        return;
    }
    if (op == "runtime.reload") { answer = "{\"value\":null}"; PostMessageW(g_hWnd, WM_NULL, 0, 0); return; }
    answer = "{\"error\":\"" + op + " is not supported yet\"}";
}

void OnMessage(WKScriptMessageRef message, WKCompletionListenerRef reply, const void* context) {
    std::string answer;
    bool deferred = false;
    if (!context) answer = "{\"error\":\"extension not found\"}";
    else HandleMessage(*(const HandlerCtx*)context, message, reply, answer, deferred);
    if (deferred) return;
    WKStringRef r = WKStringCreateWithUTF8CString(answer.c_str());
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
        WKPreferencesSetUniversalAccessFromFileURLsAllowed(s_extPrefs, true);   // fetch() to the sites it works with
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
    return h;
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
    return FileUrl(file);
}

void StopBackground(const std::wstring& id) {
    auto it = s_hosts.find(id);
    if (it == s_hosts.end()) return;
    Host& h = *it->second;
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
    h.bgReady = false;
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
        for (auto& [id, host] : s_hosts) host->windows.erase(std::remove(host->windows.begin(), host->windows.end(), h), host->windows.end());
        if (view) { SetWindowLongPtrW(h, GWLP_USERDATA, 0); WKRelease(view); }
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
    WKURLRef u = WKURLCreateWithUTF8CString(ToUtf8(url).c_str());
    WKPageLoadURL(WKViewGetPage(view), u);
    WKRelease(u);
    ShowWindow(wnd, SW_SHOW);
    return wnd;
}

// ---- tab events ---------------------------------------------------------------------------------
struct TabSeen { std::wstring url, title; bool loading = false; };
std::map<unsigned long long, TabSeen> s_tabSeen;

}  // namespace

void ExtensionsInit() {
    CreateDirectoryW(Root().c_str(), nullptr);
    LoadAll();
    Log("extensions: %zu installed", s_exts.size());
    AdBlockSetExtraUserScripts(AddAllScripts);
    for (const Extension& e : s_exts) StartBackground(e, InstallReason(e), true);
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

void ExtensionsOnTabRemoved(unsigned long long id) {
    s_tabSeen.erase(id);
    if (s_hosts.empty()) return;
    Broadcast("tabs.onRemoved", "[" + std::to_string(id) + ",{\"windowId\":1,\"isWindowClosing\":false}]");
}

void ExtensionsShutdown() {
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
    std::error_code ec;
    fs::remove_all(e->info.dir, ec);
    Log("extensions: removed %ls", id.c_str());
    LoadAll();
    Changed();
}

bool ExtensionInstall(const std::wstring& path, std::wstring& err) {
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
    // a newer version replaces the old one but keeps its data and whether it is on
    for (const wchar_t* keep : { L"lyra.ini", L"storage-local.json", L"storage-sync.json" })
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
