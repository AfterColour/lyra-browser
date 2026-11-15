// Lyra — расширения браузера (см. Extensions.h).
#include "browser/Extensions.h"
#include "app/Log.h"
#include "browser/AdBlock.h"
#include "core/Constants.h"
#include "core/Json.h"
#include "core/Localization.h"
#include "core/Settings.h"
#include "core/StringUtils.h"
#include "ui/MainWindow.h"
#include "ui/settings/SettingsWindow.h"
#include <WebKit/WKPageFullScreenClient.h>      // WKCompletionListenerComplete
#include <WebKit/WKUserContentControllerRef.h>
#include <WebKit/WKUserScriptRef.h>
#include <WebKit/WKScriptMessageRef.h>
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
std::map<std::wstring, std::unique_ptr<std::wstring>> s_handlerIds;   // context of each world's handler
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

// ---- the bridge: window.webkit.messageHandlers.lyraExt in an extension's world ---------------
void OnMessage(WKScriptMessageRef message, WKCompletionListenerRef reply, const void* context) {
    std::string answer = "{\"error\":\"extension not found\"}";
    const std::wstring id = context ? *(const std::wstring*)context : L"";
    Extension* e = Find(id);
    WKTypeRef body = WKScriptMessageGetBody(message);
    if (e && e->info.enabled && body && WKGetTypeID(body) == WKStringGetTypeID()) {
        JsonValue msg;
        if (JsonParse(ToUtf8(WKStr((WKStringRef)body)), msg) && msg.isObject()) {
            const std::string op = msg.str("op");
            if (op.rfind("storage.", 0) == 0) answer = Storage(*e, msg, op);
            else answer = "{\"error\":\"" + op + " is not supported yet\"}";
        } else answer = "{\"error\":\"bad message\"}";
    }
    WKStringRef r = WKStringCreateWithUTF8CString(answer.c_str());
    WKCompletionListenerComplete(reply, r);
    WKRelease(r);
}

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
    if (s_apiJs.empty()) {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring path = exe;
        path = path.substr(0, path.find_last_of(L'\\')) + L"\\resources\\extensions\\api.js";
        s_apiJs = ReadText(path);
        const size_t f = s_apiJs.find("function (EXT)");
        if (f != std::string::npos) s_apiJs.erase(0, f);              // (the comment block above it)
    }
    if (s_apiJs.empty()) return;
    for (Extension& e : s_exts) {
        if (!e.info.enabled || e.scripts.empty()) continue;
        const std::wstring world = World(e);
        if (!s_worldsWithHandler.count(world)) {
            auto& ctx = s_handlerIds[world];
            ctx = std::make_unique<std::wstring>(e.info.id);
            WKStringRef name = WKStringCreateWithUTF8CString("lyraExt");
            WKStringRef w = WKStringCreateWithUTF8CString(ToUtf8(world).c_str());
            WKUserContentControllerAddScriptMessageHandlerInWorld(controller, name, OnMessage, ctx.get(), w);
            WKRelease(w);
            WKRelease(name);
            s_worldsWithHandler.insert(world);
        }
        const std::string ext = "{\"id\":" + JsonQuote(ToUtf8(e.info.id)) + ",\"base\":" + JsonQuote(ToUtf8(FileUrl(e.info.dir + L"\\"))) +
                                ",\"manifest\":" + e.manifestText + ",\"messages\":" + e.messagesText +
                                ",\"lang\":" + JsonQuote(UiRussian() ? "ru" : "en") + "}";
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

}  // namespace

void ExtensionsInit() {
    CreateDirectoryW(Root().c_str(), nullptr);
    LoadAll();
    Log("extensions: %zu installed", s_exts.size());
    AdBlockSetExtraUserScripts(AddAllScripts);
}

const std::vector<ExtensionInfo>& ExtensionsList() { return s_infos; }

void ExtensionSetEnabled(const std::wstring& id, bool enabled) {
    for (auto& i : s_infos) if (i.id == id) i.enabled = enabled;
    if (Extension* e = Find(id)) e->info.enabled = enabled;
    if (Extension* e = Find(id)) WritePrivateProfileStringW(L"Lyra", L"Enabled", enabled ? L"1" : L"0", (e->info.dir + L"\\lyra.ini").c_str());
    Changed();
}

void ExtensionRemove(const std::wstring& id) {
    Extension* e = Find(id);
    if (!e) return;
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
