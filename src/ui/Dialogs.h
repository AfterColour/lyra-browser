// Lyra — модальные диалоги (ввод текста для prompt()).
#pragma once

#include "common/Platform.h"

extern int g_modalDepth;

struct ModalScope {
    ModalScope()  { ++g_modalDepth; }
    ~ModalScope() { --g_modalDepth; }
};

// Minimal modal text-input dialog built from an in-memory template (no .rc needed).
struct PromptState { std::wstring message, value; bool ok = false; };

bool PromptDialog(HWND owner, const std::wstring& title, PromptState& st);
