// Lyra — палитра команд (Ctrl+K): одно поле для вкладок, команд, закладок и истории.
#pragma once

#include "common/Platform.h"

// A box over the top of the window: open tabs, Lyra's commands, bookmarks and visited pages that
// match what is typed (in either language), and a search / "go to" row. Up / Down, Enter, Esc
// and the mouse; it closes when Lyra loses the focus.
void CommandPaletteShow();
bool CommandPaletteVisible();
