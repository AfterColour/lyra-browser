// Lyra — палитра команд (Ctrl+K): одно поле для вкладок, команд, закладок и истории.
#include "ui/CommandPalette.h"
#include "app/Log.h"
#include "browser/Bookmarks.h"
#include "browser/Favicons.h"
#include "browser/History.h"
#include "browser/StartPage.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "browser/TabSleep.h"
#include "browser/SplitView.h"
#include "core/Constants.h"
#include "core/Localization.h"
#include "core/Settings.h"
#include "core/StringUtils.h"
#include "core/Url.h"
#include "ui/Animation.h"
#include "ui/Chrome.h"
#include "ui/Commands.h"
#include "ui/Sidebar.h"
#include "ui/Draw.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/settings/SettingsPages.h"
#include "app/BugReport.h"

namespace {

struct Row {
    enum Kind { TabRow, Command, BookmarkRow, HistoryRow, Search, Go, ClosedRow } kind = Command;
    std::wstring title, detail, url;
    unsigned long long tabId = 0;
    int closedIndex = -1;                                // ClosedRow: the index in g_closedTabs
    std::function<void()> run;
};

struct CommandDef {
    const wchar_t *en, *ru, *keys, *shortcut;
    std::function<void()> run;
    const wchar_t* glyph;
    bool (*shown)() = nullptr;                           // listed only when this says so
};

HWND s_wnd = nullptr, s_edit = nullptr;
WNDPROC s_oldEdit = nullptr;
HFONT s_editFont = nullptr;
HBRUSH s_bgBrush = nullptr;
int s_editH = 20;
std::vector<Row> s_rows;
int s_sel = 0, s_hover = -1;
bool s_tracking = false;
bool s_tabsMode = false;                                 // Ctrl+Shift+A: only tabs
int s_top = 0;                                           // the first row shown (the list scrolls)
constexpr int kEditId = 4001;
constexpr int kMaxRows = 9;
constexpr int kVisibleTabRows = 10;
int VisibleRows() { return std::min((int)s_rows.size(), s_tabsMode ? kVisibleTabRows : kMaxRows); }

float FieldH() { return std::floor(S(52)); }
float RowH() { return std::floor(S(40)); }
float Pad() { return std::floor(S(6)); }
Color Bg() { return g_dark ? Color(37, 39, 44) : Color(255, 255, 255); }

const std::vector<CommandDef>& Commands() {
    static std::vector<CommandDef> list = {
        { L"New tab", L"Новая вкладка", L"open create", L"Ctrl+T", [] { RunCommand(CMD_NEWTAB); }, L"\uE710" },
        { L"New private tab", L"Новая приватная вкладка", L"incognito private инкогнито", L"Ctrl+Shift+N", [] { RunCommand(CMD_NEWPRIVATE); }, L"\uE72E" },
        { L"Reopen closed tab", L"Вернуть закрытую вкладку", L"restore undo восстановить", L"Ctrl+Shift+T", [] { RunCommand(CMD_REOPENTAB); }, L"\uE7A7" },
        { L"Search tabs", L"Поиск по вкладкам", L"tabs list find вкладки список найти", L"Ctrl+Shift+A", [] { PostMessageW(g_hWnd, WM_COMMAND, CMD_TABSEARCH, 0); }, L"\uE7C4" },
        { L"Close tab", L"Закрыть вкладку", L"", L"Ctrl+W", [] { RunCommand(CMD_CLOSETAB); }, L"\uE711" },
        { L"Reload page", L"Обновить страницу", L"refresh обновить перезагрузить", L"Ctrl+R", [] { ReloadActive(false); }, L"\uE72C" },
        { L"Find on page", L"Найти на странице", L"search поиск", L"Ctrl+F", [] { RunCommand(CMD_FIND); }, L"\uE721" },
        { L"Print", L"Печать", L"pdf печать распечатать", L"Ctrl+P", [] { RunCommand(CMD_PRINT); }, L"\uE749" },
        { L"Screenshot", L"Снимок страницы", L"screenshot capture picture скриншот снимок картинка", L"Ctrl+Shift+S", [] { PostMessageW(g_hWnd, WM_COMMAND, CMD_SCREENSHOT, 0); }, L"\uE722" },
        { L"Bookmark this page", L"Добавить в закладки", L"star favorite избранное", L"Ctrl+D", [] { RunCommand(CMD_BOOKMARK_PAGE); }, L"\uE734" },
        { L"Copy page address", L"Копировать адрес страницы", L"url link ссылка", L"", [] { RunCommand(CMD_COPYURL); }, L"\uE8C8" },
        { L"History", L"История", L"visited посещённые", L"Ctrl+H", [] { RunCommand(CMD_HISTORY); }, L"\uE81C" },
        { L"Bookmarks", L"Закладки", L"favorites избранное", L"Ctrl+Shift+O", [] { RunCommand(CMD_BOOKMARKS); }, L"\uE728" },
        { L"Show or hide bookmarks bar", L"Показать или скрыть панель закладок", L"bookmarks bar toolbar панель закладок", L"Ctrl+Shift+B", [] { RunCommand(CMD_BOOKMARKSBAR); }, L"\uE728" },
    { L"Show or hide sidebar", L"Показать или скрыть боковую панель", L"sidebar panel боковая панель", L"Ctrl+Shift+E", [] { RunCommand(CMD_SIDEBAR); }, L"\uE8A0" },
    { L"Add page to reading list", L"Добавить страницу в список для чтения", L"reading list read later читать позже", L"", [] { RunCommand(CMD_READINGLIST); }, L"\uE736" },
    { L"Notes", L"Заметки", L"notes заметки", L"", [] { SidebarOpen(4); }, L"\uE70B" },
        { L"Downloads", L"Загрузки", L"files файлы", L"Ctrl+J", [] { RunCommand(CMD_DOWNLOADS); }, L"\uE896" },
        { L"Settings", L"Настройки", L"preferences options параметры", L"Ctrl+,", [] { RunCommand(CMD_SETTINGS); }, L"\uE713" },
        { L"Saved passwords", L"Сохранённые пароли", L"logins пароли логины", L"", [] { RunCommand(CMD_PASSWORDS); }, L"\uE8D7" },
        { L"Report a problem", L"Сообщить о проблеме", L"bug issue crash freeze report ошибка баг зависает вылет", L"", [] { ReportProblem(g_hWnd); }, L"" },
        { L"Clear browsing data", L"Очистить данные браузера", L"cookies cache history кэш куки удалить", L"Ctrl+Shift+Del", [] { OpenClearDataSettings(); }, L"\uE74D" },
        { L"Reader view", L"Режим чтения", L"article статья читать", L"F9", [] { RunCommand(CMD_READER); }, L"\uE736" },
        { L"Translate page", L"Перевести страницу", L"translation перевод", L"", [] { RunCommand(CMD_TRANSLATE); }, L"\uF2B7" },
        { L"Picture in picture", L"Картинка в картинке", L"video pip видео", L"", [] { RunCommand(CMD_PIP); }, L"\uE8B9" },
        { L"Zoom in", L"Увеличить масштаб", L"bigger крупнее", L"Ctrl++", [] { RunCommand(CMD_ZOOMIN); }, L"\uE8A3" },
        { L"Zoom out", L"Уменьшить масштаб", L"smaller мельче", L"Ctrl+-", [] { RunCommand(CMD_ZOOMOUT); }, L"\uE71F" },
        { L"Reset zoom", L"Сбросить масштаб", L"100%", L"Ctrl+0", [] { RunCommand(CMD_ZOOMRESET); }, L"\uE71E" },
        { L"Split view with new tab", L"Разделённый экран с новой вкладкой", L"split side two рядом две половины", L"", [] { SplitWithNewTab(); }, L"\uE8A0", [] { return !SplitShown() && ActiveTab() && !ActiveTab()->pinned; } },
        { L"Swap split view sides", L"Поменять половины экрана местами", L"split swap разделённый", L"", [] { SplitSwap(); }, L"\uE8AB", [] { return SplitShown(); } },
        { L"Close split view", L"Закрыть разделённый экран", L"split разделённый", L"", [] { SplitClose(); }, L"\uE8A0", [] { return SplitShown(); } },
        { L"Pin tab", L"Закрепить вкладку", L"pin закрепить прикрепить", L"", [] { SetTabPinned(g_active, true); }, L"\uE718",
          [] { return ActiveTab() && !ActiveTab()->pinned; } },
        { L"Unpin tab", L"Открепить вкладку", L"unpin pin открепить", L"", [] { SetTabPinned(g_active, false); }, L"\uE77A",
          [] { return ActiveTab() && ActiveTab()->pinned; } },
        { L"Put other tabs to sleep", L"Усыпить остальные вкладки", L"memory sleep память усыпить", L"", [] { SleepOtherTabs(); }, L"\uE708" },
        { L"Block ads on this site (on / off)", L"Блокировка рекламы на этом сайте (вкл. / выкл.)", L"adblock ad реклама", L"", [] { RunCommand(CMD_ADBLOCK_SITE); }, L"\uE72E" },
        { L"Make this page the home page", L"Сделать домашней страницей", L"home домашняя", L"", [] { RunCommand(CMD_SETHOME); }, L"\uE80F" },
        { L"Developer tools", L"Инструменты разработчика", L"devtools inspector console консоль", L"F12", [] { RunCommand(CMD_DEVTOOLS); }, L"\uE943" },
    };
    return list;
}

std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}

