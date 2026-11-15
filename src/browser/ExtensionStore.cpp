// Lyra - installing extensions from Chrome Web Store / Firefox Add-ons and from .crx / .xpi.
#include "browser/ExtensionStore.h"
#include "browser/Extensions.h"
#include "app/Http.h"
#include "app/Log.h"
#include "core/Constants.h"
#include "core/Json.h"
#include "core/Localization.h"
#include "core/StringUtils.h"
#include "ui/Dialogs.h"
#include "ui/MainWindow.h"
#include "ui/settings/SettingsWindow.h"
#include <WebKit/WKPageFullScreenClient.h>      // WKCompletionListenerComplete
#include <WebKit/WKUserContentControllerRef.h>
#include <WebKit/WKUserScriptRef.h>
#include <WebKit/WKScriptMessageRef.h>
#include <WebKit/WKString.h>
#include <shlwapi.h>
#include <algorithm>
#include <atomic>
#include <memory>
#include <set>
#include <thread>

namespace {

std::atomic<bool> s_busy{ false };

struct Fetched {
    bool ok = false;
    std::wstring path, err, from;   // from: where it came from, for the question
};

// The "Add to Lyra" button. Built with DOM calls (no innerHTML: the stores use Trusted Types).
// The stores are single-page apps: the address changes without a load, so it looks every 800 ms.
const char kStoreScript[] = R"JS((() => {
  if (window.top !== window || window.__lyraStore) return;
  window.__lyraStore = true;
  const LABEL = "%INSTALL%", WAIT = "%WAIT%";
  let host = null, button = null, label = null;
  const target = () => {
    const h = location.hostname, p = location.pathname;
    let m;
    if (h === "chromewebstore.google.com" && (m = p.match(/^\/detail\/(?:[^/]+\/)?([a-p]{32})(?:[/?]|$)/))) return { store: "cws", id: m[1] };
    if (h === "chrome.google.com" && (m = p.match(/^\/webstore\/detail\/(?:[^/]+\/)?([a-p]{32})(?:[/?]|$)/))) return { store: "cws", id: m[1] };
    if (h === "addons.mozilla.org" && (m = p.match(/^\/(?:[^/]+\/)?(?:firefox|android)\/addon\/([^/]+)/))) return { store: "amo", id: decodeURIComponent(m[1]) };
    return null;
  };
  const make = () => {
    host = document.createElement("lyra-store-button");
    host.style.cssText = "all:initial;position:fixed;right:24px;bottom:24px;z-index:2147483647";
    const root = host.attachShadow({ mode: "closed" });
    const style = document.createElement("style");
    style.textContent = 'button{font:600 14px/1 "Segoe UI Variable Text","Segoe UI",system-ui,sans-serif;color:#fff;' +
      "background:linear-gradient(135deg,#8b5cf6,#6d28d9);border:0;border-radius:999px;padding:13px 22px 13px 18px;cursor:pointer;" +
      "box-shadow:0 6px 24px rgba(76,29,149,.45),0 1px 3px rgba(0,0,0,.3);display:flex;align-items:center;gap:10px;" +
      "transition:transform .15s,box-shadow .15s,opacity .15s}" +
      "button:hover{transform:translateY(-1px);box-shadow:0 10px 30px rgba(76,29,149,.55),0 1px 3px rgba(0,0,0,.3)}" +
      "button:active{transform:none}button[disabled]{opacity:.75;cursor:default}svg{width:18px;height:18px;flex:none}";
    button = document.createElement("button");
    const NS = "http://www.w3.org/2000/svg";
    const svg = document.createElementNS(NS, "svg");
    svg.setAttribute("viewBox", "0 0 24 24");
    for (const [k, v] of [["fill", "none"], ["stroke", "currentColor"], ["stroke-width", "2"], ["stroke-linecap", "round"], ["stroke-linejoin", "round"]]) svg.setAttribute(k, v);
    for (const d of ["M12 4v11", "m7 10 5 5 5-5", "M5 20h14"]) { const p = document.createElementNS(NS, "path"); p.setAttribute("d", d); svg.appendChild(p); }
    label = document.createElement("span");
    label.textContent = LABEL;
    button.append(svg, label);
    button.addEventListener("click", () => {
      const t = target();
      if (!t || button.disabled) return;
      button.disabled = true;
      label.textContent = WAIT;
      window.webkit.messageHandlers.lyraStore.postMessage(JSON.stringify(t));
      setTimeout(() => { button.disabled = false; label.textContent = LABEL; }, 5000);
    });
    root.append(style, button);
  };
  const update = () => {
    if (!target()) { if (host) host.remove(); return; }
    if (!document.documentElement) return;
    if (!host) make();
    if (!host.isConnected) document.documentElement.appendChild(host);
  };
  update();
  document.addEventListener("DOMContentLoaded", update);
  setInterval(update, 800);
})();)JS";

