// Lyra — восстановление сессии: вкладки прошлого запуска (и после сбоя).
#pragma once

#include "common/Platform.h"

struct Tab;

// The open tabs (address, title, back/forward list) are saved to %LOCALAPPDATA%\Lyra\session.txt
// whenever they change (checked about once a second) and on exit. At startup they come back:
// always after a crash, after a normal exit when Settings > General > "Restore tabs" is on.
// Restored tabs load only when they are first shown.
bool SessionRestore();               // creates the saved tabs; false = nothing to restore
void SessionTick();                  // main timer: saves when something changed
void SessionSave(bool clean);        // clean = Lyra is closing normally
void SessionLoadLazyTab(Tab& t);     // SelectTab: a restored tab is shown for the first time
std::string SessionPageState(WKPageRef page);   // the back/forward list, raw (as Tab::lazyState holds it)
