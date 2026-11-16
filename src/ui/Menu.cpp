// Lyra — свои всплывающие меню (вместо меню Windows): в цветах темы, с иконками и плавной подсветкой.
//
// MenuTrack reads an ordinary HMENU and shows it in Lyra's own windows (one per open submenu).
// Like TrackPopupMenu it runs a message loop of its own until an item is chosen or the menu is
// dismissed. The windows never take the focus: the mouse is captured by the first one and the
// keyboard is read in the loop, so the main window stays active (its title bar does not flicker).
#include "ui/Menu.h"
#include "app/Log.h"
#include "core/Settings.h"
#include "ui/Animation.h"
#include "ui/Draw.h"
#include "ui/Theme.h"

namespace {

// Sizes (before DPI scaling)
constexpr float kPad = 5;          // inside the window, around the items
constexpr float kItemH = 32, kSepH = 9;
constexpr float kIconX = 13, kIconW = 20, kTextGap = 10;
constexpr float kRightPad = 14, kArrowW = 18, kKeysGap = 32;
constexpr float kMinW = 210, kMaxW = 460;
constexpr UINT kTimerPill = 1, kTimerSub = 2;
constexpr UINT kSubDelayMs = 230;

struct Item {
    std::wstring text, keys;
    const wchar_t* icon = nullptr;
    UINT id = 0;
    HMENU sub = nullptr;
    bool sep = false, enabled = true, checked = false;
    wchar_t mnemonic = 0;
    int pos = 0;
    float y = 0, h = 0;           // client coordinates (scaled)
    bool Selectable() const { return !sep && enabled; }
};

struct Level {
    HMENU menu = nullptr;
    HWND wnd = nullptr;
    std::vector<Item> items;
    int hover = -1;
    bool iconCol = false, arrows = false;
    int w = 0, h = 0;
    float pillY = 0, pillH = 0, pillA = 0;   // the highlight, as drawn
    DWORD lastStep = 0;
};

std::vector<Level> s_levels;
bool s_active = false, s_done = false, s_downInside = false;
int s_result = 0, s_resultPos = -1;
HMENU s_resultMenu = nullptr;
HWND s_owner = nullptr;
DWORD s_openedAt = 0;
POINT s_openPt = {};
int s_subLevel = -1, s_subItem = -1;       // a submenu waiting to open (kTimerSub)
std::unique_ptr<Font> s_fText, s_fKeys, s_fIcon, s_fIconSm;

Color MenuBg() { return g_th.popup; }
Color WithAlpha(const Color& c, float a) {
    return Color((BYTE)std::lround(c.GetA() * std::clamp(a, 0.f, 1.f)), c.GetR(), c.GetG(), c.GetB());
}

std::unique_ptr<Font> UiFont(float px, int style = FontStyleRegular) {
    auto f = std::make_unique<Font>(L"Segoe UI Variable Text", px, style, UnitPixel);
    if (!f->IsAvailable()) f = std::make_unique<Font>(L"Segoe UI", px, style, UnitPixel);
    return f;
}

// "&Open\tCtrl+O" -> text "Open", keys "Ctrl+O", mnemonic 'o'
void SplitText(const std::wstring& raw, Item& it) {
    std::wstring left = raw;
    const size_t tab = raw.find(L'\t');
    if (tab != std::wstring::npos) { left = raw.substr(0, tab); it.keys = raw.substr(tab + 1); }
    for (size_t i = 0; i < left.size(); ++i) {
        if (left[i] == L'&') {
            if (i + 1 < left.size() && left[i + 1] == L'&') { it.text += L'&'; ++i; continue; }
            if (i + 1 < left.size() && !it.mnemonic) it.mnemonic = (wchar_t)towlower(left[i + 1]);
            continue;
        }
        it.text += left[i];
    }
}

std::vector<Item> ReadMenu(HMENU m) {
    std::vector<Item> out;
    const int n = GetMenuItemCount(m);
    for (int i = 0; i < n; ++i) {
        MENUITEMINFOW mi = { sizeof(mi) };
        mi.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU | MIIM_DATA | MIIM_STRING;
        mi.dwTypeData = nullptr;
        if (!GetMenuItemInfoW(m, (UINT)i, TRUE, &mi)) continue;
        Item it;
        it.pos = i;
        if (mi.fType & MFT_SEPARATOR) {
            it.sep = true;
            if (out.empty() || out.back().sep) continue;          // no leading / double separators
            out.push_back(it);
            continue;
        }
        std::wstring raw;
        if (mi.cch) {
            raw.resize(mi.cch + 1);
            mi.dwTypeData = &raw[0];
            mi.cch++;
            GetMenuItemInfoW(m, (UINT)i, TRUE, &mi);
            raw.resize(wcslen(raw.c_str()));
        }
        SplitText(raw, it);
        it.id = mi.wID;
        it.sub = mi.hSubMenu;
        it.enabled = !(mi.fState & (MFS_DISABLED | MFS_GRAYED));
        it.checked = (mi.fState & MFS_CHECKED) != 0;
        it.icon = (const wchar_t*)mi.dwItemData;
        out.push_back(it);
    }
    while (!out.empty() && out.back().sep) out.pop_back();
    return out;
}

void Layout(Level& L, float itemH = kItemH) {
    HDC dc = GetDC(nullptr);
    {
        Graphics g(dc);
        StringFormat sf(StringFormat::GenericTypographic());
        float textW = 0, keysW = 0;
        for (auto& it : L.items) {
            if (it.icon || it.checked) L.iconCol = true;
            if (it.sub) L.arrows = true;
            if (it.sep) continue;
            RectF b;
            g.MeasureString(it.text.c_str(), -1, s_fText.get(), PointF(0, 0), &sf, &b);
            textW = std::max(textW, b.Width);
            if (!it.keys.empty()) {
                g.MeasureString(it.keys.c_str(), -1, s_fKeys.get(), PointF(0, 0), &sf, &b);
                keysW = std::max(keysW, b.Width);
            }
        }
        float w = S(kPad) * 2 + (L.iconCol ? S(kIconX + kIconW + kTextGap) : S(kRightPad)) + textW +
                  (keysW > 0 ? S(kKeysGap) + keysW : 0) + (L.arrows ? S(kArrowW) : 0) + S(kRightPad);
        w = std::clamp(w, S(kMinW), S(kMaxW));
        L.w = (int)std::ceil(w);
    }
    ReleaseDC(nullptr, dc);
    float y = S(kPad);
    for (auto& it : L.items) {
        it.y = y;
        it.h = it.sep ? S(kSepH * itemH / kItemH) : S(itemH);
        y += it.h;
    }
    L.h = (int)std::ceil(y + S(kPad));
}

RectF ItemRect(const Level& L, const Item& it) { return RectF(S(kPad), it.y, (float)L.w - S(kPad) * 2, it.h); }

int LevelOf(HWND h) {
    for (size_t i = 0; i < s_levels.size(); ++i) if (s_levels[i].wnd == h) return (int)i;
    return -1;
}

void Paint(Level& L, HDC hdc) {
    RECT rc = { 0, 0, L.w, L.h };
    PaintBuffered(hdc, rc, true, [&](Graphics& g) {
        const Theme& T = g_th;
        g.SetSmoothingMode(SmoothingModeHighQuality);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        SolidBrush bg(MenuBg());
        g.FillRectangle(&bg, 0, 0, L.w, L.h);
        if (L.pillA > 0.01f) {
            const bool pressed = s_downInside && (GetKeyState(VK_LBUTTON) & 0x8000) && L.hover >= 0;
            FillRound(g, WithAlpha(pressed ? T.pressed : T.hover, L.pillA), RectF(S(kPad), L.pillY, (float)L.w - S(kPad) * 2, L.pillH), S(6));
        }
        StringFormat sf(StringFormat::GenericTypographic());
        sf.SetFormatFlags(sf.GetFormatFlags() | StringFormatFlagsNoWrap);
        sf.SetTrimming(StringTrimmingEllipsisCharacter);
        sf.SetLineAlignment(StringAlignmentCenter);
        StringFormat rf(&sf);
        rf.SetAlignment(StringAlignmentFar);
        Pen sep(T.sep, 1.f);
        for (size_t i = 0; i < L.items.size(); ++i) {
            const Item& it = L.items[i];
            const RectF r = ItemRect(L, it);
            if (it.sep) {
                const float y = std::floor(r.Y + r.Height / 2) + 0.5f;
                g.DrawLine(&sep, r.X + S(8), y, r.GetRight() - S(8), y);
                continue;
            }
            const Color text = it.enabled ? T.text : T.iconDis;
            float x = r.X + S(kRightPad) - S(kPad);
            if (L.iconCol) {
                const RectF ir(r.X + S(kIconX) - S(kPad), r.Y, S(kIconW), r.Height);
                if (it.icon) DrawGlyph(g, it.icon, ir, it.enabled ? (it.checked ? T.accent : T.icon) : T.iconDis, s_fIcon.get());
                else if (it.checked) DrawGlyph(g, L"", ir, it.enabled ? T.accent : T.iconDis, s_fIcon.get());
                x = ir.GetRight() + S(kTextGap);
            }
            float right = r.GetRight() - S(kRightPad) + S(kPad);
            if (L.arrows) {
                if (it.sub) DrawGlyph(g, L"", RectF(right - S(kArrowW) + S(4), r.Y, S(kArrowW), r.Height), it.enabled ? T.text2 : T.iconDis, s_fIconSm.get());
                right -= S(kArrowW);
            }
            if (it.checked && it.icon) {                 // the icon is tinted; a check mark on the right too
                DrawGlyph(g, L"", RectF(right - S(16), r.Y, S(16), r.Height), it.enabled ? T.accent : T.iconDis, s_fIconSm.get());
                right -= S(24);
            }
            SolidBrush tb(text), kb(it.enabled ? T.text2 : T.iconDis);
            float keysW = 0;
            if (!it.keys.empty()) {
                RectF b;
                g.MeasureString(it.keys.c_str(), -1, s_fKeys.get(), PointF(0, 0), &sf, &b);
                keysW = b.Width;
                g.DrawString(it.keys.c_str(), -1, s_fKeys.get(), RectF(right - keysW - S(2), r.Y, keysW + S(2), r.Height), &rf, &kb);
                keysW += S(16);
            }
            g.DrawString(it.text.c_str(), -1, s_fText.get(), RectF(x, r.Y, std::max(0.f, right - keysW - x), r.Height), &sf, &tb);
        }
    });
}

// The highlight follows the hovered item: it glides there (or appears on the spot when nothing
// was highlighted) and fades out when the pointer leaves the items.
void PillTarget(Level& L, bool jump) {
    if (L.hover >= 0 && L.hover < (int)L.items.size()) {
        const Item& it = L.items[L.hover];
        if (L.pillA < 0.05f || jump || !AnimHoverEnabled()) { L.pillY = it.y; L.pillH = it.h; }
    }
    if (!AnimHoverEnabled()) L.pillA = L.hover >= 0 ? 1.f : 0.f;
    L.lastStep = GetTickCount();
    SetTimer(L.wnd, kTimerPill, 10, nullptr);
    InvalidateRect(L.wnd, nullptr, FALSE);
}

bool PillStep(Level& L) {
    const DWORD now = GetTickCount();
    const float dt = std::clamp((float)(now - L.lastStep) / 1000.f, 0.001f, 0.05f);
    L.lastStep = now;
    bool moving = false;
    const float k = AnimHoverEnabled() ? AnimFactor(dt, 32.f) : 1.f;
    const float ka = AnimHoverEnabled() ? AnimFactor(dt, 45.f) : 1.f;
    if (L.hover >= 0 && L.hover < (int)L.items.size()) {
        const Item& it = L.items[L.hover];
        L.pillY += (it.y - L.pillY) * k;
        L.pillH += (it.h - L.pillH) * k;
        L.pillA += (1.f - L.pillA) * ka;
        if (std::fabs(it.y - L.pillY) > 0.3f || std::fabs(it.h - L.pillH) > 0.3f || L.pillA < 0.995f) moving = true;
        else { L.pillY = it.y; L.pillH = it.h; L.pillA = 1.f; }
    } else {
        L.pillA += (0.f - L.pillA) * ka;
        if (L.pillA > 0.01f) moving = true; else L.pillA = 0.f;
    }
    InvalidateRect(L.wnd, nullptr, FALSE);
    return moving;
}

void SetHover(int li, int idx, bool jump = false) {
    if (li < 0 || li >= (int)s_levels.size()) return;
    Level& L = s_levels[li];
    if (idx >= 0 && (idx >= (int)L.items.size() || L.items[idx].sep)) idx = -1;
    if (L.hover == idx) return;
    L.hover = idx;
    PillTarget(L, jump);
}

LRESULT CALLBACK MenuProc(HWND h, UINT msg, WPARAM wParam, LPARAM lParam);

void CloseLevelsFrom(int li) {
    while ((int)s_levels.size() > std::max(li, 0)) {
        HWND w = s_levels.back().wnd;
        s_levels.pop_back();
        if (w && li <= 0) PopupFadeOut(w);              // the whole menu goes: it fades (submenus just close)
        if (w) { PopupRevealStop(w); DestroyWindow(w); }
    }
    if (s_subLevel >= (int)s_levels.size()) s_subLevel = -1;
}

bool OpenLevel(HMENU menu, int x, int y, UINT flags, const RECT* parentItem) {
    Level L;
    L.menu = menu;
    L.items = ReadMenu(menu);
    if (L.items.empty()) return false;
    Layout(L);

    POINT anchor = { x, y };
    if (parentItem) anchor = { parentItem->right, parentItem->top };
    HMONITOR mon = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    const RECT wa = mi.rcWork;
    for (float ih = kItemH - 2; L.h > wa.bottom - wa.top && ih >= 22; ih -= 2) Layout(L, ih);
    int left, top;
    if (parentItem) {
        left = parentItem->right - (int)S(2);
        if (left + L.w > wa.right) left = parentItem->left - L.w + (int)S(2);
        top = parentItem->top - (int)S(kPad);
        if (top + L.h > wa.bottom) top = wa.bottom - L.h;
    } else {
        left = (flags & TPM_RIGHTALIGN) ? x - L.w : (flags & TPM_CENTERALIGN) ? x - L.w / 2 : x;
        top = (flags & TPM_BOTTOMALIGN) ? y - L.h : y;
        if (left + L.w > wa.right) left = (flags & TPM_RIGHTALIGN) ? wa.right - L.w : std::max((LONG)wa.left, (LONG)(x - L.w));
        if (top + L.h > wa.bottom) top = (y - L.h >= wa.top) ? y - L.h : wa.bottom - L.h;
    }
    left = std::max((int)wa.left, std::min(left, (int)wa.right - L.w));
    top = std::max((int)wa.top, top);

    HWND owner = s_levels.empty() ? s_owner : s_levels.back().wnd;
    L.wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"LyraMenu", L"", WS_POPUP,
                            left, top, L.w, L.h, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!L.wnd) return false;
    const DWORD round = 2;                               // DWMWCP_ROUND (Windows 11)
    DwmSetWindowAttribute(L.wnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
    const BOOL dark = g_dark;
    DwmSetWindowAttribute(L.wnd, kDwmDarkMode, &dark, sizeof(dark));
    s_levels.push_back(std::move(L));
    PopupShow(s_levels.back().wnd, false);
    return true;
}

void OpenSub(int li, int idx, bool keyboard) {
    if (li < 0 || li >= (int)s_levels.size()) return;
    Level& L = s_levels[li];
    if (idx < 0 || idx >= (int)L.items.size() || !L.items[idx].sub || !L.items[idx].enabled) return;
    if ((int)s_levels.size() > li + 1 && s_levels[li + 1].menu == L.items[idx].sub) {
        if (keyboard) for (size_t i = 0; i < s_levels[li + 1].items.size(); ++i)
            if (s_levels[li + 1].items[i].Selectable()) { SetHover(li + 1, (int)i, true); break; }
        return;
    }
    CloseLevelsFrom(li + 1);
    const RectF r = ItemRect(L, L.items[idx]);
    RECT wr;
    GetWindowRect(L.wnd, &wr);
    RECT pr = { wr.left + (LONG)r.X, wr.top + (LONG)r.Y, wr.left + (LONG)r.GetRight(), wr.top + (LONG)r.GetBottom() };
    HMENU sub = s_levels[li].items[idx].sub;
    if (!OpenLevel(sub, 0, 0, 0, &pr)) return;
    if (keyboard) {
        Level& S2 = s_levels.back();
        for (size_t i = 0; i < S2.items.size(); ++i) if (S2.items[i].Selectable()) { SetHover((int)s_levels.size() - 1, (int)i, true); break; }
    }
}

// The level and item under a point on the screen.
void HitAt(POINT pt, int& li, int& idx) {
    li = idx = -1;
    for (int i = (int)s_levels.size() - 1; i >= 0; --i) {
        RECT wr;
        GetWindowRect(s_levels[i].wnd, &wr);
        if (!PtInRect(&wr, pt)) continue;
        li = i;
        const float cy = (float)(pt.y - wr.top), cx = (float)(pt.x - wr.left);
        const Level& L = s_levels[i];
        for (size_t k = 0; k < L.items.size(); ++k) {
            const RectF r = ItemRect(L, L.items[k]);
            if (cy >= r.Y && cy < r.GetBottom() && cx >= r.X && cx < r.GetRight()) { idx = (int)k; break; }
        }
        return;
    }
}

void Choose(int li, int idx) {
    const Item& it = s_levels[li].items[idx];
    s_result = (int)it.id;
    s_resultMenu = s_levels[li].menu;
    s_resultPos = it.pos;
    s_done = true;
    Log("menu: chose item %d (id %u)", it.pos, it.id);
}

void Cancel() { s_result = 0; s_resultMenu = nullptr; s_resultPos = -1; s_done = true; }

void Activate(int li, int idx, bool keyboard) {
    if (li < 0 || idx < 0 || idx >= (int)s_levels[li].items.size()) return;
    const Item& it = s_levels[li].items[idx];
    if (!it.Selectable()) return;
    if (it.sub) OpenSub(li, idx, keyboard);
    else Choose(li, idx);
}

void OnMouseMove(POINT pt) {
    int li, idx;
    HitAt(pt, li, idx);
    if (li < 0) {
        // Outside: the deepest menu loses its highlight unless it leads to an open submenu.
        const int last = (int)s_levels.size() - 1;
        if (last >= 0) SetHover(last, -1);
        return;
    }
    const Level& L = s_levels[li];
    if (idx >= 0 && !L.items[idx].enabled) idx = -1;
    if (L.hover == idx) return;
    SetHover(li, idx);
    // Hovering an item of a menu closes the submenus beyond it (unless this item opens it).
    if ((int)s_levels.size() > li + 1 && !(idx >= 0 && L.items[idx].sub == s_levels[li + 1].menu)) CloseLevelsFrom(li + 1);
    if (idx >= 0 && L.items[idx].sub) {
        s_subLevel = li;
        s_subItem = idx;
        SetTimer(s_levels[0].wnd, kTimerSub, kSubDelayMs, nullptr);
    }
}

void MoveHover(int dir) {
    Level& L = s_levels.back();
    const int n = (int)L.items.size();
    if (!n) return;
    int i = L.hover;
    for (int step = 0; step < n; ++step) {
        i = (i < 0) ? (dir > 0 ? 0 : n - 1) : (i + dir + n) % n;
        if (L.items[i].Selectable()) { SetHover((int)s_levels.size() - 1, i); return; }
    }
}

bool HandleKey(UINT msg, WPARAM vk) {
    const int last = (int)s_levels.size() - 1;
    Level& L = s_levels[last];
    if (msg == WM_SYSKEYDOWN && (vk == VK_MENU || vk == VK_F10)) { Cancel(); return true; }
    switch (vk) {
    case VK_DOWN: MoveHover(1); break;
    case VK_UP: MoveHover(-1); break;
    case VK_HOME: SetHover(last, -1); MoveHover(1); break;
    case VK_END: SetHover(last, -1); MoveHover(-1); break;
    case VK_RIGHT:
        if (L.hover >= 0 && L.items[L.hover].sub) OpenSub(last, L.hover, true);
        break;
    case VK_LEFT:
        if (last > 0) CloseLevelsFrom(last);
        break;
    case VK_ESCAPE:
        if (last > 0) CloseLevelsFrom(last); else Cancel();
        break;
    case VK_RETURN: case VK_SPACE:
        if (L.hover >= 0) Activate(last, L.hover, true);
        break;
    default: break;
    }
    return true;
}

void HandleChar(wchar_t ch) {
    const int last = (int)s_levels.size() - 1;
    Level& L = s_levels[last];
    ch = (wchar_t)towlower(ch);
    for (size_t i = 0; i < L.items.size(); ++i) {
        const Item& it = L.items[i];
        if (!it.Selectable()) continue;
        const wchar_t first = it.mnemonic ? it.mnemonic : (it.text.empty() ? 0 : (wchar_t)towlower(it.text[0]));
        if (first == ch) { SetHover(last, (int)i); if (it.mnemonic) Activate(last, (int)i, true); return; }
    }
}

// Messages of the menu loop. Returns true when the message was used up here.
bool Filter(MSG& m) {
    switch (m.message) {
    case WM_MOUSEMOVE: case WM_NCMOUSEMOVE:
        OnMouseMove(m.pt);
        return true;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
    case WM_NCLBUTTONDOWN: case WM_NCRBUTTONDOWN: case WM_NCMBUTTONDOWN:
    case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: {
        int li, idx;
        HitAt(m.pt, li, idx);
        if (li < 0) { Cancel(); return true; }
        s_downInside = true;
        if (idx >= 0) InvalidateRect(s_levels[li].wnd, nullptr, FALSE);
        return true;
    }
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_NCLBUTTONUP: case WM_NCRBUTTONUP: {
        int li, idx;
        HitAt(m.pt, li, idx);
        // The release of the click that opened the menu does not choose anything.
        const bool moved = std::abs(m.pt.x - s_openPt.x) > 3 || std::abs(m.pt.y - s_openPt.y) > 3;
        const bool ok = s_downInside || moved || GetTickCount() - s_openedAt > 300;
        s_downInside = false;
        if (li >= 0 && idx >= 0 && ok) Activate(li, idx, false);
        else if (li >= 0) InvalidateRect(s_levels[li].wnd, nullptr, FALSE);
        return true;
    }
    case WM_MBUTTONUP: case WM_XBUTTONUP: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        return true;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        return HandleKey(m.message, m.wParam);
    case WM_CHAR:
        HandleChar((wchar_t)m.wParam);
        return true;
    case WM_KEYUP: case WM_SYSKEYUP: case WM_SYSCHAR: case WM_DEADCHAR: case WM_SYSDEADCHAR:
        return true;
    }
    return false;
}

LRESULT CALLBACK MenuProc(HWND h, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        const int li = LevelOf(h);
        if (li >= 0) Paint(s_levels[li], hdc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_TIMER: {
        const int li = LevelOf(h);
        if (wParam == kTimerPill) {
            if (li < 0 || !PillStep(s_levels[li])) KillTimer(h, kTimerPill);
            return 0;
        }
        if (wParam == kTimerSub) {
            KillTimer(h, kTimerSub);
            if (s_subLevel >= 0 && s_subLevel < (int)s_levels.size() && s_levels[s_subLevel].hover == s_subItem)
                OpenSub(s_subLevel, s_subItem, false);
            s_subLevel = s_subItem = -1;
            return 0;
        }
        break;
    }
    case WM_CANCELMODE:
        Cancel();
        return 0;
    }
    return DefWindowProcW(h, msg, wParam, lParam);
}

}  // namespace

