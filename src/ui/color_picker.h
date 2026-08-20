#pragma once

// A popup color picker (e.g. for CSettingsPanel's Accent Color swatch): a saturation/
// value square (hue fixed by the strip below it) plus a vertical alpha slider to the
// right, opened by clicking the swatch it's anchored to. GetCurrentColor reads live while
// dragging, not just once confirmed - a caller (CSettingsPanel) writes it straight into
// CSettings::m_clrAccent on every drag move, the same "applies live" behavior the
// original had. Closing the popup (clicking the swatch again, or anywhere outside it) is
// dismissal, not a separate confirm step - there is no cancel.
//
// The SV square is drawn through CDrawList::AddRectColorPickerSv, a real per-pixel
// shader, not vertex-color interpolation: color(S,V) = V*lerp(white,pureHue,S) has a
// genuine S*V cross term that a quad's two triangles can't reproduce exactly (see
// AddRectGradientCorners's own doc comment for the full reasoning - the original f4
// codebase shipped a wrong gradient-corners-only version of this square once and had to
// fix it, so this project keeps the two draw paths' distinction very deliberate). The hue
// strip and alpha strip *are* exact as flat vertex-gradient quads via
// AddRectGradientCorners, since both are genuine 1-D gradients.
//
// Self-contained: Open snapshots the anchor rect and window size it should lay out
// against, rather than taking them fresh on every call the way the original's free
// functions did - a live window resize while the popup happens to be open won't re-flow
// it, a minor and deliberate simplification (nothing in this fork's widget tree has a
// generic "the window resized" notification yet for a CWidget to hook into).
//
// Per-region dragging goes through CDraggable (FORK_WITH_CLASSES.md 3.2) for its
// press/track/release bookkeeping, though this widget never reads HasMoved(): a plain
// click without any subsequent move should still pick a color immediately, matching every
// real picker's UX, so the value is applied on press regardless of whether the pointer
// ever moves.

#include <cstdint>

#include "core/types.h"
#include "ui/draggable.h"
#include "ui/widget.h"

class CColorPicker : public CWidget {
  public:
	// Seeds hue/saturation/value/alpha from initial (an RGB->HSV conversion) and opens,
	// laid out relative to anchor (the swatch's own rect) within a windowW x windowH
	// window - right-aligned under the swatch, flipped above it if it wouldn't otherwise
	// fit, clamped to stay fully on screen either way.
	void Open(Color initial, Rect anchor, float windowW, float windowH);
	void Close();

	void Update(float deltaSeconds) override {}
	void Draw(CDrawList &drawList) override;

	// Starts a drag if the press landed on the SV square / hue strip / alpha strip,
	// applying the picked value immediately. Consumes (returns true for) any click
	// anywhere inside the popup, so a click that missed every control still doesn't fall
	// through to whatever's behind the popup.
	bool OnPointerDown(float x, float y) override;
	bool OnPointerMove(float x, float y) override;
	bool OnPointerUp(float x, float y) override;

	bool IsBlocking() const override
	{
		return m_bOpen;
	}
	bool IsDragging() const
	{
		return m_dragSv.IsPressed() || m_dragHue.IsPressed() || m_dragAlpha.IsPressed();
	}

	ECursorKind GetDesiredCursor() const override;

	Color GetCurrentColor() const;

  private:
	Rect m_anchor{};
	float m_flWindowW = 0.0f;
	float m_flWindowH = 0.0f;
	bool m_bOpen = false;

	float m_flHue = 0.0f;		 // 0..360
	float m_flSaturation = 0.0f; // 0..1
	float m_flValue = 0.0f;		 // 0..1
	std::uint8_t m_uAlpha = 255;

	CDraggable m_dragSv;
	CDraggable m_dragHue;
	CDraggable m_dragAlpha;
};
