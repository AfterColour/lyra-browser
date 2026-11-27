// Lyra — рабочие пространства: отдельные наборы вкладок (со своими закреплёнными), переключатель.
#include "browser/Workspaces.h"
#include "app/Log.h"
#include "browser/SplitView.h"
#include "browser/Tab.h"
#include "browser/TabGroups.h"
#include "browser/TabManager.h"
#include "core/Constants.h"
#include "core/Localization.h"
#include "core/StringUtils.h"
#include "ui/Chrome.h"
#include "ui/Commands.h"
#include "ui/Dialogs.h"
#include "ui/MainWindow.h"
#include "ui/Menu.h"
#include "ui/WindowFrame.h"

std::vector<Workspace> g_workspaces;
unsigned g_workspace = 0;
static unsigned s_nextId = 1;

namespace {

struct IconDef { const wchar_t* glyph; };
const IconDef kIcons[kWorkspaceIconCount] = {
    { L"\uE8A9" },   // workspaces (a house would pass for the Home button)
    { L"" },   // briefcase
    { L"" },   // code
    { L"" },   // game
    { L"" },   // music
    { L"" },   // cart
    { L"" },   // heart
    { L"" },   // star
    { L"" },   // globe
    { L"" },   // book
    { L"" },   // palette
    { L"" },   // people
};

std::wstring DefaultName(size_t n) {
    if (n == 0) return Tr(L"Main", L"Основное");
    return std::wstring(Tr(L"Workspace ", L"Пространство ")) + std::to_wstring(n + 1);
}

void EnsureOne() {
    if (g_workspaces.empty()) {
        Workspace w;
        w.id = s_nextId++;
        w.name = DefaultName(0);
        g_workspaces.push_back(w);
    }
    if (!FindWorkspace(g_workspace)) g_workspace = g_workspaces.front().id;
}

// The order key: the active workspace's tabs first, then the others in the order of the list;
// in each, the pinned tabs first.
int Rank(const Tab& t) {
    const int ws = t.workspace == g_workspace ? 0 : 1 + std::max(0, WorkspaceIndex(t.workspace));
    return ws * 2 + (t.pinned ? 0 : 1);
}

void Changed() {
    RefreshLayout(true);
    InvalidateChrome();
}

bool Ask(const std::wstring& title, std::wstring& value, const std::wstring& message) {
    PromptState st;
    st.message = message;
    st.value = value;
    if (!PromptDialog(g_hWnd, title, st) || !st.ok) return false;
    std::wstring v = TrimWs(st.value);
    if (v.empty()) return false;
    if (v.size() > 40) v.resize(40);
    value = v;
    return true;
}

} // namespace

const wchar_t* WorkspaceIconGlyph(int icon) {
    return kIcons[std::clamp(icon, 0, kWorkspaceIconCount - 1)].glyph;
}

Workspace* FindWorkspace(unsigned id) {
    for (auto& w : g_workspaces) if (w.id == id) return &w;
    return nullptr;
}

Workspace& ActiveWorkspace() {
    EnsureOne();
    return *FindWorkspace(g_workspace);
}

int WorkspaceIndex(unsigned id) {
    for (size_t i = 0; i < g_workspaces.size(); ++i) if (g_workspaces[i].id == id) return (int)i;
    return -1;
}

int WsTabEnd() {
    int n = 0;
    while (n < (int)g_tabs.size() && g_tabs[n]->workspace == g_workspace) ++n;
    return n;
}

bool TabInActiveWorkspace(int index) {
    return index >= 0 && index < (int)g_tabs.size() && g_tabs[index]->workspace == g_workspace;
}

bool TabHidden(int index) {
    if (index < 0 || index >= (int)g_tabs.size()) return false;
    return g_tabs[index]->workspace != g_workspace || TabHiddenByGroup(index);
}

