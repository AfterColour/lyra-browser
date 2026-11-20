// Lyra — панель закладок под адресной строкой (папка «Избранное»).
#include "ui/BookmarksBar.h"
#include "app/Log.h"
#include "browser/Bookmarks.h"
#include "browser/Favicons.h"
#include "browser/StartPage.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "core/Localization.h"
#include "core/Settings.h"
#include "core/StringUtils.h"
#include "core/Url.h"
#include "ui/Chrome.h"
#include "ui/Dialogs.h"
#include "ui/Draw.h"
#include "ui/MainWindow.h"
#include "ui/Menu.h"
#include "ui/Theme.h"

namespace {

struct Item { long long id = 0; bool folder = false; std::wstring title, url; RectF r; };

// The layout of the last bar asked about (width, the bookmarks as they were): rebuilt when either changes.
std::vector<Item> s_items;            // the ones shown
std::vector<Bookmark> s_hidden;       // the ones behind "»"
RectF s_overflow;                     // "»" (empty: everything fits)
float s_cachedW = -1, s_cachedX = 0, s_cachedY = 0;
std::wstring s_cachedSig;
bool s_empty = false;

float ItemH(const RectF& bar) { return std::floor(std::min(bar.Height - S(6), S(26))); }

std::wstring Label(const Bookmark& b) {
    if (!b.title.empty()) return b.title;
    return b.folder ? std::wstring(Tr(L"Folder", L"Папка")) : HostOf(b.url);
}

void Rebuild(const RectF& bar) {
    const std::vector<Bookmark> list = BookmarkChildren(kFavoritesFolder);
    std::wstring sig;
    for (const Bookmark& b : list) sig += std::to_wstring(b.id) + L"|" + b.title + L"|" + b.url + L"\n";
    if (bar.Width == s_cachedW && bar.X == s_cachedX && bar.Y == s_cachedY && sig == s_cachedSig) return;
    s_cachedW = bar.Width; s_cachedX = bar.X; s_cachedY = bar.Y; s_cachedSig = sig;
    s_items.clear();
    s_hidden.clear();
    s_overflow = RectF();
    s_empty = list.empty();
    Font* font = g_fTab ? g_fTab.get() : g_fText.get();
    if (!font) return;
    Bitmap probe(1, 1, PixelFormat32bppPARGB);
    Graphics pg(&probe);
    StringFormat sf(StringFormat::GenericTypographic());
    const float h = ItemH(bar), y = std::floor(bar.Y + (bar.Height - h) / 2);
    const float pad = S(8), icon = S(16), gap = S(6), maxText = S(150), between = S(2);
    const float overflowW = std::floor(S(28));
    const float right = bar.GetRight() - S(8);
    float x = bar.X + S(8);
    for (size_t i = 0; i < list.size(); ++i) {
        const std::wstring label = Label(list[i]);
        RectF box;
        pg.MeasureString(label.c_str(), -1, font, PointF(0, 0), &sf, &box);
        const float w = std::floor(pad + icon + gap + std::min(maxText, box.Width + S(1)) + pad);
        const bool last = i + 1 == list.size();
        if (x + w > (last ? right : right - overflowW - between)) {
            for (size_t k = i; k < list.size(); ++k) s_hidden.push_back(list[k]);
            break;
        }
        Item it;
        it.id = list[i].id;
        it.folder = list[i].folder;
        it.title = label;
        it.url = list[i].url;
        it.r = RectF(std::floor(x), y, w, h);
        s_items.push_back(it);
        x += w + between;
    }
    if (!s_hidden.empty()) s_overflow = RectF(std::floor(right - overflowW), y, overflowW, h);
}

const Item* ItemAt(int index) { return index >= 0 && index < (int)s_items.size() ? &s_items[index] : nullptr; }

void Open(const std::wstring& url, bool newTab, bool privateTab = false) {
    if (url.empty()) return;
    if (newTab || privateTab) OpenInNewTab(url, !privateTab, privateTab);
    else { NavigateActive(url); FocusWebView(); }
}

void OpenAll(long long folder) {
    for (const Bookmark& b : BookmarkChildren(folder)) if (!b.folder) Open(b.url, true);
}

// A folder's (or the hidden items') menu: bookmarks open, folders are submenus, "Open all" on top.
struct MenuBuild {
    std::vector<long long> ids;       // command (base + i) -> bookmark id; folders' "open all": -id
    std::vector<std::wstring> labels; // kept alive while the menu is shown
    static constexpr UINT kBase = 7000;
};

std::wstring MenuLabel(std::wstring s) {
    if (s.size() > 60) s = s.substr(0, 57) + L"\x2026";
    for (auto& c : s) if (c == L'&') c = L'+';         // (no mnemonics)
    return s;
}

void AddEntries(HMENU m, const std::vector<Bookmark>& list, long long folderForOpenAll, MenuBuild& mb, int depth) {
    if (folderForOpenAll) {
        int pages = 0;
        for (const Bookmark& b : list) if (!b.folder) ++pages;
        if (pages > 1) {
            mb.labels.push_back(std::wstring(Tr(L"Open all (", L"Открыть все (")) + std::to_wstring(pages) + L")");
            mb.ids.push_back(-folderForOpenAll);
            AppendMenuW(m, MF_STRING, MenuBuild::kBase + (UINT)mb.ids.size() - 1, mb.labels.back().c_str());
            MenuSetIcon(m, MenuBuild::kBase + (UINT)mb.ids.size() - 1, L"");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        }
    }
    if (list.empty()) {
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, Tr(L"(empty)", L"(пусто)"));
        return;
    }
    for (const Bookmark& b : list) {
        mb.labels.push_back(MenuLabel(Label(b)));
        if (b.folder && depth < 8) {
            HMENU sub = CreatePopupMenu();
            AddEntries(sub, BookmarkChildren(b.id), b.id, mb, depth + 1);
            AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, mb.labels.back().c_str());
        } else if (!b.folder) {
            mb.ids.push_back(b.id);
            const UINT cmd = MenuBuild::kBase + (UINT)mb.ids.size() - 1;
            AppendMenuW(m, MF_STRING, cmd, mb.labels.back().c_str());
            MenuSetIcon(m, cmd, L"");
        }
    }
}

