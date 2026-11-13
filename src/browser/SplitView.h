// Lyra — разделённый экран: две вкладки рядом в одном окне.
#pragma once

#include "common/Platform.h"

struct Tab;

// One pair of tabs at a time. While either of the two is the open tab, both are shown side by
// side in the page area with a divider between them (drag it to resize, double-click for half
// and half); the open one has an accent outline and gets the toolbar, the keyboard and the
// commands, a click into the other half makes that one the open tab. Picking a tab outside the
// pair shows it alone; picking either tab of the pair again brings the split back. Closing one
// of the two (or "Close split view") ends it.

bool SplitHas(const Tab& t);                         // one of the pair
bool SplitShown();                                   // the open tab is one of the pair
bool SplitVisible(const Tab& t);                     // on screen as half of the split
Tab* SplitPartnerOf(const Tab& t);                   // the other half (nullptr if t is not in the pair)
bool SplitPaired(const Tab* a, const Tab* b);

// The open tab and `otherId` side by side (the open tab keeps its side if it was in a pair, else
// left). The open tab stays open.
void SplitWith(unsigned long long otherId);
// A new tab beside the open one, opened in front (the start page, or `input`).
void SplitWithNewTab();
// A link from `fromId` beside it, in the background (the page's context menu).
void SplitOpenLink(unsigned long long fromId, const std::wstring& url, bool privateTab);
void SplitToggle();                                  // menu / palette: close it, or split with a new tab
void SplitSwap();                                    // the halves change sides
void SplitClose();
// CloseTab, after the tab left g_tabs: returns the other half's id if `id` was in the pair.
unsigned long long SplitTabClosed(unsigned long long id);

// LayoutWebViews: puts both halves in the page area (left, top, w x h). False if the split is not
// shown (then the caller lays out the open tab as usual).
bool SplitLayout(int left, int top, int w, int h);
// Element full screen: the other half is hidden until it ends.
void SplitHidePartner();
void SplitRepaintTimer();                            // TIMER_SPLIT_REPAINT

// DrawChrome, after the page area background: the gutter, the outline and the divider grip.
void SplitPaint(Gdiplus::Graphics& g);
// The main window's mouse messages in the page area (divider drag, clicks on the gutter).
// True if handled.
bool SplitMouse(UINT msg, int x, int y);
bool SplitSetCursor();                               // WM_SETCURSOR over the divider
void SplitFocusTab(unsigned long long id);           // WM_APP_SPLIT_FOCUS (a click into the other half)

// Dragging a tab onto another tab splits the two: dropped on the right half of the target the
// dragged tab goes right, else left. The main window reports the drag (target -1: over no other
// tab); the tab strip shows where it would go.
void SplitTabDragOver(unsigned long long draggedId, int targetIndex, bool right);
void SplitTabDragEnd(bool drop);
bool SplitTabDragging();
bool SplitDropHint(int index, bool* right);          // is tab `index` the drop target, and which side

// The divider glides to where the mouse is (frame clock, ui/Animation.cpp); true while it moves.
bool SplitAnimStep(float dt);
