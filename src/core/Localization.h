// Lyra — язык интерфейса: русский или английский.
#pragma once

#include "common/Platform.h"

// Settings > General > Language: 0 = as in Windows, 1 = English, 2 = Russian.
bool UiRussian();

// The string for the current interface language.
inline const wchar_t* Tr(const wchar_t* en, const wchar_t* ru) { return UiRussian() ? ru : en; }
