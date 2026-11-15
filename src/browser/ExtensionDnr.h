// Lyra - an extension's declarativeNetRequest rules as WebKit content blocker rules.
// WebKit's content blockers do the matching in the web process, as they do for Lyra's own ad
// blocking (browser/AdBlock.cpp): block, allow, allowAllRequests, upgradeScheme, redirect (to
// a URL, a transform, a regex substitution, or a file of the extension, which goes in as a data:
// URL) and modifyHeaders all have a counterpart there. The order of the rules gives DNR's
// priorities: later rules win (ignore-previous-rules), so rules go out by priority, and within
// one priority redirect < upgradeScheme < block < allowAllRequests < allow.
// Not converted (counted as skipped): conditions on tab ids or response headers, and regular
// expressions outside WebKit's subset (no |, no {n,m}, no \d \w \b, no look-arounds).
#pragma once

#include <set>
#include <string>
#include <vector>

struct DnrSource {
    std::wstring file;              // a static ruleset (read by DnrConvert) ...
    std::string text;               // ... or rules as JSON text (dynamic, session)
    std::set<long long> disabled;   // rule ids turned off (updateStaticRules)
};

struct DnrResult {
    std::string json;               // WebKit's JSON; empty: nothing to block
    std::string fallback;           // the same without regexFilter rules
    int rules = 0, converted = 0, skipped = 0;
    size_t webkitRules = 0;
    unsigned long long hash = 0;    // of json (FNV-1a), for the compiled list's name
    unsigned long long ms = 0;      // how long the conversion took
};

DnrResult DnrConvert(const std::vector<DnrSource>& sources, const std::wstring& extDir);
bool DnrRegexSupported(const std::string& regex, std::string* reason = nullptr);
