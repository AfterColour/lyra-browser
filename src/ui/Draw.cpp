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
