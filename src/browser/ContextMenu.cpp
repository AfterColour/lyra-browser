// Lyra — контекстное меню страницы: по-русски, в порядке как в других браузерах, с пунктами Lyra.
#include "browser/ContextMenu.h"
#include "browser/SplitView.h"
#include "app/Log.h"
#include "browser/PictureInPicture.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "core/Constants.h"
#include "core/Localization.h"
#include "core/Settings.h"
#include "core/StringUtils.h"
#include "core/Url.h"
#include "ui/Commands.h"
#include "ui/MainWindow.h"
#include "ui/settings/SettingsPages.h"

#include <WebKit/WKContextMenuItem.h>
#include <WebKit/WKHitTestResult.h>
#include <WebKit/WKPageContextMenuClient.h>

namespace {

// Lyra's items (WebKit hands them back through customContextMenuItemSelected).
enum : WKContextMenuItemTag {
    kOpenLinkInNewTab = kWKContextMenuItemBaseApplicationTag + 1,
    kOpenLinkInPrivateTab,
    kOpenLinkInSplit,
    kCopyLinkText,
    kOpenImageInNewTab,
    kCopyImageAddress,
    kSearchSelection,
    kUndo,
    kRedo,
    kSelectAll,
    kPrint,
    kTranslate,
    kReader,
    kPictureInPicture,
};

// What the menu is about: filled when it is built, used when an item is chosen.
struct MenuContext {
    unsigned long long tabId = 0;
    std::wstring link, linkLabel, image, media;
    bool editable = false, selection = false;
};
MenuContext s_menu;

std::wstring UrlString(WKURLRef u) {
    std::wstring s;
    if (!u) return s;
    if (WKStringRef str = WKURLCopyString(u)) { s = WKStr(str); WKRelease(str); }
    WKRelease(u);
    return s;
}
std::wstring String(WKStringRef s) {
    std::wstring out;
    if (s) { out = WKStr(s); WKRelease(s); }
    return out;
}

std::wstring SearchEngineName() {
    for (int i = 0; i < kEngineCount; ++i)
        if (g_searchEngine == kEngines[i].url) return kEngines[i].name;
    std::wstring host = HostOf(g_searchEngine);
    return host.empty() ? std::wstring(Tr(L"the web", L"интернете")) : host;
}

class Builder {
public:
    explicit Builder(WKArrayRef proposed) {
        const size_t n = proposed ? WKArrayGetSize(proposed) : 0;
        for (size_t i = 0; i < n; ++i) {
            WKContextMenuItemRef item = (WKContextMenuItemRef)WKArrayGetItemAtIndex(proposed, i);
            if (!item || WKGetTypeID(item) != WKContextMenuItemGetTypeID()) continue;
            const WKContextMenuItemType type = WKContextMenuItemGetType(item);
            const WKContextMenuItemTag tag = WKContextMenuItemGetTag(item);
            if (type == kWKContextMenuItemTypeSeparator) continue;
            if (tag == kWKContextMenuItemTagSpellingGuess || tag == kWKContextMenuItemTagNoGuessesFound ||
                tag == kWKContextMenuItemTagIgnoreSpelling || tag == kWKContextMenuItemTagLearnSpelling)
                m_spelling.push_back(item);
            else if (type != kWKContextMenuItemTypeSubmenu && !m_have.count(tag))
                m_have[tag] = item;
        }
        m_items = WKMutableArrayCreate();
    }
    ~Builder() { WKRelease(m_items); }
    bool Has(WKContextMenuItemTag tag) const { return m_have.count(tag) != 0; }
    std::wstring TitleOf(WKContextMenuItemTag tag) const {
        auto it = m_have.find(tag);
        return it == m_have.end() ? std::wstring() : String(WKContextMenuItemCopyTitle(it->second));
    }
    // One of WebKit's own items, with Lyra's title (nothing if WebKit did not propose it here).
    void Engine(WKContextMenuItemTag tag, const std::wstring& title) {
        auto it = m_have.find(tag);
        if (it == m_have.end()) return;
        WKStringRef t = WKStringCreateWithUTF8CString(ToUtf8(title).c_str());
        WKContextMenuItemRef item = WKContextMenuItemGetType(it->second) == kWKContextMenuItemTypeCheckableAction
            ? WKContextMenuItemCreateAsCheckableAction(tag, t, WKContextMenuItemGetEnabled(it->second), WKContextMenuItemGetChecked(it->second))
            : WKContextMenuItemCreateAsAction(tag, t, WKContextMenuItemGetEnabled(it->second));
        WKRelease(t);
        Append(item);
    }
    void Lyra(WKContextMenuItemTag tag, const std::wstring& title, bool enabled = true, bool checked = false, bool checkable = false) {
        WKStringRef t = WKStringCreateWithUTF8CString(ToUtf8(title).c_str());
        Append(checkable ? WKContextMenuItemCreateAsCheckableAction(tag, t, enabled, checked) : WKContextMenuItemCreateAsAction(tag, t, enabled));
        WKRelease(t);
    }
    void Separator() {
        if (!m_count || m_lastSeparator) return;
        WKArrayAppendItem(m_items, WKContextMenuItemSeparatorItem());
        m_lastSeparator = true;
    }
    // Corrections for the misspelled word under the pointer (WebKit's items: the suggestions as
    // they are, the rest with Lyra's titles).
    void Spelling() {
        for (WKContextMenuItemRef item : m_spelling) {
            const WKContextMenuItemTag tag = WKContextMenuItemGetTag(item);
            const wchar_t* title = tag == kWKContextMenuItemTagLearnSpelling ? Tr(L"Add to dictionary", L"Добавить в словарь")
                                 : tag == kWKContextMenuItemTagIgnoreSpelling ? Tr(L"Ignore", L"Пропустить")
                                 : tag == kWKContextMenuItemTagNoGuessesFound ? Tr(L"No suggestions", L"Нет вариантов")
                                 : nullptr;
            if (!title) { WKArrayAppendItem(m_items, item); ++m_count; m_lastSeparator = false; continue; }
            if (tag == kWKContextMenuItemTagIgnoreSpelling) Separator();   // (after the suggestions)
            WKStringRef t = WKStringCreateWithUTF8CString(ToUtf8(title).c_str());
            Append(WKContextMenuItemCreateAsAction(tag, t, WKContextMenuItemGetEnabled(item)));
            WKRelease(t);
        }
        Separator();
    }
    // Drops a separator at the end.
    WKArrayRef Finish() {
        if (m_lastSeparator && m_count) {
            WKMutableArrayRef trimmed = WKMutableArrayCreate();
            const size_t n = WKArrayGetSize(m_items);
            for (size_t i = 0; i + 1 < n; ++i) WKArrayAppendItem(trimmed, WKArrayGetItemAtIndex(m_items, i));
            WKRelease(m_items);
            m_items = trimmed;
        }
        return m_items;
    }

private:
    void Append(WKContextMenuItemRef item) {
        WKArrayAppendItem(m_items, item);
        WKRelease(item);
        ++m_count;
        m_lastSeparator = false;
    }
    std::map<WKContextMenuItemTag, WKContextMenuItemRef> m_have;
    std::vector<WKContextMenuItemRef> m_spelling;
    WKMutableArrayRef m_items = nullptr;
    int m_count = 0;
    bool m_lastSeparator = false;
};

WKArrayRef BuildMenu(WKArrayRef proposed, const Tab& t, Builder& b) {
    const MenuContext& c = s_menu;
    b.Spelling();
    if (!c.link.empty()) {
        b.Lyra(kOpenLinkInNewTab, Tr(L"Open link in new tab", L"Открыть ссылку в новой вкладке"));
        if (!t.pinned) b.Lyra(kOpenLinkInSplit, Tr(L"Open link in split view", L"Открыть ссылку в разделённом экране"));
        b.Lyra(kOpenLinkInPrivateTab, Tr(L"Open link in private tab", L"Открыть ссылку в приватной вкладке"));
        b.Engine(kWKContextMenuItemTagDownloadLinkToDisk, Tr(L"Save link as\x2026", L"Сохранить ссылку как\x2026"));
        b.Engine(kWKContextMenuItemTagCopyLinkToClipboard, Tr(L"Copy link address", L"Копировать адрес ссылки"));
        if (!c.linkLabel.empty() && c.image.empty()) b.Lyra(kCopyLinkText, Tr(L"Copy link text", L"Копировать текст ссылки"));
        b.Separator();
    }
    if (!c.image.empty()) {
        b.Lyra(kOpenImageInNewTab, Tr(L"Open image in new tab", L"Открыть изображение в новой вкладке"));
        b.Engine(kWKContextMenuItemTagDownloadImageToDisk, Tr(L"Save image as\x2026", L"Сохранить изображение как\x2026"));
        b.Engine(kWKContextMenuItemTagCopyImageToClipboard, Tr(L"Copy image", L"Копировать изображение"));
        if (b.Has(kWKContextMenuItemTagCopyImageURLToClipboard))
            b.Engine(kWKContextMenuItemTagCopyImageURLToClipboard, Tr(L"Copy image address", L"Копировать адрес изображения"));
        else
            b.Lyra(kCopyImageAddress, Tr(L"Copy image address", L"Копировать адрес изображения"));
        b.Separator();
    }
    if (!c.media.empty()) {
        const bool pause = b.TitleOf(kWKContextMenuItemTagMediaPlayPause).find(L"Pause") != std::wstring::npos;
        b.Engine(kWKContextMenuItemTagMediaPlayPause, pause ? Tr(L"Pause", L"Пауза") : Tr(L"Play", L"Воспроизвести"));
        const bool unmute = b.TitleOf(kWKContextMenuItemTagMediaMute).find(L"Unmute") != std::wstring::npos;
        b.Engine(kWKContextMenuItemTagMediaMute, unmute ? Tr(L"Unmute", L"Включить звук") : Tr(L"Mute", L"Выключить звук"));
        b.Engine(kWKContextMenuItemTagToggleMediaLoop, Tr(L"Loop", L"Повторять"));
        b.Engine(kWKContextMenuItemTagToggleMediaControls, Tr(L"Show controls", L"Показывать элементы управления"));
        b.Engine(kWKContextMenuItemTagEnterVideoFullscreen, Tr(L"Full screen", L"Во весь экран"));
        b.Engine(kWKContextMenuItemTagToggleVideoFullscreen, Tr(L"Full screen", L"Во весь экран"));
        if (b.Has(kWKContextMenuItemTagEnterVideoFullscreen) || b.Has(kWKContextMenuItemTagToggleVideoFullscreen))
            b.Lyra(kPictureInPicture, Tr(L"Picture in picture", L"Картинка в картинке"), true, PipActive(), true);
        b.Engine(kWKContextMenuItemTagCopyMediaLinkToClipboard, Tr(L"Copy video address", L"Копировать адрес видео"));
        b.Engine(kWKContextMenuItemTagDownloadMediaToDisk, Tr(L"Save video as\x2026", L"Сохранить видео как\x2026"));
        b.Separator();
    }
    if (c.editable) {
        b.Lyra(kUndo, Tr(L"Undo", L"Отменить"));
        b.Lyra(kRedo, Tr(L"Redo", L"Повторить"));
        b.Separator();
        b.Engine(kWKContextMenuItemTagCut, Tr(L"Cut", L"Вырезать"));
        b.Engine(kWKContextMenuItemTagCopy, Tr(L"Copy", L"Копировать"));
        b.Engine(kWKContextMenuItemTagPaste, Tr(L"Paste", L"Вставить"));
        b.Lyra(kSelectAll, Tr(L"Select all", L"Выделить всё"));
        b.Separator();
    } else if (c.selection) {
        b.Engine(kWKContextMenuItemTagCopy, Tr(L"Copy", L"Копировать"));
        b.Lyra(kSearchSelection, Tr(L"Search for the selection with ", L"Найти выделенное в ") + SearchEngineName());
        b.Separator();
    }
    if (c.link.empty() && c.image.empty() && c.media.empty() && !c.editable && !c.selection) {
        b.Engine(kWKContextMenuItemTagGoBack, Tr(L"Back", L"Назад"));
        b.Engine(kWKContextMenuItemTagGoForward, Tr(L"Forward", L"Вперёд"));
        b.Engine(kWKContextMenuItemTagReload, Tr(L"Reload", L"Обновить"));
        b.Engine(kWKContextMenuItemTagStop, Tr(L"Stop", L"Остановить"));
        b.Separator();
        b.Lyra(kSelectAll, Tr(L"Select all", L"Выделить всё"));
        b.Lyra(kPrint, Tr(L"Print\x2026", L"Печать\x2026"));
        if (t.translated) b.Lyra(kTranslate, Tr(L"Show original", L"Показать оригинал"));
        else if (t.translatable) b.Lyra(kTranslate, Tr(L"Translate page", L"Перевести страницу"));
        if (t.readerable) b.Lyra(kReader, Tr(L"Reader view", L"Режим чтения"));
        b.Separator();
    }
    b.Separator();
    b.Engine(kWKContextMenuItemTagInspectElement, Tr(L"Inspect element", L"Исследовать элемент"));
    (void)proposed;
    return b.Finish();
}

void OnGetMenu(WKPageRef, WKArrayRef proposed, WKArrayRef* newMenu, WKHitTestResultRef hit, WKTypeRef, const void* clientInfo) {
    Tab* t = ResolveTab(clientInfo, "contextMenu");
    if (!t || !hit) { if (proposed) WKRetain(proposed); *newMenu = proposed; return; }
    s_menu = MenuContext();
    s_menu.tabId = t->id;
    s_menu.link = UrlString(WKHitTestResultCopyAbsoluteLinkURL(hit));
    s_menu.linkLabel = TrimWs(String(WKHitTestResultCopyLinkLabel(hit)));
    s_menu.image = UrlString(WKHitTestResultCopyAbsoluteImageURL(hit));
    s_menu.media = UrlString(WKHitTestResultCopyAbsoluteMediaURL(hit));
    s_menu.editable = WKHitTestResultIsContentEditable(hit);
    Builder b(proposed);
    s_menu.selection = !s_menu.editable && b.Has(kWKContextMenuItemTagCopy);   // WebKit offers Copy for a selection
    WKArrayRef items = BuildMenu(proposed, *t, b);
    Log("context menu: tab#%llu link=%d image=%d media=%d editable=%d selection=%d, %zu of %zu item(s)", t->id,
        !s_menu.link.empty(), !s_menu.image.empty(), !s_menu.media.empty(), s_menu.editable, s_menu.selection,
        WKArrayGetSize(items), proposed ? WKArrayGetSize(proposed) : 0);
    if (!WKArrayGetSize(items)) items = proposed;       // never an empty menu
    if (items) WKRetain(items);                          // WebKit adopts the array
    *newMenu = items;
}

// The selected text (in a text field too), asked of the page when "search" is chosen.
const char kSelectionJs[] =
    "(function(){var a=document.activeElement;"
    "if(a&&(a.tagName==='TEXTAREA'||(a.tagName==='INPUT'&&/^(text|search|url|email|tel|)$/i.test(a.type)))&&typeof a.selectionStart==='number')"
    "return a.value.substring(a.selectionStart,a.selectionEnd);"
    "return String(getSelection());})()";

void OnSelectionForSearch(WKTypeRef result, WKErrorRef, void* ctx) {
    const unsigned long long tabId = (unsigned long long)(uintptr_t)ctx;
    Tab* t = FindTab(tabId);
    if (!t || !result || WKGetTypeID(result) != WKStringGetTypeID()) return;
    std::wstring text = TrimWs(WKStr((WKStringRef)result));
    for (auto& ch : text) if (ch == L'\n' || ch == L'\r' || ch == L'\t') ch = L' ';
    if (text.empty()) return;
    if (text.size() > 300) text.resize(300);
    OpenInNewTab(g_searchEngine + ToWide(UrlEncode(ToUtf8(text))), false, t->privateTab);
}

void OnItemSelected(WKPageRef, WKContextMenuItemRef item, const void* clientInfo) {
    if (!ResolveTab(clientInfo, "contextMenuItem")) return;
    // Out of WebKit's callback: tabs are created and dialogs shown on the main loop.
    PostMessageW(g_hWnd, WM_APP_CONTEXT_MENU, (WPARAM)WKContextMenuItemGetTag(item), 0);
}

void CopyText(const std::wstring& text) {
    if (text.empty() || !OpenClipboard(g_hWnd)) return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        memcpy(GlobalLock(h), text.c_str(), bytes);
        GlobalUnlock(h);
        if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
    }
    CloseClipboard();
}

