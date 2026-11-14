// Lyra — переключение вкладок без пустого экрана: последний кадр вкладки и плавный переход.
#pragma once

#include "common/Platform.h"

struct Tab;

// WebKit draws a page that comes back into view only after its next rendering update, so a
// switched-to tab shows an empty area for a moment. Lyra keeps the last picture of each recently
// used tab: on a switch that picture (or, for a tab without one, the tab being left) is shown
// over the page area at once and fades out as soon as WebKit has drawn the page.

// SelectTab, before the views change: remembers the picture of `from` and covers the page area.
// `to` gets WKPageForceRepaint; when it has drawn, the cover fades away.
void TransitionBegin(Tab* from, Tab* to);
// A new tab with one of Lyra's own pages: CreateTab covers the page area with the tab being
// left (before the new view exists), TransitionWaitLoad names the new tab once it does; the
// cover goes when that page has finished loading and drawn (TransitionLoaded from didFinish).
bool TransitionCoverFrom(Tab* from, bool startPage);   // startPage: covered by the start page's picture
void TransitionNewTab(unsigned long long tabId);       // not pictured until it has loaded
void TransitionWaitLoad(Tab* to);
void TransitionLoaded(unsigned long long tabId);
void TransitionPainted(unsigned long long tabId);   // WM_APP_TAB_PAINTED
void TransitionTick();                               // TIMER_TRANSITION
void TransitionCancel();                             // resize, move, fullscreen, closing
void TransitionForget(unsigned long long tabId);     // the tab closed
