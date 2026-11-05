// Lyra — разбор адресов: что ввёл пользователь -> URL или поиск, отображение адреса.
#pragma once

#include "common/Platform.h"

// The built-in start page: <Lyra.exe folder>\resources\start.html (also the first-run welcome
// screen). "lyra://start" typed or set as home page means this page.
std::wstring StartPagePath();
std::wstring StartPageFileUrl();                    // file:///.../start.html, without parameters
bool IsStartPageUrl(const std::wstring& url);       // any address of the start page (with parameters)
constexpr const wchar_t* kStartAlias = L"lyra://start";

// Host / address shown to the user ("" for blank pages and the start page)
std::wstring HostOf(const std::wstring& url);
std::wstring DisplayUrl(const std::wstring& url);
std::string ResolveInput(const std::wstring& input);
