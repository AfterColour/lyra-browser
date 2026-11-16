// Lyra — звёздочка в адресной строке: окошко «Добавлено в закладки» (название, папка, удалить).
//
// A small window owned by the main window, under the star. It takes the keyboard focus (the name
// can be edited at once) and closes when it loses it, keeping the bookmark with the new name.
#include "ui/BookmarkPopup.h"
#include "browser/Bookmarks.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "core/Localization.h"
#include "core/Url.h"
#include "ui/Animation.h"
#include "ui/Chrome.h"
#include "ui/Draw.h"
#include "ui/Menu.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"

static HWND s_popup = nullptr, s_edit = nullptr;
static WNDPROC s_oldEditProc = nullptr;
static long long s_id = 0;                    // the bookmark being edited
static bool s_justAdded = false;
static long long s_lastFolder = kFavoritesFolder;
static int s_hover = 0, s_down = 0;           // 1 folder button, 2 remove, 3 done
static bool s_inMenu = false, s_closing = false, s_tracking = false;
static DWORD s_closedAt = 0;                  // a click on the star that closed the bubble must not reopen it
static HFONT s_editFont = nullptr;
static HBRUSH s_fieldBrush = nullptr;
static std::unique_ptr<Font> s_fTitle, s_fText, s_fBold, s_fLabel, s_fGlyph;

// Logical (96 DPI) layout
constexpr float kW = 340, kPad = 16, kTitleY = 14, kNameLabelY = 46, kNameY = 64, kFolderLabelY = 106,
                kFolderY = 124, kButtonsY = 172, kRowH = 32, kH = 220;

bool CanBookmark(const std::wstring& url) {
    if (url.empty() || IsInternalPageUrl(url)) return false;
    return url.compare(0, 7, L"http://") == 0 || url.compare(0, 8, L"https://") == 0 || url.compare(0, 5, L"file:") == 0;
}

bool IsBookmarkedCached(const std::wstring& url) {
    // BookmarkFind walks every bookmark: remember the answer for the last address until a change.
    static std::wstring lastUrl;
    static bool last = false;
    static long long version = -1;
    if (url != lastUrl || version != g_bookmarksVersion) {
        lastUrl = url;
        version = g_bookmarksVersion;
        last = BookmarkFind(url) != 0;
    }
    return last;
}

static Color PopupBg() { return g_dark ? Color(37, 39, 44) : Color(255, 255, 255); }
static Color FieldBg() { return g_dark ? Color(46, 48, 54) : Color(255, 255, 255); }

// `top` (with its alpha) over the opaque `base`
static Color Over(const Color& base, const Color& top) {
    const float k = top.GetA() / 255.f;
    auto m = [k](BYTE x, BYTE y) { return (BYTE)std::lround(x + ((int)y - (int)x) * k); };
    return Color(255, m(base.GetR(), top.GetR()), m(base.GetG(), top.GetG()), m(base.GetB(), top.GetB()));
}

static RectF FolderRect() { return RectF(S(kPad), S(kFolderY), S(kW - kPad * 2), S(kRowH)); }
static RectF DoneRect() { return RectF(S(kW - kPad - 96), S(kButtonsY), S(96), S(kRowH)); }
static RectF RemoveRect() { return RectF(S(kPad), S(kButtonsY), S(96), S(kRowH)); }
static RECT NameRect() {
    return { (LONG)S(kPad), (LONG)S(kNameY), (LONG)S(kW - kPad), (LONG)S(kNameY + kRowH) };
}

static int HitAt(int x, int y) {
    const float fx = (float)x, fy = (float)y;
    if (FolderRect().Contains(fx, fy)) return 1;
    if (RemoveRect().Contains(fx, fy)) return 2;
    if (DoneRect().Contains(fx, fy)) return 3;
    return 0;
}

