// Lyra — закладки: «Избранное» (стартовая страница), папка «Закладки», импорт из других браузеров.
#pragma once

#include "common/Platform.h"

// A bookmark or a folder. Two folders always exist and cannot be removed: Favorites (shown on the
// start page) and Bookmarks (everything else). Order inside a folder is the order of the list.
// Stored in %LOCALAPPDATA%\Lyra\bookmarks.tsv.
struct Bookmark {
    long long id = 0;
    long long parent = 0;           // 0 for the two root folders
    bool folder = false;
    std::wstring title, url;
    long long added = 0;            // Unix time, milliseconds
};

constexpr long long kFavoritesFolder = 1;
constexpr long long kBookmarksFolder = 2;

void BookmarksFlush();              // at exit: the file write still waiting
void BookmarksLoad();               // also fills Favorites with a few sites on the very first run

const Bookmark* BookmarkGet(long long id);
// First bookmark of this address (also matches "http://x/" for "http://x"), or 0.
long long BookmarkFind(const std::wstring& url);
std::vector<Bookmark> BookmarkChildren(long long folder);
std::vector<Bookmark> BookmarksAll();         // folders first in tree order, then the rest
std::wstring BookmarkFolderName(long long id);

// All of these save at once and update the star, the start page and the bookmarks page.
// `index` -1 = at the end of the folder.
long long BookmarkAdd(long long folder, const std::wstring& title, const std::wstring& url, int index = -1);
long long BookmarkAddFolder(long long parent, const std::wstring& title);
void BookmarkRemove(long long id);           // a folder with everything inside
void BookmarkRename(long long id, const std::wstring& title);
void BookmarkSetUrl(long long id, const std::wstring& url);
// Into `folder` before the item now at `index` (-1 = at the end). A folder never moves into itself.
void BookmarkMove(long long id, long long folder, int index);
// Replaces the Favorites folder with these (the start page used to keep its own list).
void BookmarkReplaceFavorites(const std::vector<std::pair<std::wstring, std::wstring>>& titleUrl);

// Address bar suggestions: bookmarks whose title or address contain all words of `text`.
std::vector<Bookmark> BookmarksMatch(const std::wstring& text, size_t max);

// Other browsers on this computer whose bookmarks can be imported: {key, display name}.
std::vector<std::pair<std::wstring, std::wstring>> BookmarkImporters();
// The usual bookmarks HTML file (Netscape format, as every browser reads and writes it).
// Export: the number of bookmarks written, -1 on failure. Import: into a new folder under Bookmarks,
// leaving out addresses already bookmarked; the number added, -1 when the file is not one.
int BookmarkExportHtml(const std::wstring& path);
int BookmarkImportHtml(const std::wstring& path);
// Imports from the browser `key` (see BookmarkImporters) into a folder under Bookmarks;
// addresses already bookmarked are skipped. Returns how many were added, -1 if unreadable.
int BookmarkImport(const std::wstring& key);

// Called after every change (set by StartPage.cpp): refreshes pages and the star.
extern void (*g_bookmarksChanged)();
// Goes up by one with every change.
extern long long g_bookmarksVersion;