HWND Owner() { return g_setHwnd && IsWindowVisible(g_setHwnd) && GetForegroundWindow() == g_setHwnd ? g_setHwnd : g_hWnd; }

void Error(const std::wstring& err) {
    ModalScope modal;
    const std::wstring msg = std::wstring(Tr(L"The extension could not be installed: ", L"Не удалось установить расширение: ")) + err;
    MessageBoxW(Owner(), msg.c_str(), kAppName, MB_OK | MB_ICONWARNING);
}

std::wstring TempFile(const wchar_t* ext) {
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    return std::wstring(tmp) + L"lyra-extension-" + std::to_wstring(GetTickCount64()) + ext;
}

// In the background: (the store's API, then) the package into a temporary file.
void Fetch(std::wstring url, std::wstring amoSlug, std::wstring from) {
    if (s_busy.exchange(true)) return;
    std::thread([url, amoSlug, from]() mutable {
        auto* f = new Fetched;
        f->from = from;
        std::wstring err;
        if (!amoSlug.empty()) {
            // addons.mozilla.org: the add-on's current version names its .xpi
            std::string body;
            const std::wstring api = L"https://addons.mozilla.org/api/v5/addons/addon/" + ToWide(UrlEncode(ToUtf8(amoSlug))) + L"/";
            JsonValue j;
            if (!HttpFetch(api, &body, L"", 0, nullptr, err)) {
                f->err = err;
            } else if (!JsonParse(body, j) || !j.isObject()) {
                f->err = Tr(L"the store answered with something unexpected", L"магазин ответил чем-то непонятным");
            } else if (j.str("type") != "extension") {
                f->err = Tr(L"this is not an extension (a theme or a dictionary?)", L"это не расширение (тема или словарь?)");
            } else {
                const JsonValue* v = j.get("current_version");
                const JsonValue* file = v ? v->get("file") : nullptr;
                const std::string u = file ? file->str("url") : "";
                if (u.rfind("https://addons.mozilla.org/", 0) == 0 || u.rfind("https://addons.cdn.mozilla.net/", 0) == 0) url = ToWide(u);
                else f->err = Tr(L"the store has no file for it", L"у магазина нет файла для него");
            }
        }
        if (f->err.empty()) {
            const std::wstring plain = url.substr(0, url.find_first_of(L"?#"));
            const bool xpi = !amoSlug.empty() || _wcsicmp(PathFindExtensionW(plain.c_str()), L".xpi") == 0;
            f->path = TempFile(xpi ? L".xpi" : L".crx");
            f->ok = HttpFetch(url, nullptr, f->path, 0, nullptr, err);
            if (!f->ok) { f->err = err; DeleteFileW(f->path.c_str()); }
        }
        Log("extensions: download %ls %s%ls", url.c_str(), f->ok ? "done" : "failed: ", f->ok ? L"" : f->err.c_str());
        s_busy = false;
        if (!g_hWnd || !PostMessageW(g_hWnd, WM_APP_EXT_INSTALL, 1, (LPARAM)f)) delete f;
    }).detach();
}

