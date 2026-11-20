// Lyra — поле ввода адреса.
#include "ui/AddressBar.h"
#include "ui/Suggestions.h"
#include "core/Url.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "ui/Chrome.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/Menu.h"
#include "core/Localization.h"

HWND   g_urlEdit = nullptr;

WNDPROC g_oldEditProc = nullptr;

HFONT  g_editFont = nullptr;

HBRUSH g_editBrush = nullptr;

int    g_editTextH = 18;

bool IsEditing() { return g_urlEdit && IsWindowVisible(g_urlEdit); }

void PositionEdit() {
    if (!g_urlEdit) return;
    // Between the search glyph on the left and the reload button (if it is inside the field)
    const float left = g_L.pill.X + S(28);
    const float right = g_L.reloadInPill ? g_L.reload.X - S(4) : g_L.pill.GetRight() - S(10);
    int x = (int)left;
    int w = (int)std::max(10.f, right - left);
    int h = g_editTextH;
    int y = (int)(g_L.pill.Y + (g_L.pill.Height - (float)h) / 2);
    MoveWindow(g_urlEdit, x, y, w, h, TRUE);
}

void ShowUrlEdit() {
    if (!g_urlEdit) return;
    Tab* t = ActiveTab();
    std::wstring text = (t && t->url != L"about:blank" && !IsInternalPageUrl(t->url)) ? t->url : L"";
    SetWindowTextW(g_urlEdit, text.c_str());
    PositionEdit();
    ShowWindow(g_urlEdit, SW_SHOW);
    SetFocus(g_urlEdit);
    SendMessageW(g_urlEdit, EM_SETSEL, 0, -1);
    InvalidateChrome();
}

// The clipboard's text, on one line (what was copied from a page may have line breaks).
static std::wstring ClipboardLine() {
    std::wstring s;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(g_urlEdit)) return s;
    if (HANDLE hd = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* p = (const wchar_t*)GlobalLock(hd)) { s = p; GlobalUnlock(hd); }
    }
    CloseClipboard();
    std::wstring out;
    for (wchar_t c : s) {
        if (c == L'\r' || c == L'\n' || c == L'\t') { if (!out.empty() && out.back() != L' ') out += L' '; }
        else out += c;
    }
    while (!out.empty() && out.back() == L' ') out.pop_back();
    size_t b = out.find_first_not_of(L' ');
    return b == std::wstring::npos ? std::wstring() : out.substr(b);
}

// Roughly as ResolveInput sees it: one word with a dot or a scheme is an address, the rest a search.
static bool LooksLikeUrl(const std::wstring& s) {
    if (s.empty() || s.find(L' ') != std::wstring::npos) return false;
    return s.find(L"://") != std::wstring::npos || s.compare(0, 9, L"localhost") == 0 ||
           (s.find(L'.') != std::wstring::npos && s[0] != L'.' && s.back() != L'.') ||
           (s.size() > 2 && s[1] == L':' && (s[2] == L'\\' || s[2] == L'/'));
}