static void MakeFonts() {
    auto make = [](const wchar_t* family, float px, int style) {
        auto f = std::make_unique<Font>(family, px, style, UnitPixel);
        if (!f->IsAvailable()) f = std::make_unique<Font>(L"Segoe UI", px, style, UnitPixel);
        return f;
    };
    s_fTitle = make(L"Segoe UI Variable Text", S(14), FontStyleBold);
    s_fText = make(L"Segoe UI Variable Text", S(13), FontStyleRegular);
    s_fBold = make(L"Segoe UI Variable Text", S(13), FontStyleBold);
    s_fLabel = make(L"Segoe UI Variable Text", S(12), FontStyleRegular);
    s_fGlyph = MakeIconFont(S(10));
    if (s_editFont) DeleteObject(s_editFont);
    s_editFont = CreateFontW(-(int)std::round(S(13)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (s_fieldBrush) DeleteObject(s_fieldBrush);
    s_fieldBrush = CreateSolidBrush(ToColorRef(FieldBg()));
}

static void Paint(HDC hdc) {
    RECT rc;
    GetClientRect(s_popup, &rc);
    PaintBuffered(hdc, rc, true, [&](Graphics& g) {
        const Theme& T = g_th;
        g.SetSmoothingMode(SmoothingModeHighQuality);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        SolidBrush bg(PopupBg());
        g.FillRectangle(&bg, 0, 0, rc.right, rc.bottom);

        StringFormat sf(StringFormat::GenericTypographic());
        sf.SetFormatFlags(sf.GetFormatFlags() | StringFormatFlagsNoWrap);
        sf.SetTrimming(StringTrimmingEllipsisCharacter);
        sf.SetLineAlignment(StringAlignmentCenter);
        SolidBrush text(T.text), text2(T.text2);

        // Title with a filled star
        const float sx = S(kPad);
        DrawGlyph(g, L"\uE735", RectF(sx, S(kTitleY), S(18), S(22)), T.accent, s_fGlyph.get());
        g.DrawString(s_justAdded ? Tr(L"Bookmark added", L"Добавлено в закладки") : Tr(L"Edit bookmark", L"Изменить закладку"),
                     -1, s_fTitle.get(), RectF(sx + S(24), S(kTitleY), S(kW - kPad * 2 - 24), S(22)), &sf, &text);

        g.DrawString(Tr(L"Name", L"Название"), -1, s_fLabel.get(), RectF(sx, S(kNameLabelY), S(200), S(16)), &sf, &text2);
        g.DrawString(Tr(L"Folder", L"Папка"), -1, s_fLabel.get(), RectF(sx, S(kFolderLabelY), S(200), S(16)), &sf, &text2);

        // Name field frame (the edit control sits inside)
        {
            const RECT nr = NameRect();
            RectF r((float)nr.left, (float)nr.top, (float)(nr.right - nr.left), (float)(nr.bottom - nr.top));
            FillRound(g, FieldBg(), r, S(7));
            GraphicsPath p;
            AddRound(p, r, S(7));
            const bool focused = GetFocus() == s_edit;
            Pen pen(focused ? T.accent : T.fieldBorder, focused ? S(2) : 1.f);
            g.DrawPath(&pen, &p);
        }

        // Folder drop-down
        {
            const RectF r = FolderRect();
            FillRound(g, s_hover == 1 ? Over(FieldBg(), T.hover) : FieldBg(), r, S(7));
            GraphicsPath p;
            AddRound(p, r, S(7));
            Pen pen(T.fieldBorder, 1.f);
            g.DrawPath(&pen, &p);
            const Bookmark* b = BookmarkGet(s_id);
            const long long folder = b ? b->parent : kFavoritesFolder;
            DrawGlyph(g, folder == kFavoritesFolder ? L"\uE734" : L"\uE8B7", RectF(r.X + S(10), r.Y, S(16), r.Height), T.text2, s_fGlyph.get());
            g.DrawString(BookmarkFolderName(folder).c_str(), -1, s_fText.get(),
                         RectF(r.X + S(34), r.Y, r.Width - S(64), r.Height), &sf, &text);
            DrawGlyph(g, L"\uE70D", RectF(r.GetRight() - S(28), r.Y, S(16), r.Height), T.text2, s_fGlyph.get());
        }

        // Buttons
        StringFormat cf(StringFormat::GenericTypographic());
        cf.SetAlignment(StringAlignmentCenter);
        cf.SetLineAlignment(StringAlignmentCenter);
        {
            const RectF r = RemoveRect();
            if (s_hover == 2) FillRound(g, T.hover, r, S(7));
            GraphicsPath p;
            AddRound(p, r, S(7));
            Pen pen(T.fieldBorder, 1.f);
            g.DrawPath(&pen, &p);
            SolidBrush red(g_dark ? Color(255, 138, 128) : Color(196, 43, 28));
            g.DrawString(Tr(L"Remove", L"Удалить"), -1, s_fText.get(), r, &cf, &red);
        }
        {
            const RectF r = DoneRect();
            Color c = T.accent;
            if (s_hover == 3) c = Color(255, (BYTE)std::min(255, c.GetR() + 18), (BYTE)std::min(255, c.GetG() + 18), (BYTE)std::min(255, c.GetB() + 18));
            FillRound(g, c, r, S(7));
            SolidBrush white(Color(255, 255, 255));
            g.DrawString(Tr(L"Done", L"Готово"), -1, s_fBold.get(), r, &cf, &white);
        }
    });
}

static void ApplyName() {
    if (!s_edit || !s_id) return;
    wchar_t buf[1024] = {};
    GetWindowTextW(s_edit, buf, ARRAYSIZE(buf));
    BookmarkRename(s_id, buf);
}

void BookmarkPopupClose() {
    if (!s_popup || s_closing) return;
    s_closing = true;
    ApplyName();
    HWND h = s_popup;
    s_popup = nullptr;
    s_edit = nullptr;
    PopupFadeOut(h);
    PopupRevealStop(h);
    DestroyWindow(h);
    s_id = 0;
    s_closing = false;
    s_closedAt = GetTickCount();
    InvalidateChrome();
}

bool BookmarkPopupVisible() { return s_popup != nullptr; }

static void PickFolder() {
    const std::vector<Bookmark> all = BookmarksAll();
    HMENU m = CreatePopupMenu();
    const Bookmark* cur = BookmarkGet(s_id);
    std::vector<long long> ids;
    for (const Bookmark& b : all) {
        if (!b.folder) continue;
        int depth = 0;
        for (const Bookmark* p = BookmarkGet(b.parent); p; p = BookmarkGet(p->parent)) ++depth;
        std::wstring label(depth * 4, L' ');
        label += BookmarkFolderName(b.id);
        ids.push_back(b.id);
        AppendMenuW(m, MF_STRING | ((cur && cur->parent == b.id) ? MF_CHECKED : 0), ids.size(), label.c_str());
        if (b.id == kFavoritesFolder) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    }
    const RectF r = FolderRect();
    POINT pt = { (LONG)r.X, (LONG)r.GetBottom() + 2 };
    ClientToScreen(s_popup, &pt);
    s_inMenu = true;
    const int cmd = MenuTrack(m, TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, s_popup);
    s_inMenu = false;
    DestroyMenu(m);
    if (cmd > 0 && cmd <= (int)ids.size() && s_id) {
        s_lastFolder = ids[cmd - 1];
        if (!cur || cur->parent != s_lastFolder) BookmarkMove(s_id, s_lastFolder, -1);
    }
    if (s_popup) {
        InvalidateRect(s_popup, nullptr, FALSE);
        SetFocus(s_edit);
    }
}

static LRESULT CALLBACK EditProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_KEYDOWN && (w == VK_RETURN || w == VK_ESCAPE)) { BookmarkPopupClose(); return 0; }
    if (msg == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE)) return 0;      // no beep
    if (msg == WM_KEYDOWN && w == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) { SendMessageW(h, EM_SETSEL, 0, -1); return 0; }
    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS) InvalidateRect(GetParent(h), nullptr, FALSE);
    return CallWindowProcW(s_oldEditProc, h, msg, w, l);
}

