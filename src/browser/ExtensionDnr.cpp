// Lyra - declarativeNetRequest -> WebKit content blocker rules (see ExtensionDnr.h).
#include "browser/ExtensionDnr.h"
#include "browser/AdBlock.h"
#include "core/Json.h"
#include "core/StringUtils.h"
#include "common/Platform.h"
#include <algorithm>
#include <map>
#include <unordered_set>

namespace {

std::string ReadAll(const std::wstring& path, size_t limit) {
    std::string data;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return data;
    LARGE_INTEGER size = {};
    GetFileSizeEx(f, &size);
    if (size.QuadPart > 0 && (unsigned long long)size.QuadPart <= limit) {
        data.resize((size_t)size.QuadPart);
        DWORD read = 0;
        ReadFile(f, data.data(), (DWORD)data.size(), &read, nullptr);
        data.resize(read);
    }
    CloseHandle(f);
    return data;
}

std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// For if-domain / unless-domain: "*example.com" (the domain and its subdomains).
bool CleanDomain(const std::string& in, std::string& out) {
    std::string d = Lower(in);
    while (!d.empty() && d.back() == '.') d.pop_back();
    if (d.empty()) return false;
    for (char c : d)
        if (!(isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_')) return false;
    out = "*" + d;
    return true;
}

// The address of a request to `domain` or one of its subdomains.
bool DomainRegex(const std::string& in, std::string& re) {
    std::string d;
    if (!CleanDomain(in, d)) return false;
    re = "^[a-z][a-z0-9.+-]*:/+([^/:]*\\.)?";
    for (size_t i = 1; i < d.size(); ++i) {
        if (d[i] == '.') re += "\\.";
        else re += d[i];
    }
    re += "[:/]";
    return true;
}

std::vector<std::string> Domains(const JsonValue& cond, const char* key, const char* legacy) {
    std::vector<std::string> out;
    const JsonValue* list = cond.get(key);
    if (!list && legacy) list = cond.get(legacy);
    if (list && list->isArray())
        for (auto& v : list->items)
            if (v.isString()) out.push_back(v.s);
    return out;
}

// DNR resource types -> WebKit's
const char* const kAllTypes[] = { "top-document", "child-document", "image", "style-sheet", "script", "font", "media",
                                  "svg-document", "ping", "csp-report", "fetch", "websocket", "other" };

void MapType(const std::string& t, std::set<std::string>& out) {
    if (t == "main_frame") out.insert("top-document");
    else if (t == "sub_frame") out.insert("child-document");
    else if (t == "stylesheet") out.insert("style-sheet");
    else if (t == "script") out.insert("script");
    else if (t == "image") out.insert("image");
    else if (t == "font") out.insert("font");
    else if (t == "media") out.insert("media");
    else if (t == "xmlhttprequest") out.insert("fetch");
    else if (t == "websocket") out.insert("websocket");
    else if (t == "ping") out.insert("ping");
    else if (t == "csp_report") out.insert("csp-report");
    else if (t == "object") { out.insert("other"); out.insert("svg-document"); }
    else out.insert("other");             // webtransport, webbundle, other
}

std::vector<std::string> Types(const JsonValue& cond) {
    std::set<std::string> set;
    const JsonValue* in = cond.get("resourceTypes");
    const JsonValue* ex = cond.get("excludedResourceTypes");
    if (in && in->isArray() && !in->items.empty()) {
        for (auto& v : in->items) if (v.isString()) MapType(v.s, set);
    } else {
        for (const char* t : kAllTypes) set.insert(t);
        if (!ex) set.erase("top-document");            // the default: everything but the page itself
    }
    if (ex && ex->isArray()) {
        std::set<std::string> drop;
        for (auto& v : ex->items) if (v.isString()) MapType(v.s, drop);
        drop.erase("other");                            // ("other" also stands for types not excluded)
        for (auto& d : drop) set.erase(d);
    }
    std::vector<std::string> out;
    for (const char* t : kAllTypes) if (set.count(t)) out.push_back(t);
    return out;
}

std::vector<std::string> Methods(const JsonValue& cond) {
    static const char* const kAll[] = { "get", "head", "options", "trace", "put", "delete", "post", "patch", "connect" };
    std::vector<std::string> out;
    if (const JsonValue* in = cond.get("requestMethods")) {
        for (auto& v : in->items) if (v.isString()) out.push_back(Lower(v.s));
    } else if (const JsonValue* ex = cond.get("excludedRequestMethods")) {
        std::set<std::string> drop;
        for (auto& v : ex->items) if (v.isString()) drop.insert(Lower(v.s));
        for (const char* m : kAll) if (!drop.count(m)) out.push_back(m);
    }
    return out;
}

std::string Base64(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8) | (unsigned char)in[i + 2];
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += t[v & 63];
    }
    if (i + 1 == in.size()) {
        const unsigned v = (unsigned char)in[i] << 16;
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        const unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8);
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += '=';
    }
    return out;
}

