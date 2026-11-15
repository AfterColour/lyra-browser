// Lyra — сторож микрофризов: замечает, когда окно браузера перестаёт отвечать, и пишет в журнал, где оно стояло.
//
// A thread of its own posts a message to a hidden window on the main thread every 50 ms and
// times the answer. When the main thread is busy for 100 ms or more (a stutter you can see),
// tabs.log gets a line with how long it took and where the main thread was: the exact
// instruction plus the return addresses found on its stack, as module+offset (Lyra.exe+0x..,
// WebKit2.dll+0x..). tools/symbolize-stalls.py turns them into function names.
//
// While the main thread is stopped for the snapshot nothing is called that could take a lock
// it might hold (only GetThreadContext and a copy of the top of its stack); everything else
// happens after it runs again.
#pragma once

#include "common/Platform.h"

void StallWatchStart();      // on the main thread, after the main window exists
void StallWatchStop();
