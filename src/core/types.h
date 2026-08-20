#pragma once

// Plain data structs shared across the whole project. No C-prefix on any of these (see
// STYLE.md's naming rules) - they carry no behavior beyond trivial constructors, the
// same "bare struct" treatment Source's own Vector/QAngle/color32 get.

#include <algorithm>
#include <cstdint>

// A screen-space rectangle in logical (DPI-independent) pixels, origin top-left.
struct Rect {
	float X;
	float Y;
	float W;
	float H;
};

inline bool RectContainsPoint(const Rect &rect, float x, float y)
{
	return x >= rect.X && x < rect.X + rect.W && y >= rect.Y && y < rect.Y + rect.H;
}

// The overlapping region of two rects - empty (W/H clamped to 0, never negative) if they
// don't overlap at all. CDrawList::PushClipRect only holds one active clip at a time (see
// its own header comment - it's a single slot, not a real stack), so nesting a second,
// smaller clip inside an already-active one (a scroll region clipping a widget that
// itself wants to clip part of its own content) has to go through this: intersect first,
// push the combined result, then re-push the outer rect afterward instead of calling
// PopClipRect - a bare Pop just disables clipping entirely, which would leave everything
// drawn afterward completely unclipped rather than restoring the outer rect.
inline Rect RectIntersect(const Rect &a, const Rect &b)
{
	const float x0 = std::max(a.X, b.X);
	const float y0 = std::max(a.Y, b.Y);
	const float x1 = std::min(a.X + a.W, b.X + b.W);
	const float y1 = std::min(a.Y + a.H, b.Y + b.H);
	return Rect{x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
}

// A 2D point/offset - shares a type with nothing else in this project (no 3D math is
// ever needed), so it stays this small rather than inheriting a general-purpose Vector
// class the way a real game engine's math library would.
struct Vec2 {
	float X;
	float Y;
};

// RGBA8, one byte per channel - packed into Vertex2D::Color (gfx/renderer.h) exactly as
// stored here, so no conversion happens between "the color a widget picked" and "the
// color the GPU rasterizes."
struct Color {
	std::uint8_t R;
	std::uint8_t G;
	std::uint8_t B;
	std::uint8_t A;
};

inline Color ColorWithAlpha(Color color, std::uint8_t alpha)
{
	return Color{color.R, color.G, color.B, alpha};
}

inline Color ColorLighten(Color color, std::uint8_t amount)
{
	auto Clamp255 = [](int value) { return static_cast<std::uint8_t>(value > 255 ? 255 : value); };
	return Color{Clamp255(color.R + amount), Clamp255(color.G + amount), Clamp255(color.B + amount), color.A};
}

inline Color ColorLerp(Color a, Color b, float t)
{
	auto Lerp8 = [](std::uint8_t from, std::uint8_t to, float t) {
		return static_cast<std::uint8_t>(static_cast<float>(from) +
										 (static_cast<float>(to) - static_cast<float>(from)) * t);
	};
	return Color{Lerp8(a.R, b.R, t), Lerp8(a.G, b.G, t), Lerp8(a.B, b.B, t), 255};
}

// Per-corner rounding radii for a rounded rectangle - zero on any corner squares it off.
// Named the same way f4's own Corner_Radii is (just restyled) since there's no clearer
// name for "one radius per corner" to invent.
struct CornerRadii {
	float TopLeft;
	float TopRight;
	float BottomLeft;
	float BottomRight;
};

inline CornerRadii CornerRadiiUniform(float radius)
{
	return CornerRadii{radius, radius, radius, radius};
}

constexpr CornerRadii kCornerRadiiNone = CornerRadii{0.0f, 0.0f, 0.0f, 0.0f};

// A one-shot "did a click land on something" result: a widget latches this on
// OnPointerUp/OnRightPointerUp, a coordinating owner polls it once via a Consume* method
// (CCarousel::ConsumePendingClick/ConsumePendingRightClick, CAccountModal::
// ConsumePendingRightClickRow), and the widget resets it to PENDING_HIT_KIND_NONE on
// read. Exists specifically to replace packing three outcomes - nothing pending, a hit
// inside the widget but not on any specific item, a hit on a specific item - into a
// single std::int32_t's sentinel range (-2/-1/>=0): that encoding once caused a real bug
// (CCarousel::ConsumePendingRightClick's own history) when an edit accidentally
// collapsed the "no specific item" case into "nothing pending," and every caller
// checking "!= <the nothing-pending sentinel>" started firing on every single frame.
// Naming the three outcomes makes that class of mistake a compile error instead.
enum class EPendingHitKind : std::uint8_t {
	PENDING_HIT_KIND_NONE,	// nothing pending - no click has landed since the last Consume*
	PENDING_HIT_KIND_MISS,	// a click landed inside the widget, but not on any specific item
	PENDING_HIT_KIND_INDEX, // a click landed on Index
};

struct PendingHit {
	EPendingHitKind Kind = EPendingHitKind::PENDING_HIT_KIND_NONE;
	std::int32_t Index = -1; // meaningful only when Kind == PENDING_HIT_KIND_INDEX
};

inline PendingHit PendingHitMiss()
{
	return PendingHit{EPendingHitKind::PENDING_HIT_KIND_MISS, -1};
}

inline PendingHit PendingHitIndex(std::int32_t index)
{
	return PendingHit{EPendingHitKind::PENDING_HIT_KIND_INDEX, index};
}

// A hit-test result (>= 0 = that item's index, negative = missed everything) collapsed
// into a PendingHit - the common way OnPointerUp/OnRightPointerUp handlers built one of
// these before this type existed.
inline PendingHit PendingHitFromHitTest(std::int32_t hitTestResult)
{
	return hitTestResult >= 0 ? PendingHitIndex(hitTestResult) : PendingHitMiss();
}

// What OS cursor glyph should be showing right now - see CWidget::GetDesiredCursor and
// CWindow::SetCursorKind. CURSOR_ARROW is both the default and the "no opinion" answer, so
// CWidgetStack::GetDesiredCursor can treat it as "keep looking further down the stack"
// without a separate has-an-opinion bit.
enum class ECursorKind : std::uint8_t {
	CURSOR_ARROW,   // the plain default - decorations, plain text, anywhere with no affordance
	CURSOR_HAND,	// hovering something clickable - a button, a toggle, a row, a link
	CURSOR_IBEAM,   // hovering a text field
	CURSOR_DRAG, // a drag is actively in progress (a slider thumb, a scrollbar thumb, ...)
};
