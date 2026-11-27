// Lyra — группы вкладок: цветные именованные группы в списке вкладок, сворачивание, меню.
#include "browser/TabGroups.h"
#include "app/Log.h"
#include "browser/SplitView.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "browser/Workspaces.h"
#include "core/Constants.h"
#include "core/Localization.h"
#include "core/StringUtils.h"
#include "ui/Chrome.h"
#include "ui/AddressBar.h"
#include "ui/Commands.h"
#include "ui/WindowFrame.h"
#include "ui/MainWindow.h"
#include "ui/Menu.h"
#include "ui/Theme.h"
#include "ui/UiLayout.h"

std::vector<TabGroup> g_groups;
static unsigned s_nextGroupId = 1;

namespace {

struct PaletteEntry { const wchar_t* en; const wchar_t* ru; BYTE dark[3]; BYTE light[3]; };
// Chrome's tab group colours: lighter ones for the dark theme, deeper ones for the light theme.
const PaletteEntry kPalette[kTabGroupColorCount] = {
    { L"Blue",   L"Синий",      { 0x8A, 0xB4, 0xF8 }, { 0x1A, 0x73, 0xE8 } },
    { L"Red",    L"Красный",    { 0xF2, 0x8B, 0x82 }, { 0xD9, 0x30, 0x25 } },
    { L"Yellow", L"Жёлтый",     { 0xFD, 0xD6, 0x63 }, { 0xE3, 0x74, 0x00 } },
    { L"Green",  L"Зелёный",    { 0x81, 0xC9, 0x95 }, { 0x18, 0x80, 0x38 } },
    { L"Pink",   L"Розовый",    { 0xFF, 0x8B, 0xCB }, { 0xD0, 0x18, 0x84 } },
    { L"Purple", L"Фиолетовый", { 0xD7, 0xAE, 0xFB }, { 0x93, 0x34, 0xE6 } },
    { L"Cyan",   L"Бирюзовый",  { 0x78, 0xD9, 0xEC }, { 0x00, 0x7B, 0x83 } },
    { L"Orange", L"Оранжевый",  { 0xFC, 0xAD, 0x70 }, { 0xFA, 0x7B, 0x17 } },
    { L"Grey",   L"Серый",      { 0xDA, 0xDC, 0xE0 }, { 0x5F, 0x63, 0x68 } },
};

void Changed() {
    RefreshLayout(true);
    InvalidateChrome();
}

// The colour a new group gets: the first one no group uses, else the next in turn.
int NextColor() {
    for (int c = 0; c < kTabGroupColorCount; ++c) {
        bool used = false;
        for (auto& g : g_groups) if (g.color == c) used = true;
        if (!used) return c;
    }
    return (int)g_groups.size() % kTabGroupColorCount;
}

int IndexOfId(unsigned long long tabId) {
    int i = -1;
    return FindTab(tabId, &i) ? i : -1;
}

// Moves the tab at `from` so that it stands right after the last tab of the group (which
// must not be the tab's own group); returns its new index.
int MoveAfterGroup(int from, unsigned id) {
    int first = -1, last = -1;
    if (!TabGroupRange(id, &first, &last)) return from;
    const unsigned long long tabId = g_tabs[from]->id;
    MoveTab(from, from <= last ? last : last + 1);
    return IndexOfId(tabId);
}

} // namespace

Color TabGroupColor(int color) {
    const PaletteEntry& p = kPalette[std::clamp(color, 0, kTabGroupColorCount - 1)];
    const BYTE* c = g_dark ? p.dark : p.light;
    return Color(c[0], c[1], c[2]);
}

const wchar_t* TabGroupColorName(int color) {
    const PaletteEntry& p = kPalette[std::clamp(color, 0, kTabGroupColorCount - 1)];
    return Tr(p.en, p.ru);
}

TabGroup* FindTabGroup(unsigned id) {
    if (!id) return nullptr;
    for (auto& g : g_groups) if (g.id == id) return &g;
    return nullptr;
}

bool TabGroupRange(unsigned id, int* first, int* last) {
    int f = -1, l = -1;
    for (int i = 0; i < (int)g_tabs.size(); ++i)
        if (g_tabs[i]->group == id) { if (f < 0) f = i; l = i; }
    if (first) *first = f;
    if (last) *last = l;
    return f >= 0;
}

