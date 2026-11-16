// Lyra — преобразование строк (UTF-8/UTF-16, WKString) и экранирование HTML/JS.
#include "core/StringUtils.h"


std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::wstring WKStr(WKStringRef s) {
    if (!s) return std::wstring();
    const size_t len = WKStringGetLength(s);
    std::wstring w(len, L'\0');
    if (len) w.resize(WKStringGetCharacters(s, reinterpret_cast<WKChar*>(&w[0]), len));
    return w;
}

std::string WKStrUtf8(WKStringRef s) {
    if (!s) return std::string();
    std::string out(WKStringGetMaximumUTF8CStringSize(s), '\0');
    const size_t n = WKStringGetUTF8CString(s, &out[0], out.size());   // counts the closing 0
    out.resize(n ? n - 1 : 0);
    return out;
}

std::wstring TrimWs(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string UrlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else if (c == ' ') o += '+';
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

std::wstring UrlDecode(const std::wstring& s) {
    std::string bytes;
    for (size_t i = 0; i < s.size(); ++i) {
        const wchar_t c = s[i];
        if (c == L'+') { bytes += ' '; continue; }
        if (c == L'%' && i + 2 < s.size() && iswxdigit(s[i + 1]) && iswxdigit(s[i + 2])) {
            bytes += (char)wcstol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
            continue;
        }
        if (c < 0x80) bytes += (char)c;
        else bytes += ToUtf8(std::wstring(1, c));
    }
    return ToWide(bytes);
}

std::wstring HtmlEscape(const std::wstring& s) {
    std::wstring o;
    o.reserve(s.size());
    for (wchar_t c : s) {
        switch (c) {
        case L'&': o += L"&amp;"; break;
        case L'<': o += L"&lt;"; break;
        case L'>': o += L"&gt;"; break;
        case L'"': o += L"&quot;"; break;
        case L'\'': o += L"&#39;"; break;
        default: o += c;
        }
    }
    return o;
}

// A JavaScript string literal that is also safe inside an HTML <script>/attribute.
std::wstring JsString(const std::wstring& s) {
    std::wstring o = L"\"";
    for (wchar_t c : s) {
        if (c == L'\\' || c == L'"' || c == L'\'' || c == L'<' || c == L'>' || c == L'&' ||
            c < 0x20 || c == 0x2028 || c == 0x2029) {
            wchar_t buf[8];
            swprintf(buf, 8, L"\\u%04X", (unsigned)c);
            o += buf;
        } else {
            o += c;
        }
    }
    return o + L"\"";
}
