// Lyra — общие функции рисования GDI+ (скругления, значки).
#pragma once

#include "common/Platform.h"

void AddRound(GraphicsPath& p, const RectF& r, float rad);
void FillRound(Graphics& g, const Color& c, const RectF& r, float rad);
void DrawGlyph(Graphics& g, const wchar_t* glyph, const RectF& r, const Color& c, Font* f);
