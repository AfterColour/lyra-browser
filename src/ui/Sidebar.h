// Lyra - the sidebar: bookmarks, history, reading list, downloads, notes and web panels.
#pragma once

#include "common/Platform.h"

struct Layout;

// A narrow strip of icons along one side of the page (Settings > Sidebar: left or right) and,
// when one of them is chosen, a panel between it and the page. The built-in panels are one
// Lyra page (resources/sidebar.html, in a view of its own that only it uses, with a private
// message channel); web panels are sites kept running beside the page (Telegram, Discord...),
// in a phone-sized layout by default.

void SidebarInit();                  // after the WebKit context exists
void SidebarShutdown();

// Layout (ui/Chrome ComputeTarget): the strip and the panel go below the toolbar, on the chosen
// side; returns the width they take (0 when hidden or in fullscreen). `left0` is where the left
// side begins (after the vertical tab list), `w` / `h` the client size.
float SidebarComputeLayout(Layout& L, float left0, float top, float w, float h);
void SidebarPaint(Graphics& g, bool transparent);          // the strip and the panel's header
void SidebarPositionViews();         // LayoutWebViews: the panel's view in place, the rest hidden
void SidebarInvalidate();            // the strip and the panel are drawn again (the panel's view is not)

// Hit testing (client coordinates): what is under (x, y), or false.
//   item: 0..4 the built-in panels, 100 + i the web panel i, 999 the "+" (add a web panel);
//   button: header buttons (0 close, 1 open in a tab, 2 reload, 3 back); resize: the panel's edge.
enum class SideHit { None, Item, Button, Resize, Empty };
SideHit SidebarHitTest(float x, float y, int& index);
void SidebarItemClick(int index);
void SidebarItemMiddleClick(int index);
void SidebarItemMenu(int index, POINT screenPt);
void SidebarButtonClick(int index);
void SidebarResizeBegin(int x);
void SidebarResizeMove(int x);
void SidebarResizeEnd();
bool SidebarResizing();

void SidebarToggle();                // Ctrl+Shift+E: opens the last panel / closes the open one
void SidebarOpen(int item);          // one of the built-in panels (0 bookmarks .. 4 notes)
bool SidebarShown();                 // the strip is on (Settings)
bool SidebarPanelOpen();
void SidebarSettingsChanged();       // shown / side changed in Settings
void SidebarAddCurrentToReadingList();
void SidebarThemeChanged();          // the built-in page follows the theme
bool SidebarAnimStep(float dt);      // ui/Animation: the panel sliding in / out, the strip's highlight
RectF SidebarPanelRect();            // the panel (empty when closed): kept out of Mica (UpdateMainBackdrop)
void SidebarRaise();                 // after a tab's view was put on top: the panel goes back over it
// Tests only (LYRA_TEST_SIDEBAR=1): the main window passes this registered message on; wParam 0
// moves a pretend pointer to lParam (client x, y), 1 also clicks there. 0 when not testing.
UINT SidebarTestMessage();
void SidebarTestPointer(WPARAM kind, LPARAM lp);