int TabGroupTabCount(unsigned id) {
    int n = 0;
    for (auto& t : g_tabs) if (t->group == id) ++n;
    return n;
}

bool TabHiddenByGroup(int index) {
    if (index < 0 || index >= (int)g_tabs.size() || index == g_active) return false;
    const TabGroup* g = FindTabGroup(g_tabs[index]->group);
    if (!g) return false;
    if (g->collapsed) return true;
    return g->stacked && !TabGroupStackOpen(g->id) && index != TabGroupFrontIndex(g->id);
}

int TabGroupFrontIndex(unsigned id) {
    const TabGroup* g = FindTabGroup(id);
    if (!g) return -1;
    if (g_active >= 0 && g_active < (int)g_tabs.size() && g_tabs[g_active]->group == id) return g_active;
    int first = -1, last = -1;
    if (!TabGroupRange(id, &first, &last)) return -1;
    for (int i = first; i <= last; ++i) if (g_tabs[i]->id == g->front) return i;
    return first;
}

bool TabGroupStackOpen(unsigned id) {
    const TabGroup* g = FindTabGroup(id);
    return g && g->stacked && !g->collapsed && g_active >= 0 && g_active < (int)g_tabs.size() && g_tabs[g_active]->group == id;
}

bool TabInStackRow(int index) {
    if (index < 0 || index >= (int)g_tabs.size() || index == g_active) return false;
    if (UiActive().mode == kLayoutVertical) return false;
    return TabGroupStackOpen(g_tabs[index]->group);
}

void TabGroupSetStacked(unsigned id, bool stacked) {
    TabGroup* g = FindTabGroup(id);
    if (!g || g->stacked == stacked) return;
    g->stacked = stacked;
    if (stacked) {
        g->collapsed = false;
        const int f = TabGroupFrontIndex(id);
        if (f >= 0) g->front = g_tabs[f]->id;
    }
    Log("group %u %s", id, stacked ? "-> stack" : "-> group");
    Changed();
}

void TabGroupLabelClick(unsigned id) {
    const TabGroup* g = FindTabGroup(id);
    if (!g) return;
    if (g->stacked && !g->collapsed && !TabGroupStackOpen(id)) {   // a closed stack: its front tab opens
        const int f = TabGroupFrontIndex(id);
        if (f >= 0) SelectTab(f);
        return;
    }
    TabGroupToggle(id);
}

void TabGroupsNormalize() {
    const int n = (int)g_tabs.size();
    // Pinned tabs are not grouped; a group that no longer exists lets its tabs go.
    for (auto& t : g_tabs)
        if (t->pinned || (t->group && !FindTabGroup(t->group))) t->group = 0;
    // A split view is one tab in the list: its right half follows the left half's group.
    int li = -1, ri = -1;
    if (SplitStripPair(&li, &ri) && li >= 0 && ri < n) g_tabs[ri]->group = g_tabs[li]->group;
    // Contiguous: a group's tabs after a gap leave it.
    std::vector<unsigned> closed;                         // groups whose run has ended
    for (int i = 0; i < n; ++i) {
        const unsigned g = g_tabs[i]->group;
        if (g && std::find(closed.begin(), closed.end(), g) != closed.end()) g_tabs[i]->group = 0;
        if (i > 0 && g_tabs[i - 1]->group && g_tabs[i - 1]->group != g_tabs[i]->group) closed.push_back(g_tabs[i - 1]->group);
    }
    // An ungrouped tab between two tabs of one group belongs to it.
    for (int i = 1; i + 1 < n; ++i)
        if (!g_tabs[i]->group && !g_tabs[i]->pinned && g_tabs[i - 1]->group && g_tabs[i - 1]->group == g_tabs[i + 1]->group)
            g_tabs[i]->group = g_tabs[i - 1]->group;
    // No empty groups; the open tab is never in a collapsed group.
    g_groups.erase(std::remove_if(g_groups.begin(), g_groups.end(),
                                  [](const TabGroup& g) { return TabGroupTabCount(g.id) == 0; }), g_groups.end());
    if (g_active >= 0 && g_active < n)
        if (TabGroup* g = FindTabGroup(g_tabs[g_active]->group)) g->collapsed = false;
}