// Every word typed must be in `hay` (lowercase). Score: the title starting with it ranks first,
// then a word of the title starting with it.
int MatchScore(const std::vector<std::wstring>& words, const std::wstring& hay, const std::wstring& titleLower) {
    if (words.empty()) return 1;
    int score = 1;
    for (const auto& w : words) {
        const size_t at = hay.find(w);
        if (at == std::wstring::npos) return 0;
        const size_t t = titleLower.find(w);
        if (t == 0) score += 4;
        else if (t != std::wstring::npos && !iswalnum(titleLower[t - 1])) score += 2;
    }
    return score;
}

bool LooksLikeAddress(const std::wstring& s) {
    if (s.find(L' ') != std::wstring::npos) return false;
    return s.find(L"://") != std::wstring::npos || s.compare(0, 9, L"localhost") == 0 ||
           (s.find(L'.') != std::wstring::npos && s.front() != L'.' && s.back() != L'.');
}

std::wstring EngineName() {
    for (int i = 0; i < kEngineCount; ++i) if (g_searchEngine == kEngines[i].url) return kEngines[i].name;
    return HostOf(g_searchEngine);
}

// Tab search: the open tabs in their order, then the recently closed ones (newest first).
void RebuildTabs(const std::vector<std::wstring>& words) {
    const Tab* active = ActiveTab();
    std::vector<std::pair<int, Row>> open, closed;
    for (auto& tp : g_tabs) {
        const Tab& t = *tp;
        if (t.state != TabState::Live) continue;
        const std::wstring url = t.lazyPending ? t.lazyUrl : t.url;
        const std::wstring title = t.title.empty() ? HostOf(url) : t.title;
        const int sc = MatchScore(words, Lower(title + L" " + url), Lower(title));
        if (!sc) continue;
        Row r;
        r.kind = Row::TabRow;
        r.title = title;
        r.url = url;
        r.tabId = t.id;
        std::wstring detail = IsInternalPageUrl(url) ? std::wstring(L"Lyra") : HostOf(url);
        if (&t == active) detail = Tr(L"Open now", L"Открыта сейчас");
        else if (t.audible && !t.muted) detail = Tr(L"Playing · ", L"Звук · ") + detail;
        else if (t.sleeping || t.lazyPending) detail = Tr(L"Not loaded · ", L"Не загружена · ") + detail;
        r.detail = detail;
        open.push_back({ sc, r });
    }
    for (int k = (int)g_closedTabs.size() - 1, n = 0; k >= 0 && n < 15; --k, ++n) {
        const ClosedTab& c = g_closedTabs[k];
        const std::wstring title = c.title.empty() ? HostOf(c.url) : c.title;
        const int sc = MatchScore(words, Lower(title + L" " + c.url), Lower(title));
        if (!sc) continue;
        Row r;
        r.kind = Row::ClosedRow;
        r.title = title;
        r.url = c.url;
        r.closedIndex = k;
        r.detail = Tr(L"Closed · ", L"Закрыта · ") + HostOf(c.url);
        closed.push_back({ sc, r });
    }
    if (!words.empty()) {
        auto byScore = [](const std::pair<int, Row>& a, const std::pair<int, Row>& b) { return a.first > b.first; };
        std::stable_sort(open.begin(), open.end(), byScore);
        std::stable_sort(closed.begin(), closed.end(), byScore);
    }
    for (auto& o : open) s_rows.push_back(o.second);
    for (auto& c : closed) s_rows.push_back(c.second);
    s_sel = s_rows.empty() ? -1 : 0;
    if (words.empty())                                   // nothing typed: start at the open tab
        for (int i = 0; i < (int)s_rows.size(); ++i)
            if (s_rows[i].kind == Row::TabRow && active && s_rows[i].tabId == active->id) { s_sel = i; break; }
    s_hover = -1;
    s_top = std::max(0, std::min(s_sel - kVisibleTabRows / 2, (int)s_rows.size() - kVisibleTabRows));
}

