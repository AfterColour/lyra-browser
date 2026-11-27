// Lyra — раскладка, отрисовка и попадание мышью для панели вкладок и инструментов.
#pragma once

#include "common/Platform.h"
#include "ui/UiLayout.h"

// Sizes of the window chrome in 96-DPI pixels for the active layout (ui/UiLayout).
struct ChromeDims {
    float bar;      // row with the address field
    float strip;    // tab strip row (separate tab bar layout)
    float btn;      // toolbar button
    float tab;      // tab / address field height
    float icon;     // toolbar glyph size
    float cap;      // width of a window button (minimise / maximise / close)
    float font;     // tab title size
    float sideW;    // width of the vertical tab list
};
ChromeDims CurrentDims();

enum TabLayoutMode { kLayoutCompact = 0, kLayoutSeparate = 1, kLayoutVertical = 2 };

// ----------------------------------------------------------------------------
//  Layout & hit testing (client coordinates, physical pixels)
// ----------------------------------------------------------------------------
struct Layout {
    int   mode = kLayoutCompact;
    float chromeH = 0;               // height of the rows above the page
    float sideW = 0;                 // vertical tab list width (0 in the other layouts)
    float captionH = 0;              // height of the row holding the window buttons
    bool  reloadInPill = true;       // false: reload is a toolbar button (separate layout)
    RectF back, fwd, more, pill, reload, newTab;
    RectF star;                      // bookmark star at the right end of the address field
    RectF downloads;                 // downloads button left of "more" (empty while there are none)
    std::vector<RectF> ext;          // extensions' buttons, left of downloads (ExtensionButtons())
    RectF capMin, capMax, capClose;
    RectF bmBar;                     // the bookmarks bar under the toolbar (ui/BookmarksBar), empty when hidden
    float pageL = 0, pageR = 0;      // the page area's left and right edges (after the tab list and the sidebar)
    std::vector<RectF> tabs;         // one per tab; in the compact layout the active one is the address field
    // Tab groups (browser/TabGroups): a label before each group's first tab.
    std::vector<RectF> groups;
    std::vector<unsigned> groupIds;  // the group of each label
    std::vector<int> groupFirst;     // the index of its first tab
    // An open stack (a group shown as a stack) in the horizontal layouts: a row under the tabs
    // with all its tabs (their L.tabs rectangles); the open one is stackActive.
    RectF stackRow, stackActive;
    // The layout's items (ui/UiLayout): where each went. Hidden ones have empty rectangles.
    RectF home, sidebarBtn, workspacesBtn;
    bool  inField[kItemCount] = {};  // inside the address field (drawn small, not while typing)
    bool  fieldItems = false;        // any: they begin at fieldX
    float fieldX = 0;
    bool  newTabRow = false;         // vertical layout: "New tab" is a row under the list
    RectF zoneRect[kZoneCount];      // each zone's extent (customizing: empty ones get a placeholder)
    struct Slot { int item, zone, index; RectF r; };   // zone -1: the palette of hidden items
    std::vector<Slot> slots;
    RectF palette, palDone;          // customizing: the row of hidden items and its "Done" button
};
extern Layout g_L;

struct Hit {
    enum Kind { None, Back, Forward, Reload, More, NewTab, Pill, TabBody, TabClose, CapMin, CapMax, CapClose, Star, Downloads, TabAudio, Key, Reader, Translate, ExtAction, GroupLabel, Zoom, BookmarkItem, SideItem, SideButton, SideResize, Home, SidebarBtn, WorkspacesBtn };
    Kind kind = None;
    int  index = -1;
    bool operator==(const Hit& o) const { return kind == o.kind && index == o.index; }
    bool IsCaptionButton() const { return kind == CapMin || kind == CapMax || kind == CapClose; }
};

extern Hit g_hover, g_down;

void InvalidateChrome();
// Only the extensions' buttons (a badge or an icon changed).
void InvalidateChromeExtButtons();
// After an animation: the pieces painted with fast blending (Mica) again, at full quality.
void ChromeRepaintFastPieces();
// Recomputes where everything goes. With `animate` (and tab animations on) tabs glide from
// where they are to the new places; otherwise they jump there.
void ComputeLayout(int clientW, bool animate = false);
Hit HitTest(int px, int py);
// A split view is one tab in the list: the index of the half's tab under (x, y) when `index`
// is that tab, else `index`.
int SplitTabAt(int index, int x, int y);
// A tab being dragged would go before the tab at `index` (g_tabs.size(): at the end; -1: none): the
// tabs from there on move aside to open a gap with an accent bar in it.
void SetTabInsertHint(int index);
int TabInsertHint();
struct Tab;
// A tab as it looks when open (field colour, border, icon, title) in `r`: the dragged tab.
void DrawTabFace(Graphics& g, const Tab& t, const RectF& r);
// Use instead of assigning g_hover: starts the hover fade.
void SetHover(const Hit& h);
// Advances tab and hover animations; returns true while anything still moves.
bool ChromeAnimStep(float dt);

// After a tab is closed with the mouse the remaining tabs keep their width (they do not widen),
// so the next tab's close button lands under the pointer. They widen again once the pointer
// has left the tab strip (MainWindow.cpp, TIMER_TAB_UNFREEZE).
void FreezeTabWidths();
// A download has just started: the downloads button plays its "here it comes" animation.
void ChromeDownloadStarted();
void UnfreezeTabWidths();
bool TabWidthsFrozen();
float TabStripBottom();             // the tab strip spans y in [0, TabStripBottom())
// Where menus of the menu button open: the button, or (hidden) the top right corner of the toolbar.
RectF ChromeMenuAnchor();

extern std::unique_ptr<Font> g_fText, g_fTab, g_fTabBold, g_fIcon, g_fIconSm, g_fCaption, g_fBadge;

// Fonts and the address field font; call after a DPI or toolbar size change.
void RebuildResources();
void ReleaseResources();
// Paints the chrome and the empty page area inside `rc` (client coordinates).
void PaintChrome(HDC hdc, const RECT& rc);
// Mica behind the tab strip / toolbar / vertical tab list (follows the layout and fullscreen).
void UpdateMainBackdrop();
// With Mica DWM draws the system minimise / maximise / close buttons in the backdrop area (a
// window without the caption style gets no Mica). Lyra's buttons then take exactly their place
// and only draw their hover / pressed state; hit-testing and clicks stay Lyra's.
bool NativeCaptionButtons();
