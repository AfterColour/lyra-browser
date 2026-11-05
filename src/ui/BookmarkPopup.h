// Lyra — звёздочка в адресной строке: окошко «Добавлено в закладки» (название, папка, удалить).
#pragma once

#include "common/Platform.h"

// The star (or Ctrl+D): bookmarks the current page if it is not bookmarked yet (into the folder
// used last, Favorites at first) and shows the bubble under the star to rename it, pick another
// folder or remove it. Does nothing for pages that cannot be bookmarked.
void BookmarkCurrentPage();
bool BookmarkPopupVisible();
void BookmarkPopupClose();          // keeps the bookmark, applies the edited name

// The star is shown for ordinary web pages and files, not for Lyra's own pages.
bool CanBookmark(const std::wstring& url);
// Whether `url` is bookmarked (cached; cheap enough to call while painting).
bool IsBookmarkedCached(const std::wstring& url);