void ShowListMenu(const std::vector<Bookmark>& list, long long folder, const RectF& under) {
    HMENU m = CreatePopupMenu();
    MenuBuild mb;
    mb.ids.reserve(256);
    mb.labels.reserve(512);
    AddEntries(m, list, folder, mb, 0);
    POINT pt = { (LONG)under.X, (LONG)(under.GetBottom() + S(2)) };
    ClientToScreen(g_hWnd, &pt);
    const int cmd = MenuTrack(m, TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, g_hWnd);
    DestroyMenu(m);
    if (cmd < (int)MenuBuild::kBase || cmd >= (int)(MenuBuild::kBase + mb.ids.size())) return;
    const long long id = mb.ids[cmd - MenuBuild::kBase];
    if (id < 0) { OpenAll(-id); return; }
    if (const Bookmark* b = BookmarkGet(id)) {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        Open(b->url, ctrl);
    }
}

bool PromptText(const wchar_t* title, const wchar_t* message, std::wstring& value) {
    PromptState st;
    st.message = message;
    st.value = value;
    ModalScope modal;
    if (!PromptDialog(g_hWnd, title, st) || !st.ok) return false;
    value = st.value;
    return true;
}

}  // namespace

bool BookmarksBarShown() { return g_prefs.bookmarksBar; }

void BookmarksBarToggle() {
    g_prefs.bookmarksBar = !g_prefs.bookmarksBar;
    SaveSettings();
    Log("bookmarks bar: %s", g_prefs.bookmarksBar ? "shown" : "hidden");
    RefreshLayout();
}

float BookmarksBarHeight() { return std::floor(S(32)); }

int BookmarksBarHitTest(const RectF& bar, float x, float y) {
    if (!bar.Contains(x, y)) return -1;
    Rebuild(bar);
    for (int i = 0; i < (int)s_items.size(); ++i) if (s_items[i].r.Contains(x, y)) return i;
    if (s_overflow.Width > 0 && s_overflow.Contains(x, y)) return -2;
    return -1;
}

void BookmarksBarPaint(Graphics& g, const RectF& bar, int hover, int pressed, float hoverAmount) {
    if (bar.Width <= 0 || bar.Height <= 0) return;
    Rebuild(bar);
    const Theme& T = g_th;
    Font* font = g_fTab ? g_fTab.get() : g_fText.get();
    StringFormat sf(StringFormat::GenericTypographic());
    sf.SetFormatFlags(sf.GetFormatFlags() | StringFormatFlagsNoWrap);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    sf.SetLineAlignment(StringAlignmentCenter);
    SolidBrush tb(T.text), t2(T.text2);
    if (s_empty) {
        g.DrawString(Tr(L"Bookmarks of the Favorites folder show here: add pages with the star in the address field.",
                        L"Здесь показываются закладки папки «Избранное»: добавляйте страницы звёздочкой в адресной строке."),
                     -1, font, RectF(bar.X + S(12), bar.Y, bar.Width - S(24), bar.Height), &sf, &t2);
        return;
    }
    const float rad = S(6);
    auto back = [&](const RectF& r, int index) {
        if (index == pressed) FillRound(g, T.pressed, r, rad);
        else if (index == hover && hoverAmount > 0) FillRound(g, Color((BYTE)(T.hover.GetA() * hoverAmount), T.hover.GetR(), T.hover.GetG(), T.hover.GetB()), r, rad);
    };
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    for (int i = 0; i < (int)s_items.size(); ++i) {
        const Item& it = s_items[i];
        back(it.r, i);
        const float is = S(16);
        const RectF icon(it.r.X + S(8), std::floor(it.r.Y + (it.r.Height - is) / 2), is, is);
        Bitmap* fav = it.folder ? nullptr : FaviconFor(it.url);
        if (fav) g.DrawImage(fav, icon);
        else DrawGlyph(g, it.folder ? L"" : L"", icon, T.icon, g_fIconSm.get());
        const float tx = icon.GetRight() + S(6);
        g.DrawString(it.title.c_str(), -1, font, RectF(tx, it.r.Y, it.r.GetRight() - S(8) - tx, it.r.Height), &sf, &tb);
    }
    if (s_overflow.Width > 0) {
        back(s_overflow, -2);
        DrawGlyph(g, L"", s_overflow, T.icon, g_fIconSm.get());   // chevron right
    }
}

