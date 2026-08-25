#include "ui/text.h"

#include <cmath>

#include "gfx/font.h"
#include "ui/draw_list.h"

#include "stb/stb_truetype.h"

float TextWidth(const CFont &font, CStringView text)
{
	// stbtt_GetPackedQuad accumulates in the atlas's baked (physical) unit space - divide
	// back down to logical pixels at the end, once, rather than per-glyph.
	float x = 0.0f;
	float y = 0.0f;
	for (std::uint64_t i = 0; i < text.Length; i += 1) {
		const auto c = static_cast<unsigned char>(text.pData[i]);
		if (c < CFont::kFirstChar || c >= CFont::kFirstChar + CFont::kCharCount) {
			continue;
		}
		stbtt_aligned_quad quad;
		stbtt_GetPackedQuad(font.GetPackedChars(), static_cast<int>(font.GetAtlasSize()),
							static_cast<int>(font.GetAtlasSize()), static_cast<int>(c - CFont::kFirstChar), &x, &y,
							&quad, 1);
	}
	return x / font.GetBakeScale();
}

void DrawText(CDrawList &list, const CFont &font, float x, float y, CStringView text, Color color)
{
	if (font.GetAtlas() == nullptr) {
		return;
	}

	// Snapped to the nearest logical pixel before anything else touches it. The atlas is a
	// plain baked bitmap (no SDF), sampled bilinearly - a sub-pixel baseline shift changes
	// exactly which texels land under each glyph edge, so every caller that feeds an
	// EaseToward-animated value straight into a draw position (a hover pill's text, a
	// centered button label, ...) redraws its antialiasing fringe fresh every frame. That's
	// invisible while the value is actually moving fast, but EaseToward only asymptotically
	// approaches its target - right at the tail of any animation the per-frame delta is tiny
	// but still nonzero and still sub-pixel, so the glyph edges keep reflowing with nothing
	// else on screen to mask it, which is exactly what reads as text "bouncing"/"dancing" as
	// an animation settles. Rounding the origin here (not every call site) fixes it in the
	// one place every animated text draw already funnels through.
	x = std::round(x);
	y = std::round(y);

	// stbtt_GetPackedQuad accumulates the pen position and emits quads in the atlas's
	// baked (physical) unit space, not the logical space the rest of this project's draw
	// calls use - convert the baseline in, and each quad's position (not its UVs, which
	// are already atlas-relative 0..1) back out, so glyphs land at the same logical
	// coordinates a caller measuring with TextWidth would expect, while still sampling
	// the crisper, higher-texel-density atlas underneath. See gfx/font.h.
	float penX = x * font.GetBakeScale();
	float penY = y * font.GetBakeScale();
	const float invScale = 1.0f / font.GetBakeScale();

	for (std::uint64_t i = 0; i < text.Length; i += 1) {
		const auto c = static_cast<unsigned char>(text.pData[i]);
		if (c < CFont::kFirstChar || c >= CFont::kFirstChar + CFont::kCharCount) {
			continue;
		}

		stbtt_aligned_quad quad;
		stbtt_GetPackedQuad(font.GetPackedChars(), static_cast<int>(font.GetAtlasSize()),
							static_cast<int>(font.GetAtlasSize()), static_cast<int>(c - CFont::kFirstChar), &penX,
							&penY, &quad, 1);

		list.AddRectTexturedUv(quad.x0 * invScale, quad.y0 * invScale, (quad.x1 - quad.x0) * invScale,
							   (quad.y1 - quad.y0) * invScale, quad.s0, quad.t0, quad.s1, quad.t1, font.GetAtlas(),
							   color);
	}
}

void DrawCenteredText(CDrawList &list, const CFont &font, float x, float y, float w, float h, CStringView text,
					  Color color)
{
	const float textW = TextWidth(font, text);
	// The glyphs' own visual center on the box's center - ascent alone would ignore how far
	// descenders hang below the baseline (GetDescent is negative, see font.h) and sit every
	// label in the app a couple of pixels low inside its button.
	const float baselineY = y + h * 0.5f + (font.GetAscent() + font.GetDescent()) * 0.5f;
	DrawText(list, font, x + (w - textW) * 0.5f, baselineY, text, color);
}

void DrawTextEllipsized(CDrawList &list, const CFont &font, float x, float y, CStringView text, float maxWidth,
						Color color)
{
	if (maxWidth <= 0.0f) {
		return;
	}
	if (TextWidth(font, text) <= maxWidth) {
		DrawText(list, font, x, y, text, color);
		return;
	}

	const CStringView ellipsis = StringViewFromCString("...");
	const float ellipsisWidth = TextWidth(font, ellipsis);
	// Not even the ellipsis fits - drawing a lone "..." in a sliver of space says less
	// than drawing nothing at all, and can still overhang what's beside it.
	if (ellipsisWidth > maxWidth) {
		return;
	}

	// Longest prefix that still leaves room for the ellipsis. Linear from the front rather
	// than a binary search: these are short single-line labels, and walking forward is the
	// only way to stay correct if TextWidth ever stops being a plain sum of advances.
	std::uint64_t fit = 0;
	while (fit < text.Length) {
		const float width = TextWidth(font, CStringView{text.pData, fit + 1});
		if (width + ellipsisWidth > maxWidth) {
			break;
		}
		fit += 1;
	}

	DrawText(list, font, x, y, CStringView{text.pData, fit}, color);
	DrawText(list, font, x + TextWidth(font, CStringView{text.pData, fit}), y, ellipsis, color);
}
