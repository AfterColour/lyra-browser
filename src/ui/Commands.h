// Lyra — команды браузера, горячие клавиши и меню «Ещё».
#pragma once

#include "common/Platform.h"

void SetZoom(double z);
void GoBack();
void GoForward();
void ReloadActive(bool stopIfLoading);
void RunCommand(int cmd);
void ShowMoreMenu();
bool HandleShortcut(const MSG& m);
