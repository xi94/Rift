#include "ui/color_picker.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "ui/draw_list.h"

namespace {
constexpr float kPopupPadding = 12.0f;
constexpr float kSvSize = 180.0f;
constexpr float kStripGap = 10.0f;
constexpr float kAlphaStripWidth = 20.0f;
constexpr float kHueStripHeight = 16.0f;
constexpr float kPopupRadius = 12.0f;
constexpr float kHandleRadius = 6.0f;

// The hue strip is 6 exact gradient segments between these stops (red -> yellow ->
// green -> cyan -> blue -> magenta -> red), not a per-pixel shader - the hue circle in
// RGB space *is* piecewise-linear through exactly these 6 points, so a linear-
// interpolated gradient between them is mathematically exact, not an approximation.
constexpr Color kHueStops[7]{
	{255, 0, 0, 255}, {255, 255, 0, 255}, {0, 255, 0, 255}, {0, 255, 255, 255},
	{0, 0, 255, 255}, {255, 0, 255, 255}, {255, 0, 0, 255},
};
constexpr std::uint32_t kHueStopCount = 7;

constexpr Color kColorBg{30, 30, 34, 255};
constexpr Color kColorBorder{70, 70, 76, 255};

Color HsvToRgb(float hue, float saturation, float value)
{
	const float c = value * saturation;
	const float hPrime = std::fmod(hue, 360.0f) / 60.0f;
	const float x = c * (1.0f - std::fabs(std::fmod(hPrime, 2.0f) - 1.0f));
	float r = 0.0f, g = 0.0f, b = 0.0f;
	if (hPrime < 1.0f) {
		r = c;
		g = x;
	} else if (hPrime < 2.0f) {
		r = x;
		g = c;
	} else if (hPrime < 3.0f) {
		g = c;
		b = x;
	} else if (hPrime < 4.0f) {
		g = x;
		b = c;
	} else if (hPrime < 5.0f) {
		r = x;
		b = c;
	} else {
		r = c;
		b = x;
	}
	const float m = value - c;
	return Color{
		static_cast<std::uint8_t>(std::clamp((r + m) * 255.0f, 0.0f, 255.0f)),
		static_cast<std::uint8_t>(std::clamp((g + m) * 255.0f, 0.0f, 255.0f)),
		static_cast<std::uint8_t>(std::clamp((b + m) * 255.0f, 0.0f, 255.0f)),
		255,
	};
}

void RgbToHsv(Color color, float &outHue, float &outSaturation, float &outValue)
{
	const float r = static_cast<float>(color.R) / 255.0f;
	const float g = static_cast<float>(color.G) / 255.0f;
	const float b = static_cast<float>(color.B) / 255.0f;
	const float maxC = std::max({r, g, b});
	const float minC = std::min({r, g, b});
	const float delta = maxC - minC;

	float hue = 0.0f;
	if (delta > 0.0001f) {
		if (maxC == r) {
			hue = 60.0f * std::fmod((g - b) / delta, 6.0f);
		} else if (maxC == g) {
			hue = 60.0f * ((b - r) / delta + 2.0f);
		} else {
			hue = 60.0f * ((r - g) / delta + 4.0f);
		}
	}
	if (hue < 0.0f) {
		hue += 360.0f;
	}

	outHue = hue;
	outSaturation = maxC > 0.0001f ? delta / maxC : 0.0f;
	outValue = maxC;
}

Rect SvRect(Rect popup)
{
	return Rect{popup.X + kPopupPadding, popup.Y + kPopupPadding, kSvSize, kSvSize};
}

Rect HueRect(Rect popup)
{
	const Rect sv = SvRect(popup);
	return Rect{sv.X, sv.Y + sv.H + kStripGap, kSvSize, kHueStripHeight};
}

Rect AlphaRect(Rect popup)
{
	const Rect sv = SvRect(popup);
	return Rect{sv.X + sv.W + kStripGap, sv.Y, kAlphaStripWidth, kSvSize};
}

std::pair<float, float> PopupSize()
{
	return {kPopupPadding * 2.0f + kSvSize + kStripGap + kAlphaStripWidth,
			kPopupPadding * 2.0f + kSvSize + kStripGap + kHueStripHeight};
}

// A white outer ring around the picked color itself, not a plain filled dot - a
// fill-only handle would disappear against a same-colored background (e.g. the SV
// square's own white corner at low saturation), so the marker needs a fixed-contrast
// border regardless of what color it's currently pointing at.
void DrawHandleRing(CDrawList &drawList, float cx, float cy, Color fill)
{
	drawList.AddRectRoundedFilled(cx - kHandleRadius, cy - kHandleRadius, kHandleRadius * 2.0f, kHandleRadius * 2.0f,
								  CDrawList::UniformRadii(kHandleRadius), Color{255, 255, 255, 255});
	const float inner = kHandleRadius - 2.5f;
	drawList.AddRectRoundedFilled(cx - inner, cy - inner, inner * 2.0f, inner * 2.0f, CDrawList::UniformRadii(inner),
								  fill);
}

void ApplySv(float &saturation, float &value, float x, float y, Rect sv)
{
	saturation = std::clamp((x - sv.X) / sv.W, 0.0f, 1.0f);
	value = std::clamp(1.0f - (y - sv.Y) / sv.H, 0.0f, 1.0f);
}

void ApplyHue(float &hue, float x, Rect hueRect)
{
	hue = std::clamp((x - hueRect.X) / hueRect.W, 0.0f, 1.0f) * 360.0f;
}

void ApplyAlpha(std::uint8_t &alpha, float y, Rect alphaRect)
{
	alpha = static_cast<std::uint8_t>(std::clamp(1.0f - (y - alphaRect.Y) / alphaRect.H, 0.0f, 1.0f) * 255.0f);
}

// The single geometry function every hit-test/draw call below shares, so the popup's
// on-screen position and its clickable area can never drift apart.
Rect PopupRect(Rect anchor, float windowW, float windowH)
{
	const auto [w, h] = PopupSize();
	constexpr float kWindowMargin = 8.0f;
	constexpr float kAnchorGap = 8.0f;

	float x = anchor.X + anchor.W - w; // right-aligned under the swatch
	x = std::clamp(x, kWindowMargin, std::max(kWindowMargin, windowW - kWindowMargin - w));

	float y = anchor.Y + anchor.H + kAnchorGap;
	if (y + h > windowH - kWindowMargin) {
		y = anchor.Y - kAnchorGap - h; // flip above if it wouldn't fit below
	}
	y = std::clamp(y, kWindowMargin, std::max(kWindowMargin, windowH - kWindowMargin - h));

	return Rect{x, y, w, h};
}
} // namespace

