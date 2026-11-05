// Lyra — разбор адресов: что ввёл пользователь -> URL или поиск, отображение адреса.
#pragma once

#include "common/Platform.h"

// The built-in start page: <Lyra.exe folder>\resources\start.html (also the first-run welcome
// screen). "lyra://start" typed or set as home page means this page.
std::wstring StartPagePath();
std::wstring StartPageFileUrl();                    // file:///.../start.html, without parameters
bool IsStartPageUrl(const std::wstring& url);       // any address of the start page (with parameters)
constexpr const wchar_t* kStartAlias = L"lyra://start";
// The history page is generated into %LOCALAPPDATA%\Lyra\history.html ("lyra://history").
std::wstring HistoryPagePath();
bool IsHistoryPageUrl(const std::wstring& url);
constexpr const wchar_t* kHistoryAlias = L"lyra://history";
// The bookmarks page, generated the same way into %LOCALAPPDATA%\Lyra\bookmarks.html.
std::wstring BookmarksPagePath();
bool IsBookmarksPageUrl(const std::wstring& url);
constexpr const wchar_t* kBookmarksAlias = L"lyra://bookmarks";
// The downloads page, %LOCALAPPDATA%\Lyra\downloads.html.
std::wstring DownloadsPagePath();
bool IsDownloadsPageUrl(const std::wstring& url);
constexpr const wchar_t* kDownloadsAlias = L"lyra://downloads";
// Lyra's own pages: shown with an empty address field, never bookmarked or recorded.
inline bool IsInternalPageUrl(const std::wstring& url) {
    return IsStartPageUrl(url) || IsHistoryPageUrl(url) || IsBookmarksPageUrl(url) || IsDownloadsPageUrl(url);
}

// Host / address shown to the user ("" for blank pages and the start page)
std::wstring HostOf(const std::wstring& url);
std::wstring DisplayUrl(const std::wstring& url);
std::string ResolveInput(const std::wstring& input);