void Rebuild() {
    wchar_t buf[512] = {};
    if (s_edit) GetWindowTextW(s_edit, buf, ARRAYSIZE(buf));
    const std::wstring text = TrimWs(buf), lower = Lower(text);
    std::vector<std::wstring> words;
    for (size_t i = 0; i < lower.size();) {
        while (i < lower.size() && iswspace(lower[i])) ++i;
        size_t j = i;
        while (j < lower.size() && !iswspace(lower[j])) ++j;
        if (j > i) words.push_back(lower.substr(i, j - i));
        i = j;
    }
    s_rows.clear();
    s_top = 0;
    if (s_tabsMode) { RebuildTabs(words); return; }
    const Tab* active = ActiveTab();

    // Open tabs (not the one already open).
    std::vector<std::pair<int, Row>> tabs;
    for (auto& tp : g_tabs) {
        const Tab& t = *tp;
        if (&t == active || t.state != TabState::Live) continue;
        const std::wstring title = t.title.empty() ? HostOf(t.url) : t.title;
        const int sc = MatchScore(words, Lower(title + L" " + t.url), Lower(title));
        if (!sc) continue;
        Row r;
        r.kind = Row::TabRow;
        r.title = title;
        r.url = t.url;
        r.tabId = t.id;
        r.detail = Tr(L"Switch to tab", L"Перейти на вкладку");
        tabs.push_back({ sc, r });
    }
    // Commands, in both languages.
    std::vector<std::pair<int, Row>> cmds;
    for (const CommandDef& c : Commands()) {
        if (c.shown && !c.shown()) continue;
        const std::wstring title = Tr(c.en, c.ru);
        const int sc = MatchScore(words, Lower(std::wstring(c.en) + L" " + c.ru + L" " + c.keys), Lower(title));
        if (!sc) continue;
        Row r;
        r.kind = Row::Command;
        r.title = title;
        r.detail = c.shortcut;
        r.url = c.glyph;                                 // (commands: the glyph)
        r.run = c.run;
        cmds.push_back({ sc, r });
    }
    auto byScore = [](const std::pair<int, Row>& a, const std::pair<int, Row>& b) { return a.first > b.first; };
    std::stable_sort(tabs.begin(), tabs.end(), byScore);
    std::stable_sort(cmds.begin(), cmds.end(), byScore);

    if (words.empty()) {
        // Nothing typed: the other tabs, then the commands used most.
        for (size_t i = 0; i < tabs.size() && s_rows.size() < 4; ++i) s_rows.push_back(tabs[i].second);
        const wchar_t* top[] = { L"New tab", L"History", L"Bookmarks", L"Downloads", L"Settings", L"Reopen closed tab", L"Print", L"Find on page" };
        for (const wchar_t* name : top) {
            if ((int)s_rows.size() >= kMaxRows) break;
            for (const CommandDef& c : Commands())
                if (wcscmp(c.en, name) == 0) {
                    Row r; r.kind = Row::Command; r.title = Tr(c.en, c.ru); r.detail = c.shortcut; r.url = c.glyph; r.run = c.run;
                    s_rows.push_back(r);
                }
        }
    } else {
        for (size_t i = 0; i < tabs.size() && i < 3; ++i) s_rows.push_back(tabs[i].second);
        for (size_t i = 0; i < cmds.size() && i < 4; ++i) s_rows.push_back(cmds[i].second);
        auto seen = [](const std::wstring& url) {
            for (auto& r : s_rows) if ((r.kind == Row::TabRow || r.kind == Row::BookmarkRow || r.kind == Row::HistoryRow) && r.url == url) return true;
            return false;
        };
        for (const ::Bookmark& b : BookmarksMatch(text, 3)) {
            if ((int)s_rows.size() >= kMaxRows - 1) break;
            if (seen(b.url)) continue;
            Row r; r.kind = Row::BookmarkRow; r.url = b.url; r.title = b.title.empty() ? HostOf(b.url) : b.title;
            r.detail = DisplayUrl(b.url);
            s_rows.push_back(r);
        }
        for (const HistoryEntry& e : HistoryMatch(text, 6)) {
            if ((int)s_rows.size() >= kMaxRows - 1) break;
            if (seen(e.url)) continue;
            Row r; r.kind = Row::HistoryRow; r.url = e.url; r.title = e.title.empty() ? HostOf(e.url) : e.title;
            r.detail = DisplayUrl(e.url);
            s_rows.push_back(r);
        }
        Row last;
        last.kind = LooksLikeAddress(text) ? Row::Go : Row::Search;
        last.title = text;
        last.url = text;
        last.detail = last.kind == Row::Go ? std::wstring(Tr(L"Go to address", L"Перейти по адресу"))
                                           : Tr(L"Search with ", L"Найти в ") + EngineName();
        s_rows.push_back(last);
    }
    if ((int)s_rows.size() > kMaxRows) s_rows.resize(kMaxRows);
    s_sel = s_rows.empty() ? -1 : 0;
    s_hover = -1;
}

