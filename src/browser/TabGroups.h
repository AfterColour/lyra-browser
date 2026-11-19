// Lyra — группы вкладок: цветные именованные группы в списке вкладок, сворачивание, меню.
#pragma once

#include "common/Platform.h"

struct Tab;

// A group's tabs always stand next to each other in g_tabs (TabGroupsNormalize keeps that);
// pinned tabs are never in a group. Tab::group holds the group's id (0: none).
struct TabGroup {
    unsigned     id = 0;
    std::wstring name;                   // may be empty: the label is then a coloured dot
    int          color = 0;              // index into the palette, 0 .. kTabGroupColorCount - 1
    bool         collapsed = false;      // its tabs are hidden, only the label shows
};
extern std::vector<TabGroup> g_groups;

constexpr int kTabGroupColorCount = 9;
Color TabGroupColor(int color);                      // for the current theme
const wchar_t* TabGroupColorName(int color);
TabGroup* FindTabGroup(unsigned id);
// The indexes in g_tabs of the first and the last tab of the group; false if it has none.
bool TabGroupRange(unsigned id, int* first, int* last);
int  TabGroupTabCount(unsigned id);
// The tab is in a collapsed group: it gets no place in the tab list.
bool TabHiddenByGroup(int index);
// Keeps the rules: groups contiguous, no pinned tab in a group, a split view's two tabs in the
// same group, no empty groups, the open tab never hidden. ComputeTarget runs it before layout.
void TabGroupsNormalize();

unsigned TabGroupCreate(int tabIndex);               // a new group holding the tab; its id
void TabGroupAddTab(unsigned id, int tabIndex);      // the tab moves to the end of the group
void TabGroupRemoveTab(int tabIndex);                // the tab leaves (it moves right after the group)
void TabGroupSetCollapsed(unsigned id, bool collapsed);
void TabGroupToggle(unsigned id);
void TabGroupRenameDialog(unsigned id);
void TabGroupSetColor(unsigned id, int color);
void TabGroupUngroup(unsigned id);
void TabGroupCloseTabs(unsigned id);
void TabGroupNewTab(unsigned id);
// The label's menu (right click), at a point in screen coordinates.
void TabGroupMenu(unsigned id, POINT screen);
// Adds "new group" / "add to group" / "remove from group" to a tab's menu with command ids from
// `firstId` on; TabGroupsTabMenuCommand handles the chosen one (true if it was one of these).
void TabGroupsAppendTabMenu(HMENU menu, int tabIndex, UINT firstId);
bool TabGroupsTabMenuCommand(UINT cmd, int tabIndex, UINT firstId);

void TabGroupsTabSelected(Tab& t);                   // SelectTab: a tab in a collapsed group opens it
// A tab a page (or the page's context menu) opened joins the group of the tab it came from.
void TabGroupsTabOpenedFrom(Tab& t, const Tab* opener);
void TabGroupsAfterDrop(int index);                  // DropTabAt: the group follows the neighbours

// Session: the groups as "group <id> <color> <collapsed> <name>" lines; reading them back.
std::string TabGroupsSessionLines();
bool TabGroupsSessionParse(const std::string& lineAfterKey, TabGroup* out);
void TabGroupsRestore(std::vector<TabGroup> groups); // after the tabs got their ids back
