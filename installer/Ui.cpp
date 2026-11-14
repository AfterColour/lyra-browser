// The installer window: one quiet screen on a night sky that matches the icon.
//
// Everything is drawn with GDI+ in DIPs (the Graphics is scaled by the window's DPI) and
// animated on a frame timer: stars twinkle, a shooting star crosses now and then, the icon
// rises in, the options slide open, the button turns into the progress line and back.
// Values move towards their targets with an exponential ease (Smooth), so any change of
// state mid-animation still looks continuous.
#include "Setup.h"
#include <dwmapi.h>
#include <objidl.h>
#include <algorithm>
namespace Gdiplus { using std::min; using std::max; }   // gdiplus.h needs them (NOMINMAX)
#include <gdiplus.h>
#include <shobjidl.h>
#include <algorithm>
#include <cmath>
#include <thread>

using namespace Gdiplus;

namespace {
constexpr UINT WM_APP_DONE = WM_APP + 1;      // the worker finished; wParam 1 ok / 0 failed
constexpr float kW = 560, kH = 380;           // window size in DIPs
enum Id { kNone, kPrimary, kOptionsLink, kChangeDir, kToggle, kClose, kMinimize, kDefaultLink };
enum class Screen { Welcome, Busy, Done, Failed };

const Color kAccentA(255, 158, 128, 255), kAccentB(255, 106, 76, 224), kGold(255, 255, 214, 140);

struct Star { float x, y, r, phase, speed, base; };
struct Hit { int id; RectF r; };

struct {
    HWND hwnd = nullptr;
    Mode mode = Mode::Install;
    Payload* payload = nullptr;
    Options opt;
    std::wstring installedVersion;
    Screen screen = Screen::Welcome;
    bool showOptions = false, confirmClose = false, ok = false;
    Progress prog;
    std::wstring err;
    float scale = 1;
    double start = 0, last = 0, now = 0, doneAt = -10, shootAt = 4, shootStart = -10;
    float shootX = 0, shootY = 0;
    // animated values (0..1 unless noted)
    float optsT = 0, morph = 0, progShown = 0, toggleT = 1, press = 0;
    float hover[8] = {};
    std::wstring sub, subOld;
    float subT = 1;
    PointF mouse{-100, -100};
    bool tracking = false;
    int downId = kNone;
    std::vector<Hit> hits;
    std::vector<Star> stars;
    Bitmap* icon = nullptr;
    Bitmap* iconCache = nullptr;
    int iconCacheSize = 0;
    HDC memDC = nullptr;
    HBITMAP memBmp = nullptr;
    SIZE memSize{};
} S;

double Now() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
float Clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
float Lerp(float a, float b, float t) { return a + (b - a) * t; }
float EaseOut(float t) { t = Clamp01(t); return 1 - (1 - t) * (1 - t) * (1 - t); }
bool Smooth(float& v, float target, float dt, float speed = 10.f) {
    v += (target - v) * (1 - std::exp(-dt * speed));
    if (std::fabs(target - v) < 0.002f) { v = target; return false; }
    return true;
}
Color Alpha(const Color& c, float a) { return Color((BYTE)(Clamp01(a) * c.GetA()), c.GetR(), c.GetG(), c.GetB()); }
Color White(float a) { return Color((BYTE)(Clamp01(a) * 255), 255, 255, 255); }

// ---- fonts ------------------------------------------------------------------------
const wchar_t* Family(std::initializer_list<const wchar_t*> names) {
    for (const wchar_t* n : names) {
        FontFamily f(n);
        if (f.IsAvailable()) return n;
    }
    return L"Segoe UI";
}
Font& TitleFont()  { static Font f(Family({ L"Segoe UI Variable Display Light", L"Segoe UI Light" }), 34, FontStyleRegular, UnitPixel); return f; }
Font& BodyFont()   { static Font f(Family({ L"Segoe UI Variable Text", L"Segoe UI" }), 14, FontStyleRegular, UnitPixel); return f; }
Font& SmallFont()  { static Font f(Family({ L"Segoe UI Variable Text", L"Segoe UI" }), 12.5f, FontStyleRegular, UnitPixel); return f; }
Font& ButtonFont() { static Font f(Family({ L"Segoe UI Variable Text Semibold", L"Segoe UI Semibold" }), 15, FontStyleRegular, UnitPixel); return f; }
Font& GlyphFont()  { static Font f(Family({ L"Segoe Fluent Icons", L"Segoe MDL2 Assets" }), 10, FontStyleRegular, UnitPixel); return f; }

void Text(Graphics& g, const std::wstring& s, Font& f, const Color& c, const RectF& r,
          StringAlignment h = StringAlignmentCenter, bool ellipsisPath = false, bool wrap = false) {
    StringFormat fmt;
    fmt.SetAlignment(h);
    fmt.SetLineAlignment(StringAlignmentCenter);
    if (!wrap) fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    fmt.SetTrimming(ellipsisPath ? StringTrimmingEllipsisPath : StringTrimmingEllipsisCharacter);
    SolidBrush b(c);
    g.DrawString(s.c_str(), (INT)s.size(), &f, r, &fmt, &b);
}

float TextWidth(Graphics& g, const std::wstring& s, Font& f) {
    RectF out;
    g.MeasureString(s.c_str(), (INT)s.size(), &f, PointF(0, 0), &out);
    return out.Width;
}

void RoundRect(GraphicsPath& p, const RectF& r, float rad) {
    rad = std::min(rad, std::min(r.Width, r.Height) / 2);
    const float d = rad * 2;
    if (d <= 0.01f) { p.AddRectangle(r); return; }
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    p.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    p.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    p.CloseFigure();
}

void Glow(Graphics& g, float cx, float cy, float rx, float ry, const Color& c) {
    GraphicsPath p;
    p.AddEllipse(cx - rx, cy - ry, 2 * rx, 2 * ry);
    PathGradientBrush b(&p);
    b.SetCenterColor(c);
    Color edge(0, c.GetR(), c.GetG(), c.GetB());
    INT n = 1;
    b.SetSurroundColors(&edge, &n);
    g.FillPath(&b, &p);
}

Bitmap* LoadPng(const wchar_t* name) {
    HRSRC r = FindResourceW(nullptr, name, (LPCWSTR)RT_RCDATA);
    if (!r) return nullptr;
    const DWORD size = SizeofResource(nullptr, r);
    const void* data = LockResource(LoadResource(nullptr, r));
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size);
    memcpy(GlobalLock(mem), data, size);
    GlobalUnlock(mem);
    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(mem, TRUE, &stream))) return nullptr;
    Bitmap* b = Bitmap::FromStream(stream);
    stream->Release();
    return b;
}

