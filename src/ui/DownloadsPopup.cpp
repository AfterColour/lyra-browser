// Lyra — кнопка загрузок: список последних загрузок под кнопкой (прогресс, открыть, показать в папке).
//
// A window owned by the main window, under the downloads button, like the bookmark bubble: it is
// activated and closes as soon as it loses the focus.
#include "ui/DownloadsPopup.h"
#include "browser/Downloads.h"
#include "browser/StartPage.h"
#include "browser/TabManager.h"
#include "core/Settings.h"
#include "core/Localization.h"
#include "core/StringUtils.h"
#include "core/Url.h"
#include "ui/Chrome.h"
#include "ui/Draw.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"

#include <commoncontrols.h>

static HWND s_popup = nullptr;
static DWORD s_closedAt = 0;
static bool s_tracking = false, s_inMenu = false;
static std::unique_ptr<Font> s_fTitle, s_fName, s_fSmall, s_fGlyph;
static std::map<std::wstring, HICON> s_icons;          // file icon per extension

// What the pointer is over: a row (index) and which part of it.
struct PHit {
    enum Part { None, Row, Action, Clear, ShowAll, Folder } part = None;
    int row = -1;
    bool operator==(const PHit& o) const { return part == o.part && row == o.row; }
};
static PHit s_hover, s_down;

constexpr float kW = 380, kHeadH = 46, kRowH = 60, kFootH = 44, kPad = 16;
constexpr int kMaxRows = 6;

static Color PopupBg() { return g_dark ? Color(37, 39, 44) : Color(255, 255, 255); }

static int RowCount() { return (int)std::min<size_t>(kMaxRows, DownloadsList().size()); }
static float BodyH() { return RowCount() ? kRowH * (float)RowCount() : kRowH; }
static int PopupH() { return (int)S(kHeadH + BodyH() + kFootH); }

static RectF RowRect(int i) { return RectF(S(6), S(kHeadH) + S(kRowH) * (float)i, S(kW - 12), S(kRowH)); }
static RectF ActionRect(int i) {
    const RectF r = RowRect(i);
    const float b = S(30);
    return RectF(r.GetRight() - S(8) - b, r.Y + (r.Height - b) / 2, b, b);
}
static RectF ClearRect() { return RectF(S(kW - kPad - 90), S(9), S(90), S(28)); }
static RectF ShowAllRect() { return RectF(S(8), S(kHeadH) + S(BodyH()) + S(8), S(150), S(28)); }
static RectF FolderRect() { return RectF(S(kW - 8 - 150), S(kHeadH) + S(BodyH()) + S(8), S(150), S(28)); }

static bool HasFinished() {
    for (auto& d : DownloadsList()) if (d.state != DownloadItem::Running) return true;
    return false;
}

// The action button of a row: cancel while running, "show in folder" when done, retry otherwise.
static const wchar_t* ActionGlyph(const DownloadItem& d) {
    if (d.state == DownloadItem::Running) return L"\uE711";
    if (d.state == DownloadItem::Done) return DownloadFileExists(d) ? L"\uE838" : nullptr;
    return d.url.empty() ? nullptr : L"\uE72C";
}

static PHit HitAt(int x, int y) {
    const float fx = (float)x, fy = (float)y;
    PHit h;
    if (HasFinished() && ClearRect().Contains(fx, fy)) { h.part = PHit::Clear; return h; }
    if (ShowAllRect().Contains(fx, fy)) { h.part = PHit::ShowAll; return h; }
    if (FolderRect().Contains(fx, fy)) { h.part = PHit::Folder; return h; }
    const auto& list = DownloadsList();
    for (int i = 0; i < RowCount(); ++i) {
        if (!RowRect(i).Contains(fx, fy)) continue;
        h.row = i;
        h.part = (ActionGlyph(list[i]) && ActionRect(i).Contains(fx, fy)) ? PHit::Action : PHit::Row;
        return h;
    }
    return h;
}

std::wstring FormatBytes(long long b) {
    const bool ru = UiRussian();
    const wchar_t* units[] = { ru ? L"Б" : L"B", ru ? L"КБ" : L"KB", ru ? L"МБ" : L"MB", ru ? L"ГБ" : L"GB" };
    double v = (double)std::max(0LL, b);
    int u = 0;
    while (v >= 1000 && u < 3) { v /= 1024; ++u; }
    wchar_t buf[32];
    if (u == 0 || v >= 100) swprintf(buf, 32, L"%.0f %ls", v, units[u]);
    else swprintf(buf, 32, L"%.1f %ls", v, units[u]);
    std::wstring s = buf;
    if (ru) for (auto& c : s) if (c == L'.') c = L',';
    return s;
}

