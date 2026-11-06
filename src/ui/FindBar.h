// Lyra — поиск на странице (Ctrl+F): панель с полем, счётчиком совпадений и стрелками.
#pragma once

#include "common/Platform.h"

// A small panel at the top right of the page. Typing searches as you type; every match is
// highlighted and the current one is scrolled into view. Enter / F3 go to the next match,
// Shift+Enter / Shift+F3 to the previous one, Esc closes the panel and removes the highlights.
void FindBarShow();                  // Ctrl+F: opens the panel, or selects its text if it is open
void FindBarClose();
bool FindBarVisible();
void FindBarNext(bool backwards);    // F3 / Shift+F3 (opens the panel if there is nothing to find)
void FindBarReposition();            // the main window moved or changed size
void FindBarTabChanged();            // another tab became active, or the searched tab closed
void FindBarPageLoaded(unsigned long long tabId);   // a page finished loading: search it again

// Find client callbacks (PageClients.cpp).
void FindBarDidFind(unsigned long long tabId, unsigned matchCount);
void FindBarDidFail(unsigned long long tabId);