// What the extension will be able to do, in words, and what of it Lyra does not do yet.
std::wstring Describe(const ExtensionInfo& e) {
    static const std::set<std::wstring> kAllSites = { L"<all_urls>", L"*://*/*", L"http://*/*", L"https://*/*", L"*://*/" };
    struct Known { const wchar_t* name; const wchar_t* en; const wchar_t* ru; bool missing; };
    static const Known kKnown[] = {
        { L"tabs",                L"see your tabs and their addresses",  L"видеть вкладки и их адреса", false },
        { L"scripting",           L"run its scripts on pages",           L"запускать свои скрипты на страницах", false },
        { L"history",             L"see your browsing history",          L"видеть историю посещений", true },
        { L"bookmarks",           L"read and change bookmarks",          L"читать и изменять закладки", true },
        { L"downloads",           L"manage downloads",                   L"управлять загрузками", true },
        { L"cookies",             L"read and change cookies",            L"читать и изменять cookie", true },
        { L"clipboardRead",       L"read the clipboard",                 L"читать буфер обмена", false },
        { L"notifications",       L"show notifications",                 L"показывать уведомления", true },
        { L"webRequest",          L"see and change network requests",    L"видеть и изменять сетевые запросы", true },
        { L"declarativeNetRequest", L"block network requests",           L"блокировать сетевые запросы", false },
        { L"declarativeNetRequestWithHostAccess", L"block network requests", L"блокировать сетевые запросы", false },
        { L"webNavigation",       L"follow page loads",                  L"следить за загрузкой страниц", true },
        { L"proxy",               L"change the proxy",                   L"менять прокси", true },
        { L"nativeMessaging",     L"talk to programs on this computer",  L"общаться с программами на компьютере", true },
    };
    std::vector<std::wstring> can, missing, hosts;
    bool all = false;
    for (const std::wstring& p : e.permissions) {
        if (kAllSites.count(p)) { all = true; continue; }
        if (p.find(L"://") != std::wstring::npos) {
            std::wstring h = p.substr(p.find(L"://") + 3);
            h = h.substr(0, h.find(L'/'));
            if (h.rfind(L"*.", 0) == 0) h.erase(0, 2);
            if (!h.empty() && h != L"*" && std::find(hosts.begin(), hosts.end(), h) == hosts.end()) hosts.push_back(h);
            continue;
        }
        for (const Known& k : kKnown)
            if (p == k.name) {
                const std::wstring s = Tr(k.en, k.ru);
                if (std::find(can.begin(), can.end(), s) == can.end()) can.push_back(s);
                if (k.missing && std::find(missing.begin(), missing.end(), p) == missing.end()) missing.push_back(p);
            }
    }
    std::wstring out;
    if (all) can.insert(can.begin(), Tr(L"read and change everything on all sites", L"читать и изменять всё на всех сайтах"));
    else if (!hosts.empty()) {
        std::wstring list;
        for (size_t i = 0; i < hosts.size() && i < 4; ++i) list += (i ? L", " : L"") + hosts[i];
        if (hosts.size() > 4) list += L"…";
        can.insert(can.begin(), std::wstring(Tr(L"read and change everything on ", L"читать и изменять всё на ")) + list);
    }
    if (!can.empty()) {
        out += Tr(L"It will be able to:", L"Оно сможет:");
        for (auto& c : can) out += L"\n  •  " + c;
    } else {
        out += Tr(L"It asks for no special permissions.", L"Особых разрешений оно не просит.");
    }
    if (!missing.empty()) {
        std::wstring list;
        for (size_t i = 0; i < missing.size(); ++i) list += (i ? L", " : L"") + missing[i];
        out += std::wstring(L"\n\n") + Tr(L"Not in Lyra yet: ", L"Пока нет в Lyra: ") + list +
               Tr(L". It may work only in part.", L". Оно может работать не полностью.");
    }
    return out;
}

bool Ask(const ExtensionInfo& e, const std::wstring& from) {
    bool update = false;
    std::wstring oldVersion;
    for (const ExtensionInfo& x : ExtensionsList()) if (x.id == e.id) { update = true; oldVersion = x.version; }
    std::wstring q = update
        ? std::wstring(Tr(L"Update «", L"Обновить «")) + e.name + L"» " + oldVersion + L" → " + e.version + L"?"
        : std::wstring(Tr(L"Add «", L"Установить «")) + e.name + L"» " + e.version + L"?";
    if (!from.empty()) q += L"\n" + from;
    q += L"\n\n" + Describe(e);
    ModalScope modal;
    return MessageBoxW(Owner(), q.c_str(), Tr(L"Add an extension", L"Установка расширения"), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
}

void InstallAsking(const std::wstring& path, const std::wstring& from) {
    bool asked = false, yes = false;
    std::wstring err, name;
    const bool ok = ExtensionInstall(path, err, [&](const ExtensionInfo& e) {
        asked = true;
        name = e.name;
        return yes = Ask(e, from);
    });
    if (ok) {
        ModalScope modal;
        const std::wstring msg = L"«" + name + L"» " +
            Tr(L"is added. It is in Settings → Extensions.", L"установлено. Оно в Настройках → Расширения.");
        MessageBoxW(Owner(), msg.c_str(), kAppName, MB_OK | MB_ICONINFORMATION);
    } else if ((!asked || yes) && !err.empty()) {
        Error(err);
    }
}

void OnStoreMessage(WKScriptMessageRef message, WKCompletionListenerRef reply, const void*) {
    WKTypeRef body = WKScriptMessageGetBody(message);
    JsonValue msg;
    if (body && WKGetTypeID(body) == WKStringGetTypeID() && JsonParse(ToUtf8(WKStr((WKStringRef)body)), msg) && msg.isObject()) {
        const std::string store = msg.str("store"), id = msg.str("id");
        if (store == "cws" && id.size() == 32 && id.find_first_not_of("abcdefghijklmnop") == std::string::npos) {
            Fetch(L"https://clients2.google.com/service/update2/crx?response=redirect&prodversion=131.0.0.0"
                  L"&acceptformat=crx2,crx3&x=id%3D" + ToWide(id) + L"%26uc", L"", Tr(L"From Chrome Web Store.", L"Из Chrome Web Store."));
        } else if (store == "amo" && !id.empty() && id.size() < 200) {
            Fetch(L"", ToWide(id), Tr(L"From Firefox Add-ons.", L"Из Firefox Add-ons."));
        }
    }
    WKStringRef r = WKStringCreateWithUTF8CString("{}");
    WKCompletionListenerComplete(reply, r);
    WKRelease(r);
}

void Replace(std::string& s, const std::string& what, const std::string& with) {
    const size_t at = s.find(what);
    if (at != std::string::npos) s.replace(at, what.size(), with);
}

}  // namespace