void Place() {
    RECT cr;
    GetClientRect(g_hWnd, &cr);
    const float w = std::min(S(640), (float)cr.right - S(40));
    const float h = FieldH() + (s_rows.empty() ? 0.f : Pad() * 2 + RowH() * (float)VisibleRows());
    POINT tl = { (LONG)((cr.right - w) / 2), (LONG)(g_L.chromeH + S(12)) };
    ClientToScreen(g_hWnd, &tl);
    SetWindowPos(s_wnd, HWND_TOP, tl.x, tl.y, (int)w, (int)h, SWP_NOACTIVATE);
    MoveWindow(s_edit, (int)S(48), (int)((FieldH() - s_editH) / 2), (int)(w - S(64)), s_editH, TRUE);
    InvalidateRect(s_wnd, nullptr, FALSE);
}

void Close() {
    if (!s_wnd) return;
    HWND w = s_wnd;
    s_wnd = nullptr;
    s_edit = nullptr;
    PopupFadeOut(w);
    DestroyWindow(w);
}

void Run(int i) {
    if (i < 0 || i >= (int)s_rows.size()) return;
    const Row r = s_rows[i];
    Close();
    SetForegroundWindow(g_hWnd);
    Log("palette: %s", ToUtf8(r.title).c_str());
    switch (r.kind) {
    case Row::TabRow: { int index = -1; if (FindTab(r.tabId, &index)) SelectTab(index); break; }
    case Row::Command: if (r.run) r.run(); break;
    case Row::BookmarkRow: case Row::HistoryRow: NavigateActive(r.url); FocusWebView(); break;
    case Row::Search: case Row::Go: NavigateActive(r.url); FocusWebView(); break;
    case Row::ClosedRow: if (r.closedIndex >= 0 && r.closedIndex < (int)g_closedTabs.size()) ReopenClosedTab(r.closedIndex); break;
    }
}

