// Lyra — «картинка в картинке»: видео в маленьком окне поверх всех окон.
#pragma once

#include "common/Platform.h"

struct Tab;

// The tab's own view moves into a small always-on-top window and the page shows only the video
// (a script hides the rest and stretches the video over the page). A button appears over videos
// while the pointer is on them; sites' own picture-in-picture buttons work too, and the "More"
// menu has the command. "Back to tab" returns the view; × pauses the video and returns it.
void PipInit();                      // after AdBlockInit (the shared user content controller)
void PipStart(Tab& t);               // the playing (or largest) video of the tab
void PipToggleActive();              // the "More" menu: start for the active tab, or end
void PipExit(bool pause, bool showTab);
void PipDetach(Tab& t);              // the tab is being destroyed: give the view back first
bool PipActive();
void PipMessage(WPARAM tabId, LPARAM what);   // WM_APP_PIP (posted by the page's requests)
