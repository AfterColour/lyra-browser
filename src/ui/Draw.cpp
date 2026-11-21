// Lyra — общие функции рисования GDI+ (скругления, значки).
#include "ui/Draw.h"

// ----------------------------------------------------------------------------
//  Painting
// ----------------------------------------------------------------------------
void AddRound(GraphicsPath& p, const RectF& r, float rad) {
    float d = rad * 2;
    if (d > r.Width) d = r.Width;
    if (d > r.Height) d = r.Height;
    p.AddArc(r.X, r.Y, d, d, 180.f, 90.f);
    p.AddArc(r.GetRight() - d, r.Y, d, d, 270.f, 90.f);
    p.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0.f, 90.f);
    p.AddArc(r.X, r.GetBottom() - d, d, d, 90.f, 90.f);
    p.CloseFigure();
}

void FillRound(Graphics& g, const Color& c, const RectF& r, float rad) {
    GraphicsPath p;
    AddRound(p, r, rad);
    SolidBrush b(c);
    g.FillPath(&b, &p);
}

void DrawGlyph(Graphics& g, const wchar_t* glyph, const RectF& r, const Color& c, Font* f) {
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsNoClip);
    SolidBrush b(c);
    g.DrawString(glyph, -1, f, r, &sf, &b);
}

// ----------------------------------------------------------------------------
//  Icon font
// ----------------------------------------------------------------------------
// Some trimmed-down Windows builds have no Segoe icon font, or one without these glyphs; then the
// buttons came out as empty boxes. GDI says whether the font really is there and has the glyphs.
static bool SystemIconFontWorks(const wchar_t* family) {
    HDC dc = GetDC(nullptr);
    HFONT f = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, family);
    HGDIOBJ old = SelectObject(dc, f);
    wchar_t face[LF_FACESIZE] = {};
    GetTextFaceW(dc, LF_FACESIZE, face);
    WORD idx[4] = {};
    bool ok = _wcsicmp(face, family) == 0 &&
              GetGlyphIndicesW(dc, L"\uE76B\uE712\uE72C\uE710", 4, idx, GGI_MARK_NONEXISTING_GLYPHS) == 4;
    for (WORD i : idx) if (i == 0xFFFF || i == 0) ok = false;
    SelectObject(dc, old);
    DeleteObject(f);
    ReleaseDC(nullptr, dc);
    return ok;
}

static PrivateFontCollection* s_iconFonts = nullptr;   // the bundled font, when it is used

static const wchar_t* IconFamily() {
    static const wchar_t* family = []() -> const wchar_t* {
        wchar_t force[8] = {};
        const bool bundled = GetEnvironmentVariableW(L"LYRA_BUNDLED_ICONS", force, 8) > 0;   // for testing
        if (!bundled && SystemIconFontWorks(L"Segoe Fluent Icons")) return L"Segoe Fluent Icons";
        if (!bundled && SystemIconFontWorks(L"Segoe MDL2 Assets")) return L"Segoe MDL2 Assets";
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring path = exe;
        path = path.substr(0, path.find_last_of(L'\\')) + L"\\resources\\icons\\LyraIcons.ttf";
        auto* fonts = new PrivateFontCollection();
        if (fonts->AddFontFile(path.c_str()) == Ok && fonts->GetFamilyCount() > 0) {
            s_iconFonts = fonts;
            return L"Lyra Icons";
        }
        delete fonts;
        return L"Segoe MDL2 Assets";
    }();
    return family;
}

std::unique_ptr<Font> MakeIconFont(float px) {
    const wchar_t* family = IconFamily();
    if (s_iconFonts) {
        FontFamily ff(family, s_iconFonts);
        return std::make_unique<Font>(&ff, px, FontStyleRegular, UnitPixel);
    }
    return std::make_unique<Font>(family, px, FontStyleRegular, UnitPixel);
}

// The buffer the pieces are drawn in, kept between paints (main thread only): making a bitmap and
// a DC for each piece of an animation frame was about a third of its time. Pieces bigger than
// kKeepPx get a buffer of their own.
namespace {
constexpr int kKeepPx = 1 << 20;
struct PaintBuffer {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    void* bits = nullptr;
    int w = 0, h = 0;
    bool Make(HDC ref, int bw, int bh) {
        Free();
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = bw;
        bi.bmiHeader.biHeight = -bh;               // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        bmp = CreateDIBSection(ref, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp || !bits) { if (bmp) DeleteObject(bmp); bmp = nullptr; bits = nullptr; return false; }
        dc = CreateCompatibleDC(ref);
        old = SelectObject(dc, bmp);
        w = bw;
        h = bh;
        return true;
    }
    void Free() {
        if (dc) { SelectObject(dc, old); DeleteDC(dc); }
        if (bmp) DeleteObject(bmp);
        dc = nullptr; bmp = nullptr; bits = nullptr; w = h = 0;
    }
};
PaintBuffer s_kept;
}

void PaintBuffered(HDC hdc, const RECT& rc, bool opaque, const std::function<void(Graphics&)>& draw, bool fastBlend) {
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    PaintBuffer own;
    PaintBuffer* buf = &own;
    static int s_depth = 0;                            // (a paint inside a paint: its own buffer)
    struct Depth { Depth() { ++s_depth; } ~Depth() { --s_depth; } } depth;
    if ((long long)w * h <= kKeepPx && s_depth == 1) {
        buf = &s_kept;
        if (buf->w < w || buf->h < h) {                // grows to the largest piece seen (up to kKeepPx)
            int bw = std::max(w, buf->w), bh = std::max(h, buf->h);
            if ((long long)bw * bh > kKeepPx) { bw = w; bh = h; }
            if (!buf->Make(hdc, bw, bh)) return;
        }
    } else if (!own.Make(hdc, w, h)) {
        return;
    }
    const int stride = buf->w;                         // (pixels per row of the buffer)
    DWORD* px = (DWORD*)buf->bits;
    for (int y = 0; y < h; ++y) memset(px + (size_t)y * stride, 0, (size_t)w * 4);   // (as a new bitmap)
    if (opaque) {
        {
            Graphics g(buf->dc);
            g.SetClip(Rect(0, 0, w, h));
            g.TranslateTransform(-(REAL)rc.left, -(REAL)rc.top);
            g.SetSmoothingMode(SmoothingModeHighQuality);
            g.SetPixelOffsetMode(PixelOffsetModeHalf);
            g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
            draw(g);
        }
        GdiFlush();
        // GDI+ leaves alpha 0 where it fills through GDI; DWM would show those pixels as
        // see-through in the area that used to be the title bar.
        for (int y = 0; y < h; ++y) {
            DWORD* row = px + (size_t)y * stride;
            for (int x = 0; x < w; ++x) row[x] |= 0xFF000000u;
        }
    } else {
        Bitmap canvas(w, h, stride * 4, PixelFormat32bppPARGB, (BYTE*)px);
        Graphics g(&canvas);
        g.TranslateTransform(-(REAL)rc.left, -(REAL)rc.top);
        g.SetSmoothingMode(SmoothingModeHighQuality);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.SetCompositingQuality(fastBlend ? CompositingQualityHighSpeed : CompositingQualityHighQuality);
        draw(g);
        g.Flush(FlushIntentionSync);
    }
    BitBlt(hdc, rc.left, rc.top, w, h, buf->dc, 0, 0, SRCCOPY);
    own.Free();
}

