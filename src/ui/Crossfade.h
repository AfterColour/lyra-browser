// Lyra - smooth changes of the whole window: the old picture dissolves over the new one.
#pragma once

#include "common/Platform.h"

// Call right BEFORE a change that redraws the whole main window at once (toolbar size, tab
// layout, theme, accent). Takes a picture of the window as it is now, lays it over the window,
// and fades it out once the new look is drawn underneath, so the change dissolves instead of
// jumping. Does nothing while animations are off or the window is hidden or minimised.
void CrossfadeBegin();
