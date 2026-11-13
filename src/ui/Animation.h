// Lyra — часы анимаций: кадры в такт обновлению экрана, плавное приближение к цели.
#pragma once

#include "common/Platform.h"

// Animations run only while something moves: AnimRequest() starts the frame clock, which posts
// WM_APP_ANIM_FRAME to the main window once per display refresh (DwmFlush), and stops by itself
// when AnimOnFrame() finds nothing left to animate.
void AnimRequest();
void AnimOnFrame();                 // main window: on WM_APP_ANIM_FRAME
void AnimShutdown();                // main window: on WM_DESTROY
// The settings window's smooth scrolling runs on the same frame clock (ui/settings/SettingsWindow.cpp);
// returns true while it still moves.
bool SettingsAnimStep(float dt);

bool AnimTabsEnabled();
bool AnimHoverEnabled();

// Exponential approach: the fraction of the remaining distance covered in `dt` seconds for an
// animation whose time constant is `tauMs` at 100 % speed (about 3 tau to settle).
float AnimFactor(float dt, float tauMs);

// A popup that has just been shown unfolds downwards from its top edge (bookmark bubble,
// downloads list, address suggestions). Call right after ShowWindow; the window must already
// have its final position and size. Nothing happens with animations off.
void PopupReveal(HWND popup);
void PopupRevealStop(HWND popup);   // before the popup is resized, hidden or destroyed
// Shows a popup that is already in place without a flash of an unpainted (white) window: it is
// painted while DWM keeps it hidden, then appears (and unfolds, see PopupReveal).
void PopupShow(HWND popup, bool activate);
// The same for a popup that SetWindowPos(SWP_SHOWWINDOW) is about to show: call Begin before
// and End after it.
void PopupShowBegin(HWND popup);
void PopupShowEnd(HWND popup);
