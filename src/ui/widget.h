#pragma once

// CWidget is the base class every UI element in this rewrite derives from - see the
// original f4 repo's FORK_WITH_CLASSES.md 3.1 for the full rationale (this is the class
// that replaces f4's free-function-per-widget architecture). A widget owns its own
// state, advances its own animations in Update, draws itself in Draw, and answers input
// through the On* virtuals rather than every call site in a frame loop having to know
// which widget wants which event.
//
// Not everything needs to be a CWidget - see FORK_WITH_CLASSES.md 3.3's rule of thumb:
// something is a CWidget only if it can independently be "the foreground/blocking thing"
// or needs its own place in the CWidgetStack z-order. A text field, a glyph icon, or a
// color swatch is a plain value member a CWidget owns and forwards calls to directly
// (CScrollable below is the template for that relationship).

#include <cstdint>

#include "core/types.h"

class CDrawList;

class CWidget {
  public:
	virtual ~CWidget() = default;

	// Called once per frame, top of the stack down (see CWidgetStack::Update), before
	// Draw. Advances this widget's own animations (via EaseToward, same primitive as
	// f4's original ease_toward) using the already mouse-gated state SetMouseGated just
	// set.
	virtual void Update(float deltaSeconds) = 0;

	virtual void Draw(CDrawList &drawList) = 0;

	// Each returns true if this widget consumed the event, which stops CWidgetStack from
	// offering it to anything further down. The base defaults do nothing and return
	// false, so a leaf widget that doesn't care about a given input kind (a pure
	// decoration, a widget with no keyboard focus) needs zero boilerplate to ignore it.
	virtual bool OnPointerDown(float x, float y)
	{
		return false;
	}
	virtual bool OnPointerMove(float x, float y)
	{
		return false;
	}
	virtual bool OnPointerUp(float x, float y)
	{
		return false;
	}

	// WM_RBUTTONUP (window.h's INPUT_EVENT_RIGHT_MOUSE_UP) - a right-click-down isn't
	// tracked separately (nothing needs a right-drag), so this is the only right-click
	// entry point. A widget that wants to open a context menu on right-click typically
	// can't do the whole job itself (it doesn't own CContextMenu) - the usual shape is to
	// hit-test here, latch the result as one-shot consumable state (the same
	// ConsumeAction/ConsumeSelection pattern CSettingsMenu/CContextMenu already use), and
	// return true so a coordinating owner polls it next and decides what to open.
	virtual bool OnRightPointerUp(float x, float y)
	{
		return false;
	}

	virtual bool OnScroll(float x, float y, float wheelDelta)
	{
		return false;
	}
	virtual bool OnKeyDown(std::uint32_t keyCode)
	{
		return false;
	}
	virtual bool OnChar(std::uint32_t character)
	{
		return false;
	}

	// Does this widget currently want exclusive input (an open modal, an open popup)?
	// CWidgetStack uses this to decide whether widgets further down the stack see the
	// real mouse position or a gated-away one - the single generalized replacement for
	// the original codebase's hand-written compute_layer_mouse cascade. A plain
	// non-modal widget's default is false.
	virtual bool IsBlocking() const
	{
		return false;
	}

	// What cursor should be showing right now, given this widget's own current hover/drag
	// state (m_bIsHovered, a CDraggable's IsPressed, whatever else it already tracks for
	// drawing) - CWidgetStack::GetDesiredCursor asks every widget this in the same top-down,
	// gated order Dispatch* uses, taking the first answer that isn't the CURSOR_ARROW
	// default. The base default is correct for anything with no click/drag affordance of its
	// own (a plain label, a decoration) without any override needed.
	virtual ECursorKind GetDesiredCursor() const
	{
		return ECursorKind::CURSOR_ARROW;
	}

	// Called once per frame by CWidgetStack, before Update, with either the real cursor
	// position or a gated-away one (see IsBlocking's own comment for why). Updates
	// m_bIsHovered/m_flMouseX/m_flMouseY so a subclass never has to recompute
	// point-in-rect against its own bounds by hand just to know whether it's hovered.
	void SetMouseGated(bool gated, float realX, float realY);

	Rect m_vecBounds{};
	bool m_bVisible = true;

  protected:
	bool m_bIsHovered = false;
	float m_flMouseX = -1.0f;
	float m_flMouseY = -1.0f;
};
