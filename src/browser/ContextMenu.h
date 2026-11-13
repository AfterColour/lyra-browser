// Lyra — контекстное меню страницы: по-русски, в порядке как в других браузерах, с пунктами Lyra.
#pragma once

#include "common/Platform.h"

struct TabCtx;

// WebKit proposes the menu for what is under the pointer; Lyra rewrites it: the engine's own items
// (copy, paste, save image, inspect element...) with Lyra's titles and in Lyra's order, and Lyra's
// items (open in a new or private tab, search for the selection, translate, reader view, print,
// picture-in-picture, undo / redo). The menu itself is WebKit's (a Windows popup menu, in Lyra's
// dark or light theme).
void ContextMenuSetClient(WKPageRef page, TabCtx* ctx);
void ContextMenuClearClient(WKPageRef page);
void ContextMenuCommand(WPARAM tag);           // WM_APP_CONTEXT_MENU: a Lyra item was chosen