int RowAt(int y) {
    const int i = (int)std::floor(((float)y - FieldH() - Pad()) / RowH());
    return (y >= FieldH() + Pad() && i >= 0 && i < VisibleRows() && s_top + i < (int)s_rows.size()) ? s_top + i : -1;
}

void KeepSelectionShown() {
    const int vis = VisibleRows();
    if (s_sel < s_top) s_top = s_sel;
    else if (s_sel >= s_top + vis) s_top = s_sel - vis + 1;
    s_top = std::max(0, std::min(s_top, (int)s_rows.size() - vis));
}

void Scroll(int rows) {
    const int vis = VisibleRows();
    s_top = std::max(0, std::min(s_top + rows, (int)s_rows.size() - vis));
    if (s_wnd) InvalidateRect(s_wnd, nullptr, FALSE);
}

// Tab search: Delete closes the selected open tab; the list stays.
void CloseSelectedTab() {
    if (!s_tabsMode || s_sel < 0 || s_sel >= (int)s_rows.size() || s_rows[s_sel].kind != Row::TabRow) return;
    int index = -1;
    if (!FindTab(s_rows[s_sel].tabId, &index)) return;
    const int keep = s_sel;
    CloseTab(index);
    if (!s_wnd) return;
    SetForegroundWindow(s_wnd);
    Rebuild();
    s_sel = s_rows.empty() ? -1 : std::min(keep, (int)s_rows.size() - 1);
    KeepSelectionShown();
    Place();
}