unsigned TabGroupCreate(int tabIndex) {
    if (tabIndex < 0 || tabIndex >= (int)g_tabs.size()) return 0;
    const unsigned long long tabId = g_tabs[tabIndex]->id;
    if (g_tabs[tabIndex]->pinned) SetTabPinned(tabIndex, false);
    tabIndex = IndexOfId(tabId);
    if (tabIndex < 0) return 0;
    if (unsigned old = g_tabs[tabIndex]->group) {        // out of its group first (it stays a run)
        g_tabs[tabIndex]->group = 0;
        tabIndex = MoveAfterGroup(tabIndex, old);
        if (tabIndex < 0) return 0;
    }
    TabGroup g;
    g.id = s_nextGroupId++;
    g.color = NextColor();
    g_groups.push_back(g);
    g_tabs[tabIndex]->group = g.id;
    Log("tab#%llu -> new group %u", tabId, g.id);
    Changed();
    return g.id;
}

void TabGroupAddTab(unsigned id, int tabIndex) {
    if (!FindTabGroup(id) || tabIndex < 0 || tabIndex >= (int)g_tabs.size()) return;
    if (g_tabs[tabIndex]->group == id) return;
    const unsigned long long tabId = g_tabs[tabIndex]->id;
    if (g_tabs[tabIndex]->pinned) SetTabPinned(tabIndex, false);
    tabIndex = IndexOfId(tabId);
    if (tabIndex < 0) return;
    g_tabs[tabIndex]->group = 0;
    tabIndex = MoveAfterGroup(tabIndex, id);
    if (tabIndex < 0) return;
    g_tabs[tabIndex]->group = id;
    if (TabGroup* g = FindTabGroup(id)) if (tabIndex == g_active) g->collapsed = false;
    Changed();
}

void TabGroupRemoveTab(int tabIndex) {
    if (tabIndex < 0 || tabIndex >= (int)g_tabs.size()) return;
    const unsigned id = g_tabs[tabIndex]->group;
    if (!id) return;
    g_tabs[tabIndex]->group = 0;
    MoveAfterGroup(tabIndex, id);
    Changed();
}

void TabGroupSetCollapsed(unsigned id, bool collapsed) {
    TabGroup* g = FindTabGroup(id);
    if (!g || g->collapsed == collapsed) return;
    if (collapsed && g_active >= 0 && g_active < (int)g_tabs.size() && g_tabs[g_active]->group == id) {
        // The open tab is in it: the nearest tab outside the group opens instead (right, then left).
        int first = -1, last = -1, next = -1;
        TabGroupRange(id, &first, &last);
        for (int i = last + 1; i < (int)g_tabs.size() && next < 0; ++i)
            if (!TabHidden(i)) next = i;
        for (int i = first - 1; i >= 0 && next < 0; --i)
            if (!TabHidden(i)) next = i;
        if (next < 0) {                                  // nothing else: a new tab after the group
            RunCommand(CMD_NEWTAB);
            g = FindTabGroup(id);
            if (!g) return;
        } else {
            SelectTab(next);
            g = FindTabGroup(id);
            if (!g) return;
        }
    }
    g->collapsed = collapsed;
    Changed();
}

void TabGroupToggle(unsigned id) {
    if (const TabGroup* g = FindTabGroup(id)) TabGroupSetCollapsed(id, !g->collapsed);
}

// ---- Renaming on the label --------------------------------------------------------------
static HWND s_edit = nullptr;
static WNDPROC s_editOldProc = nullptr;
static unsigned s_editId = 0;
constexpr int kGroupEditId = 1002;

static LRESULT CALLBACK GroupEditProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_KEYDOWN:
        if (w == VK_RETURN) { TabGroupEditFinish(true); return 0; }
        if (w == VK_ESCAPE) { TabGroupEditFinish(false); return 0; }
        if (w == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) { SendMessageW(h, EM_SETSEL, 0, -1); return 0; }
        break;
    case WM_CHAR:
        if (w == VK_RETURN || w == VK_ESCAPE || w == 1) return 0;   // no beep
        break;
    case WM_SETFOCUS: {                                  // the whole name selected: typing replaces it
        const LRESULT r = CallWindowProcW(s_editOldProc, h, msg, w, l);
        SendMessageW(h, EM_SETSEL, 0, -1);
        return r;
    }
    }
    return CallWindowProcW(s_editOldProc, h, msg, w, l);
}

