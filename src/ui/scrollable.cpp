#include "ui/scrollable.h"

#include <algorithm>

#include "core/animator.h"
#include "ui/draw_list.h"

namespace {
constexpr float kScrollableEaseRate = 16.0f;
constexpr float kEdgeFadeHeight = 28.0f;

float MaxScroll(float contentHeight, float visibleHeight)
{
	return std::max(0.0f, contentHeight - visibleHeight);
}

// Where the thumb's top would sit for a given (already-clamped) scroll offset - shared
// by every function below so dragging and drawing can never drift apart.
Rect ThumbRectFor(float scrollOffset, Rect track, float contentHeight, float visibleHeight)
{
	const float maxOffset = MaxScroll(contentHeight, visibleHeight);
	const float thumbHeight = std::max(kScrollbarMinThumbHeight, track.H * (visibleHeight / contentHeight));
	const float travel = track.H - thumbHeight;
	const float t = maxOffset > 0.0f ? std::clamp(scrollOffset / maxOffset, 0.0f, 1.0f) : 0.0f;
	return Rect{track.X, track.Y + travel * t, track.W, thumbHeight};
}

// A few pixels of horizontal slop around the thumb - a generous grab target around a
// thin visual element - shared by the click hit-test and the hover-highlight check so
// the widget never visually invites a click a few pixels wider than what registers one.
Rect ThumbHitRectFor(float scrollOffset, Rect track, float contentHeight, float visibleHeight)
{
	const Rect thumb = ThumbRectFor(scrollOffset, track, contentHeight, visibleHeight);
	return Rect{thumb.X - 4.0f, thumb.Y, thumb.W + 8.0f, thumb.H};
}
} // namespace

void CScrollable::Update(float deltaSeconds)
{
	m_flScrollOffset =
		CAnimator::EaseToward(m_flScrollOffset, m_flTargetScrollOffset, kScrollableEaseRate, deltaSeconds);
}

bool CScrollable::IsVisible(float contentHeight, float visibleHeight)
{
	return contentHeight > visibleHeight + 0.5f;
}

void CScrollable::Draw(CDrawList &drawList, Rect track, float contentHeight, float visibleHeight, Color thumbColor,
					   float mouseX, float mouseY) const
{
	if (!IsVisible(contentHeight, visibleHeight)) {
		return;
	}

	const Rect thumb = ThumbRectFor(m_flScrollOffset, track, contentHeight, visibleHeight);
	const bool hovered =
		m_bDragging ||
		RectContainsPoint(ThumbHitRectFor(m_flScrollOffset, track, contentHeight, visibleHeight), mouseX, mouseY);
	const Color color = hovered ? ColorLighten(thumbColor, 40) : thumbColor;
	drawList.AddRectRoundedFilled(thumb.X, thumb.Y, thumb.W, thumb.H, CDrawList::UniformRadii(thumb.W * 0.5f), color);
}

bool CScrollable::OnPointerDown(float x, float y, Rect track, float contentHeight, float visibleHeight)
{
	if (!IsVisible(contentHeight, visibleHeight)) {
		return false;
	}

	if (!RectContainsPoint(ThumbHitRectFor(m_flTargetScrollOffset, track, contentHeight, visibleHeight), x, y)) {
		return false;
	}

	m_bDragging = true;
	m_flDragStartPointerY = y;
	m_flDragStartScrollOffset = m_flTargetScrollOffset;
	return true;
}

void CScrollable::OnPointerMove(float y, Rect track, float contentHeight, float visibleHeight)
{
	if (!m_bDragging) {
		return;
	}

	const float thumbHeight = std::max(kScrollbarMinThumbHeight, track.H * (visibleHeight / contentHeight));
	const float travel = track.H - thumbHeight;
	const float maxOffset = MaxScroll(contentHeight, visibleHeight);
	if (travel <= 0.0f || maxOffset <= 0.0f) {
		return;
	}

	const float deltaPixels = y - m_flDragStartPointerY;
	const float newOffset = m_flDragStartScrollOffset + deltaPixels * (maxOffset / travel);
	m_flTargetScrollOffset = std::clamp(newOffset, 0.0f, maxOffset);
	m_flScrollOffset = m_flTargetScrollOffset; // 1:1 tracking while dragging, same as every other drag in this project
}

void CScrollable::OnPointerUp()
{
	m_bDragging = false;
}

void CScrollable::OnScroll(float wheelDelta, float contentHeight, float visibleHeight)
{
	const float maxOffset = MaxScroll(contentHeight, visibleHeight);
	m_flTargetScrollOffset =
		std::clamp(m_flTargetScrollOffset - wheelDelta * kScrollbarWheelPixelsPerNotch, 0.0f, maxOffset);
}

void CScrollable::DrawEdgeFade(CDrawList &drawList, Rect area, float contentHeight, float visibleHeight,
							   Color edgeColor) const
{
	if (!IsVisible(contentHeight, visibleHeight)) {
		return; // nothing clipped in either direction - nothing to hint at
	}

	const float fadeHeight = std::min(kEdgeFadeHeight, area.H * 0.5f);
	const Color clear = ColorFadeAlpha(edgeColor, 0);
	const float maxOffset = MaxScroll(contentHeight, visibleHeight);

	// The row content above/below this area is clipped to `area` by the caller's own
	// separate PushClipRect - if that clip and this fade's own geometry land on slightly
	// different physical pixels (a rounding difference between the GPU scissor rect and
	// vertex positions), a sliver of unfaded content shows through right at the edge. A
	// solid strip of the same opaque edgeColor overshooting a few pixels past area's own
	// top/bottom - clipped away by the PushClipRect below in the ordinary case - closes
	// that gap regardless of which way the rounding goes, without touching the real
	// gradient's own slope (which still starts exactly on area's edge).
	constexpr float kOvershoot = 4.0f;
	drawList.PushClipRect(area);
	if (m_flScrollOffset > 0.5f) {
		drawList.AddRectFilled(area.X, area.Y - kOvershoot, area.W, kOvershoot, edgeColor);
		drawList.AddRectGradientCorners(area.X, area.Y, area.W, fadeHeight, edgeColor, edgeColor, clear, clear);
	}
	if (m_flScrollOffset < maxOffset - 0.5f) {
		drawList.AddRectGradientCorners(area.X, area.Y + area.H - fadeHeight, area.W, fadeHeight, clear, clear,
										edgeColor, edgeColor);
		drawList.AddRectFilled(area.X, area.Y + area.H, area.W, kOvershoot, edgeColor);
	}
	drawList.PopClipRect();
}
