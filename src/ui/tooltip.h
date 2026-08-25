#pragma once

// A reusable hover tooltip - a small rounded bubble of text pointing at whatever the
// cursor is currently resting on. A component, not a CWidget (see FORK_WITH_CLASSES.md
// 3.3's rule of thumb, the same reasoning CScrollable/CTextInput follow): a tooltip never
// takes input, never blocks, and never wants its own place in CWidgetStack's z-order - it
// just draws on top of whatever its owner already drew. A widget HAS a CTooltip.
//
// The contract is one call per frame from whichever hover branch already knows what's
// under the cursor:
//
//     if (RectContainsPoint(button, mouseX, mouseY)) {
//         m_tooltip.Request(StringViewFromCString("Restore default setting."), button);
//     }
//     m_tooltip.Update(deltaSeconds);   // no Request this frame => fades back out
//     ...
//     m_tooltip.Draw(drawList, fonts, bounds, alpha);   // last, so it layers over everything
//
// Request only states what *would* be shown; the delay before a bubble actually appears
// (kTooltipDelaySeconds) and the fade in/out are this class's own business, so no caller
// has to reimplement "don't flash a tooltip the instant the cursor crosses a button." A
// frame with no Request is what starts the fade back out, which means a caller can never
// leave a stale bubble on screen by forgetting to call some Hide() on every exit path.
//
// Request is meant to be called from Update (where m_flMouseX/m_flMouseY are already
// current), not Draw - Draw stays a pure paint of whatever state Update settled on, and
// the fade advances the same frame the hover starts rather than one behind it.

#include <cstdint>

#include "core/string_view.h"
#include "core/types.h"

class CDrawList;
class CFontManager;

// How long the cursor has to rest on the target before the bubble starts fading in - long
// enough that sweeping the cursor across a row of buttons doesn't strobe a tooltip per
// button, short enough that deliberately pausing on one feels answered rather than slow.
constexpr float kTooltipDelaySeconds = 0.35f;

class CTooltip {
  public:
	// Call every frame the cursor is over the thing being described. anchor is that
	// thing's own rect - the bubble centers itself horizontally on it and sits just above
	// it (flipping below only when there's no room, see Draw). Retargeting to a different
	// anchor while a bubble is already visible moves it immediately without re-waiting the
	// delay, so sliding along a row of reset buttons reads as one tooltip following the
	// cursor rather than a fresh fade per button.
	void Request(CStringView text, Rect anchor);

	// Advances the delay timer and the fade. Call once per frame, after every Request
	// branch has had its chance to fire - a frame with no Request fades the bubble out.
	void Update(float deltaSeconds);

	// bounds is the region the bubble must stay inside (the owning panel/window rect) - it
	// gets clamped horizontally and flipped to below the anchor rather than being allowed
	// to paint off the edge. alpha scales the whole bubble on top of its own fade, so a
	// tooltip inside a panel that's itself still fading in/out doesn't pop in at full
	// opacity over a half-transparent panel. Draw last: this deliberately does no clipping
	// of its own and is meant to layer over everything its owner already painted.
	void Draw(CDrawList &drawList, const CFontManager &fonts, Rect bounds, std::uint8_t alpha = 255) const;

	// Kills any visible bubble immediately, skipping the fade - for a caller whose whole
	// surface just went away (a panel closing) and would otherwise leave a bubble hanging
	// over whatever is underneath for a few frames.
	void Reset();

  private:
	// Owns its own copy of the text rather than borrowing the caller's pointer: a caller
	// naturally passes a string literal today, but a stack buffer (a formatted value) would
	// dangle by the time Draw runs, and that's exactly the kind of trap this component
	// exists to not have.
	static constexpr std::uint64_t kMaxTextLength = 127;
	char m_szText[kMaxTextLength + 1]{};
	std::uint64_t m_nTextLength = 0;

	Rect m_anchor{};
	bool m_bRequestedThisFrame = false;
	float m_flHoverSeconds = 0.0f; // resets whenever a frame passes with no Request
	float m_flVisibleAmount = 0.0f;
};
