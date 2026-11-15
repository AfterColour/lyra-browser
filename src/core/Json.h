// Lyra — небольшой разбор JSON (манифесты расширений, их хранилище).
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

// A JSON value. Objects keep their members in order. `raw` is the value's own text as it was in
// the source, so a value can be passed on (to a page, to a file) without writing it out again.
// The parser also accepts what Chrome accepts in manifest.json: comments and a byte order mark.
struct JsonValue {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double n = 0;
    std::string s;                                               // String (UTF-8)
    std::vector<JsonValue> items;                                // Array
    std::vector<std::pair<std::string, JsonValue>> members;      // Object
    std::string raw;

    const JsonValue* get(const std::string& key) const;          // Object member or null
    std::string str(const std::string& key, const std::string& fallback = "") const;
    bool flag(const std::string& key, bool fallback = false) const;
    bool isString() const { return type == String; }
    bool isArray() const { return type == Array; }
    bool isObject() const { return type == Object; }
};

bool JsonParse(const std::string& text, JsonValue& out, std::string* error = nullptr);
std::string JsonQuote(const std::string& utf8);                  // "..." with escapes
