// Lyra — полноэкранный режим для видео и элементов страницы.
#pragma once

#include "common/Platform.h"

void RestoreFromFullScreen();
LRESULT OnFullScreenMessage(bool enter, HWND view);
void ExitFullScreenForTabSwitch();