// ---- texts -----------------------------------------------------------------------
std::wstring Version() { return S.mode == Mode::Install && S.payload ? S.payload->version : S.installedVersion; }

std::wstring WelcomeSubtitle() {
    if (S.mode == Mode::Uninstall)
        return S.installedVersion.empty() ? std::wstring(Tr(L"Lyra will be removed from this computer", L"Lyra будет удалена с этого компьютера"))
                                          : Tr(L"Lyra ", L"Lyra ") + S.installedVersion + Tr(L" will be removed from this computer", L" будет удалена с этого компьютера");
    if (S.confirmClose) return Tr(L"Lyra is open - it will close before installing", L"Lyra открыта - она закроется перед установкой");
    if (S.installedVersion == Version()) return Tr(L"Version ", L"Версия ") + Version() + Tr(L" is already installed", L" уже установлена");
    if (!S.installedVersion.empty()) return Tr(L"Update from ", L"Обновление с версии ") + S.installedVersion + Tr(L" to ", L" до ") + Version();
    return Tr(L"Web browser  \x00B7  version ", L"Браузер  \x00B7  версия ") + Version();
}

std::wstring PrimaryText() {
    switch (S.screen) {
    case Screen::Welcome:
        if (S.mode == Mode::Uninstall) return Tr(L"Uninstall", L"Удалить");
        if (S.confirmClose) return Tr(L"Close Lyra and install", L"Закрыть Lyra и установить");
        if (S.installedVersion == Version()) return Tr(L"Reinstall", L"Переустановить");
        return S.installedVersion.empty() ? Tr(L"Install", L"Установить") : Tr(L"Update", L"Обновить");
    case Screen::Failed: return Tr(L"Try again", L"Повторить");
    case Screen::Done: return S.mode == Mode::Install ? Tr(L"Launch Lyra", L"Запустить Lyra") : Tr(L"Close", L"Закрыть");
    default: return L"";
    }
}

