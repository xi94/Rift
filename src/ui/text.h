#pragma once

// Draws text glyph-by-glyph from a baked CFont atlas. Kept separate from ui/draw_list.h
// so CDrawList itself stays font-agnostic (it only knows about the generic explicit-UV
// textured quad these build on). Free functions, not a class - stateless operations over
// a CFont/CDrawList a caller already owns, per STYLE.md's math-helper carve-out.

#include "core/string_view.h"
#include "core/types.h"

class CFont;
class CDrawList;

// Logical-pixel width, regardless of what DPI scale `font` was baked with.
float TextWidth(const CFont &font, CStringView text);

// (x,y) is the text's baseline start (not top-left) - matches stb_truetype convention, so
// glyphs with descenders (g, y, p) render correctly relative to the line.
void DrawText(CDrawList &list, const CFont &font, float x, float y, CStringView text, Color color);

// Centers `text` within the box (x,y,w,h) - a button label (Login/Save/Cancel/Delete/
// Set/Unlock), shared here rather than each widget defining its own copy, which is how
// the original f4 project's Modal and Settings_Panel drifted out of sync with each other
// in the first place before being de-duplicated into this same shared-helper shape.
void DrawCenteredText(CDrawList &list, const CFont &font, float x, float y, float w, float h, CStringView text,
					  Color color);

// DrawText, but never wider than maxWidth: text that doesn't fit is cut and given a
// trailing ellipsis. Exists because this project's panels are laid out in real glyph
// metrics - a label that fits comfortably at the default Font Size can genuinely run into
// (or straight through) the control on the right of its own row once the user raises it,
// and silently painting over a control is worse than an honest "...". CDrawList's clip
// rect isn't an option for this: it's a single active rect rather than a stack (see
// PushClipRect), so a per-label clip inside an already-clipped scroll region would
// clobber the region's own.
void DrawTextEllipsized(CDrawList &list, const CFont &font, float x, float y, CStringView text, float maxWidth,
						Color color);
