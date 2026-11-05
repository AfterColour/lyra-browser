// Lyra — часы анимаций: кадры в такт обновлению экрана, плавное приближение к цели.
#pragma once

#include "common/Platform.h"

// Animations run only while something moves: AnimRequest() starts the frame clock, which posts
// WM_APP_ANIM_FRAME to the main window once per display refresh (DwmFlush), and stops by itself
// when AnimOnFrame() finds nothing left to animate.
void AnimRequest();
void AnimOnFrame();                 // main window: on WM_APP_ANIM_FRAME
void AnimShutdown();                // main window: on WM_DESTROY

bool AnimTabsEnabled();
bool AnimHoverEnabled();

// Exponential approach: the fraction of the remaining distance covered in `dt` seconds for an
// animation whose time constant is `tauMs` at 100 % speed (about 3 tau to settle).
float AnimFactor(float dt, float tauMs);