void Execute(Tab& t, const char* command) {
    WKStringRef c = WKStringCreateWithUTF8CString(command);
    WKPageExecuteCommand(t.page, c);
    WKRelease(c);
}

} // namespace

void ContextMenuSetClient(WKPageRef page, TabCtx* ctx) {
    WKPageContextMenuClientV3 cm = {};
    cm.base.version = 3;
    cm.base.clientInfo = ctx;
    cm.customContextMenuItemSelected = OnItemSelected;
    cm.getContextMenuFromProposedMenu = OnGetMenu;
    WKPageSetPageContextMenuClient(page, &cm.base);
}

void ContextMenuClearClient(WKPageRef page) {
    WKPageContextMenuClientV0 cm = {};
    cm.base.version = 0;
    WKPageSetPageContextMenuClient(page, &cm.base);
}

void ContextMenuCommand(WPARAM tag) {
    const MenuContext c = s_menu;
    Tab* t = FindTab(c.tabId);
    if (!t) return;
    Log("context menu: item %u", (unsigned)(tag - kWKContextMenuItemBaseApplicationTag));
    switch ((WKContextMenuItemTag)tag) {
    case kOpenLinkInNewTab:     OpenInNewTab(c.link, true, t->privateTab); break;
    case kOpenLinkInPrivateTab: OpenInNewTab(c.link, false, true); break;
    case kOpenLinkInSplit:      SplitOpenLink(t->id, c.link, t->privateTab); break;
    case kCopyLinkText:         CopyText(c.linkLabel); break;
    case kOpenImageInNewTab:    OpenInNewTab(c.image, true, t->privateTab); break;
    case kCopyImageAddress:     CopyText(c.image); break;
    case kSearchSelection: {
        WKStringRef js = WKStringCreateWithUTF8CString(kSelectionJs);
        WKPageEvaluateJavaScriptInMainFrame(t->page, js, (void*)(uintptr_t)t->id, OnSelectionForSearch);
        WKRelease(js);
        break;
    }
    case kUndo:                 Execute(*t, "Undo"); break;
    case kRedo:                 Execute(*t, "Redo"); break;
    case kSelectAll:            Execute(*t, "SelectAll"); break;
    case kPrint:                RunCommand(CMD_PRINT); break;
    case kTranslate:            RunCommand(CMD_TRANSLATE); break;
    case kReader:               RunCommand(CMD_READER); break;
    case kPictureInPicture:     RunCommand(CMD_PIP); break;
    default: break;
    }
}