void SetSub(const std::wstring& s, bool fade) {
    if (s == S.sub) return;
    if (fade) { S.subOld = S.sub; S.subT = 0; }
    S.sub = s;
}

// ---- actions -----------------------------------------------------------------------
void StartWork() {
    S.screen = Screen::Busy;
    S.showOptions = false;
    S.prog.done = 0;
    S.prog.total = 1;
    S.progShown = 0;
    S.err.clear();
    HWND hwnd = S.hwnd;
    std::thread([hwnd] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool ok = S.mode == Mode::Install ? Install(*S.payload, S.opt, S.prog, S.err) : Uninstall(S.opt, S.prog, S.err);
        CoUninitialize();
        PostMessageW(hwnd, WM_APP_DONE, ok ? 1 : 0, 0);
    }).detach();
}

void ChooseFolder() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD flags = 0;
    dlg->GetOptions(&flags);
    dlg->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(Tr(L"Where to install Lyra", L"Куда установить Lyra"));
    std::wstring parent = S.opt.dir.substr(0, S.opt.dir.find_last_of(L'\\'));
    IShellItem* start = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(parent.c_str(), nullptr, IID_PPV_ARGS(&start)))) {
        dlg->SetFolder(start);
        start->Release();
    }
    if (SUCCEEDED(dlg->Show(S.hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                std::wstring dir = path;
                while (dir.size() > 3 && dir.back() == L'\\') dir.pop_back();
                const std::wstring leaf = dir.substr(dir.find_last_of(L'\\') + 1);
                if (_wcsicmp(leaf.c_str(), L"Lyra") != 0) dir += (dir.back() == L'\\' ? L"" : L"\\") + std::wstring(L"Lyra");
                S.opt.dir = dir;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
}

void Click(int id) {
    switch (id) {
    case kPrimary:
        if (S.screen == Screen::Welcome) {
            if (S.mode == Mode::Install && !S.confirmClose && LyraRunning(S.opt.dir)) {
                S.confirmClose = true;
                SetSub(WelcomeSubtitle(), true);
                return;
            }
            StartWork();
        } else if (S.screen == Screen::Failed) {
            StartWork();
        } else if (S.screen == Screen::Done) {
            if (S.mode == Mode::Install) LaunchLyra(S.opt.dir);
            DestroyWindow(S.hwnd);
        }
        break;
    case kOptionsLink: S.showOptions = !S.showOptions; break;
    case kChangeDir: ChooseFolder(); break;
    case kToggle:
        if (S.mode == Mode::Install) S.opt.desktopShortcut = !S.opt.desktopShortcut;
        else S.opt.removeData = !S.opt.removeData;
        break;
    case kClose: if (S.screen != Screen::Busy) DestroyWindow(S.hwnd); break;
    case kMinimize: ShowWindow(S.hwnd, SW_MINIMIZE); break;
    case kDefaultLink: OpenDefaultAppsSettings(); break;
    }
}

int HitAt(PointF p) {
    for (auto it = S.hits.rbegin(); it != S.hits.rend(); ++it)
        if (it->r.Contains(p)) return it->id;
    return kNone;
}

// ---- frame -----------------------------------------------------------------------
// Moves every animated value one frame on; true while something still moves.
bool Step() {
    const double now = Now();
    const float dt = (float)std::min(0.05, now - S.last);
    S.last = S.now = now;
    bool moving = now - S.start < 1.4;                      // the intro
    const bool uninstall = S.mode == Mode::Uninstall;
    moving |= Smooth(S.optsT, (uninstall ? S.screen == Screen::Welcome || S.screen == Screen::Failed : S.showOptions && S.screen == Screen::Welcome) ? 1.f : 0.f, dt, 9);
    const float frac = (float)S.prog.done.load() / (float)std::max<uint64_t>(1, S.prog.total.load());
    const float progTarget = S.screen == Screen::Busy ? frac : S.screen == Screen::Done ? 1.f : S.progShown;
    moving |= Smooth(S.progShown, progTarget, dt, S.screen == Screen::Done ? 9.f : 4.f);
    // The button becomes the progress line while busy, and comes back once the line is full.
    const bool line = S.screen == Screen::Busy || (S.screen == Screen::Done && S.progShown < 0.995f);
    moving |= Smooth(S.morph, line ? 1.f : 0.f, dt, 8);
    moving |= Smooth(S.toggleT, (uninstall ? S.opt.removeData : S.opt.desktopShortcut) ? 1.f : 0.f, dt, 14);
    moving |= Smooth(S.press, S.downId == kPrimary && HitAt(S.mouse) == kPrimary ? 1.f : 0.f, dt, 22);
    const int over = HitAt(S.mouse);
    for (int i = 0; i < 8; i++) moving |= Smooth(S.hover[i], over == i ? 1.f : 0.f, dt, 14);
    if (S.subT < 1) { S.subT = std::min(1.f, S.subT + dt * 4); moving = true; }
    if (S.screen == Screen::Busy) {
        wchar_t pct[16];
        wsprintfW(pct, L" %d%%", (int)std::lround(S.progShown * 100));
        SetSub((uninstall ? Tr(L"Removing...", L"Удаление...") : Tr(L"Installing...", L"Установка...")) + std::wstring(pct), false);
        moving = true;
    }
    if (now > S.shootAt) {                                  // a shooting star now and then
        S.shootStart = now;
        S.shootAt = now + 7 + (rand() % 600) / 100.0;
        S.shootX = 260 + (float)(rand() % 260);
        S.shootY = 18 + (float)(rand() % 70);
    }
    moving |= now - S.shootStart < 1.0 || now - S.doneAt < 1.6;
    return moving;
}

void DrawToggle(Graphics& g, float x, float y, float t) {
    GraphicsPath track;
    RoundRect(track, RectF(x, y, 38, 22), 11);
    const Color off = White(0.16f);
    Color on = kAccentB;
    Color c((BYTE)Lerp(off.GetA(), on.GetA(), t), (BYTE)Lerp(off.GetR(), on.GetR(), t),
            (BYTE)Lerp(off.GetG(), on.GetG(), t), (BYTE)Lerp(off.GetB(), on.GetB(), t));
    SolidBrush tb(c);
    g.FillPath(&tb, &track);
    SolidBrush knob(White(0.95f));
    g.FillEllipse(&knob, x + 4 + t * 16, y + 4, 14.f, 14.f);
}

void Paint(Graphics& g) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    g.ScaleTransform(S.scale, S.scale);
    S.hits.clear();
    const float t = (float)(S.now - S.start);
    const bool uninstall = S.mode == Mode::Uninstall;

    // Sky
    LinearGradientBrush sky(PointF(0, 0), PointF(kW, kH), Color(255, 20, 16, 50), Color(255, 44, 26, 88));
    g.FillRectangle(&sky, -1.f, -1.f, kW + 2, kH + 2);

    // Layout: the options panel pushes the icon and title up
    const float e = EaseOut(S.optsT) * (uninstall ? 1.f : 1.f);
    const float intro = EaseOut((t - 0.05f) / 0.9f);
    const float iconSize = Lerp(104, 72, e);
    const float iconY = Lerp(52, 30, e) + (1 - intro) * 14;
    const float cx = kW / 2;
    const float titleY = iconY + iconSize + Lerp(10, 4, e);
    const float subY = titleY + 44;
    const float panelY = subY + 36;
    const float btnCY = Lerp(286, 300, e);

    Glow(g, cx, iconY + iconSize * 0.6f, 230, 150, Color((BYTE)(70 * intro), 120, 90, 230));   // nebula
    for (const Star& s : S.stars) {
        const float a = s.base * (0.55f + 0.45f * std::sin(t * s.speed + s.phase)) * Clamp01(t / 1.2f);
        SolidBrush b(White(a));
        g.FillEllipse(&b, s.x - s.r, s.y - s.r, s.r * 2, s.r * 2);
        if (s.r > 1.2f) { SolidBrush h(White(a * 0.18f)); g.FillEllipse(&h, s.x - s.r * 3, s.y - s.r * 3, s.r * 6, s.r * 6); }
    }
    const float st = (float)(S.now - S.shootStart);
    if (st >= 0 && st < 1.0f) {                         // shooting star: a short fading streak
        const float k = EaseOut(st / 0.9f), fade = std::sin(Clamp01(st) * 3.14159f);
        const PointF head(S.shootX - 150 * k, S.shootY + 60 * k), tail(head.X + 70, head.Y - 28);
        LinearGradientBrush lb(tail, head, White(0), White(0.75f * fade));
        Pen p(&lb, 1.4f);
        p.SetStartCap(LineCapRound); p.SetEndCap(LineCapRound);
        g.DrawLine(&p, tail, head);
    }

    // Icon: rises in, then rests; a golden ring spreads from it when the work is done
    if (S.icon) {
        const int px = (int)std::lround(iconSize * S.scale);
        if (px != S.iconCacheSize) {
            delete S.iconCache;
            S.iconCache = new Bitmap(px, px, PixelFormat32bppPARGB);
            Graphics ig(S.iconCache);
            ig.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            ig.SetPixelOffsetMode(PixelOffsetModeHighQuality);
            ig.DrawImage(S.icon, 0, 0, px, px);
            S.iconCacheSize = px;
        }
        const float sc = Lerp(0.92f, 1.f, intro);
        const float sz = iconSize * sc, x = cx - sz / 2, y = iconY + (iconSize - sz) / 2;
        Glow(g, cx, y + sz * 0.55f, sz * 0.75f, sz * 0.62f, Color((BYTE)(60 * intro), 140, 110, 255));
        ColorMatrix cm = { { {1,0,0,0,0}, {0,1,0,0,0}, {0,0,1,0,0}, {0,0,0,intro,0}, {0,0,0,0,1} } };
        ImageAttributes ia;
        ia.SetColorMatrix(&cm);
        g.SetInterpolationMode(InterpolationModeHighQualityBilinear);
        g.DrawImage(S.iconCache, RectF(x, y, sz, sz), 0, 0, (REAL)px, (REAL)px, UnitPixel, &ia);
        const float d = (float)(S.now - S.doneAt);
        if (d >= 0 && d < 1.6f && S.screen == Screen::Done) {
            const float k = EaseOut(d / 1.5f);
            Pen ring(Alpha(kGold, (1 - k) * 0.7f), 2.f);
            const float r = sz * (0.55f + 0.5f * k);
            g.DrawEllipse(&ring, cx - r, y + sz / 2 - r, 2 * r, 2 * r);
        }
    }

    // Title and subtitle (the subtitle cross-fades when it changes)
    Text(g, L"Lyra", TitleFont(), White(intro), RectF(0, titleY, kW, 46));
    const float introSub = EaseOut((t - 0.25f) / 0.8f);
    const RectF subR(60, subY, kW - 120, S.screen == Screen::Failed ? 40.f : 24.f);
    const Color subColor = S.screen == Screen::Failed ? Color(255, 255, 170, 170) : White(0.62f);
    if (S.subT < 1 && !S.subOld.empty()) Text(g, S.subOld, BodyFont(), Alpha(White(0.62f), introSub * (1 - S.subT)), subR, StringAlignmentCenter, false, true);
    Text(g, S.sub, BodyFont(), Alpha(subColor, introSub * S.subT), subR, StringAlignmentCenter, false, true);

    // Options panel
    if (S.optsT > 0.01f) {
        const float a = Clamp01(S.optsT * 1.4f - 0.3f), dy = (1 - EaseOut(S.optsT)) * 10;
        const float x0 = 110, x1 = kW - 110;
        float y = panelY + dy;
        if (!uninstall) {
            Text(g, Tr(L"Folder", L"Папка"), SmallFont(), White(0.55f * a), RectF(x0, y, 80, 30), StringAlignmentNear);
            const std::wstring change = Tr(L"Change", L"Изменить");
            const float cw = TextWidth(g, change, SmallFont()) + 4;
            const RectF changeR(x1 - cw, y, cw, 30);
            Text(g, change, SmallFont(), Alpha(kAccentA, a * (0.85f + 0.15f * S.hover[kChangeDir])), changeR, StringAlignmentFar);
            Text(g, S.opt.dir, SmallFont(), White(0.85f * a), RectF(x0 + 70, y, x1 - cw - 14 - (x0 + 70), 30), StringAlignmentFar, true);
            if (a > 0.5f) S.hits.push_back({ kChangeDir, RectF(changeR.X - 6, y, cw + 12, 30) });
            y += 34;
        }
        const std::wstring label = uninstall ? Tr(L"Also delete settings, history and bookmarks", L"Удалить также настройки, историю и закладки")
                                             : Tr(L"Shortcut on the desktop", L"Ярлык на рабочем столе");
        Text(g, label, SmallFont(), White((uninstall ? 0.8f : 0.55f) * a), RectF(x0, y, x1 - x0 - 50, 30), StringAlignmentNear);
        if (S.screen == Screen::Welcome || S.screen == Screen::Failed || !uninstall) {
            GraphicsContainer gc = g.BeginContainer();
            DrawToggle(g, x1 - 38, y + 4, S.toggleT);
            g.EndContainer(gc);
        }
        if (a > 0.5f && S.screen != Screen::Busy && S.screen != Screen::Done) S.hits.push_back({ kToggle, RectF(x0, y, x1 - x0, 30) });
    }

    // Primary button, which turns into the progress line
    {
        const float m = EaseOut(S.morph) * (S.morph > 0.999f ? 1.f : 1.f);
        const float bi = EaseOut((t - 0.35f) / 0.8f);
        const float w = Lerp(220, 340, m), h = Lerp(46, 4, m);
        const float sc = 1 - 0.03f * S.press;
        const RectF r(cx - w * sc / 2, btnCY - h * sc / 2 + (1 - bi) * 12, w * sc, h * sc);
        GraphicsPath p;
        RoundRect(p, r, r.Height / 2);
        if (m < 0.98f) {
            const float hv = S.hover[kPrimary];
            Glow(g, cx, r.Y + r.Height * 0.8f, w * 0.55f, 26, Alpha(kAccentB, bi * (0.35f + 0.35f * hv) * (1 - m)));
            LinearGradientBrush fill(PointF(r.X, r.Y), PointF(r.GetRight(), r.GetBottom()),
                                     Alpha(kAccentA, bi), Alpha(kAccentB, bi));
            g.FillPath(&fill, &p);
            if (hv > 0) { SolidBrush lift(White(0.10f * hv * bi)); g.FillPath(&lift, &p); }
            Text(g, PrimaryText(), ButtonFont(), White(bi * Clamp01(1 - m * 3)), r);
            if (bi > 0.5f && m < 0.2f) S.hits.push_back({ kPrimary, r });
        }
        if (m > 0.02f) {                                  // the line: track + filled part + glowing head
            SolidBrush track(White(0.12f * m));
            g.FillPath(&track, &p);
            const float fw = std::max(r.Height, r.Width * S.progShown);
            GraphicsPath fp;
            RoundRect(fp, RectF(r.X, r.Y, fw, r.Height), r.Height / 2);
            LinearGradientBrush fill(PointF(r.X, r.Y), PointF(r.GetRight() + 1, r.Y), Alpha(kAccentA, m), Alpha(kGold, m));
            g.FillPath(&fill, &fp);
            Glow(g, r.X + fw, r.Y + r.Height / 2, 14, 10, Alpha(kGold, 0.75f * m));
        }
    }

    // Link under the button
    {
        std::wstring link;
        int id = kNone;
        if (S.screen == Screen::Welcome && !uninstall) {
            link = S.showOptions ? Tr(L"Hide options", L"Скрыть параметры") : Tr(L"Installation options", L"Параметры установки");
            id = kOptionsLink;
        } else if (S.screen == Screen::Done && !uninstall && S.opt.registerApp && S.morph < 0.05f) {
            link = Tr(L"Make Lyra your default browser", L"Сделать Lyra браузером по умолчанию");
            id = kDefaultLink;
        }
        if (id != kNone) {
            const float li = id == kOptionsLink ? EaseOut((t - 0.5f) / 0.8f) : EaseOut((float)(S.now - S.doneAt - 0.4) / 0.6f);
            const float lw = TextWidth(g, link, SmallFont()) + 8;
            const RectF lr(cx - lw / 2, btnCY + 36, lw, 26);
            const float hv = S.hover[id];
            Text(g, link, SmallFont(), White((0.55f + 0.35f * hv) * li), lr);
            if (hv > 0.01f) {
                Pen u(White(0.4f * hv * li), 1);
                g.DrawLine(&u, lr.X + 6, lr.GetBottom() - 4, lr.GetRight() - 6, lr.GetBottom() - 4);
            }
            if (li > 0.5f) S.hits.push_back({ id, lr });
        }
    }

    // Window buttons
    {
        const RectF closeR(kW - 44, 8, 36, 30), minR(kW - 82, 8, 36, 30);
        for (int i = 0; i < 2; i++) {
            const RectF& r = i ? minR : closeR;
            const int id = i ? kMinimize : kClose;
            const bool enabled = id == kMinimize || S.screen != Screen::Busy;
            if (S.hover[id] > 0.01f) {
                GraphicsPath p;
                RoundRect(p, r, 8);
                SolidBrush b(id == kClose ? Color((BYTE)(170 * S.hover[id]), 196, 43, 28) : White(0.10f * S.hover[id]));
                g.FillPath(&b, &p);
            }
            Text(g, i ? L"\xE921" : L"\xE8BB", GlyphFont(), White(enabled ? 0.75f : 0.25f), r);
            if (enabled) S.hits.push_back({ id, r });
        }
    }
}

void Render(HWND hwnd, HDC target) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const SIZE size{ rc.right, rc.bottom };
    if (!S.memDC || size.cx != S.memSize.cx || size.cy != S.memSize.cy) {
        if (S.memDC) { DeleteDC(S.memDC); DeleteObject(S.memBmp); }
        BITMAPINFO bi = {};
        bi.bmiHeader = { sizeof(BITMAPINFOHEADER), size.cx, -size.cy, 1, 32, BI_RGB };
        void* bits = nullptr;
        S.memDC = CreateCompatibleDC(target);
        S.memBmp = CreateDIBSection(target, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        SelectObject(S.memDC, S.memBmp);
        S.memSize = size;
    }
    {
        Graphics g(S.memDC);
        Paint(g);
    }
    BitBlt(target, 0, 0, size.cx, size.cy, S.memDC, 0, 0, SRCCOPY);
}

void UpdateTimer(bool moving) {
    static UINT current = 0;
    const UINT want = moving ? 15 : 33;
    if (want != current) { SetTimer(S.hwnd, 1, want, nullptr); current = want; }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp) return 0;                         // the whole window is ours to draw
        break;
    case WM_NCHITTEST: {
        POINT pt{ (short)LOWORD(lp), (short)HIWORD(lp) };
        ScreenToClient(hwnd, &pt);
        return HitAt(PointF(pt.x / S.scale, pt.y / S.scale)) != kNone ? HTCLIENT : HTCAPTION;   // drag anywhere else
    }
    case WM_NCRBUTTONUP: return 0;                // no system menu on the background
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        Render(hwnd, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER:
        UpdateTimer(Step());
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE:
        S.mouse = PointF((short)LOWORD(lp) / S.scale, (short)HIWORD(lp) / S.scale);
        if (!S.tracking) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            S.tracking = TrackMouseEvent(&tme);
        }
        UpdateTimer(true);
        return 0;
    case WM_MOUSELEAVE:
        S.tracking = false;
        S.mouse = PointF(-100, -100);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && HitAt(S.mouse) != kNone) { SetCursor(LoadCursor(nullptr, IDC_HAND)); return TRUE; }
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        return TRUE;
    case WM_LBUTTONDOWN:
        S.downId = HitAt(S.mouse);
        SetCapture(hwnd);
        return 0;
    case WM_LBUTTONUP: {
        const int id = S.downId;
        S.downId = kNone;
        ReleaseCapture();
        if (id != kNone && HitAt(S.mouse) == id) Click(id);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_RETURN && S.screen != Screen::Busy) Click(kPrimary);
        else if (wp == VK_ESCAPE) Click(kClose);
        return 0;
    case WM_APP_DONE:
        S.ok = wp != 0;
        S.screen = S.ok ? Screen::Done : Screen::Failed;
        S.doneAt = Now() + 0.25;
        if (S.ok) SetSub(S.mode == Mode::Uninstall ? Tr(L"Lyra has been removed. Thank you for trying it", L"Lyra удалена. Спасибо, что попробовали её")
                         : S.installedVersion.empty() ? Tr(L"Lyra is installed", L"Lyra установлена")
                                                      : Tr(L"Lyra is up to date", L"Lyra обновлена"), true);
        else SetSub(S.err.empty() ? Tr(L"Something went wrong", L"Что-то пошло не так") : S.err, true);
        if (S.ok) FlashWindow(hwnd, TRUE);
        UpdateTimer(true);
        return 0;
    case WM_DPICHANGED: {
        S.scale = HIWORD(wp) / 96.f;
        const RECT* r = (const RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_CLOSE:
        if (S.screen == Screen::Busy) return 0;   // not in the middle of copying files
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}  // namespace

int RunUi(HINSTANCE hi, Mode mode, Payload* payload, const Options& opts) {
    S.mode = mode;
    S.payload = payload;
    S.opt = opts;
    S.installedVersion = InstalledVersion();
    S.icon = LoadPng(L"ICONPNG");
    srand(GetTickCount());
    for (int i = 0; i < 70; i++) {
        const float big = (rand() % 100) < 12 ? 1.f : 0.f;
        S.stars.push_back({ (float)(rand() % (int)kW), (float)(rand() % (int)kH), 0.5f + (rand() % 60) / 100.f + big * 0.6f,
                            (rand() % 628) / 100.f, 0.6f + (rand() % 140) / 100.f, 0.25f + (rand() % 55) / 100.f });
    }
    SetSub(WelcomeSubtitle(), false);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"LyraSetupWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    RegisterClassExW(&wc);

    // On the monitor under the mouse, centred in its work area
    POINT cur;
    GetCursorPos(&cur);
    HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    UINT dpi = GetDpiForSystem();
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    if (HMODULE shcore = LoadLibraryW(L"shcore.dll"))
        if (auto fn = (GetDpiForMonitorFn)GetProcAddress(shcore, "GetDpiForMonitor")) { UINT y; fn(mon, 0, &dpi, &y); }
    S.scale = dpi / 96.f;
    const int w = (int)(kW * S.scale), h = (int)(kH * S.scale);
    const RECT& wa = mi.rcWork;
    S.hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName,
                             mode == Mode::Install ? Tr(L"Lyra Setup", L"Установка Lyra") : Tr(L"Uninstall Lyra", L"Удаление Lyra"),
                             WS_POPUP | WS_THICKFRAME | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2, w, h,
                             nullptr, nullptr, hi, nullptr);
    if (!S.hwnd) return 1;
    const DWORD round = 2;                        // DWMWCP_ROUND
    DwmSetWindowAttribute(S.hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &round, sizeof(round));
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(S.hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    const COLORREF border = RGB(58, 42, 104);    // the frame line in the sky's colour, not grey
    DwmSetWindowAttribute(S.hwnd, 34 /*DWMWA_BORDER_COLOR*/, &border, sizeof(border));
    const MARGINS shadow = { 0, 0, 0, 1 };        // keeps the system shadow around the frameless window
    DwmExtendFrameIntoClientArea(S.hwnd, &shadow);
    SetWindowPos(S.hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    S.start = S.last = S.now = Now();
    S.shootAt = S.start + 3.5;
    ShowWindow(S.hwnd, SW_SHOW);
    SetForegroundWindow(S.hwnd);
    UpdateTimer(true);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    delete S.iconCache;
    delete S.icon;
    return S.ok ? 0 : 1;
}
