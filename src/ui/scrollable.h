#pragma once

// CScrollable is a reusable component, not a CWidget base class - a widget HAS a
// CScrollable rather than IS one, since e.g. CCarousel only needs one in two of its three
// view modes while CAccountModal always needs exactly one for its row list (see
// FORK_WITH_CLASSES.md 3.2). This is deliberately the fork's flagship example of
// composition solving a real bug the original codebase hit: its Grid/List view modes never
// got scrollbar drag support because that wiring lived as free functions a call site had to
// remember to invoke by hand. Every consumer here goes through the same four methods below,
// so a missing call site becomes "I forgot to call m_scroll.OnPointerDown(...) in this one
// branch" instead of "I forgot the whole feature exists."
//
// Owns no opinion about *what* it's scrolling - every method takes content/visible height
// as parameters rather than storing them, the same input/output shape as f4's original
// ui::Text_Input took for its own state.

#include "core/types.h"

class CDrawList;

constexpr float kScrollbarWidth = 8.0f;
constexpr float kScrollbarMinThumbHeight = 24.0f;
constexpr float kScrollbarWheelPixelsPerNotch = 48.0f;

class CScrollable {
  public:
	// Eases m_flScrollOffset toward m_flTargetScrollOffset; call once per frame before
	// Draw.
	void Update(float deltaSeconds);

	// track is the full vertical strip the thumb can travel within (typically a thin
	// strip along the scrolling area's right edge). Draws (and, per OnPointerDown below,
	// hit-tests) nothing at all when contentHeight <= visibleHeight - a widget with
	// nothing to scroll has no scrollbar, not a disabled/greyed one; IsVisible is the
	// single source of truth for that so draw and hit-testing can never disagree.
	void Draw(CDrawList &drawList, Rect track, float contentHeight, float visibleHeight, Color thumbColor, float mouseX,
			  float mouseY) const;

	// Returns true (and starts a drag) only if the click actually landed on the thumb and
	// there's something to scroll; false otherwise, so a caller can fall through to
	// whatever's behind the track when this returns false.
	bool OnPointerDown(float x, float y, Rect track, float contentHeight, float visibleHeight);
	void OnPointerMove(float y, Rect track, float contentHeight, float visibleHeight);
	void OnPointerUp();

	// Nudges the target offset by a fixed number of pixels per wheel notch, clamped to
	// [0, max scroll] - m_flScrollOffset itself eases toward it via Update, same as a
	// drag or thumb click.
	void OnScroll(float wheelDelta, float contentHeight, float visibleHeight);

	static bool IsVisible(float contentHeight, float visibleHeight);

	// Whether a thumb drag is currently in progress - a consumer whose own release
	// handling needs to tell "this release ended a scrollbar drag" apart from "this
	// release is a plain click on whatever's behind the track" (see CCarousel::
	// OnPointerUp) checks this before calling OnPointerUp below, since OnPointerUp itself
	// just unconditionally clears the flag rather than reporting what it was.
	bool IsDragging() const
	{
		return m_bDragging;
	}

	// Fades `area`'s content into `edgeColor` near its top/bottom edges - the vertical
	// sibling of CCarousel::DrawCarouselMode's own horizontal edge fade, same reasoning:
	// content scrolled up against a clip rect just gets cut off mid-row with no visual
	// cue there's more of it, so a gradient into the area's own background color there
	// reads as an intentional fade instead of an abrupt clip. Top fades in only once
	// there's real content scrolled out of view above (not on an unscrolled list, which
	// has nothing above to hint at), bottom only while there's still more below -
	// `edgeColor` is the area's own background, alpha pre-composed by the caller same as
	// every other primitive here.
	void DrawEdgeFade(CDrawList &drawList, Rect area, float contentHeight, float visibleHeight, Color edgeColor) const;

	float m_flScrollOffset = 0.0f;		 // animated, pixels
	float m_flTargetScrollOffset = 0.0f; // where m_flScrollOffset eases toward

  private:
	bool m_bDragging = false;
	float m_flDragStartPointerY = 0.0f;
	float m_flDragStartScrollOffset = 0.0f;
};