void BookmarksBarClick(int index, bool newTab) {
    if (index == -2) { ShowListMenu(s_hidden, 0, s_overflow); return; }
    const Item* it = ItemAt(index);
    if (!it) return;
    if (it->folder) { ShowListMenu(BookmarkChildren(it->id), it->id, it->r); return; }
    Log("bookmarks bar: open #%lld", it->id);
    Open(it->url, newTab);
}

void BookmarksBarMiddleClick(int index) {
    const Item* it = ItemAt(index);
    if (!it) return;
    if (it->folder) OpenAll(it->id);
    else Open(it->url, true);
}

void BookmarksBarContextMenu(int index, POINT screenPt) {
    enum { kOpen = 1, kOpenNew, kOpenPrivate, kOpenAll, kRename, kAddress, kDelete, kHide, kManager };
    const Item* it = ItemAt(index);
    const long long id = it ? it->id : 0;
    const bool folder = it && it->folder;
    HMENU m = CreatePopupMenu();
    if (it && !folder) {
        AppendMenuW(m, MF_STRING, kOpen, Tr(L"Open", L"Открыть"));
        AppendMenuW(m, MF_STRING, kOpenNew, Tr(L"Open in new tab", L"Открыть в новой вкладке"));
        AppendMenuW(m, MF_STRING, kOpenPrivate, Tr(L"Open in private tab", L"Открыть в приватной вкладке"));
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, kRename, Tr(L"Rename\x2026", L"Переименовать\x2026"));
        AppendMenuW(m, MF_STRING, kAddress, Tr(L"Change address\x2026", L"Изменить адрес\x2026"));
        AppendMenuW(m, MF_STRING, kDelete, Tr(L"Delete", L"Удалить"));
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    } else if (folder) {
        AppendMenuW(m, MF_STRING, kOpenAll, Tr(L"Open all in new tabs", L"Открыть все в новых вкладках"));
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, kRename, Tr(L"Rename\x2026", L"Переименовать\x2026"));
        AppendMenuW(m, MF_STRING, kDelete, Tr(L"Delete folder", L"Удалить папку"));
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(m, MF_STRING, kManager, Tr(L"Bookmarks\tCtrl+Shift+O", L"Закладки\tCtrl+Shift+O"));
    AppendMenuW(m, MF_STRING | MF_CHECKED, kHide, Tr(L"Show bookmarks bar\tCtrl+Shift+B", L"Показывать панель закладок\tCtrl+Shift+B"));
    MenuSetIcon(m, kOpenNew, L"");
    MenuSetIcon(m, kOpenPrivate, L"");
    MenuSetIcon(m, kOpenAll, L"");
    MenuSetIcon(m, kRename, L"");
    MenuSetIcon(m, kDelete, L"");
    MenuSetIcon(m, kManager, L"");
    const int cmd = MenuTrack(m, TPM_LEFTALIGN | TPM_TOPALIGN, screenPt.x, screenPt.y, g_hWnd);
    DestroyMenu(m);
    const Bookmark* b = id ? BookmarkGet(id) : nullptr;
    switch (cmd) {
    case kOpen: if (b) Open(b->url, false); break;
    case kOpenNew: if (b) Open(b->url, true); break;
    case kOpenPrivate: if (b) Open(b->url, false, true); break;
    case kOpenAll: if (b) OpenAll(b->id); break;
    case kRename:
        if (b) {
            std::wstring name = b->title;
            if (PromptText(Tr(L"Rename", L"Переименовать"), Tr(L"Name:", L"Название:"), name) && !name.empty()) BookmarkRename(id, name);
        }
        break;
    case kAddress:
        if (b) {
            std::wstring url = b->url;
            if (PromptText(Tr(L"Change address", L"Изменить адрес"), Tr(L"Address:", L"Адрес:"), url) && !url.empty())
                BookmarkSetUrl(id, ToWide(ResolveInput(url)));
        }
        break;
    case kDelete: if (b) BookmarkRemove(id); break;
    case kHide: BookmarksBarToggle(); break;
    case kManager: OpenBookmarks(); break;
    }
    InvalidateChrome();
}