static std::wstring FormatTimeLeft(double seconds) {
    const bool ru = UiRussian();
    if (seconds < 60) return ru ? L"осталось меньше минуты" : L"less than a minute left";
    const long long m = (long long)std::ceil(seconds / 60);
    if (m < 60) return (ru ? L"осталось " : L"") + std::to_wstring(m) + (ru ? L" мин" : L" min left");
    const long long h = m / 60, mm = m % 60;
    return (ru ? L"осталось " : L"") + std::to_wstring(h) + (ru ? L" ч " : L" h ") + std::to_wstring(mm) +
           (ru ? L" мин" : L" min left");
}

std::wstring DownloadStatusText(long long id) {
    const DownloadItem* d = DownloadGet(id);
    if (!d) return std::wstring();
    switch (d->state) {
    case DownloadItem::Running: {
        std::wstring s = FormatBytes(d->received);
        if (d->total > 0) s += Tr(L" of ", L" из ") + FormatBytes(d->total);
        if (d->speed > 1) {
            s += L" \x2014 " + FormatBytes((long long)d->speed) + Tr(L"/s", L"/с");
            if (d->total > d->received) s += L", " + FormatTimeLeft((double)(d->total - d->received) / d->speed);
        }
        return s;
    }
    case DownloadItem::Done: {
        if (!DownloadFileExists(*d)) return Tr(L"Moved or deleted", L"Файл перемещён или удалён");
        std::wstring s = FormatBytes(d->received);
        const std::wstring host = HostOf(d->url);
        if (!host.empty()) s += L" \x2014 " + host;
        return s;
    }
    case DownloadItem::Failed:
        return d->error == L"interrupted" ? Tr(L"Interrupted when Lyra closed", L"Прервана при закрытии Lyra")
             : d->error.empty() ? Tr(L"Failed", L"Не удалось загрузить")
             : std::wstring(Tr(L"Failed \x2014 ", L"Ошибка \x2014 ")) + d->error;
    case DownloadItem::Cancelled:
        return Tr(L"Cancelled", L"Отменена");
    }
    return std::wstring();
}

// The system's icon for files with this name's extension (cached).
static HICON FileIcon(const std::wstring& name) {
    const size_t dot = name.rfind(L'.');
    std::wstring ext = dot == std::wstring::npos ? L"" : name.substr(dot);
    CharLowerBuffW(ext.empty() ? nullptr : &ext[0], (DWORD)ext.size());
    auto it = s_icons.find(ext);
    if (it != s_icons.end()) return it->second;
    HICON icon = nullptr;
    SHFILEINFOW fi = {};
    const std::wstring probe = L"file" + ext;
    if (SHGetFileInfoW(probe.c_str(), FILE_ATTRIBUTE_NORMAL, &fi, sizeof(fi), SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX)) {
        IImageList* list = nullptr;
        if (SUCCEEDED(SHGetImageList(SHIL_EXTRALARGE, IID_PPV_ARGS(&list))) && list) {
            list->GetIcon(fi.iIcon, ILD_TRANSPARENT, &icon);
            list->Release();
        }
    }
    s_icons[ext] = icon;
    return icon;
}

