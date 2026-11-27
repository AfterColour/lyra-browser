// Lyra — настройка интерфейса прямо в окне: перетаскивание кнопок, панель свойств (ui/UiLayout).
#pragma once

#include "common/Platform.h"

bool CustomizeActive();
void CustomizeBegin();
void CustomizeEnd();
// The main window's mouse while customizing (WM_LBUTTONDOWN / WM_MOUSEMOVE / WM_LBUTTONUP /
// WM_CAPTURECHANGED); true: handled, nothing else should act on it.
bool CustomizeMouse(UINT msg, int x, int y);
bool CustomizeSetCursor(int x, int y);       // WM_SETCURSOR over the chrome
void CustomizePaint(Graphics& g);            // over the chrome, last
void CustomizeReposition();
void CustomizeModelChanged();            // the layouts changed elsewhere (Settings): resend the panel's model                  // the main window moved or resized: the panel follows