static LRESULT CALLBACK PopupProc(HWND h, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        Paint(hdc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wParam;
        SetTextColor(dc, ToColorRef(g_th.text));
        SetBkColor(dc, ToColorRef(FieldBg()));
        return (LRESULT)s_fieldBrush;
    }
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE && !s_inMenu) PostMessageW(h, WM_CLOSE, 0, 0);
        return 0;
    case WM_CLOSE:
        if (h == s_popup) BookmarkPopupClose();
        return 0;
    case WM_MOUSEMOVE: {
        const int hit = HitAt(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (hit != s_hover) { s_hover = hit; InvalidateRect(h, nullptr, FALSE); }
        if (!s_tracking) {
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
            s_tracking = true;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        s_tracking = false;
        if (s_hover) { s_hover = 0; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONDOWN: {
        s_down = HitAt(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (s_down == 1) { s_down = 0; PickFolder(); }
        return 0;
    }
    case WM_LBUTTONUP: {
        const int hit = HitAt(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        const int down = s_down;
        s_down = 0;
        if (hit != down) return 0;
        if (hit == 2) {
            const long long id = s_id;
            s_id = 0;                                // nothing to rename
            BookmarkPopupClose();
            BookmarkRemove(id);
        } else if (hit == 3) {
            BookmarkPopupClose();
        }
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && (HWND)wParam == h) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(h, &pt);
            SetCursor(LoadCursor(nullptr, HitAt(pt.x, pt.y) ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_DESTROY:
        s_tracking = false;
        s_hover = s_down = 0;
        return 0;
    }
    return DefWindowProcW(h, msg, wParam, lParam);
}

void BookmarkCurrentPage() {
    Tab* t = ActiveTab();
    if (!t || !CanBookmark(t->url) || !g_hWnd) return;
    if (s_popup) { BookmarkPopupClose(); return; }      // a second click on the star closes it
    if (GetTickCount() - s_closedAt < 300) return;       // ...or already did, when the click took the focus

    long long id = BookmarkFind(t->url);
    s_justAdded = id == 0;
    if (!id) {
        if (!BookmarkGet(s_lastFolder)) s_lastFolder = kFavoritesFolder;
        id = BookmarkAdd(s_lastFolder, t->title, t->url);
        if (!id) return;
    }
    s_id = id;

    static bool registered = false;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = PopupProc;
        wc.hInstance = hi;
        wc.lpszClassName = L"LyraBookmarkPopup";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        if (!RegisterClassExW(&wc)) return;
        registered = true;
    }
    MakeFonts();

    // Under the star, its right edge lined up with the star's, inside the window.
    RECT cr;
    GetClientRect(g_hWnd, &cr);
    const float w = S(kW), hgt = S(kH);
    float x = std::min(g_L.star.GetRight() + S(8) - w, (float)cr.right - S(8) - w);
    x = std::max(S(8), x);
    POINT tl = { (LONG)x, (LONG)(g_L.pill.GetBottom() + S(6)) };
    ClientToScreen(g_hWnd, &tl);
    s_popup = CreateWindowExW(WS_EX_TOOLWINDOW, L"LyraBookmarkPopup", L"", WS_POPUP | WS_CLIPCHILDREN,
                              tl.x, tl.y, (int)w, (int)hgt, g_hWnd, nullptr, hi, nullptr);
    if (!s_popup) return;
    const DWORD round = 2;                               // DWMWCP_ROUND (Windows 11)
    DwmSetWindowAttribute(s_popup, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
    const BOOL dark = g_dark;
    DwmSetWindowAttribute(s_popup, kDwmDarkMode, &dark, sizeof(dark));

    const RECT nr = NameRect();
    const int pad = (int)S(9);
    TEXTMETRICW tm = {};
    {
        HDC dc = GetDC(s_popup);
        HGDIOBJ prev = SelectObject(dc, s_editFont);
        GetTextMetricsW(dc, &tm);
        SelectObject(dc, prev);
        ReleaseDC(s_popup, dc);
    }
    const int eh = tm.tmHeight ? tm.tmHeight : (int)S(18);
    s_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT,
                             nr.left + pad, nr.top + (nr.bottom - nr.top - eh) / 2, nr.right - nr.left - pad * 2, eh,
                             s_popup, nullptr, hi, nullptr);
    if (s_edit) {
        SetWindowTheme(s_edit, L"", L"");
        SendMessageW(s_edit, WM_SETFONT, (WPARAM)s_editFont, FALSE);
        const Bookmark* b = BookmarkGet(s_id);
        SetWindowTextW(s_edit, b ? b->title.c_str() : L"");
        s_oldEditProc = (WNDPROC)SetWindowLongPtrW(s_edit, GWLP_WNDPROC, (LONG_PTR)EditProc);
    }
    PopupShow(s_popup, true);
    SetForegroundWindow(s_popup);
    if (s_edit) {
        SetFocus(s_edit);
        SendMessageW(s_edit, EM_SETSEL, 0, -1);
    }
    InvalidateChrome();
}