static void Paint(HDC hdc) {
    RECT rc;
    GetClientRect(s_popup, &rc);
    const auto& list = DownloadsList();
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
        StringFormat cf(StringFormat::GenericTypographic());
        cf.SetFormatFlags(cf.GetFormatFlags() | StringFormatFlagsNoWrap);
        cf.SetAlignment(StringAlignmentCenter);
        cf.SetLineAlignment(StringAlignmentCenter);
        SolidBrush text(T.text), text2(T.text2), accent(T.accent);
        auto hoverBox = [&](const RectF& r, PHit::Part part, int row) {
            PHit h; h.part = part; h.row = row;
            if (s_down == h && s_hover == h) FillRound(g, T.pressed, r, S(6));
            else if (s_hover == h) FillRound(g, T.hover, r, S(6));
        };

        // Header
        g.DrawString(Tr(L"Downloads", L"Загрузки"), -1, s_fTitle.get(), RectF(S(kPad), 0, S(200), S(kHeadH)), &sf, &text);
        if (HasFinished()) {
            const RectF r = ClearRect();
            hoverBox(r, PHit::Clear, -1);
            g.DrawString(Tr(L"Clear", L"Очистить"), -1, s_fSmall.get(), r, &cf, &accent);
        }
        Pen sep(T.sep, 1.f);
        g.DrawLine(&sep, 0.f, S(kHeadH) - 0.5f, (float)rc.right, S(kHeadH) - 0.5f);

        // Rows
        if (list.empty()) {
            g.DrawString(Tr(L"No downloads yet", L"Загрузок пока нет"), -1, s_fName.get(),
                         RectF(0, S(kHeadH), (float)rc.right, S(kRowH)), &cf, &text2);
        }
        for (int i = 0; i < RowCount(); ++i) {
            const DownloadItem& d = list[i];
            const RectF r = RowRect(i);
            hoverBox(r, PHit::Row, i);
            // file icon
            const float is = S(32);
            if (HICON icon = FileIcon(d.name)) {
                HDC dc = g.GetHDC();
                DrawIconEx(dc, (int)(r.X + S(10)), (int)(r.Y + (r.Height - is) / 2), icon, (int)is, (int)is, 0, nullptr, DI_NORMAL);
                g.ReleaseHDC(dc);
            } else {
                DrawGlyph(g, L"\uE8A5", RectF(r.X + S(10), r.Y, is, r.Height), T.text2, s_fGlyph.get());
            }
            const float tx = r.X + S(10) + is + S(12);
            const float right = (ActionGlyph(d) ? ActionRect(i).X : r.GetRight()) - S(8);
            const bool faded = d.state == DownloadItem::Done && !DownloadFileExists(d);
            SolidBrush nameBrush(faded || d.state == DownloadItem::Cancelled ? T.text2 : T.text);
            g.DrawString(d.name.c_str(), -1, s_fName.get(), RectF(tx, r.Y + S(9), right - tx, S(20)), &sf, &nameBrush);
            const std::wstring status = DownloadStatusText(d.id);
            const bool failed = d.state == DownloadItem::Failed;
            SolidBrush statusBrush(failed ? (g_dark ? Color(255, 138, 128) : Color(196, 43, 28)) : T.text2);
            if (d.state == DownloadItem::Running) {
                // progress bar, then the numbers
                const float bh = std::max(2.f, std::floor(S(4)));
                const RectF track(tx, r.Y + S(32), right - tx, bh);
                FillRound(g, Color(g_dark ? 60 : 30, T.text.GetR(), T.text.GetG(), T.text.GetB()), track, bh / 2);
                if (d.total > 0) {
                    const float p = std::min(1.f, (float)((double)d.received / (double)d.total));
                    if (p > 0) FillRound(g, T.accent, RectF(track.X, track.Y, std::max(bh, track.Width * p), bh), bh / 2);
                } else {
                    const float t = (float)(GetTickCount() % 1400) / 1400.f;
                    const float sw = track.Width * 0.25f, sx = track.X + (track.Width - sw) * (t < 0.5f ? t * 2 : 2 - t * 2);
                    FillRound(g, T.accent, RectF(sx, track.Y, sw, bh), bh / 2);
                }
                g.DrawString(status.c_str(), -1, s_fSmall.get(), RectF(tx, r.Y + S(38), right - tx, S(16)), &sf, &statusBrush);
            } else {
                g.DrawString(status.c_str(), -1, s_fSmall.get(), RectF(tx, r.Y + S(31), right - tx, S(18)), &sf, &statusBrush);
            }
            if (const wchar_t* glyph = ActionGlyph(d)) {
                const RectF a = ActionRect(i);
                hoverBox(a, PHit::Action, i);
                DrawGlyph(g, glyph, a, T.icon, s_fGlyph.get());
            }
        }

        // Footer
        const float fy = S(kHeadH) + S(BodyH());
        g.DrawLine(&sep, 0.f, fy + 0.5f, (float)rc.right, fy + 0.5f);
        {
            const RectF r = ShowAllRect();
            hoverBox(r, PHit::ShowAll, -1);
            g.DrawString(Tr(L"Show all downloads", L"Все загрузки"), -1, s_fSmall.get(), r, &cf, &accent);
        }
        {
            const RectF r = FolderRect();
            hoverBox(r, PHit::Folder, -1);
            g.DrawString(Tr(L"Open folder", L"Открыть папку"), -1, s_fSmall.get(), r, &cf, &accent);
        }
    });
}

