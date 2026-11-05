// Lyra — последний рубеж: запись исключения и стека в журнал при падении.
#pragma once

#include "common/Platform.h"

// Records exception code, fault address and a stack trace (module + offset, resolve
// with the matching .pdb) into the log, then lets Windows handle the crash as usual.
LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep);
