// Lyra — менеджер паролей: предложение сохранить вход, автозаполнение, хранение (шифрование Windows).
#include "browser/Passwords.h"
#include "app/Log.h"
#include "browser/AdBlock.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "core/Settings.h"
#include "core/StringUtils.h"
#include "core/Localization.h"
#include "ui/MainWindow.h"
#include "ui/Menu.h"
#include "ui/PermissionPrompt.h"
#include "ui/settings/SettingsPages.h"

#include <WebKit/WKFrameInfoRef.h>
#include <WebKit/WKPageFullScreenClient.h>      // WKCompletionListenerComplete
#include <WebKit/WKScriptMessageRef.h>
#include <WebKit/WKUserContentControllerRef.h>
#include <WebKit/WKUserScriptRef.h>

#include <wincrypt.h>
#include <dpapi.h>
#include <ctime>
#include <fstream>
#include <iterator>
#pragma comment(lib, "crypt32")

namespace {

// Injected into every frame at document start.
const char kScript[] = R"JS((function () {
  if (window.__lyraPw) return;
  window.__lyraPw = 1;
  var h = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.lyraPasswords;
  if (!h) return;
  var done = false, sent = '';
  function visible(e) {
    var r = e.getBoundingClientRect();
    return r.width > 0 && r.height > 0 && getComputedStyle(e).visibility !== 'hidden';
  }
  function all(sel, scope) { return Array.prototype.slice.call((scope || document).querySelectorAll(sel)); }
  function userFieldFor(pw) {
    var scope = pw.form || document, list = all('input', scope), idx = list.indexOf(pw), best = null;
    for (var i = 0; i < idx; i++) {
      var t = (list[i].type || 'text').toLowerCase();
      if ((t === 'text' || t === 'email' || t === 'tel') && visible(list[i])) best = list[i];
    }
    return best || scope.querySelector('input[autocomplete~=username]');
  }
  function setValue(e, v) {
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set.call(e, v);
    e.dispatchEvent(new Event('input', { bubbles: true }));
    e.dispatchEvent(new Event('change', { bubbles: true }));
  }
  function fill() {
    if (done) return;
    var pws = all('input[type=password]').filter(visible);
    if (!pws.length) return;
    var pw = pws[0];
    if (pw.value || pw.autocomplete === 'new-password') return;
    var u = userFieldFor(pw);
    done = true;
    h.postMessage(['fill', (u && u.value) || '']).then(function (r) {
      var p = String(r || '').split('\x01');
      if (p.length < 2) return;
      if (u && !u.value && p[0]) setValue(u, p[0]);
      if (!pw.value) setValue(pw, p[1]);
    }, function () {});
  }
  function capture() {
    var pws = all('input[type=password]').filter(function (e) { return e.value; });
    if (!pws.length) return;
    var pw = pws.length >= 3 ? pws[1] : pws[0];           // change forms: current, new, confirm
    var u = userFieldFor(pws[0]), user = (u && u.value) || '';
    var key = user + '\x01' + pw.value;
    if (key === sent) return;
    sent = key;
    h.postMessage(['save', user, pw.value]).catch(function () {});
  }
  document.addEventListener('submit', capture, true);
  document.addEventListener('keydown', function (e) {
    if (e.key === 'Enter' && e.target && e.target.tagName === 'INPUT') setTimeout(capture, 0);
  }, true);
  document.addEventListener('click', function (e) {
    var b = e.target && e.target.closest && e.target.closest('button, input[type=submit], input[type=button], [role=button]');
    if (b) capture();
  }, true);
  document.addEventListener('focusin', function (e) {
    if (e.target && e.target.type === 'password') fill();
  }, true);
  window.addEventListener('pagehide', capture, true);
  function start() {
    fill();
    var n = 0, t = setInterval(function () { fill(); if (done || ++n > 10) clearInterval(t); }, 1000);
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', start); else start();
})();)JS";

std::vector<SavedPassword> s_saved;
std::vector<std::string> s_never;

struct Pending { PasswordAsk ask; std::wstring password; };
std::vector<Pending> s_pending;
unsigned long long s_nextSeq = 1;

std::wstring File() { return g_settingsDir + L"\\passwords.dat"; }

const char kEntropy[] = "Lyra saved passwords";