static void ContextMenu(int row, POINT screen) {
    const auto& list = DownloadsList();
    if (row < 0 || row >= (int)list.size()) return;
    const DownloadItem d = list[row];
    const bool exists = d.state == DownloadItem::Done && DownloadFileExists(d);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (exists ? 0 : MF_GRAYED), 1, Tr(L"Open", L"Открыть"));
    AppendMenuW(m, MF_STRING | (exists ? 0 : MF_GRAYED), 2, Tr(L"Show in Folder", L"Показать в папке"));
    if (d.state == DownloadItem::Running) AppendMenuW(m, MF_STRING, 3, Tr(L"Cancel", L"Отменить"));
    else if (!d.url.empty() && d.state != DownloadItem::Done) AppendMenuW(m, MF_STRING, 4, Tr(L"Retry", L"Повторить"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (d.url.empty() ? MF_GRAYED : 0), 5, Tr(L"Copy Download Link", L"Копировать ссылку"));
    AppendMenuW(m, MF_STRING | (d.state == DownloadItem::Running ? MF_GRAYED : 0), 6, Tr(L"Remove from List", L"Убрать из списка"));
    s_inMenu = true;
    const int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, screen.x, screen.y, 0, s_popup, nullptr);
    s_inMenu = false;
    DestroyMenu(m);
    switch (cmd) {
    case 1: DownloadsPopupClose(); DownloadOpen(d.id); break;
    case 2: DownloadsPopupClose(); DownloadShowInFolder(d.id); break;
    case 3: DownloadCancel(d.id); break;
    case 4: DownloadsPopupClose(); DownloadRetry(d.id); break;
    case 5:
        if (OpenClipboard(s_popup)) {
            EmptyClipboard();
            const size_t bytes = (d.url.size() + 1) * sizeof(wchar_t);
            if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
                memcpy(GlobalLock(h), d.url.c_str(), bytes);
                GlobalUnlock(h);
                if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
            }
            CloseClipboard();
        }
        break;
    case 6: DownloadRemove(d.id); break;
    }
}

