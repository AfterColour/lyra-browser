// Lyra — поле ввода адреса.
#pragma once

#include "common/Platform.h"

extern HWND g_urlEdit;
extern WNDPROC g_oldEditProc;
extern HFONT g_editFont;
extern HBRUSH g_editBrush;
extern int g_editTextH;

bool IsEditing();
void PositionEdit();
void ShowUrlEdit();
LRESULT CALLBACK UrlEditProc(HWND h, UINT msg, WPARAM w, LPARAM l);
void PasteAndGo();   // the clipboard's text into the field, and go (a search when it is not an address)