unsigned TabGroupEditingId() { return s_editId; }
bool TabGroupIsEdit(HWND h) { return h && h == s_edit; }

void TabGroupEditReposition() {
    if (!s_editId || !s_edit) return;
    int k = -1;
    for (size_t i = 0; i < g_L.groupIds.size(); ++i) if (g_L.groupIds[i] == s_editId) k = (int)i;
    if (k < 0 || k >= (int)g_L.groups.size()) { ShowWindow(s_edit, SW_HIDE); return; }
    const RectF& r = g_L.groups[k];
    float left, right;
    if (g_L.mode == kLayoutVertical) { left = r.X + S(30); right = r.GetRight() - S(28); }
    else { left = r.X + S(10); right = r.GetRight() - S(10); }
    const int h = g_editTextH;
    const int x = (int)left, y = (int)(r.Y + (r.Height - (float)h) / 2), w = (int)std::max(10.f, right - left);
    RECT cur = {};
    GetWindowRect(s_edit, &cur);
    MapWindowPoints(nullptr, g_hWnd, (POINT*)&cur, 2);
    if (cur.left != x || cur.top != y || cur.right - cur.left != w || cur.bottom - cur.top != h)
        MoveWindow(s_edit, x, y, w, h, TRUE);
}

void TabGroupStartRename(unsigned id) {
    const TabGroup* g = FindTabGroup(id);
    if (!g || !g_hWnd) return;
    if (s_editId) TabGroupEditFinish(true);
    if (!s_edit) {
        s_edit = CreateOpaqueEdit(g_hWnd, kGroupEditId, (HINSTANCE)GetWindowLongPtrW(g_hWnd, GWLP_HINSTANCE));
        if (!s_edit) return;
        s_editOldProc = (WNDPROC)SetWindowLongPtrW(s_edit, GWLP_WNDPROC, (LONG_PTR)GroupEditProc);
        SendMessageW(s_edit, EM_LIMITTEXT, 60, 0);
    }
    SendMessageW(s_edit, WM_SETFONT, (WPARAM)g_editFont, FALSE);
    s_editId = id;
    if (g->collapsed) TabGroupSetCollapsed(id, false);   // (its tabs show while it is named)
    SetWindowTextW(s_edit, g->name.c_str());
    RefreshLayout(true);                                 // the label widens for the field
    TabGroupEditReposition();
    ShowWindow(s_edit, SW_SHOW);
    SetFocus(s_edit);
    SendMessageW(s_edit, EM_SETSEL, 0, -1);
    InvalidateChrome();
}

void TabGroupEditFinish(bool keep) {
    if (!s_editId) return;
    const unsigned id = s_editId;
    s_editId = 0;                                        // (first: hiding the field sends EN_KILLFOCUS)
    std::wstring name;
    if (keep && s_edit) {
        const int n = GetWindowTextLengthW(s_edit);
        name.resize(n + 1);
        GetWindowTextW(s_edit, &name[0], n + 1);
        name.resize(n);
    }
    const bool hadFocus = s_edit && GetFocus() == s_edit;
    if (s_edit) ShowWindow(s_edit, SW_HIDE);
    if (TabGroup* g = FindTabGroup(id); g && keep) {
        for (auto& c : name) if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
        while (!name.empty() && name.back() == L' ') name.pop_back();
        while (!name.empty() && name.front() == L' ') name.erase(name.begin());
        g->name = name;
        Log("group %u renamed", id);
    }
    if (hadFocus) FocusWebView();
    Changed();
}

void TabGroupSetColor(unsigned id, int color) {
    if (TabGroup* g = FindTabGroup(id)) {
        g->color = std::clamp(color, 0, kTabGroupColorCount - 1);
        InvalidateChrome();
    }
}

void TabGroupUngroup(unsigned id) {
    for (auto& t : g_tabs) if (t->group == id) t->group = 0;
    g_groups.erase(std::remove_if(g_groups.begin(), g_groups.end(), [id](const TabGroup& g) { return g.id == id; }), g_groups.end());
    Changed();
}

void TabGroupCloseTabs(unsigned id) {
    std::vector<unsigned long long> ids;
    for (auto& t : g_tabs) if (t->group == id) ids.push_back(t->id);
    if (ids.empty()) return;
    if ((int)ids.size() >= WsTabEnd()) RunCommand(CMD_NEWTAB);   // the window stays open
    for (unsigned long long tabId : ids) {
        int i = IndexOfId(tabId);
        if (i >= 0) CloseTab(i);
    }
    Changed();
}

