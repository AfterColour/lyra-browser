// Lyra — адрес ссылки под курсором: плашка в нижнем левом углу страницы.
#pragma once

#include "common/Platform.h"

struct Tab;

// The page reports what is under the pointer (WebKit's mouseDidMoveOverElement): `url` is the
// link's address, empty when the pointer is not over a link. Only the tabs on screen show it.
void LinkStatusUpdate(const Tab& t, const std::wstring& url);
void LinkStatusHide();
