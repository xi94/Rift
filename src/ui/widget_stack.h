#pragma once

// CWidgetStack replaces f4's hand-cascaded z-order/input-routing logic (Main.cpp's
// compute_layer_mouse plus its long if/else-if input-dispatch chain, kept in sync by
// hand and re-edited for every new overlay - see FORK_WITH_CLASSES.md 4) with one
// generalized stack: push order defines z-order bottom-to-top, and both the mouse-gating
// cascade and input dispatch derive from that single order instead of two separately
// hand-maintained copies of it.
//
// The title bar is the one deliberate exception (FORK_WITH_CLASSES.md 5): it must always
// render on top and always receive input regardless of whatever dialog is currently
// blocking, since minimizing/closing the app shouldn't require closing popups first.
// Rather than the original's trick of physically redrawing it a second time after
// everything else, Push's alwaysTopmost flag marks a widget as implicitly above every
// normal entry for both Draw and dispatch, independent of when it was pushed.

#include <memory>
#include <vector>

#include "ui/widget.h"

class CDrawList;

class CWidgetStack {
  public:
	// Takes ownership; returns a non-owning pointer for the caller to keep around (e.g.
	// to call widget-specific methods CWidget itself doesn't expose). alwaysTopmost is
	// for exactly the title bar's case described above - leave false for every ordinary
	// widget.
	CWidget *Push(std::unique_ptr<CWidget> widget, bool alwaysTopmost = false);

	// Destroys the widget and removes it from the stack. Safe to call on a pointer this
	// stack doesn't own (no-op).
	void Remove(CWidget *pWidget);

	// Records where the real cursor is this frame. Must be called before Update so the
	// mouse-gating cascade below has something to gate.
	void SetRealMousePosition(float x, float y);

	// Top-down (visually topmost first): gates each widget's mouse position by whether
	// anything drawn above it is currently blocking (CWidget::IsBlocking), then calls
	// that widget's own Update - the general form of what the original's
	// compute_layer_mouse hand-computed for a fixed five layers. Adding a new overlay
	// here needs zero new code beyond one more Push() call somewhere.
	void Update(float deltaSeconds);

	// Bottom-up (visually topmost last, so it paints over everything below it).
	void Draw(CDrawList &drawList);

	// Top-down: the first widget that consumes the event stops it from propagating
	// further. This is the one place "who gets this input" is decided now, replacing
	// the original's long if/else-if chain in Main.cpp.
	bool DispatchPointerDown(float x, float y);
	bool DispatchPointerMove(float x, float y);
	bool DispatchPointerUp(float x, float y);
	bool DispatchRightPointerUp(float x, float y);
	bool DispatchScroll(float x, float y, float wheelDelta);
	bool DispatchKeyDown(std::uint32_t keyCode);
	bool DispatchChar(std::uint32_t character);

	// Top-down, same order and gating as Dispatch*: the first visible widget whose
	// GetDesiredCursor isn't the CURSOR_ARROW default wins: a widget below a blocking one
	// was already gated to a hidden mouse position by Update, so its own hover state (and
	// therefore its answer here) is already correctly ECursorKind::CURSOR_ARROW without this
	// needing to know about blocking itself. Called once per frame, after Update, by
	// whoever owns the actual CWindow::SetCursorKind call.
	ECursorKind GetDesiredCursor() const;

	// Public only so widget_stack.cpp's file-local IterateTopDown() helper (an ordinary
	// function, not a member - nothing outside that file needs it) can name it. Not part
	// of this class's real public contract; callers outside widget_stack.cpp have no
	// business touching it directly.
	struct Entry {
		std::unique_ptr<CWidget> m_pWidget;
		bool m_bAlwaysTopmost = false;
	};

  private:
	// Visual top-to-bottom order: always-topmost entries first (most-recently-pushed
	// topmost entry first), then normal entries in reverse push order. Every Update/
	// Draw/Dispatch* method walks this same order (or its reverse for Draw) so the two
	// concerns this class exists to unify - "what's blocking what" and "what paints over
	// what" - can never drift apart the way the original's two independent hand-written
	// copies could.

	std::vector<Entry> m_aWidgets;
	float m_flRealMouseX = -1.0f;
	float m_flRealMouseY = -1.0f;
};
