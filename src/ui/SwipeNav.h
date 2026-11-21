// Lyra - swiping back and forward with two fingers: the page follows the fingers.
#pragma once

#include "common/Platform.h"

struct Tab;

// Going back, the page slides away to the right and uncovers a picture of the previous page;
// going forward, a picture of the next page slides in from the right. Pictures of pages are
// taken as they are left (SwipeNavLeavingPage); without one the uncovered part is the empty
// page colour. Let go past a third of the width (or with a flick) and the tab navigates; the
// picture stays until the page has drawn underneath.

// The touchpad (browser/Touchpad) found a sideways gesture the page cannot use: begins the swipe
// in the active tab `t` with the fingers already `offset` px along. False: no swipe.
bool SwipeNavBegin(Tab& t, bool back, float offset);
void SwipeNavMove(float dx);         // fingers moved (px, positive = right)
void SwipeNavRelease();              // fingers lifted: navigate or slide back
bool SwipeNavAnimStep(float dt);     // frame clock; true while something moves or waits
bool SwipeNavActive();

void SwipeNavLeavingPage(Tab& t);    // didStartProvisionalNavigation: a picture of the page left
void SwipeNavCommitted(unsigned long long tabId);   // didCommitNavigation
void SwipeNavPainted(unsigned long long tabId);     // WM_APP_SWIPE_PAINTED
void SwipeNavForget(unsigned long long tabId);      // the tab closed or was unloaded
void SwipeNavCancel();               // resize, fullscreen, closing: the picture goes at once
