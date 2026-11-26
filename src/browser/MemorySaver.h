// Lyra — сжатие памяти простаивающих процессов WebKit.
#pragma once

#include "common/Platform.h"

// Settings > Performance, on by default. The WebKit processes that show nothing on screen and have
// been idle for half a minute (background tabs, the prepared new tab, extensions' background
// pages, the sidebar's pages while it is closed; the network process; with the window minimised
// also the open tab and the GPU process) give their memory back to Windows, which compresses it
// (memory compression) and gives it back in a moment when the process works again. Nothing is
// unloaded: a page stays as it was.
void MemorySaverTick();              // main timer, every few seconds