const char* Mime(const std::string& path) {
    const std::string ext = Lower(path.substr(path.find_last_of('.') == std::string::npos ? path.size() : path.find_last_of('.')));
    if (ext == ".js" || ext == ".mjs") return "application/javascript";
    if (ext == ".css") return "text/css";
    if (ext == ".html" || ext == ".htm") return "text/html";
    if (ext == ".json") return "application/json";
    if (ext == ".png") return "image/png";
    if (ext == ".gif") return "image/gif";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".webp") return "image/webp";
    if (ext == ".mp4") return "video/mp4";
    if (ext == ".mp3") return "audio/mpeg";
    if (ext == ".xml") return "text/xml";
    return "text/plain";
}

// A file of the extension ("/web_accessible_resources/noop.js") as a data: URL.
thread_local std::map<std::string, std::string>* s_fileUrls = nullptr;     // per conversion: path -> data: URL

bool ExtensionFileUrl(const std::wstring& dir, const std::string& path, std::string& url) {
    if (s_fileUrls) {
        auto it = s_fileUrls->find(path);
        if (it != s_fileUrls->end()) { url = it->second; return !url.empty(); }
    }
    const bool ok = [&] {
    std::string rel = path;
    while (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
    if (rel.empty() || rel.find("..") != std::string::npos || rel.find(':') != std::string::npos) return false;
    std::wstring native = ToWide(rel.substr(0, rel.find_first_of("?#")));
    for (auto& c : native) if (c == L'/') c = L'\\';
    const std::string data = ReadAll(dir + L"\\" + native, 512u << 10);
    if (data.empty()) return false;
    url = std::string("data:") + Mime(rel) + ";base64," + Base64(data);
    return true;
    }();
    if (!ok) url.clear();
    if (s_fileUrls) (*s_fileUrls)[path] = url;
    return ok;
}

std::string List(const std::vector<std::string>& items) {
    std::string s = "[";
    for (size_t i = 0; i < items.size(); ++i) s += (i ? "," : "") + JsonQuote(items[i]);
    return s + "]";
}

struct Trigger {
    std::string url = ".*";
    bool caseSensitive = false;
    std::vector<std::string> ifDomain, unlessDomain, types, ifTopUrl, ifFrameUrl;
    std::string loadType, method;
    std::string Json() const {
        std::string s = "{\"url-filter\":" + JsonQuote(url);
        if (caseSensitive) s += ",\"url-filter-is-case-sensitive\":true";
        if (!ifDomain.empty()) s += ",\"if-domain\":" + List(ifDomain);
        else if (!unlessDomain.empty()) s += ",\"unless-domain\":" + List(unlessDomain);
        else if (!ifTopUrl.empty()) s += ",\"if-top-url\":" + List(ifTopUrl);
        else if (!ifFrameUrl.empty()) s += ",\"if-frame-url\":" + List(ifFrameUrl);
        if (!types.empty()) s += ",\"resource-type\":" + List(types);
        if (!loadType.empty()) s += ",\"load-type\":[" + JsonQuote(loadType) + "]";
        if (!method.empty()) s += ",\"request-method\":" + JsonQuote(method);
        return s + "}";
    }
};

struct Out {
    long long priority;
    int rank;                 // within one priority: later wins
    bool regex;               // from a regexFilter (left out of the fallback)
    std::string json;
};

std::string RedirectTransform(const JsonValue& t) {
    std::string s = "{";
    auto add = [&](const std::string& kv) { s += (s.size() > 1 ? "," : "") + kv; };
    for (const char* k : { "scheme", "host", "port", "path", "query", "fragment", "username", "password" })
        if (const JsonValue* v = t.get(k)) if (v->isString()) add(JsonQuote(k) + ":" + JsonQuote(v->s));
    if (const JsonValue* q = t.get("queryTransform")) {
        std::string qs = "{";
        if (const JsonValue* rm = q->get("removeParams")) qs += "\"remove-parameters\":" + rm->raw;
        if (const JsonValue* ar = q->get("addOrReplaceParams")) {
            std::string list = "[";
            for (auto& p : ar->items) {
                if (list.size() > 1) list += ",";
                list += "{\"key\":" + JsonQuote(p.str("key")) + ",\"value\":" + JsonQuote(p.str("value"));
                if (p.flag("replaceOnly")) list += ",\"replace-only\":true";
                list += "}";
            }
            qs += std::string(qs.size() > 1 ? "," : "") + "\"add-or-replace-parameters\":" + list + "]";
        }
        add("\"query-transform\":" + qs + "}");
    }
    return s + "}";
}

std::string Headers(const JsonValue* list) {
    std::string s = "[";
    if (list && list->isArray())
        for (auto& h : list->items) {
            const std::string op = h.str("operation"), name = h.str("header");
            if (name.empty() || (op != "set" && op != "append" && op != "remove")) continue;
            if (s.size() > 1) s += ",";
            s += "{\"operation\":" + JsonQuote(op) + ",\"header\":" + JsonQuote(name);
            if (op != "remove") s += ",\"value\":" + JsonQuote(h.str("value"));
            s += "}";
        }
    return s + "]";
}

void Emit(std::vector<Out>& out, long long priority, int rank, bool regex, const Trigger& t, const std::string& action) {
    out.push_back({ priority, rank, regex, "{\"trigger\":" + t.Json() + ",\"action\":" + action + "}" });
}

// One DNR rule -> WebKit rules. False: not converted.
bool ConvertRule(const JsonValue& r, const std::wstring& dir, std::vector<Out>& out) {
    const JsonValue* action = r.get("action");
    if (!action || !action->isObject()) return false;
    static const JsonValue kEmpty = [] { JsonValue v; JsonParse("{}", v); return v; }();
    const JsonValue* condPtr = r.get("condition");
    const JsonValue& cond = condPtr && condPtr->isObject() ? *condPtr : kEmpty;
    const long long priority = r.get("priority") ? (long long)r.get("priority")->n : 1;
    const std::string type = action->str("type");
    if (const JsonValue* tabs = cond.get("tabIds")) if (!tabs->items.empty()) return false;
    if (cond.get("responseHeaders") || cond.get("excludedResponseHeaders")) return false;

    // the address
    std::string re;
    const std::string regexFilter = cond.str("regexFilter"), urlFilter = cond.str("urlFilter");
    const bool isRegex = !regexFilter.empty();
    if (isRegex) {
        if (!DnrRegexSupported(regexFilter)) return false;
        re = regexFilter;
    } else if (!urlFilter.empty()) {
        if (!AdBlockUrlFilter(urlFilter, re) || (re != ".*" && !DnrRegexSupported(re))) return false;
    } else {
        re = ".*";
    }

    Trigger base;
    base.caseSensitive = cond.flag("isUrlFilterCaseSensitive", false);
    for (auto& d : Domains(cond, "initiatorDomains", "domains")) { std::string c; if (CleanDomain(d, c)) base.ifDomain.push_back(c); }
    if (base.ifDomain.empty())
        for (auto& d : Domains(cond, "excludedInitiatorDomains", "excludedDomains")) { std::string c; if (CleanDomain(d, c)) base.unlessDomain.push_back(c); }
    base.types = Types(cond);
    if (base.types.empty()) return false;
    const std::string domainType = cond.str("domainType");
    if (domainType == "firstParty") base.loadType = "first-party";
    else if (domainType == "thirdParty") base.loadType = "third-party";

    // requestDomains: one rule per domain (WebKit has no condition on the request's host but the address)
    auto withDomain = [&](const std::string& domain, std::string& u) {
        std::string d;
        if (!DomainRegex(domain, d)) return false;
        if (re == ".*") u = d;
        else if (re[0] == '^') u = re;               // anchored already (usually to a host)
        else u = d + ".*" + re;
        return true;
    };
    std::vector<std::string> urls;
    const std::vector<std::string> requestDomains = Domains(cond, "requestDomains", nullptr);
    if (requestDomains.empty()) urls.push_back(re);
    else {
        std::unordered_set<std::string> seen;
        for (auto& d : requestDomains) { std::string u; if (withDomain(d, u) && seen.insert(u).second) urls.push_back(std::move(u)); }
    }
    if (urls.empty()) return false;
    std::vector<std::string> methods = Methods(cond);
    if (methods.empty()) methods.push_back("");

    std::string act;
    int rank = 0;
    if (type == "block") { act = "{\"type\":\"block\"}"; rank = 4; }
    else if (type == "allow") { act = "{\"type\":\"ignore-previous-rules\"}"; rank = 8; }
    else if (type == "upgradeScheme") { act = "{\"type\":\"make-https\"}"; rank = 2; }
    else if (type == "redirect") {
        const JsonValue* rd = action->get("redirect");
        if (!rd) return false;
        std::string target;
        std::string u;
        if (!rd->str("extensionPath").empty()) {
            if (ExtensionFileUrl(dir, rd->str("extensionPath"), u)) target = "{\"url\":" + JsonQuote(u) + "}";
        } else if (!rd->str("url").empty()) {
            const std::string url = rd->str("url");
            if (_strnicmp(url.c_str(), "javascript:", 11) != 0) target = "{\"url\":" + JsonQuote(url) + "}";
        } else if (const JsonValue* t = rd->get("transform")) {
            target = "{\"transform\":" + RedirectTransform(*t) + "}";
        } else if (!rd->str("regexSubstitution").empty() && isRegex) {
            target = "{\"regex-substitution\":" + JsonQuote(rd->str("regexSubstitution")) + "}";
        }
        if (target.empty()) { act = "{\"type\":\"block\"}"; rank = 4; }   // a file it does not have: at least not loaded
        else { act = "{\"type\":\"redirect\",\"redirect\":" + target + ",\"priority\":" + std::to_string(std::max(0LL, priority)) + "}"; rank = 0; }
    } else if (type == "modifyHeaders") {
        const std::string req = Headers(action->get("requestHeaders")), resp = Headers(action->get("responseHeaders"));
        if (req == "[]" && resp == "[]") return false;
        act = "{\"type\":\"modify-headers\",\"request-headers\":" + req + ",\"response-headers\":" + resp +
              ",\"priority\":" + std::to_string(std::max(0LL, priority)) + "}";
        rank = 0;
    } else if (type == "allowAllRequests") {
        // Everything the page (or frame) loads, and the page itself.
        const bool top = std::find(base.types.begin(), base.types.end(), "top-document") != base.types.end();
        const bool child = std::find(base.types.begin(), base.types.end(), "child-document") != base.types.end();
        if (!top && !child) return false;
        const std::string ignore = "{\"type\":\"ignore-previous-rules\"}";
        for (auto& u : urls) {
            Trigger doc = base;
            doc.url = u;
            doc.types.clear();
            if (top) doc.types.push_back("top-document");
            if (child) doc.types.push_back("child-document");
            Emit(out, priority, 6, isRegex, doc, ignore);
            Trigger all;
            all.loadType = base.loadType;
            if (top) { all.ifTopUrl = { u }; Emit(out, priority, 6, isRegex, all, ignore); all.ifTopUrl.clear(); }
            if (child) { all.ifFrameUrl = { u }; Emit(out, priority, 6, isRegex, all, ignore); }
        }
        return true;
    } else {
        return false;
    }

    for (auto& u : urls)
        for (auto& m : methods) {
            Trigger t = base;
            t.url = u;
            t.method = m;
            Emit(out, priority, rank, isRegex, t, act);
        }
    // excludedRequestDomains: those requests are let through right after (not for allow rules)
    if (type != "allow")
        for (auto& d : Domains(cond, "excludedRequestDomains", nullptr)) {
            std::string u;
            if (!withDomain(d, u)) continue;
            Trigger t = base;
            t.url = u;
            Emit(out, priority, rank + 1, isRegex, t, "{\"type\":\"ignore-previous-rules\"}");
        }
    return true;
}

}  // namespace