void TabGroupNewTab(unsigned id) {
    if (!FindTabGroup(id)) return;
    RunCommand(CMD_NEWTAB);
    if (g_active >= 0 && g_active < (int)g_tabs.size()) TabGroupAddTab(id, g_active);
}

void TabGroupMenu(unsigned id, POINT screen) {
    const TabGroup* g = FindTabGroup(id);
    if (!g) return;
    enum : UINT { kNewTab = 1, kRename, kCollapse, kUngroup, kClose, kStack, kColor0 = 100 };
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, kNewTab, Tr(L"New tab in group", L"Новая вкладка в группе"));
    AppendMenuW(m, MF_STRING, kRename, Tr(L"Rename group...", L"Переименовать группу..."));
    HMENU colors = CreatePopupMenu();
    for (int c = 0; c < kTabGroupColorCount; ++c)
        AppendMenuW(colors, MF_STRING | (c == g->color ? MF_CHECKED : 0), kColor0 + c, TabGroupColorName(c));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)colors, Tr(L"Colour", L"Цвет"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kStack, g->stacked ? Tr(L"Show as group", L"Показать как группу")
                                                 : Tr(L"Show as stack", L"Показать стопкой"));
    AppendMenuW(m, MF_STRING, kCollapse, g->collapsed ? Tr(L"Expand group", L"Развернуть группу")
                                                      : Tr(L"Collapse group", L"Свернуть группу"));
    AppendMenuW(m, MF_STRING, kUngroup, Tr(L"Ungroup", L"Разгруппировать"));
    AppendMenuW(m, MF_STRING, kClose, Tr(L"Close group", L"Закрыть группу"));
    MenuSetIcon(m, kNewTab, L"");
    MenuSetIcon(m, kRename, L"");
    MenuSetIcon(m, kCollapse, g->collapsed ? L"" : L"");
    MenuSetIcon(m, kStack, g->stacked ? L"\uE8FD" : L"\uE8A9");
    MenuSetIcon(m, kUngroup, L"");
    MenuSetIcon(m, kClose, L"");
    const int cmd = MenuTrack(m, TPM_LEFTALIGN | TPM_TOPALIGN, screen.x, screen.y, g_hWnd);
    DestroyMenu(m);                                      // (destroys the submenu too)
    if (!FindTabGroup(id)) return;
    if (cmd == kNewTab) TabGroupNewTab(id);
    else if (cmd == kRename) TabGroupStartRename(id);
    else if (cmd == kCollapse) TabGroupToggle(id);
    else if (cmd == kStack) TabGroupSetStacked(id, !FindTabGroup(id)->stacked);
    else if (cmd == kUngroup) TabGroupUngroup(id);
    else if (cmd == kClose) TabGroupCloseTabs(id);
    else if (cmd >= kColor0 && cmd < kColor0 + kTabGroupColorCount) TabGroupSetColor(id, cmd - kColor0);
}

// The group's name for a menu: its name, or "Unnamed group (Blue)".
static std::wstring MenuName(const TabGroup& g) {
    if (!g.name.empty()) return g.name;
    return std::wstring(Tr(L"Unnamed group", L"Группа без названия")) + L" (" + TabGroupColorName(g.color) + L")";
}

void TabGroupsAppendTabMenu(HMENU menu, int tabIndex, UINT firstId) {
    if (tabIndex < 0 || tabIndex >= (int)g_tabs.size()) return;
    const Tab& t = *g_tabs[tabIndex];
    AppendMenuW(menu, MF_STRING, firstId, Tr(L"Add tab to new group", L"Добавить вкладку в новую группу"));
    MenuSetIcon(menu, firstId, L"");
    auto here = [](unsigned id) {                        // a group of the active workspace
        int first = -1;
        return TabGroupRange(id, &first, nullptr) && TabInActiveWorkspace(first);
    };
    bool others = false;
    for (auto& g : g_groups) if (g.id != t.group && here(g.id)) others = true;
    if (others) {
        HMENU sub = CreatePopupMenu();
        for (size_t k = 0; k < g_groups.size(); ++k)
            if (g_groups[k].id != t.group && here(g_groups[k].id))
                AppendMenuW(sub, MF_STRING, firstId + 2 + (UINT)k, MenuName(g_groups[k]).c_str());
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, Tr(L"Add tab to group", L"Добавить вкладку в группу"));
    }
    if (t.group) {
        AppendMenuW(menu, MF_STRING, firstId + 1, Tr(L"Remove from group", L"Убрать из группы"));
        MenuSetIcon(menu, firstId + 1, L"");
    }
}

