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

// The Windows Open / Save As dialogs for one kind of file ("" when cancelled).
// spec: "*.html;*.htm"; defaultExt: "html" (Save As adds it).
std::wstring PickOpenFile(HWND owner, const wchar_t* title, const wchar_t* typeName, const wchar_t* spec);
std::wstring PickSaveFile(HWND owner, const wchar_t* title, const std::wstring& fileName,
                          const wchar_t* typeName, const wchar_t* spec, const wchar_t* defaultExt);
