// Lyra - touchpad gestures: two-finger scrolling, swiping back/forward, pinch zoom.
#pragma once

#include "common/Platform.h"

// A precision touchpad's input in a page window goes through DirectManipulation (as in Chrome and
// Edge): Windows hands over the fingers' movement with the touchpad's own inertia, and Lyra turns
// it into exact scrolling, the swipe between pages (ui/SwipeNav) or pinch zoom.

// Right after WKViewCreate: the page window `view` takes touchpad gestures (forgotten by itself
// when the window is destroyed).
void TouchpadAttach(HWND view);
// The frame clock (ui/Animation): DirectManipulation is driven once per frame during a gesture.
// True while it still needs frames.
bool TouchpadAnimStep(float dt);
// A wheel event the page did not use (WKPageUIClient didNotHandleWheelEvent): a sideways
// gesture over a page that cannot scroll that way becomes the swipe back or forward.
void TouchpadWheelNotHandled(WKPageRef page);
void TouchpadShutdown();             // main window: WM_DESTROY

// Developer test: LYRA_TEST_TOUCHPAD="<delay ms>:<kind>:<amount>;..." plays gestures in the
// active tab as if fingers were on the touchpad (kinds: back, fwd (sideways px), scroll
// (vertical px), pinch (scale)). Started by the first TouchpadAttach.
