// Lyra — рабочие пространства: отдельные наборы вкладок (со своими закреплёнными), переключатель.
#pragma once

#include "common/Platform.h"

// Every tab belongs to one workspace (Tab::workspace). All tabs stay in g_tabs, loaded or not:
// the active workspace's tabs come first (its pinned ones first among them), the other
// workspaces' tabs after them, hidden. So the tab list, Ctrl+Tab and the rest work on the
// prefix [0, WsTabEnd()) and never see the others; WorkspacesNormalize keeps that order.
struct Workspace {
    unsigned     id = 0;
    std::wstring name;
    int          icon = 0;                   // index into the icon set (WorkspaceIconGlyph)
    unsigned long long lastTab = 0;          // the tab that was open when it was left
};
extern std::vector<Workspace> g_workspaces;
extern unsigned g_workspace;                 // the active one's id

constexpr int kWorkspaceIconCount = 12;
const wchar_t* WorkspaceIconGlyph(int icon);
Workspace* FindWorkspace(unsigned id);
Workspace& ActiveWorkspace();
int  WorkspaceIndex(unsigned id);            // its place in g_workspaces, -1: none

int  WsTabEnd();                             // how many tabs the active workspace has (they come first)
bool TabInActiveWorkspace(int index);
// The tab gets no place in the tab list: another workspace's, or in a collapsed group.
bool TabHidden(int index);
// Ids that exist, at least one workspace, the order above. Fixes g_active by tab id.
void WorkspacesNormalize();

void WorkspaceSwitch(unsigned id);           // its last open tab opens (or a new tab, if it has none)
void WorkspaceSwitchByIndex(int index);      // Ctrl+Alt+1..9
void WorkspaceSwitchRelative(int delta);
void WorkspaceNew();                         // asks for a name, then switches to it
void WorkspaceRename(unsigned id);
void WorkspaceSetIcon(unsigned id, int icon);
void WorkspaceDelete(unsigned id);           // asks first if it has tabs; they close
void WorkspaceMoveTab(int tabIndex, unsigned id);
// SelectTab with another workspace's tab: that workspace becomes the active one first (the tab
// list reorders; the index of the tab is returned again).
int  WorkspaceActivateFor(int tabIndex);

// The switcher (the toolbar's Workspaces button): the list, new, rename, icon, delete.
void WorkspaceMenu(POINT screen);
// "Move to workspace" in a tab's menu, command ids from `firstId` on.
void WorkspacesAppendTabMenu(HMENU menu, int tabIndex, UINT firstId);
bool WorkspacesTabMenuCommand(UINT cmd, int tabIndex, UINT firstId);
constexpr UINT kWorkspaceTabMenuIds = 40;    // ids the tab menu reserves

// Session: "workspace <id> <active 0|1> <icon> <name>" lines; a tab's "ws <id>".
std::string WorkspacesSessionLines();
bool WorkspacesSessionParse(const char* value, Workspace* out, bool* active);
void WorkspacesRestore(const std::vector<Workspace>& list, unsigned active);