void Paint(HDC hdc) {
    RECT rc;
    GetClientRect(s_wnd, &rc);
    PaintBuffered(hdc, rc, true, [&](Graphics& g) {
        const Theme& T = g_th;
        const float w = (float)rc.right;
        SolidBrush bg(Bg());
        g.FillRectangle(&bg, 0.f, 0.f, w, (float)rc.bottom);
        DrawGlyph(g, L"\uE721", RectF(S(14), 0, S(22), FieldH()), T.text2, g_fIcon.get());
        if (s_rows.empty()) return;
        Pen sep(g_dark ? Color(54, 57, 64) : Color(228, 230, 235), 1.f);
        g.DrawLine(&sep, 0.f, FieldH() - 0.5f, w, FieldH() - 0.5f);
        StringFormat sf(StringFormat::GenericTypographic());
        sf.SetFormatFlags(sf.GetFormatFlags() | StringFormatFlagsNoWrap);
        sf.SetTrimming(StringTrimmingEllipsisCharacter);
        sf.SetLineAlignment(StringAlignmentCenter);
        const int vis = VisibleRows();
        for (int i = s_top; i < s_top + vis && i < (int)s_rows.size(); ++i) {
            const Row& r = s_rows[i];
            RectF row(Pad(), FieldH() + Pad() + RowH() * (float)(i - s_top), w - Pad() * 2, RowH());
            if (i == s_sel) FillRound(g, Color(g_dark ? 70 : 40, T.accent.GetR(), T.accent.GetG(), T.accent.GetB()), row, S(6));
            else if (i == s_hover) FillRound(g, T.hover, row, S(6));
            const float is = S(16);
            RectF icon(row.X + S(12), row.Y + (row.Height - is) / 2, is, is);
            Bitmap* fav = (r.kind == Row::TabRow || r.kind == Row::BookmarkRow || r.kind == Row::HistoryRow || r.kind == Row::ClosedRow) ? FaviconFor(r.url) : nullptr;
            if (fav) {
                g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
                g.DrawImage(fav, icon);
            } else {
                const wchar_t* glyph = r.kind == Row::Command ? r.url.c_str() : r.kind == Row::Search ? L"\uE721"
                                     : r.kind == Row::Go ? L"\uE774" : r.kind == Row::BookmarkRow ? L"\uE734"
                                     : r.kind == Row::HistoryRow ? L"\uE81C" : r.kind == Row::ClosedRow ? L"\uE7A7" : L"\uE737";
                DrawGlyph(g, glyph, icon, T.text2, g_fIconSm.get());
            }
            const float tx = icon.GetRight() + S(12), right = row.GetRight() - S(12);
            RectF dm;
            g.MeasureString(r.detail.c_str(), -1, g_fText.get(), RectF(0, 0, 10000, 1000), &sf, &dm);
            const float detailW = std::min(dm.Width + S(2), (right - tx) * 0.45f);
            SolidBrush tb(T.text), t2(T.text2);
            g.DrawString(r.title.c_str(), -1, g_fText.get(), RectF(tx, row.Y, right - tx - detailW - S(12), row.Height), &sf, &tb);
            std::unique_ptr<StringFormat> rfp(sf.Clone());
            StringFormat& rf = *rfp;
            rf.SetAlignment(StringAlignmentFar);
            g.DrawString(r.detail.c_str(), -1, g_fText.get(), RectF(right - detailW, row.Y, detailW, row.Height), &rf, &t2);
        }
        if ((int)s_rows.size() > vis) {                  // where the shown rows are in the list
            const float trackTop = FieldH() + Pad(), trackH = RowH() * (float)vis;
            const float thumbH = std::max(S(24), trackH * vis / (float)s_rows.size());
            const float thumbY = trackTop + (trackH - thumbH) * s_top / (float)std::max(1, (int)s_rows.size() - vis);
            FillRound(g, Color(g_dark ? 90 : 60, T.text2.GetR(), T.text2.GetG(), T.text2.GetB()), RectF(w - S(5), thumbY, S(3), thumbH), S(1.5f));
        }
    });
}