void WorkspacesNormalize() {
    EnsureOne();
    for (auto& t : g_tabs)
        if (!FindWorkspace(t->workspace)) t->workspace = g_workspace;
    auto less = [](const std::unique_ptr<Tab>& a, const std::unique_ptr<Tab>& b) { return Rank(*a) < Rank(*b); };
    if (std::is_sorted(g_tabs.begin(), g_tabs.end(), less)) return;
    const unsigned long long activeId = ActiveTab() ? ActiveTab()->id : 0;
    std::stable_sort(g_tabs.begin(), g_tabs.end(), less);
    if (activeId) FindTab(activeId, &g_active);
    g_hover = Hit();
    g_down = Hit();
}

// The workspace becomes the active one; the open tab stays what it was (SelectTab follows).
static void SetActive(unsigned id) {
    if (!FindWorkspace(id) || id == g_workspace) return;
    if (Tab* t = ActiveTab()) if (Workspace* old = FindWorkspace(g_workspace)) if (t->workspace == old->id) old->lastTab = t->id;
    g_workspace = id;
    WorkspacesNormalize();
}

int WorkspaceActivateFor(int tabIndex) {
    if (tabIndex < 0 || tabIndex >= (int)g_tabs.size()) return tabIndex;
    Tab* t = g_tabs[tabIndex].get();
    if (t->workspace == g_workspace) return tabIndex;
    const unsigned long long id = t->id;
    Log("workspace %u -> %u (for tab#%llu)", g_workspace, t->workspace, id);
    SetActive(t->workspace);
    int at = tabIndex;
    FindTab(id, &at);
    return at;
}

void WorkspaceSwitch(unsigned id) {
    Workspace* w = FindWorkspace(id);
    if (!w || id == g_workspace) return;
    Log("workspace %u -> %u", g_workspace, id);
    SetActive(id);
    w = FindWorkspace(id);
    int at = -1;
    if (w->lastTab && FindTab(w->lastTab, &at) && at < WsTabEnd()) {
        SelectTab(at);
    } else if (WsTabEnd() > 0) {
        SelectTab(0);
    } else {
        RunCommand(CMD_NEWTAB);                          // nothing in it yet: a new tab
    }
    RefreshLayout(false);                                // the other list at once, no glide
    InvalidateChrome();
}

void WorkspaceSwitchByIndex(int index) {
    if (index >= 0 && index < (int)g_workspaces.size()) WorkspaceSwitch(g_workspaces[index].id);
}

void WorkspaceSwitchRelative(int delta) {
    const int n = (int)g_workspaces.size();
    if (n < 2) return;
    const int i = std::max(0, WorkspaceIndex(g_workspace));
    WorkspaceSwitchByIndex(((i + delta) % n + n) % n);
}

void WorkspaceNew() {
    EnsureOne();
    std::wstring name = DefaultName(g_workspaces.size());
    if (!Ask(Tr(L"New workspace", L"Новое пространство"), name, Tr(L"Name:", L"Название:"))) return;
    Workspace w;
    w.id = s_nextId++;
    w.name = name;
    w.icon = (int)(g_workspaces.size() % kWorkspaceIconCount);
    g_workspaces.push_back(w);
    Log("workspace %u created", w.id);
    WorkspaceSwitch(w.id);
}

void WorkspaceRename(unsigned id) {
    Workspace* w = FindWorkspace(id);
    if (!w) return;
    std::wstring name = w->name;
    if (!Ask(Tr(L"Rename workspace", L"Переименовать пространство"), name, Tr(L"Name:", L"Название:"))) return;
    if ((w = FindWorkspace(id))) w->name = name;
    InvalidateChrome();
}

void WorkspaceSetIcon(unsigned id, int icon) {
    if (Workspace* w = FindWorkspace(id)) w->icon = std::clamp(icon, 0, kWorkspaceIconCount - 1);
    InvalidateChrome();
}

