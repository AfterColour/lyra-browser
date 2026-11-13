// Lyra — общие функции рисования GDI+ (скругления, значки).
#pragma once

#include "common/Platform.h"

void AddRound(GraphicsPath& p, const RectF& r, float rad);
void FillRound(Graphics& g, const Color& c, const RectF& r, float rad);
void DrawGlyph(Graphics& g, const wchar_t* glyph, const RectF& r, const Color& c, Font* f);

// Paints `rc` (client coordinates) of a window through a 32-bit off-screen buffer.
//  - opaque: GDI+ draws on the buffer's DC (ClearType text); every pixel is made opaque.
//  - !opaque: the buffer starts fully transparent and is drawn as premultiplied ARGB (greyscale
//    anti-aliased text), so a DWM backdrop (Mica / Acrylic) shows through where nothing is drawn.
//  - fastBlend (with !opaque): blending without gamma correction, about 20 times faster on a large
//    window; text comes out a hair thinner. For frames of an animation; the still picture after it
//    is painted without it.
void PaintBuffered(HDC hdc, const RECT& rc, bool opaque, const std::function<void(Graphics&)>& draw, bool fastBlend = false);