void Move(int d) {
    if (s_rows.empty()) return;
    const int n = (int)s_rows.size();
    s_sel = ((s_sel < 0 ? 0 : s_sel) + d + n) % n;
    KeepSelectionShown();
    InvalidateRect(s_wnd, nullptr, FALSE);
}

LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN) {
        switch (w) {
        case VK_DOWN: Move(1); return 0;
        case VK_UP: Move(-1); return 0;
        case VK_TAB: Move((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1); return 0;
        case VK_PRIOR: Move(-std::max(1, VisibleRows() - 1)); return 0;
        case VK_NEXT: Move(std::max(1, VisibleRows() - 1)); return 0;
        case VK_DELETE:
            if (s_tabsMode && GetWindowTextLengthW(h) == 0) { CloseSelectedTab(); return 0; }
            break;
        case VK_RETURN: Run(s_sel); return 0;
        case VK_ESCAPE: Close(); SetForegroundWindow(g_hWnd); FocusWebView(); return 0;
        }
        if (w == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) { SendMessageW(h, EM_SETSEL, 0, -1); return 0; }
        if (w == 'K' && (GetKeyState(VK_CONTROL) & 0x8000)) { Close(); return 0; }
        if (w == 'W' && (GetKeyState(VK_CONTROL) & 0x8000) && s_tabsMode) { CloseSelectedTab(); return 0; }
        if (w == 'A' && (GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_SHIFT) & 0x8000)) { Close(); return 0; }
    }
    if (m == WM_MOUSEWHEEL) { Scroll(GET_WHEEL_DELTA_WPARAM(w) > 0 ? -3 : 3); return 0; }
    if (m == WM_CHAR && w == 23) return 0;                // (Ctrl+W)
    if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE || w == VK_TAB || w == 1 || w == 11)) return 0;
    return CallWindowProcW(s_oldEdit, h, m, w, l);
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        Paint(hdc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wParam, ToColorRef(g_th.text));
        SetBkColor((HDC)wParam, ToColorRef(Bg()));
        return (LRESULT)s_bgBrush;
    case WM_COMMAND:
        if (LOWORD(wParam) == kEditId && HIWORD(wParam) == EN_CHANGE && s_wnd) { Rebuild(); Place(); }
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE && s_wnd == h) PostMessageW(h, WM_CLOSE, 0, 0);   // clicked elsewhere
        else if (s_edit) SetFocus(s_edit);
        return 0;
    case WM_CLOSE:
        if (s_wnd == h) Close();
        return 0;
    case WM_MOUSEMOVE: {
        const int i = RowAt(GET_Y_LPARAM(lParam));
        if (i != s_hover) { s_hover = i; InvalidateRect(h, nullptr, FALSE); }
        if (!s_tracking) { TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t); s_tracking = true; }
        return 0;
    }
    case WM_MOUSEWHEEL:
        Scroll(GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? -3 : 3);
        return 0;
    case WM_MOUSELEAVE:
        s_tracking = false;
        if (s_hover >= 0) { s_hover = -1; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONUP: {
        const int i = RowAt(GET_Y_LPARAM(lParam));
        if (i >= 0) Run(i);
        return 0;
    }
    case WM_DESTROY:
        if (s_editFont) { DeleteObject(s_editFont); s_editFont = nullptr; }
        if (s_bgBrush) { DeleteObject(s_bgBrush); s_bgBrush = nullptr; }
        return 0;
    }
    return DefWindowProcW(h, msg, wParam, lParam);
}

} // namespace

