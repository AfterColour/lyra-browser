// Lyra — небольшой разбор JSON (см. Json.h).
#include "core/Json.h"
#include <cstdlib>
#include <cstring>

namespace {

struct Parser {
    const std::string& t;
    size_t p = 0;
    std::string error;

    explicit Parser(const std::string& text) : t(text) {}

    void skip() {
        for (;;) {
            while (p < t.size() && (t[p] == ' ' || t[p] == '\t' || t[p] == '\r' || t[p] == '\n')) ++p;
            if (p + 1 < t.size() && t[p] == '/' && t[p + 1] == '/') {
                while (p < t.size() && t[p] != '\n') ++p;
                continue;
            }
            if (p + 1 < t.size() && t[p] == '/' && t[p + 1] == '*') {
                const size_t e = t.find("*/", p + 2);
                p = e == std::string::npos ? t.size() : e + 2;
                continue;
            }
            return;
        }
    }

    bool fail(const char* what) {
        if (error.empty()) error = std::string(what) + " at " + std::to_string(p);
        return false;
    }

    static void putUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 63)); out += (char)(0x80 | (cp & 63)); }
        else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 63)); out += (char)(0x80 | ((cp >> 6) & 63)); out += (char)(0x80 | (cp & 63)); }
    }

    bool string(std::string& out) {
        if (t[p] != '"') return fail("string expected");
        ++p;
        while (p < t.size() && t[p] != '"') {
            char c = t[p++];
            if (c != '\\') { out += c; continue; }
            if (p >= t.size()) break;
            c = t[p++];
            switch (c) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                if (p + 4 > t.size()) return fail("bad escape");
                unsigned cp = (unsigned)strtoul(t.substr(p, 4).c_str(), nullptr, 16);
                p += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && p + 6 <= t.size() && t[p] == '\\' && t[p + 1] == 'u') {
                    const unsigned lo = (unsigned)strtoul(t.substr(p + 2, 4).c_str(), nullptr, 16);
                    if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
                }
                putUtf8(out, cp);
                break;
            }
            default: out += c; break;
            }
        }
        if (p >= t.size()) return fail("unterminated string");
        ++p;
        return true;
    }

    bool value(JsonValue& v, int depth) {
        if (depth > 200) return fail("too deep");
        skip();
        if (p >= t.size()) return fail("value expected");
        const size_t start = p;
        const char c = t[p];
        if (c == '{') {
            v.type = JsonValue::Object;
            ++p;
            skip();
            if (p < t.size() && t[p] == '}') { ++p; v.raw = t.substr(start, p - start); return true; }
            for (;;) {
                skip();
                std::string key;
                if (p >= t.size() || !string(key)) return fail("member name expected");
                skip();
                if (p >= t.size() || t[p] != ':') return fail("':' expected");
                ++p;
                JsonValue item;
                if (!value(item, depth + 1)) return false;
                v.members.emplace_back(std::move(key), std::move(item));
                skip();
                if (p < t.size() && t[p] == ',') {
                    ++p;
                    skip();
                    if (p < t.size() && t[p] == '}') { ++p; break; }      // a trailing comma
                    continue;
                }
                if (p < t.size() && t[p] == '}') { ++p; break; }
                return fail("',' or '}' expected");
            }
        } else if (c == '[') {
            v.type = JsonValue::Array;
            ++p;
            skip();
            if (p < t.size() && t[p] == ']') { ++p; v.raw = t.substr(start, p - start); return true; }
            for (;;) {
                JsonValue item;
                if (!value(item, depth + 1)) return false;
                v.items.push_back(std::move(item));
                skip();
                if (p < t.size() && t[p] == ',') {
                    ++p;
                    skip();
                    if (p < t.size() && t[p] == ']') { ++p; break; }
                    continue;
                }
                if (p < t.size() && t[p] == ']') { ++p; break; }
                return fail("',' or ']' expected");
            }
        } else if (c == '"') {
            v.type = JsonValue::String;
            if (!string(v.s)) return false;
        } else if (t.compare(p, 4, "true") == 0) { v.type = JsonValue::Bool; v.b = true; p += 4; }
        else if (t.compare(p, 5, "false") == 0) { v.type = JsonValue::Bool; p += 5; }
        else if (t.compare(p, 4, "null") == 0) { v.type = JsonValue::Null; p += 4; }
        else if (c == '-' || (c >= '0' && c <= '9')) {
            char* end = nullptr;
            v.type = JsonValue::Number;
            v.n = strtod(t.c_str() + p, &end);
            if (!end || end == t.c_str() + p) return fail("bad number");
            p = (size_t)(end - t.c_str());
        } else return fail("unexpected character");
        v.raw = t.substr(start, p - start);
        return true;
    }
};

}  // namespace

const JsonValue* JsonValue::get(const std::string& key) const {
    if (type != Object) return nullptr;
    for (auto& m : members) if (m.first == key) return &m.second;
    return nullptr;
}

std::string JsonValue::str(const std::string& key, const std::string& fallback) const {
    const JsonValue* v = get(key);
    return v && v->type == String ? v->s : fallback;
}

bool JsonValue::flag(const std::string& key, bool fallback) const {
    const JsonValue* v = get(key);
    return v && v->type == Bool ? v->b : fallback;
}

bool JsonParse(const std::string& text, JsonValue& out, std::string* error) {
    std::string body = text;
    if (body.size() >= 3 && (unsigned char)body[0] == 0xEF && (unsigned char)body[1] == 0xBB && (unsigned char)body[2] == 0xBF) body.erase(0, 3);
    Parser parser(body);
    out = JsonValue();
    bool ok = parser.value(out, 0);
    if (ok) {
        parser.skip();
        if (parser.p != body.size()) ok = parser.fail("text after the value");
    }
    if (!ok && error) *error = parser.error;
    return ok;
}

std::string JsonQuote(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
            else out += (char)c;
        }
    }
    // U+2028 / U+2029 end a line in older JavaScript: escape them, the text goes into scripts too.
    for (size_t i; (i = out.find("\xE2\x80\xA8")) != std::string::npos;) out.replace(i, 3, "\\u2028");
    for (size_t i; (i = out.find("\xE2\x80\xA9")) != std::string::npos;) out.replace(i, 3, "\\u2029");
    return out + "\"";
}