std::string Protect(const std::string& plain, bool encrypt) {
    DATA_BLOB in = { (DWORD)plain.size(), (BYTE*)plain.data() }, out = {};
    DATA_BLOB entropy = { (DWORD)sizeof(kEntropy), (BYTE*)kEntropy };
    const BOOL ok = encrypt
        ? CryptProtectData(&in, L"Lyra", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)
        : CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out);
    if (!ok) return std::string();
    std::string s((const char*)out.pbData, out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return s;
}

std::string Escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == '\t') o += "\\t";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else o += c;
    }
    return o;
}

std::string Unescape(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 == s.size()) { o += s[i]; continue; }
        const char c = s[++i];
        o += c == 't' ? '\t' : c == 'n' ? '\n' : c == 'r' ? '\r' : c;
    }
    return o;
}

std::vector<std::string> SplitTabs(const std::string& line) {
    std::vector<std::string> f(1);
    for (char c : line) { if (c == '\t') f.emplace_back(); else f.back() += c; }
    return f;
}

void Load() {
    s_saved.clear();
    s_never.clear();
    std::ifstream f(File(), std::ios::binary);
    if (!f) return;
    std::string blob((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    std::string text = Protect(blob, false);
    if (text.empty()) { if (!blob.empty()) Log("passwords: passwords.dat cannot be decrypted"); return; }
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const auto fl = SplitTabs(text.substr(pos, end - pos));
        pos = end + 1;
        if (fl[0] == "L" && fl.size() >= 5) {
            SavedPassword p;
            p.origin = Unescape(fl[1]);
            p.username = ToWide(Unescape(fl[2]));
            p.password = ToWide(Unescape(fl[3]));
            p.lastUsed = atoll(fl[4].c_str());
            s_saved.push_back(std::move(p));
        } else if (fl[0] == "N" && fl.size() >= 2) {
            s_never.push_back(Unescape(fl[1]));
        }
    }
    SecureZeroMemory(&text[0], text.size());
    Log("passwords: %zu saved, %zu never", s_saved.size(), s_never.size());
}

void Save() {
    std::string text = "LyraPasswords 1\n";
    for (auto& p : s_saved)
        text += "L\t" + Escape(p.origin) + "\t" + Escape(ToUtf8(p.username)) + "\t" + Escape(ToUtf8(p.password)) +
                "\t" + std::to_string(p.lastUsed) + "\n";
    for (auto& o : s_never) text += "N\t" + Escape(o) + "\n";
    const std::string blob = Protect(text, true);
    SecureZeroMemory(&text[0], text.size());
    if (blob.empty()) { Log("passwords: encryption failed, not saved"); return; }
    const std::wstring path = File(), tmp = path + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f.write(blob.data(), (std::streamsize)blob.size());
        if (!f) return;
    }
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

long long Now() { return (long long)time(nullptr); }

bool IsNever(const std::string& origin) {
    return std::find(s_never.begin(), s_never.end(), origin) != s_never.end();
}

SavedPassword* Find(const std::string& origin, const std::wstring& username) {
    for (auto& p : s_saved) if (p.origin == origin && p.username == username) return &p;
    return nullptr;
}

Tab* TabOfPage(WKPageRef page) {
    for (auto& t : g_tabs) if (t->page == page && t->state == TabState::Live) return t.get();
    return nullptr;
}

void Prune() {
    for (size_t i = 0; i < s_pending.size();) {
        bool live = false;
        for (auto& t : g_tabs) if (t->id == s_pending[i].ask.tabId && t->state == TabState::Live) { live = true; break; }
        if (live) ++i; else s_pending.erase(s_pending.begin() + i);
    }
}

void OfferSave(WKPageRef page, const std::string& origin, std::wstring user, const std::wstring& password) {
    if (!g_prefs.savePasswords || password.empty() || IsNever(origin)) return;
    if (Tab* pt = TabOfPage(page)) if (pt->privateTab) return;      // nothing is kept from private tabs
    // A step with only the password (the name was on the previous page): the site's only sign-in.
    if (user.empty()) {
        int n = 0;
        for (auto& p : s_saved) if (p.origin == origin) { ++n; user = p.username; }
        if (n != 1) user.clear();
    }
    SavedPassword* existing = Find(origin, user);
    if (existing && existing->password == password) {
        existing->lastUsed = Now();
        Save();
        return;
    }
    Tab* t = TabOfPage(page);
    if (!t) return;
    for (auto& p : s_pending)
        if (p.ask.tabId == t->id && p.ask.origin == origin) {      // the same sign-in typed again
            p.ask.username = user;
            p.ask.update = existing != nullptr;
            p.password = password;
            PermissionPromptUpdate();
            return;
        }
    Pending p;
    p.ask.seq = s_nextSeq++;
    p.ask.tabId = t->id;
    p.ask.update = existing != nullptr;
    p.ask.origin = origin;
    p.ask.site = PasswordsSiteName(origin);
    p.ask.username = user;
    p.password = password;
    Log("passwords: asking to %s for %s (tab#%llu)", p.ask.update ? "update" : "save", origin.c_str(), t->id);
    s_pending.push_back(std::move(p));
    PermissionPromptUpdate();
}

std::wstring FillAnswer(const std::string& origin, const std::wstring& typedUser) {
    if (!g_prefs.savePasswords) return std::wstring();
    SavedPassword* best = nullptr;
    for (auto& p : s_saved) {
        if (p.origin != origin) continue;
        if (!typedUser.empty()) { if (p.username == typedUser) { best = &p; break; } continue; }
        if (!best || p.lastUsed > best->lastUsed) best = &p;
    }
    if (!best) return std::wstring();
    return best->username + L'\x01' + best->password;
}

void OnMessage(WKScriptMessageRef message, WKCompletionListenerRef reply, const void*) {
    std::vector<std::wstring> args;
    WKTypeRef body = WKScriptMessageGetBody(message);
    if (body && WKGetTypeID(body) == WKArrayGetTypeID()) {
        WKArrayRef a = (WKArrayRef)body;
        for (size_t i = 0; i < WKArrayGetSize(a); ++i) {
            WKTypeRef item = WKArrayGetItemAtIndex(a, i);
            args.push_back(item && WKGetTypeID(item) == WKStringGetTypeID() ? WKStr((WKStringRef)item) : std::wstring());
        }
    }
    // The origin is the frame's own, never what the page says.
    std::string origin;
    WKPageRef page = nullptr;
    if (WKFrameInfoRef frame = WKScriptMessageGetFrameInfo(message)) {
        page = WKFrameInfoGetPage(frame);
        if (WKSecurityOriginRef o = WKFrameInfoCopySecurityOrigin(frame)) {
            if (WKStringRef s = WKSecurityOriginCopyToString(o)) { origin = WKStrUtf8(s); WKRelease(s); }
            WKRelease(o);
        }
    }
    const bool web = origin.rfind("https://", 0) == 0 || origin.rfind("http://", 0) == 0;
    std::wstring answer;
    if (web && !args.empty()) {
        if (args[0] == L"fill") answer = FillAnswer(origin, args.size() > 1 ? args[1] : std::wstring());
        else if (args[0] == L"save" && args.size() >= 3) OfferSave(page, origin, args[1], args[2]);
    }
    WKStringRef r = WKStringCreateWithUTF8CString(ToUtf8(answer).c_str());
    WKCompletionListenerComplete(reply, r);
    WKRelease(r);
}

} // namespace

