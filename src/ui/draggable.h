#pragma once

// CDraggable is the shared component for "press somewhere, track the pointer while held,
// tell a click apart from a real drag" - a pattern the original codebase reimplemented
// independently at every drag site (CCarousel's own card-drag-to-scroll, the mode-
// switcher track thumb, CColorPicker's saturation/value square, CSettingsPanel's
// Animation Speed slider) - see FORK_WITH_CLASSES.md 3.2's Draggable paragraph. Like
// CScrollable, it owns no opinion about what the drag actually controls: a caller starts
// tracking on its own OnPointerDown, feeds every OnPointerMove through Update, and reads
// DeltaX/DeltaY back to apply whatever conversion factor (pixels-to-scroll-offset,
// pixels-to-saturation, ...) is specific to that widget.
//
// HasMoved is the click-vs-drag disambiguation: it only flips true once the pointer has
// travelled kDragThresholdPixels from the press position, so a plain click (press,
// release, no real movement) doesn't get misread as a zero-length drag - the same
// distinction the original's own drag_moved flags existed for.

class CDraggable {
  public:
	void Begin(float x, float y);

	// Call on every pointer-move while IsPressed() is true. Updates the tracked current
	// position and, once the total distance from the start position crosses
	// kDragThresholdPixels, latches HasMoved() true for the remainder of this press.
	void Update(float x, float y);

	void End();

	bool IsPressed() const
	{
		return m_bPressed;
	}
	bool HasMoved() const
	{
		return m_bHasMoved;
	}

	float StartX() const
	{
		return m_flStartX;
	}
	float StartY() const
	{
		return m_flStartY;
	}
	float DeltaX() const
	{
		return m_flCurrentX - m_flStartX;
	}
	float DeltaY() const
	{
		return m_flCurrentY - m_flStartY;
	}

  private:
	bool m_bPressed = false;
	bool m_bHasMoved = false;
	float m_flStartX = 0.0f;
	float m_flStartY = 0.0f;
	float m_flCurrentX = 0.0f;
	float m_flCurrentY = 0.0f;
};
