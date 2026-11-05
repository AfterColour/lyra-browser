// Lyra — раскладка, отрисовка и попадание мышью для панели вкладок и инструментов.
#pragma once

#include "common/Platform.h"

// ----------------------------------------------------------------------------
//  Layout & hit testing
// ----------------------------------------------------------------------------
struct Layout {
    float toolbarH = 0, tabBarH = 0, chromeH = 0;
    RectF back, fwd, more, pill, reload, newTab;
    std::vector<RectF> tabs;
};
extern Layout g_L;

struct Hit {
    enum Kind { None, Back, Forward, Reload, More, NewTab, Pill, TabBody, TabClose };
    Kind kind = None;
    int  index = -1;
    bool operator==(const Hit& o) const { return kind == o.kind && index == o.index; }
};

extern Hit g_hover, g_down;

void InvalidateChrome();
void ComputeLayout(int clientW);
Hit HitTest(int px, int py);

extern std::unique_ptr<Font> g_fText, g_fTab, g_fIcon, g_fIconSm;

void RebuildResources();
void PaintChrome(HDC hdc, int w);
