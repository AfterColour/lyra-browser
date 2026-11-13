// Lyra — перетаскивание вкладки: вкладка «поднимается» и летает за мышью.
#pragma once

#include "common/Platform.h"

// A dragged tab is lifted out of the tab list: a copy of it (slightly larger, with a shadow)
// follows the mouse anywhere on the screen, and its place in the list shows an empty slot.
// Dropped on a target it shrinks into it; dropped elsewhere it flies back to its place.
// Positions are client coordinates of the main window.
void TabDragStart(unsigned long long tabId, POINT grab, const RectF& tabRect);
void TabDragMoveTo(POINT pt);
void TabDragFinish(const RectF* into);              // into: where it lands, nullptr: back to its place
unsigned long long TabDragGhostId();                // the lifted tab (0: none)
bool TabDragAnimStep(float dt);                      // frame clock (ui/Animation.cpp)
// After the lifted tab has landed, its tab gives a short pulse: 0..1 while it runs, -1 otherwise.
float TabLandPulse(unsigned long long tabId);