bool DnrRegexSupported(const std::string& re, std::string* reason) {
    auto fail = [&](const char* why) { if (reason) *reason = why; return false; };
    if (re.empty()) return fail("empty");
    const size_t n = re.size();
    int depth = 0;
    bool inClass = false, atomBefore = false;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = (unsigned char)re[i];
        if (c >= 0x80 || c < 0x20) return fail("not ASCII");
        if (inClass) {
            if (c == '\\') {
                if (i + 1 >= n || isalnum((unsigned char)re[i + 1])) return fail("an escape in a character class");
                ++i;
            } else if (c == ']') {
                inClass = false;
                atomBefore = true;
            }
            continue;
        }
        switch (c) {
        case '\\':
            if (i + 1 >= n || isalnum((unsigned char)re[i + 1])) return fail("\\d \\w \\b or a back reference");
            ++i;
            atomBefore = true;
            break;
        case '[':
            inClass = true;
            if (i + 1 < n && re[i + 1] == '^') ++i;
            if (i + 1 < n && re[i + 1] == ']') ++i;       // "[]...]": a literal ]
            break;
        case '(':
            if (i + 1 < n && re[i + 1] == '?') {
                if (i + 2 < n && re[i + 2] == ':') i += 2;
                else return fail("a look-around");
            }
            ++depth;
            atomBefore = false;
            break;
        case ')':
            if (--depth < 0) return fail("unbalanced )");
            atomBefore = true;
            break;
        case '|': return fail("alternation");
        case '{': case '}': return fail("counted repetition");
        case '^':
            if (i != 0) return fail("^ not at the start");
            break;
        case '$':
            if (i != n - 1) return fail("$ not at the end");
            break;
        case '*': case '+': case '?':
            if (!atomBefore) return fail("nothing to repeat");
            if (i + 1 < n && re[i + 1] == '?') ++i;       // lazy: the same for matching
            atomBefore = false;
            break;
        default:
            atomBefore = true;
        }
    }
    if (inClass || depth) return fail("unbalanced");
    return true;
}

