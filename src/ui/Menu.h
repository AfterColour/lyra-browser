// Lyra — свои всплывающие меню (вместо меню Windows): в цветах темы, с иконками и плавной подсветкой.
#pragma once

#include "common/Platform.h"

// Lyra's popup menus are built like Windows menus (CreatePopupMenu + AppendMenuW: "Text\tCtrl+X",
// MF_GRAYED, MF_CHECKED, MF_SEPARATOR, MF_POPUP for a submenu) and shown with MenuTrack instead of
// TrackPopupMenu: drawn like the rest of Lyra, with the theme's colours, rounded corners, icons and
// a highlight that glides from item to item.

// An icon (a glyph of the icon font, e.g. L"") for the item with this command id. The
// string must stay alive while the menu is shown (a literal).
void MenuSetIcon(HMENU menu, UINT id, const wchar_t* glyph);

// Shows the menu at (x, y) in screen coordinates and returns the chosen command id, 0 when it is
// dismissed (like TrackPopupMenu with TPM_RETURNCMD). flags: TPM_LEFTALIGN / TPM_CENTERALIGN /
// TPM_RIGHTALIGN and TPM_TOPALIGN / TPM_BOTTOMALIGN; the menu flips to stay on the screen.
// chosenMenu / chosenPos: which (sub)menu and position the chosen item has.
int MenuTrack(HMENU menu, UINT flags, int x, int y, HWND owner, HMENU* chosenMenu = nullptr, int* chosenPos = nullptr);

bool MenuActive();
void MenuCancel();