std::wstring PasswordsSiteName(const std::string& origin) {
    std::string s = origin;
    const size_t scheme = s.find("://");
    if (scheme != std::string::npos) s = s.substr(scheme + 3);
    if (s.rfind("www.", 0) == 0) s = s.substr(4);
    return ToWide(s);
}

const PasswordAsk* PasswordsPendingForTab(unsigned long long tabId) {
    Prune();
    for (auto& p : s_pending) if (p.ask.tabId == tabId) return &p.ask;
    return nullptr;
}

void PasswordsAnswer(unsigned long long seq, int answer) {
    auto it = std::find_if(s_pending.begin(), s_pending.end(), [seq](const Pending& p) { return p.ask.seq == seq; });
    if (it == s_pending.end()) return;
    const Pending p = *it;
    s_pending.erase(it);
    if (answer == 1) {
        if (SavedPassword* e = Find(p.ask.origin, p.ask.username)) {
            e->password = p.password;
            e->lastUsed = Now();
        } else {
            SavedPassword n;
            n.origin = p.ask.origin;
            n.username = p.ask.username;
            n.password = p.password;
            n.lastUsed = Now();
            s_saved.push_back(std::move(n));
        }
        Save();
        Log("passwords: %s for %s", p.ask.update ? "updated" : "saved", p.ask.origin.c_str());
    } else if (answer == 0 && !p.ask.update) {
        if (!IsNever(p.ask.origin)) s_never.push_back(p.ask.origin);
        Save();
    }
    PermissionPromptUpdate();
}

