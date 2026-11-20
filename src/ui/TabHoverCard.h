// Lyra — карточка вкладки при наведении: название, адрес и картинка страницы.
#pragma once

#include "common/Platform.h"

// The pointer rests on a tab for a moment: a card under it (beside it in the vertical layout)
// shows the whole title, the site and, for a loaded tab in the background, a small picture of
// the page. Once a card is up, moving to the next tab swaps it at once.
void TabHoverCardHover(int tabIndex);   // SetHover: the tab under the pointer, -1 for none
void TabHoverCardHide();                // a click, a drag, the window going to the back
void TabHoverCardForget(unsigned long long tabId);   // the tab closed: its picture goes