bool CommandPaletteVisible() { return s_wnd != nullptr; }

static void ShowPalette(bool tabs);

void CommandPaletteShow() { ShowPalette(false); }
void CommandPaletteShowTabs() { ShowPalette(true); }

static void ShowPalette(bool tabs) {
    if (s_wnd && s_tabsMode != tabs) Close();            // the other kind is open: this one instead
    s_tabsMode = tabs;
    if (s_wnd) { SetForegroundWindow(s_wnd); SetFocus(s_edit); SendMessageW(s_edit, EM_SETSEL, 0, -1); return; }
    static bool registered = false;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hi;
        wc.lpszClassName = L"LyraCommandPalette";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        if (!RegisterClassExW(&wc)) return;
        registered = true;
    }
    s_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"LyraCommandPalette", L"", WS_POPUP | WS_CLIPCHILDREN,
                            0, 0, 10, 10, g_hWnd, nullptr, hi, nullptr);
    if (!s_wnd) return;
    const DWORD round = 2;                               // DWMWCP_ROUND (Windows 11)
    DwmSetWindowAttribute(s_wnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
    const BOOL dark = g_dark;
    DwmSetWindowAttribute(s_wnd, kDwmDarkMode, &dark, sizeof(dark));
    s_bgBrush = CreateSolidBrush(ToColorRef(Bg()));
    s_editFont = CreateFontW(-(int)std::round(S(16)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    s_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, s_wnd,
                             (HMENU)(INT_PTR)kEditId, hi, nullptr);
    SetWindowTheme(s_edit, L"", L"");
    SendMessageW(s_edit, WM_SETFONT, (WPARAM)s_editFont, FALSE);
    SendMessageW(s_edit, EM_SETLIMITTEXT, 300, 0);
    // Placeholder (shown while the field is empty, also with the focus).
    SendMessageW(s_edit, 0x1501 /* EM_SETCUEBANNER */, TRUE,
                 (LPARAM)(tabs ? Tr(L"Search open and recently closed tabs", L"Поиск по открытым и недавно закрытым вкладкам")
                               : Tr(L"Search tabs, commands, bookmarks and history", L"Вкладки, команды, закладки и история")));
    s_oldEdit = (WNDPROC)SetWindowLongPtrW(s_edit, GWLP_WNDPROC, (LONG_PTR)EditProc);
    {
        HDC dc = GetDC(s_edit);
        HGDIOBJ old = SelectObject(dc, s_editFont);
        TEXTMETRICW tm = {};
        GetTextMetricsW(dc, &tm);
        s_editH = tm.tmHeight;
        SelectObject(dc, old);
        ReleaseDC(s_edit, dc);
    }
    Rebuild();
    Place();
    PopupShow(s_wnd, true);
    SetForegroundWindow(s_wnd);
    SetFocus(s_edit);
    Log(tabs ? "palette: tab search open" : "palette: open");
}