std::vector<SavedPassword> PasswordsList() {
    std::vector<SavedPassword> v = s_saved;
    std::sort(v.begin(), v.end(), [](const SavedPassword& a, const SavedPassword& b) {
        const std::wstring sa = PasswordsSiteName(a.origin), sb = PasswordsSiteName(b.origin);
        return sa != sb ? sa < sb : a.username < b.username;
    });
    return v;
}

std::vector<std::string> PasswordsNeverList() { return s_never; }

void PasswordsDelete(const std::string& origin, const std::wstring& username) {
    s_saved.erase(std::remove_if(s_saved.begin(), s_saved.end(), [&](const SavedPassword& p) {
        return p.origin == origin && p.username == username;
    }), s_saved.end());
    Save();
}

void PasswordsRemoveNever(const std::string& origin) {
    s_never.erase(std::remove(s_never.begin(), s_never.end(), origin), s_never.end());
    Save();
}

// "https://example.com/path" -> "https://example.com" (as WebKit writes origins).
static std::string OriginOfUrl(const std::wstring& url) {
    const size_t scheme = url.find(L"://");
    if (scheme == std::wstring::npos) return std::string();
    size_t end = url.find_first_of(L"/?#", scheme + 3);
    if (end == std::wstring::npos) end = url.size();
    std::wstring o = url.substr(0, end);
    const size_t at = o.find(L'@', scheme + 3);          // user:pass@host
    if (at != std::wstring::npos) o = o.substr(0, scheme + 3) + o.substr(at + 1);
    for (auto& ch : o) ch = (wchar_t)towlower(ch);
    std::string s = ToUtf8(o);
    if (s.size() > 4 && s.compare(s.size() - 4, 4, ":443") == 0 && s.rfind("https://", 0) == 0) s.resize(s.size() - 4);
    if (s.size() > 3 && s.compare(s.size() - 3, 3, ":80") == 0 && s.rfind("http://", 0) == 0) s.resize(s.size() - 3);
    return s;
}

size_t PasswordsCountForUrl(const std::wstring& url) {
    if (s_saved.empty()) return 0;
    const std::string origin = OriginOfUrl(url);
    size_t n = 0;
    for (auto& p : s_saved) if (p.origin == origin) ++n;
    return n;
}

static void CopyText(const std::wstring& s) {
    if (!OpenClipboard(g_hWnd)) return;
    EmptyClipboard();
    const size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        memcpy(GlobalLock(h), s.c_str(), bytes);
        GlobalUnlock(h);
        if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
    }
    CloseClipboard();
}

void PasswordsSiteMenu() {
    Tab* t = ActiveTab();
    if (!t) return;
    const std::string origin = OriginOfUrl(t->url);
    std::vector<SavedPassword> here;
    for (auto& p : s_saved) if (p.origin == origin) here.push_back(p);
    HMENU m = CreatePopupMenu();
    std::wstring head = Tr(L"Saved for ", L"Сохранено для ") + PasswordsSiteName(origin);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, head.c_str());
    for (size_t i = 0; i < here.size(); ++i) {
        const std::wstring user = here[i].username.empty() ? std::wstring(Tr(L"(no username)", L"(без имени)")) : here[i].username;
        HMENU sub = CreatePopupMenu();
        AppendMenuW(sub, MF_STRING, 1000 + i * 2, Tr(L"Copy username", L"Копировать имя пользователя"));
        AppendMenuW(sub, MF_STRING, 1001 + i * 2, Tr(L"Copy password", L"Копировать пароль"));
        AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, user.c_str());
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 1, Tr(L"Manage passwords\x2026", L"Управление паролями\x2026"));
    POINT pt;
    GetCursorPos(&pt);
    MenuSetIcon(m, 1, L"\uE8D7");
    const int cmd = MenuTrack(m, TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, g_hWnd);
    DestroyMenu(m);
    if (cmd == 1) OpenPasswordsSettings();
    else if (cmd >= 1000 && (size_t)(cmd - 1000) / 2 < here.size()) {
        const SavedPassword& p = here[(cmd - 1000) / 2];
        CopyText((cmd - 1000) % 2 ? p.password : p.username);
    }
    for (auto& p : here) SecureZeroMemory(&p.password[0], p.password.size() * sizeof(wchar_t));
}