bool TabGroupsTabMenuCommand(UINT cmd, int tabIndex, UINT firstId) {
    if (cmd < firstId || cmd >= firstId + 2 + (UINT)g_groups.size()) return false;
    if (cmd == firstId) {
        const unsigned id = TabGroupCreate(tabIndex);
        if (id) TabGroupStartRename(id);                 // like Chrome: name it right away (or not)
    } else if (cmd == firstId + 1) {
        TabGroupRemoveTab(tabIndex);
    } else {
        TabGroupAddTab(g_groups[cmd - firstId - 2].id, tabIndex);
    }
    return true;
}

void TabGroupsTabSelected(Tab& t) {
    if (TabGroup* g = FindTabGroup(t.group)) {
        g->front = t.id;                                 // (a stack shows it from now on)
        if (g->collapsed) { g->collapsed = false; InvalidateChrome(); }
    }
}

void TabGroupsTabOpenedFrom(Tab& t, const Tab* opener) {
    if (!opener || !opener->group || t.pinned || t.group || !FindTabGroup(opener->group)) return;
    const int i = IndexOfId(t.id);
    if (i < 0) return;
    const unsigned id = opener->group;
    const int at = MoveAfterGroup(i, id);
    if (at >= 0) g_tabs[at]->group = id;
    Changed();
}

void TabGroupsAfterDrop(int index) {
    if (index < 0 || index >= (int)g_tabs.size()) return;
    Tab& t = *g_tabs[index];
    if (t.pinned) { t.group = 0; return; }
    const unsigned before = index > 0 ? g_tabs[index - 1]->group : 0;
    const unsigned after = index + 1 < (int)g_tabs.size() ? g_tabs[index + 1]->group : 0;
    if (before && before == after) t.group = before;     // dropped inside a group: it joins
    else if (t.group && (t.group == before || t.group == after)) {}   // still at its group's edge
    else t.group = 0;
}

std::string TabGroupsSessionLines() {
    std::string out;
    for (auto& g : g_groups) {
        bool saved = false;                              // (a group of private tabs only is not saved)
        for (auto& t : g_tabs) if (t->group == g.id && !t->privateTab && t->state == TabState::Live) saved = true;
        if (!saved) continue;
        std::string name = ToUtf8(g.name);
        for (auto& c : name) if (c == '\r' || c == '\n') c = ' ';
        out += "group " + std::to_string(g.id) + " " + std::to_string(g.color) + " " + std::to_string((g.collapsed ? 1 : 0) | (g.stacked ? 2 : 0)) + " " + name + "\n";
    }
    return out;
}

bool TabGroupsSessionParse(const std::string& s, TabGroup* out) {
    unsigned id = 0;
    int color = 0, collapsed = 0, used = 0;
    if (sscanf_s(s.c_str(), "%u %d %d%n", &id, &color, &collapsed, &used) < 3 || !id) return false;
    out->id = id;
    out->color = std::clamp(color, 0, kTabGroupColorCount - 1);
    out->collapsed = (collapsed & 1) != 0;
    out->stacked = (collapsed & 2) != 0;
    std::string name = s.substr(std::min((size_t)used, s.size()));
    if (!name.empty() && name[0] == ' ') name.erase(0, 1);
    out->name.clear();
    if (!name.empty()) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, name.data(), (int)name.size(), nullptr, 0);
        out->name.resize(n);
        MultiByteToWideChar(CP_UTF8, 0, name.data(), (int)name.size(), &out->name[0], n);
    }
    return true;
}

void TabGroupsRestore(std::vector<TabGroup> groups) {
    for (auto& g : groups) {
        if (FindTabGroup(g.id)) continue;
        g_groups.push_back(g);
        s_nextGroupId = std::max(s_nextGroupId, g.id + 1);
    }
    TabGroupsNormalize();
}