// The field's own menu: the usual edit commands and "Paste and go".
static void UrlEditMenu(HWND h, LPARAM l) {
    enum { kUndo = 1, kCut, kCopy, kPaste, kPasteGo, kDelete, kSelectAll };
    DWORD a = 0, b = 0;
    SendMessageW(h, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
    const bool sel = b > a;
    const bool any = GetWindowTextLengthW(h) > 0;
    const std::wstring clip = ClipboardLine();
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (SendMessageW(h, EM_CANUNDO, 0, 0) ? 0 : MF_GRAYED), kUndo, Tr(L"Undo\tCtrl+Z", L"Отменить\tCtrl+Z"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (sel ? 0 : MF_GRAYED), kCut, Tr(L"Cut\tCtrl+X", L"Вырезать\tCtrl+X"));
    AppendMenuW(m, MF_STRING | (sel ? 0 : MF_GRAYED), kCopy, Tr(L"Copy\tCtrl+C", L"Копировать\tCtrl+C"));
    AppendMenuW(m, MF_STRING | (clip.empty() ? MF_GRAYED : 0), kPaste, Tr(L"Paste\tCtrl+V", L"Вставить\tCtrl+V"));
    AppendMenuW(m, MF_STRING | (clip.empty() ? MF_GRAYED : 0), kPasteGo,
                LooksLikeUrl(clip) || clip.empty() ? Tr(L"Paste and go\tCtrl+Shift+V", L"Вставить и перейти\tCtrl+Shift+V")
                                                   : Tr(L"Paste and search\tCtrl+Shift+V", L"Вставить и найти\tCtrl+Shift+V"));
    AppendMenuW(m, MF_STRING | (sel ? 0 : MF_GRAYED), kDelete, Tr(L"Delete", L"Удалить"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (any ? 0 : MF_GRAYED), kSelectAll, Tr(L"Select all\tCtrl+A", L"Выделить всё\tCtrl+A"));
    MenuSetIcon(m, kCut, L"\uE8C6");
    MenuSetIcon(m, kCopy, L"\uE8C8");
    MenuSetIcon(m, kPaste, L"\uE77F");
    MenuSetIcon(m, kPasteGo, L"\uE72A");
    POINT pt = { (short)LOWORD(l), (short)HIWORD(l) };
    if (l == -1) { RECT r; GetWindowRect(h, &r); pt = { r.left + 8, r.bottom }; }   // (from the keyboard)
    const int cmd = MenuTrack(m, TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, GetAncestor(h, GA_ROOT));
    DestroyMenu(m);
    switch (cmd) {
    case kUndo: SendMessageW(h, EM_UNDO, 0, 0); break;
    case kCut: SendMessageW(h, WM_CUT, 0, 0); break;
    case kCopy: SendMessageW(h, WM_COPY, 0, 0); break;
    case kPaste: SendMessageW(h, WM_PASTE, 0, 0); break;
    case kDelete: SendMessageW(h, WM_CLEAR, 0, 0); break;
    case kSelectAll: SendMessageW(h, EM_SETSEL, 0, -1); break;
    case kPasteGo: PasteAndGo(); break;
    }
}

void PasteAndGo() {
    const std::wstring clip = ClipboardLine();
    if (clip.empty()) return;
    SuggestCancel();
    if (g_urlEdit) SetWindowTextW(g_urlEdit, clip.c_str());
    NavigateActive(clip);
    FocusWebView();
}

// ----------------------------------------------------------------------------
//  Address field subclass
// ----------------------------------------------------------------------------
LRESULT CALLBACK UrlEditProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_KEYDOWN:
        if (w == VK_RETURN) {
            if (SuggestAccept()) return 0;           // a suggestion was selected with the arrows
            wchar_t buf[2048] = {};
            GetWindowTextW(h, buf, ARRAYSIZE(buf));
            NavigateActive(buf);
            FocusWebView();
            return 0;
        }
        if (w == VK_ESCAPE) { if (!SuggestCancel()) FocusWebView(); return 0; }
        if (w == VK_DOWN && SuggestMove(1)) return 0;
        if (w == VK_UP && SuggestMove(-1)) return 0;
        if (w == 'V' && (GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_SHIFT) & 0x8000)) {
            PasteAndGo();
            return 0;
        }
        if (w == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessageW(h, EM_SETSEL, 0, -1);
            return 0;
        }
        break;
    case WM_CHAR:
        if (w == VK_RETURN || w == VK_ESCAPE || w == 1) return 0;  // no beep
        if (w == 0x16 && (GetKeyState(VK_SHIFT) & 0x8000)) return 0;   // (Ctrl+Shift+V: done on the key)
        break;
    case WM_CONTEXTMENU:
        UrlEditMenu(h, l);
        return 0;
    }
    return CallWindowProcW(g_oldEditProc, h, msg, w, l);
}