void PasswordsInit() {
    Load();
    WKUserContentControllerRef controller = AdBlockUserContentController();
    if (!controller) return;
    WKStringRef name = WKStringCreateWithUTF8CString("lyraPasswords");
    WKUserContentControllerAddScriptMessageHandler(controller, name, OnMessage, nullptr);
    WKRelease(name);
    AdBlockAddUserScript(kScript, false);
}

void PasswordsShutdown() {
    s_pending.clear();
    for (auto& p : s_saved) SecureZeroMemory(&p.password[0], p.password.size() * sizeof(wchar_t));
    s_saved.clear();
}

// ----------------------------------------------------------------------------
//  Import from CSV
// ----------------------------------------------------------------------------
namespace {

// RFC 4180: fields separated by commas, quoted ones may hold commas, quotes ("") and line breaks.
std::vector<std::vector<std::string>> ParseCsv(const std::string& s) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    bool quoted = false, any = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < s.size() && s[i + 1] == '"') { field += '"'; ++i; }
                else quoted = false;
            } else field += c;
            continue;
        }
        if (c == '"') { quoted = true; any = true; }
        else if (c == ',') { row.push_back(std::move(field)); field.clear(); any = true; }
        else if (c == '\r') {}
        else if (c == '\n') {
            if (any || !field.empty()) { row.push_back(std::move(field)); rows.push_back(std::move(row)); }
            row.clear(); field.clear(); any = false;
        } else { field += c; any = true; }
    }
    if (any || !field.empty()) { row.push_back(std::move(field)); rows.push_back(std::move(row)); }
    return rows;
}

std::string LowerA(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    while (!s.empty() && (s.back() == ' ')) s.pop_back();
    while (!s.empty() && s[0] == ' ') s.erase(0, 1);
    return s;
}

}  // namespace

PasswordsImportResult PasswordsImportCsv(const std::wstring& path) {
    PasswordsImportResult r;
    std::string data;
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) { r.added = -1; return r; }
        data.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    if (data.size() >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB) data.erase(0, 3);
    auto rows = ParseCsv(data);
    SecureZeroMemory(data.data(), data.size());
    if (rows.size() < 1) { r.added = -1; return r; }
    int cUrl = -1, cUser = -1, cPass = -1;
    for (int i = 0; i < (int)rows[0].size(); ++i) {
        const std::string h = LowerA(rows[0][i]);
        if (cUrl < 0 && (h == "url" || h == "login_uri" || h == "website" || h == "origin" || h == "origin_url")) cUrl = i;
        else if (cUser < 0 && (h == "username" || h == "login_username" || h == "login" || h == "username_value")) cUser = i;
        else if (cPass < 0 && (h == "password" || h == "login_password" || h == "password_value")) cPass = i;
    }
    if (cUrl < 0 || cPass < 0) {
        Log("passwords: %ls has no url/password columns", path.c_str());
        for (auto& row : rows) for (auto& f : row) SecureZeroMemory(f.data(), f.size());
        r.added = -1;
        return r;
    }
    bool changed = false;
    for (size_t i = 1; i < rows.size(); ++i) {
        auto& row = rows[i];
        auto cell = [&](int c) -> std::string { return c >= 0 && c < (int)row.size() ? row[c] : std::string(); };
        std::string url = cell(cUrl);
        // Bitwarden may list several addresses ("a,b"): the first. A bare host gets https://.
        if (const size_t comma = url.find(','); comma != std::string::npos) url.resize(comma);
        if (!url.empty() && url.find("://") == std::string::npos) url = "https://" + url;
        const std::string origin = OriginOfUrl(ToWide(url));
        const std::wstring user = ToWide(cell(cUser)), pass = ToWide(cell(cPass));
        if (origin.empty() || pass.empty() || (origin.rfind("https://", 0) != 0 && origin.rfind("http://", 0) != 0)) {
            ++r.skipped;
            continue;
        }
        if (SavedPassword* have = Find(origin, user)) {
            if (have->password == pass) ++r.same; else ++r.kept;
            continue;
        }
        SavedPassword p;
        p.origin = origin;
        p.username = user;
        p.password = pass;
        p.lastUsed = 0;
        s_saved.push_back(std::move(p));
        ++r.added;
        changed = true;
    }
    for (auto& row : rows) for (auto& f : row) SecureZeroMemory(f.data(), f.size());
    if (changed) Save();
    Log("passwords: imported %d from a CSV (%d already saved, %d kept, %d skipped)", r.added, r.same, r.kept, r.skipped);
    return r;
}
