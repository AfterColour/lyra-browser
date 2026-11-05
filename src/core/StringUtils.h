// Lyra — преобразование строк (UTF-8/UTF-16, WKString) и экранирование HTML/JS.
#pragma once

#include "common/Platform.h"

std::string ToUtf8(const std::wstring& w);
std::wstring WKStr(WKStringRef s);
std::wstring TrimWs(const std::wstring& s);
std::string UrlEncode(const std::string& s);
std::wstring HtmlEscape(const std::wstring& s);
std::wstring JsString(const std::wstring& s);