void CColorPicker::Open(Color initial, Rect anchor, float windowW, float windowH)
{
	RgbToHsv(initial, m_flHue, m_flSaturation, m_flValue);
	m_uAlpha = initial.A;
	m_anchor = anchor;
	m_flWindowW = windowW;
	m_flWindowH = windowH;
	m_bOpen = true;
	m_dragSv.End();
	m_dragHue.End();
	m_dragAlpha.End();
}

void CColorPicker::Close()
{
	m_bOpen = false;
	m_dragSv.End();
	m_dragHue.End();
	m_dragAlpha.End();
}

Color CColorPicker::GetCurrentColor() const
{
	Color color = HsvToRgb(m_flHue, m_flSaturation, m_flValue);
	color.A = m_uAlpha;
	return color;
}

bool CColorPicker::OnPointerDown(float x, float y)
{
	if (!m_bOpen) {
		return false;
	}
	const Rect popup = PopupRect(m_anchor, m_flWindowW, m_flWindowH);
	if (!RectContainsPoint(popup, x, y)) {
		return false;
	}

	const Rect sv = SvRect(popup);
	const Rect hue = HueRect(popup);
	const Rect alpha = AlphaRect(popup);

	if (RectContainsPoint(sv, x, y)) {
		m_dragSv.Begin(x, y);
		ApplySv(m_flSaturation, m_flValue, x, y, sv);
	} else if (RectContainsPoint(hue, x, y)) {
		m_dragHue.Begin(x, y);
		ApplyHue(m_flHue, x, hue);
	} else if (RectContainsPoint(alpha, x, y)) {
		m_dragAlpha.Begin(x, y);
		ApplyAlpha(m_uAlpha, y, alpha);
	}
	return true;
}