void WorkspaceDelete(unsigned id) {
    Workspace* w = FindWorkspace(id);
    if (!w || g_workspaces.size() < 2) return;
    std::vector<unsigned long long> ids;
    for (auto& t : g_tabs) if (t->workspace == id && t->state == TabState::Live) ids.push_back(t->id);
    if (!ids.empty()) {
        const std::wstring msg = std::wstring(Tr(L"Delete the workspace \x201C", L"Удалить пространство \x00AB")) + w->name +
            Tr(L"\x201D and close its ", L"\x00BB и закрыть его вкладки (") + std::to_wstring(ids.size()) +
            Tr(L" tab(s)?", L")?");
        if (MessageBoxW(g_hWnd, msg.c_str(), kAppName, MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    }
    if (id == g_workspace) {                             // to the one before it (or after it)
        const int i = WorkspaceIndex(id);
        WorkspaceSwitch(g_workspaces[i > 0 ? i - 1 : 1].id);
        if (g_workspace == id) return;
    }
    Log("workspace %u deleted (%zu tab(s))", id, ids.size());
    for (unsigned long long tid : ids) {
        int at = -1;
        if (FindTab(tid, &at)) CloseTab(at);
    }
    // Tabs it still has (a page kept one open): to the active workspace.
    for (auto& t : g_tabs) if (t->workspace == id) t->workspace = g_workspace;
    const int i = WorkspaceIndex(id);
    if (i >= 0) g_workspaces.erase(g_workspaces.begin() + i);
    WorkspacesNormalize();
    Changed();
}

void WorkspaceMoveTab(int tabIndex, unsigned id) {
    if (tabIndex < 0 || tabIndex >= (int)g_tabs.size() || !FindWorkspace(id)) return;
    Tab* t = g_tabs[tabIndex].get();
    if (t->workspace == id) return;
    const unsigned long long tid = t->id;
    if (SplitHas(*t)) SplitClose();
    // The open tab leaves: the one next to it opens (a new tab, if it was the only one).
    if (tabIndex == g_active) {
        const int end = WsTabEnd();
        if (end <= 1) RunCommand(CMD_NEWTAB);
        else SelectTab(tabIndex + 1 < end ? tabIndex + 1 : tabIndex - 1);
        if (!FindTab(tid, &tabIndex)) return;
        t = g_tabs[tabIndex].get();
    }
    Log("tab#%llu -> workspace %u", tid, id);
    t->group = 0;                                        // (its group stays where it was)
    t->workspace = id;
    MoveTab(tabIndex, (int)g_tabs.size() - 1);           // at the end of that workspace's tabs
    WorkspacesNormalize();
    if (Workspace* w = FindWorkspace(id)) if (!w->lastTab) w->lastTab = tid;
    Changed();
}

// ---- menus ----------------------------------------------------------------------------------
void WorkspaceMenu(POINT screen) {
    EnsureOne();
    enum : UINT { kNew = 1, kRename, kDelete, kNext, kPrev, kSwitch0 = 100, kIcon0 = 200 };
    HMENU m = CreatePopupMenu();
    for (size_t i = 0; i < g_workspaces.size(); ++i) {
        const Workspace& w = g_workspaces[i];
        std::wstring text = w.name;
        if (i < 9) text += L"\tCtrl+Alt+" + std::to_wstring(i + 1);
        AppendMenuW(m, MF_STRING | (w.id == g_workspace ? MF_CHECKED : 0), kSwitch0 + (UINT)i, text.c_str());
        MenuSetIcon(m, kSwitch0 + (UINT)i, WorkspaceIconGlyph(w.icon));
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kNew, Tr(L"New workspace\x2026", L"Новое пространство\x2026"));
    MenuSetIcon(m, kNew, L"");
    const Workspace& cur = ActiveWorkspace();
    AppendMenuW(m, MF_STRING, kRename, Tr(L"Rename\x2026", L"Переименовать\x2026"));
    MenuSetIcon(m, kRename, L"");
    HMENU icons = CreatePopupMenu();
    static const wchar_t* const kIconNames[kWorkspaceIconCount][2] = {
        { L"General", L"Общее" }, { L"Work", L"Работа" }, { L"Code", L"Код" }, { L"Games", L"Игры" },
        { L"Music", L"Музыка" }, { L"Shopping", L"Покупки" }, { L"Heart", L"Сердце" }, { L"Star", L"Звезда" },
        { L"Globe", L"Глобус" }, { L"Book", L"Книга" }, { L"Art", L"Творчество" }, { L"People", L"Люди" } };
    for (int k = 0; k < kWorkspaceIconCount; ++k) {
        AppendMenuW(icons, MF_STRING | (k == cur.icon ? MF_CHECKED : 0), kIcon0 + k, Tr(kIconNames[k][0], kIconNames[k][1]));
        MenuSetIcon(icons, kIcon0 + k, WorkspaceIconGlyph(k));
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR)icons, Tr(L"Icon", L"Значок"));
    AppendMenuW(m, MF_STRING | (g_workspaces.size() < 2 ? MF_GRAYED : 0), kDelete, Tr(L"Delete workspace", L"Удалить пространство"));
    MenuSetIcon(m, kDelete, L"");
    const unsigned curId = cur.id;
    const int cmd = MenuTrack(m, TPM_LEFTALIGN | TPM_TOPALIGN, screen.x, screen.y, g_hWnd);
    DestroyMenu(m);
    if (cmd >= (int)kSwitch0 && cmd < (int)kSwitch0 + (int)g_workspaces.size()) WorkspaceSwitchByIndex(cmd - kSwitch0);
    else if (cmd == kNew) WorkspaceNew();
    else if (cmd == kRename) WorkspaceRename(curId);
    else if (cmd == kDelete) WorkspaceDelete(curId);
    else if (cmd >= (int)kIcon0 && cmd < (int)kIcon0 + kWorkspaceIconCount) WorkspaceSetIcon(curId, cmd - kIcon0);
}

void WorkspacesAppendTabMenu(HMENU menu, int tabIndex, UINT firstId) {
    if (tabIndex < 0 || tabIndex >= (int)g_tabs.size() || g_workspaces.size() < 2) return;
    const Tab& t = *g_tabs[tabIndex];
    if (t.privateTab) return;
    HMENU sub = CreatePopupMenu();
    for (size_t i = 0; i < g_workspaces.size() && i < kWorkspaceTabMenuIds; ++i) {
        if (g_workspaces[i].id == t.workspace) continue;
        AppendMenuW(sub, MF_STRING, firstId + (UINT)i, g_workspaces[i].name.c_str());
        MenuSetIcon(sub, firstId + (UINT)i, WorkspaceIconGlyph(g_workspaces[i].icon));
    }
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, Tr(L"Move to workspace", L"Переместить в пространство"));
}