void ExtensionStoreAddScript(WKUserContentControllerRef controller) {
    static WKUserContentControllerRef s_withHandler = nullptr;
    WKStringRef world = WKStringCreateWithUTF8CString("lyra-store");
    if (s_withHandler != controller) {
        s_withHandler = controller;
        WKStringRef name = WKStringCreateWithUTF8CString("lyraStore");
        WKUserContentControllerAddScriptMessageHandlerInWorld(controller, name, OnStoreMessage, nullptr, world);
        WKRelease(name);
    }
    std::string source = kStoreScript;
    Replace(source, "%INSTALL%", ToUtf8(Tr(L"Add to Lyra", L"Установить в Lyra")));
    Replace(source, "%WAIT%", ToUtf8(Tr(L"Downloading…", L"Скачиваю…")));
    std::vector<WKTypeRef> allow;
    for (const char* m : { "https://chromewebstore.google.com/*", "https://chrome.google.com/webstore/*", "https://addons.mozilla.org/*" })
        allow.push_back(WKStringCreateWithUTF8CString(m));
    WKArrayRef allowList = WKArrayCreateAdoptingValues(allow.data(), allow.size());
    WKArrayRef blockList = WKArrayCreateAdoptingValues(nullptr, 0);
    WKStringRef src = WKStringCreateWithUTF8CStringWithLength(source.data(), source.size());
    WKUserScriptRef script = WKUserScriptCreateWithSourceInWorld(src, kWKInjectAtDocumentEnd, true, world, allowList, blockList);
    WKUserContentControllerAddUserScript(controller, script);
    WKRelease(script);
    WKRelease(src);
    WKRelease(blockList);
    WKRelease(allowList);
    WKRelease(world);
}

bool ExtensionIsPackage(const std::wstring& url, const std::wstring& mime) {
    if (_wcsicmp(mime.c_str(), L"application/x-xpinstall") == 0 || _wcsicmp(mime.c_str(), L"application/x-chrome-extension") == 0)
        return true;
    std::wstring path = url.substr(0, url.find_first_of(L"?#"));
    if (path.size() < 5) return false;
    const std::wstring tail = path.substr(path.size() - 4);
    return _wcsicmp(tail.c_str(), L".crx") == 0 || _wcsicmp(tail.c_str(), L".xpi") == 0;
}

void ExtensionInstallFromUrl(const std::wstring& url) {
    std::wstring host = url.substr(url.find(L"://") == std::wstring::npos ? 0 : url.find(L"://") + 3);
    host = host.substr(0, host.find_first_of(L"/?#"));
    Fetch(url, L"", std::wstring(Tr(L"From ", L"С сайта ")) + host + L".");
}

void ExtensionInstallFromPath(const std::wstring& path) {
    InstallAsking(path, std::wstring(Tr(L"From the file ", L"Из файла ")) + PathFindFileNameW(path.c_str()) + L".");
}

void ExtensionStoreMessage(WPARAM, LPARAM lParam) {
    std::unique_ptr<Fetched> f((Fetched*)lParam);
    if (!f->ok) { Error(f->err.empty() ? std::wstring(Tr(L"the download failed", L"не удалось скачать")) : f->err); return; }
    InstallAsking(f->path, f->from);
    DeleteFileW(f->path.c_str());
}
