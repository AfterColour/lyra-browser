// Lyra — медиа-клавиши и плашка Windows для звука/видео со страниц (YouTube, музыка).
#pragma once

#include "common/Platform.h"

// The tab that plays (or last played) sound is the "media tab": its title, artist and cover
// (from the page's Media Session, otherwise the page title) appear in Windows' media flyout
// (volume keys, lock screen), and the keyboard's play/pause, next and previous keys control it.
void MediaControlsInit();            // after AdBlockInit (the shared user content controller) and g_hWnd
void MediaControlsShutdown();
void MediaControlsTick();            // main timer
void MediaControlsButton(WPARAM button);   // WM_APP_MEDIA
void MediaControlsAudioChanged();    // a page started or stopped playing sound
void MediaToggleMute(int index);     // the speaker on a tab