bool WorkspacesTabMenuCommand(UINT cmd, int tabIndex, UINT firstId) {
    if (cmd < firstId || cmd >= firstId + kWorkspaceTabMenuIds) return false;
    const size_t i = cmd - firstId;
    if (i < g_workspaces.size()) WorkspaceMoveTab(tabIndex, g_workspaces[i].id);
    return true;
}

// ---- session --------------------------------------------------------------------------------
std::string WorkspacesSessionLines() {
    EnsureOne();
    std::string out;
    for (auto& w : g_workspaces) {
        std::string name = ToUtf8(w.name);
        for (auto& c : name) if (c == '\r' || c == '\n') c = ' ';
        out += "workspace " + std::to_string(w.id) + " " + (w.id == g_workspace ? "1" : "0") + " " +
               std::to_string(w.icon) + " " + name + "\n";
    }
    return out;
}

bool WorkspacesSessionParse(const char* value, Workspace* out, bool* active) {
    unsigned id = 0;
    int act = 0, icon = 0, used = 0;
    if (sscanf_s(value, "%u %d %d%n", &id, &act, &icon, &used) < 3 || !id) return false;
    out->id = id;
    out->icon = std::clamp(icon, 0, kWorkspaceIconCount - 1);
    std::string name = value + std::min((size_t)used, strlen(value));
    if (!name.empty() && name[0] == ' ') name.erase(0, 1);
    out->name = ToWide(name);
    if (active) *active = act != 0;
    return true;
}

void WorkspacesRestore(const std::vector<Workspace>& list, unsigned active) {
    if (list.empty()) { EnsureOne(); return; }
    // The tabs made so far went into the first workspace (the default one): they keep the ids
    // the session gave them, which the caller set.
    g_workspaces = list;
    for (auto& w : g_workspaces) { w.lastTab = 0; s_nextId = std::max(s_nextId, w.id + 1); }
    g_workspace = FindWorkspace(active) ? active : g_workspaces.front().id;
    WorkspacesNormalize();
}