DnrResult DnrConvert(const std::vector<DnrSource>& sources, const std::wstring& extDir) {
    DnrResult res;
    const ULONGLONG started = GetTickCount64();
    std::map<std::string, std::string> fileUrls;
    s_fileUrls = &fileUrls;
    std::vector<Out> out;
    for (const DnrSource& s : sources) {
        const std::string text = s.file.empty() ? s.text : ReadAll(s.file, 64u << 20);
        JsonValue rules;
        if (text.empty() || !JsonParse(text, rules) || !rules.isArray()) continue;
        for (const JsonValue& r : rules.items) {
            if (!r.isObject()) continue;
            const long long id = r.get("id") ? (long long)r.get("id")->n : 0;
            if (s.disabled.count(id)) continue;
            ++res.rules;
            if (ConvertRule(r, extDir, out)) ++res.converted;
            else ++res.skipped;
        }
    }
    s_fileUrls = nullptr;
    res.webkitRules = out.size();
    std::stable_sort(out.begin(), out.end(), [](const Out& a, const Out& b) {
        return a.priority != b.priority ? a.priority < b.priority : a.rank < b.rank;
    });
    if (out.empty()) { res.ms = GetTickCount64() - started; return res; }
    bool anyRegex = false;
    size_t total = 0;
    for (const Out& o : out) total += o.json.size() + 1;
    res.json.reserve(total + 2);
    res.fallback.reserve(total + 2);
    res.json = "[";
    res.fallback = "[";
    for (const Out& o : out) {
        if (res.json.size() > 1) res.json += ",";
        res.json += o.json;
        if (o.regex) { anyRegex = true; continue; }
        if (res.fallback.size() > 1) res.fallback += ",";
        res.fallback += o.json;
    }
    res.json += "]";
    res.fallback += "]";
    if (!anyRegex || res.fallback == "[]") res.fallback.clear();
    res.hash = 1469598103934665603ULL;
    for (unsigned char c : res.json) res.hash = (res.hash ^ c) * 1099511628211ULL;
    res.ms = GetTickCount64() - started;
    return res;
}