void MenuSetIcon(HMENU menu, UINT id, const wchar_t* glyph) {
    MENUITEMINFOW mi = { sizeof(mi) };
    mi.fMask = MIIM_DATA;
    mi.dwItemData = (ULONG_PTR)glyph;
    SetMenuItemInfoW(menu, id, FALSE, &mi);
}

bool MenuActive() { return s_active; }
void MenuCancel() { if (s_active) Cancel(); }

int MenuTrack(HMENU menu, UINT flags, int x, int y, HWND owner, HMENU* chosenMenu, int* chosenPos) {
    if (chosenMenu) *chosenMenu = nullptr;
    if (chosenPos) *chosenPos = -1;
    if (s_active || !menu) return 0;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = MenuProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"LyraMenu";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        if (!RegisterClassExW(&wc)) return 0;
        registered = true;
    }
    s_fText = UiFont(S(13));
    s_fKeys = UiFont(S(12));
    s_fIcon = MakeIconFont(S(15));
    s_fIconSm = MakeIconFont(S(12));
    s_owner = owner ? GetAncestor(owner, GA_ROOT) : nullptr;
    if (owner && owner != s_owner && IsWindowVisible(owner)) s_owner = owner;
    s_levels.reserve(8);
    s_active = true;
    s_done = s_downInside = false;
    s_result = 0;
    s_resultMenu = nullptr;
    s_resultPos = -1;
    s_subLevel = s_subItem = -1;
    s_openedAt = GetTickCount();
    GetCursorPos(&s_openPt);
    if (!OpenLevel(menu, x, y, flags, nullptr)) { s_active = false; return 0; }
    HWND root = s_levels[0].wnd;
    Log("menu: open, %zu item(s)", s_levels[0].items.size());
    SetCapture(root);
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    HWND fg = GetForegroundWindow();

    MSG m;
    while (!s_done) {
        const BOOL got = GetMessageW(&m, nullptr, 0, 0);
        if (got == 0) { PostQuitMessage((int)m.wParam); break; }
        if (got < 0) break;
        if (!Filter(m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (s_done) break;
        // Another window took the mouse, or the user switched to another program.
        if (GetCapture() != root || GetForegroundWindow() != fg) {
            Log("menu: dismissed (%ls)", GetCapture() != root ? L"the mouse capture was taken" : L"another window came to the front");
            Cancel();
        }
    }

    if (GetCapture() == root) ReleaseCapture();
    CloseLevelsFrom(0);
    s_active = false;
    if (chosenMenu) *chosenMenu = s_resultMenu;
    if (chosenPos) *chosenPos = s_resultPos;
    return s_result;
}
