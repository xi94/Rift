#pragma once

// CHoverable is a small reusable component tracking eased hover/press amounts, the
// composition-over-inheritance answer to the original codebase's bespoke, independently-
// reimplemented hover feedback (Modal.cpp's draw_hover_lift call sites, the title bar's
// separate hover-color constants, GameSelectPopup's row hover) - see
// FORK_WITH_CLASSES.md 3.2's Hoverable/Pressable paragraph. A widget composes one of
// these per clickable region it owns (a button, a row, a toggle) rather than hand-rolling
// its own hover/press bookkeeping, so "does this thing show hover feedback" stops being a
// question you can only answer by reading that widget's own Draw method.
//
// Like CScrollable, this owns no opinion about what it's attached to - Update just takes
// the raw hovered/pressed booleans the owning widget already knows (from its own
// m_bIsHovered / a click-in-progress flag) and eases two 0..1 amounts a widget's Draw can
// read however it likes (a brighten, a lift, a press-scale). DrawLift is the one shared
// *visual* this component also owns outright, since it was independently duplicated at
// nearly a dozen call sites in the original (every footer/header button in its account
// modal) with identical math each time - exactly the kind of drift a shared component
// should make impossible.

#include <cstdint>

#include "core/types.h"

class CDrawList;

class CHoverable {
  public:
	void Update(bool hovered, bool pressed, float deltaSeconds);

	// A soft accent-tinted outer glow behind rect (five layered rounded rects expanding
	// outward with a quadratic falloff) - the shared "raised on hover" cue every footer/
	// header button in this project's account modal and settings panel uses. alpha scales
	// the whole effect (0 = invisible), for widgets that are themselves fading in/out.
	static void DrawLift(CDrawList &drawList, Rect rect, float radius, Color accent, std::uint8_t alpha);

	float m_flHoverAmount = 0.0f; // eased 0..1
	float m_flPressAmount = 0.0f; // eased 0..1
};
