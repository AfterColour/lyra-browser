// Lyra — последний рубеж: запись исключения и стека в журнал при падении.
#include "app/CrashHandler.h"
#include "app/Log.h"
#include "browser/TabManager.h"

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* er = ep ? ep->ExceptionRecord : nullptr;
    if (er) {
        Log("!!! UNHANDLED EXCEPTION code=0x%08lX flags=0x%lX", er->ExceptionCode, er->ExceptionFlags);
        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
            Log("!!! access violation: %s address %p",
                er->ExceptionInformation[0] == 0 ? "reading" : er->ExceptionInformation[0] == 1 ? "writing" : "executing",
                (void*)er->ExceptionInformation[1]);
        LogAddress("!!! faulting instruction", er->ExceptionAddress);
    }
    Log("!!! state: tabs=%d active=%d tabOp=%d closing=%d", (int)g_tabs.size(), g_active, (int)g_tabOp, (int)g_closing);
    void* frames[48] = {};
    USHORT n = CaptureStackBackTrace(0, 48, frames, nullptr);
    for (USHORT k = 0; k < n; ++k) {
        char label[16];
        _snprintf_s(label, sizeof(label), _TRUNCATE, "!!! #%u", (unsigned)k);
        LogAddress(label, frames[k]);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
