// Lyra — усыпление вкладок: давно не открывавшиеся вкладки выгружаются из памяти.
#pragma once

#include "common/Platform.h"

struct Tab;

// A tab not shown for a while (Settings > Tabs, 30 minutes by default; a couple of minutes when
// Windows is short of memory) is put to sleep: its page and web process are unloaded, and it
// loads again, with its back/forward list, when it is shown. Never a tab that plays sound, is in
// picture-in-picture or full screen, is loading, is private or a Lyra page, or where text was
// typed into the page (it would be lost).
void TabSleepInit();                 // after AdBlockInit (the shared user content controller)
void TabSleepTick();                 // main timer, about once a second
// Why the tab cannot sleep now (empty: it can), for the tab menu.
std::wstring TabSleepBlocker(const Tab& t);
void SleepTabNow(Tab& t);            // the tab menu: asks first if text was typed into the page
void SleepOtherTabs();               // the tab menu: every background tab that can sleep