bool CColorPicker::OnPointerMove(float x, float y)
{
	if (!IsDragging()) {
		return false;
	}
	const Rect popup = PopupRect(m_anchor, m_flWindowW, m_flWindowH);

	if (m_dragSv.IsPressed()) {
		m_dragSv.Update(x, y);
		ApplySv(m_flSaturation, m_flValue, x, y, SvRect(popup));
	}
	if (m_dragHue.IsPressed()) {
		m_dragHue.Update(x, y);
		ApplyHue(m_flHue, x, HueRect(popup));
	}
	if (m_dragAlpha.IsPressed()) {
		m_dragAlpha.Update(x, y);
		ApplyAlpha(m_uAlpha, y, AlphaRect(popup));
	}
	return true;
}

bool CColorPicker::OnPointerUp(float x, float y)
{
	(void)x;
	(void)y;
	const bool wasDragging = IsDragging();
	m_dragSv.End();
	m_dragHue.End();
	m_dragAlpha.End();
	return wasDragging;
}

ECursorKind CColorPicker::GetDesiredCursor() const
{
	if (!m_bOpen) {
		return ECursorKind::CURSOR_ARROW;
	}
	if (IsDragging()) {
		return ECursorKind::CURSOR_DRAG;
	}

	const Rect popup = PopupRect(m_anchor, m_flWindowW, m_flWindowH);
	if (RectContainsPoint(SvRect(popup), m_flMouseX, m_flMouseY) ||
		RectContainsPoint(HueRect(popup), m_flMouseX, m_flMouseY) ||
		RectContainsPoint(AlphaRect(popup), m_flMouseX, m_flMouseY)) {
		return ECursorKind::CURSOR_HAND;
	}
	return ECursorKind::CURSOR_ARROW;
}

void CColorPicker::Draw(CDrawList &drawList)
{
	if (!m_bOpen) {
		return;
	}

	const Rect popup = PopupRect(m_anchor, m_flWindowW, m_flWindowH);
	drawList.AddRectRoundedFilled(popup.X - 1.0f, popup.Y - 1.0f, popup.W + 2.0f, popup.H + 2.0f,
								  CDrawList::UniformRadii(kPopupRadius), kColorBorder);
	drawList.AddRectRoundedFilled(popup.X, popup.Y, popup.W, popup.H, CDrawList::UniformRadii(kPopupRadius - 1.0f),
								  kColorBg);

	// A real per-pixel shader, not vertex-color interpolation - see this file's own
	// header comment for why.
	const Rect sv = SvRect(popup);
	drawList.AddRectColorPickerSv(sv.X, sv.Y, sv.W, sv.H, m_flHue);
	DrawHandleRing(drawList, sv.X + m_flSaturation * sv.W, sv.Y + (1.0f - m_flValue) * sv.H,
				   HsvToRgb(m_flHue, m_flSaturation, m_flValue));

	// 6 exact gradient segments between kHueStops instead of a per-pixel shader.
	const Rect hue = HueRect(popup);
	const float hueSegW = hue.W / static_cast<float>(kHueStopCount - 1);
	for (std::uint32_t i = 0; i + 1 < kHueStopCount; i += 1) {
		drawList.AddRectGradientCorners(hue.X + static_cast<float>(i) * hueSegW, hue.Y, hueSegW, hue.H, kHueStops[i],
										kHueStops[i + 1], kHueStops[i], kHueStops[i + 1]);
	}
	const float hueMarkerX = hue.X + (m_flHue / 360.0f) * hue.W;
	drawList.AddRectFilled(hueMarkerX - 1.5f, hue.Y - 2.0f, 3.0f, hue.H + 4.0f, Color{255, 255, 255, 255});

	// One exact vertical gradient (opaque at top to transparent at bottom) instead of a
	// per-pixel shader - real per-pixel alpha, blended against kColorBg beneath by the
	// renderer's existing SrcAlpha/InvSrcAlpha blend state.
	const Rect alpha = AlphaRect(popup);
	const Color base = HsvToRgb(m_flHue, m_flSaturation, m_flValue);
	Color baseOpaque = base;
	baseOpaque.A = 255;
	Color baseTransparent = base;
	baseTransparent.A = 0;
	drawList.AddRectGradientCorners(alpha.X, alpha.Y, alpha.W, alpha.H, baseOpaque, baseOpaque, baseTransparent,
									baseTransparent);
	const float alphaMarkerY = alpha.Y + (1.0f - static_cast<float>(m_uAlpha) / 255.0f) * alpha.H;
	drawList.AddRectFilled(alpha.X - 2.0f, alphaMarkerY - 1.5f, alpha.W + 4.0f, 3.0f, Color{255, 255, 255, 255});
}
