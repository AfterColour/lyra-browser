// Lyra — язык интерфейса: русский или английский.
#include "core/Localization.h"
#include "core/Settings.h"

bool UiRussian() {
    if (g_prefs.language == 1) return false;
    if (g_prefs.language == 2) return true;
    static const bool systemRussian = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
    return systemRussian;
}