static void Activate(const PHit& h) {
    const auto& list = DownloadsList();
    switch (h.part) {
    case PHit::Clear: DownloadsClear(); break;
    case PHit::ShowAll: DownloadsPopupClose(); OpenDownloads(); break;
    case PHit::Folder: DownloadsPopupClose(); OpenDownloadsFolder(); break;
    case PHit::Row:
        if (h.row >= 0 && h.row < (int)list.size() && list[h.row].state == DownloadItem::Done && DownloadFileExists(list[h.row])) {
            const long long id = list[h.row].id;
            DownloadsPopupClose();
            DownloadOpen(id);
        }
        break;
    case PHit::Action:
        if (h.row >= 0 && h.row < (int)list.size()) {
            const DownloadItem& d = list[h.row];
            const long long id = d.id;
            if (d.state == DownloadItem::Running) DownloadCancel(id);
            else if (d.state == DownloadItem::Done) { DownloadsPopupClose(); DownloadShowInFolder(id); }
            else { DownloadsPopupClose(); DownloadRetry(id); }
        }
        break;
    default: break;
    }
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
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE && !s_inMenu) PostMessageW(h, WM_CLOSE, 0, 0);
        return 0;
    case WM_CLOSE:
        if (h == s_popup) DownloadsPopupClose();
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) DownloadsPopupClose();
        return 0;
    case WM_MOUSEMOVE: {
        const PHit hit = HitAt(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (!(hit == s_hover)) { s_hover = hit; InvalidateRect(h, nullptr, FALSE); }
        if (!s_tracking) {
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
            s_tracking = true;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        s_tracking = false;
        s_hover = PHit();
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        s_down = HitAt(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        const PHit up = HitAt(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        const PHit down = s_down;
        s_down = PHit();
        if (s_popup) InvalidateRect(h, nullptr, FALSE);
        if (up == down && up.part != PHit::None) Activate(up);
        return 0;
    }
    case WM_RBUTTONUP: {
        const PHit hit = HitAt(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ClientToScreen(h, &pt);
        if (hit.row >= 0) ContextMenu(hit.row, pt);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(h, &pt);
            const PHit hit = HitAt(pt.x, pt.y);
            bool hand = hit.part == PHit::Action || hit.part == PHit::Clear || hit.part == PHit::ShowAll || hit.part == PHit::Folder;
            if (hit.part == PHit::Row) {
                const auto& list = DownloadsList();
                hand = hit.row < (int)list.size() && list[hit.row].state == DownloadItem::Done && DownloadFileExists(list[hit.row]);
            }
            SetCursor(LoadCursor(nullptr, hand ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_DESTROY:
        s_tracking = false;
        s_hover = s_down = PHit();
        return 0;
    }
    return DefWindowProcW(h, msg, wParam, lParam);
}

static void PlacePopup() {
    if (!s_popup) return;
    RECT cr;
    GetClientRect(g_hWnd, &cr);
    const float w = S(kW);
    const RectF& b = g_L.downloads.Width > 0 ? g_L.downloads : g_L.more;
    float x = std::min(b.GetRight() + S(4) - w, (float)cr.right - S(8) - w);
    x = std::max(S(8), x);
    POINT tl = { (LONG)x, (LONG)(b.GetBottom() + S(6)) };
    ClientToScreen(g_hWnd, &tl);
    SetWindowPos(s_popup, nullptr, tl.x, tl.y, (int)w, PopupH(), SWP_NOZORDER | SWP_NOACTIVATE);
}

void DownloadsPopupShow() {
    if (!g_hWnd || g_closing) return;
    if (s_popup) { DownloadsPopupRefresh(); return; }
    static bool registered = false;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = PopupProc;
        wc.hInstance = hi;
        wc.lpszClassName = L"LyraDownloadsPopup";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        if (!RegisterClassExW(&wc)) return;
        registered = true;
    }
    auto make = [](const wchar_t* family, float px, int style) {
        auto f = std::make_unique<Font>(family, px, style, UnitPixel);
        if (!f->IsAvailable()) f = std::make_unique<Font>(L"Segoe UI", px, style, UnitPixel);
        return f;
    };
    s_fTitle = make(L"Segoe UI Variable Text", S(14), FontStyleBold);
    s_fName = make(L"Segoe UI Variable Text", S(13), FontStyleRegular);
    s_fSmall = make(L"Segoe UI Variable Text", S(12), FontStyleRegular);
    s_fGlyph = make(L"Segoe Fluent Icons", S(12), FontStyleRegular);
    if (!s_fGlyph->IsAvailable()) s_fGlyph = make(L"Segoe MDL2 Assets", S(12), FontStyleRegular);

    s_popup = CreateWindowExW(WS_EX_TOOLWINDOW, L"LyraDownloadsPopup", L"", WS_POPUP, 0, 0, 10, 10, g_hWnd, nullptr, hi, nullptr);
    if (!s_popup) return;
    const DWORD round = 2;                               // DWMWCP_ROUND (Windows 11)
    DwmSetWindowAttribute(s_popup, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
    const BOOL dark = g_dark;
    DwmSetWindowAttribute(s_popup, kDwmDarkMode, &dark, sizeof(dark));
    PlacePopup();
    ShowWindow(s_popup, SW_SHOW);
    SetForegroundWindow(s_popup);
    SetFocus(s_popup);
    InvalidateChrome();
}

void DownloadsPopupToggle() {
    if (s_popup) { DownloadsPopupClose(); return; }
    if (GetTickCount() - s_closedAt < 300) return;       // this click already closed it (focus moved)
    DownloadsPopupShow();
}

void DownloadsPopupClose() {
    if (!s_popup) return;
    HWND h = s_popup;
    s_popup = nullptr;
    DestroyWindow(h);
    s_closedAt = GetTickCount();
    InvalidateChrome();
}

bool DownloadsPopupVisible() { return s_popup != nullptr; }

// Every change to the downloads: the toolbar button (it appears with the first download), this
// list, an open downloads page; a new download opens the list (Settings > Downloads).
static void OnDownloadsChanged(bool listChanged) {
    static bool hadButton = false;
    static long long newestSeen = -1;
    const auto& list = DownloadsList();
    const bool button = !list.empty();
    if (button != hadButton) {
        hadButton = button;
        RefreshLayout(true);
    }
    InvalidateChrome();
    DownloadsPopupRefresh();
    PushDownloadsToPages();
    if (!listChanged) return;
    const long long newest = list.empty() ? 0 : list.front().id;
    if (newestSeen >= 0 && newest > newestSeen && list.front().state == DownloadItem::Running) {
        ChromeDownloadStarted();
        if (g_prefs.downloadsPopup && !s_popup) DownloadsPopupShow();
    }
    newestSeen = std::max(newestSeen, newest);
}
static const bool s_hooked = (g_downloadsChanged = &OnDownloadsChanged, true);

void DownloadsPopupRefresh() {
    if (!s_popup) return;
    RECT wr;
    GetWindowRect(s_popup, &wr);
    if (wr.bottom - wr.top != PopupH()) PlacePopup();
    InvalidateRect(s_popup, nullptr, FALSE);
}
