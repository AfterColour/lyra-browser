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
