#include "ui/draw_list.h"

#include <algorithm>
#include <cassert>
#include <cmath>

#include "core/memory_arena.h"
#include "gfx/texture.h"

namespace {
constexpr float kPi = 3.14159265358979323846f;

bool g_bRoundedCornersEnabled = true;
float g_flCornerRoundnessScale = 1.0f;

// Plain structural equality - Rect has no operator== of its own, and this is only
// ever used to answer "did the active clip change since the last draw call" below.
bool ClipRectsEqual(Rect a, Rect b)
{
	return a.X == b.X && a.Y == b.Y && a.W == b.W && a.H == b.H;
}

// Same role as ClipRectsEqual, for BannerGlowParams - see SetTarget's glowChanged
// check for why this specific struct needs one.
bool GlowParamsEqual(BannerGlowParams a, BannerGlowParams b)
{
	return a.QuadWidth == b.QuadWidth && a.QuadHeight == b.QuadHeight && a.CornerRadius == b.CornerRadius &&
		   a.RingWidth == b.RingWidth;
}

// Same role as GlowParamsEqual, for CircularProgressParams.
bool ProgressParamsEqual(CircularProgressParams a, CircularProgressParams b)
{
	return a.QuadWidth == b.QuadWidth && a.QuadHeight == b.QuadHeight && a.OuterRadius == b.OuterRadius &&
		   a.InnerRadius == b.InnerRadius && a.StartAngle == b.StartAngle && a.SweepAngle == b.SweepAngle &&
		   a.GlowStrength == b.GlowStrength;
}

constexpr std::uint32_t kRoundedRectPointsPerCorner = CDrawList::kCornerSegments + 1;
constexpr std::uint32_t kRoundedRectPointCount = kRoundedRectPointsPerCorner * 4;
} // namespace

std::uint32_t ColorPack(Color color)
{
	return static_cast<std::uint32_t>(color.R) | (static_cast<std::uint32_t>(color.G) << 8) |
		   (static_cast<std::uint32_t>(color.B) << 16) | (static_cast<std::uint32_t>(color.A) << 24);
}

Color ColorFadeAlpha(Color color, std::uint8_t alpha)
{
	return Color{color.R, color.G, color.B, static_cast<std::uint8_t>((color.A * alpha) / 255)};
}

float CDrawList::ScaledRadius(float radius)
{
	return g_bRoundedCornersEnabled ? radius * g_flCornerRoundnessScale : 0.0f;
}

void CDrawList::SetCornerRoundnessScale(float scale)
{
	// Negative would flip corners inside out; the rounded-rect builder clamps the upper end
	// against the shape's own size anyway (see kCornerSegments' comment), so a large scale
	// just turns a small control into a pill rather than self-intersecting.
	g_flCornerRoundnessScale = std::max(0.0f, scale);
}

CornerRadii CDrawList::UniformRadii(float radius)
{
	const float r = ScaledRadius(radius);
	return CornerRadii{r, r, r, r};
}

void CDrawList::SetRoundedCornersEnabled(bool enabled)
{
	g_bRoundedCornersEnabled = enabled;
}

bool CDrawList::RoundedCornersEnabled()
{
	return g_bRoundedCornersEnabled;
}

void CDrawList::Init(CMemoryArena &arena, std::uint32_t vertexCapacity, std::uint32_t indexCapacity)
{
	m_pVertices = arena.AllocArray<Vertex2D>(vertexCapacity);
	m_pIndices = arena.AllocArray<std::uint32_t>(indexCapacity);
	m_nVertexCapacity = vertexCapacity;
	m_nIndexCapacity = indexCapacity;
	Clear();
}

void CDrawList::Clear()
{
	m_nVertexCount = 0;
	m_nIndexCount = 0;
	m_nCommandCount = 0;
	m_currentKind = EDrawCommandKind::DRAW_COMMAND_KIND_SOLID;
	m_pCurrentTexture = nullptr;
	m_bCurrentHasClip = false;
	m_nCommandStartIndex = 0;
	m_bHasOpenCommand = false;
	m_bActiveClipEnabled = false; // a stray unmatched push in a previous frame must never leak into the next
	m_currentGlow = BannerGlowParams{};
	m_currentProgress = CircularProgressParams{};
}

void CDrawList::PushClipRect(Rect rect)
{
	m_bActiveClipEnabled = true;
	m_activeClipRect = rect;
}

void CDrawList::PopClipRect()
{
	m_bActiveClipEnabled = false;
}

void CDrawList::SetTarget(const CTexture *pTexture, EDrawCommandKind kind, BannerGlowParams glow,
						  CircularProgressParams progress)
{
	const bool clipChanged = m_bCurrentHasClip != m_bActiveClipEnabled ||
							 (m_bActiveClipEnabled && !ClipRectsEqual(m_currentClipRect, m_activeClipRect));
	// Only relevant for BannerGlow/CircularProgress - two quads of either with different
	// geometry can't share a draw call (one constant buffer per draw), so a change forces a
	// new command the same way a texture switch does.
	const bool glowChanged =
		kind == EDrawCommandKind::DRAW_COMMAND_KIND_BANNER_GLOW && !GlowParamsEqual(m_currentGlow, glow);
	const bool progressChanged = kind == EDrawCommandKind::DRAW_COMMAND_KIND_CIRCULAR_PROGRESS &&
								 !ProgressParamsEqual(m_currentProgress, progress);
	if (m_bHasOpenCommand && m_pCurrentTexture == pTexture && m_currentKind == kind && !clipChanged && !glowChanged &&
		!progressChanged) {
		return;
	}

	if (m_bHasOpenCommand && m_nIndexCount > m_nCommandStartIndex) {
		assert(m_nCommandCount < kMaxCommands);
		m_aCommands[m_nCommandCount] = DrawCommand{
			.Kind = m_currentKind,
			.pTexture = m_pCurrentTexture,
			.HasClip = m_bCurrentHasClip,
			.ClipRect = m_currentClipRect,
			.IndexOffset = m_nCommandStartIndex,
			.IndexCount = m_nIndexCount - m_nCommandStartIndex,
			.Glow = m_currentGlow,
			.Progress = m_currentProgress,
		};
		m_nCommandCount += 1;
	}

	m_pCurrentTexture = pTexture;
	m_currentKind = kind;
	m_bCurrentHasClip = m_bActiveClipEnabled;
	m_currentClipRect = m_activeClipRect;
	m_currentGlow = glow;
	m_currentProgress = progress;
	m_nCommandStartIndex = m_nIndexCount;
	m_bHasOpenCommand = true;
}

void CDrawList::SetTargetTexture(const CTexture *pTexture)
{
	SetTarget(pTexture, pTexture == nullptr ? EDrawCommandKind::DRAW_COMMAND_KIND_SOLID
											: EDrawCommandKind::DRAW_COMMAND_KIND_TEXTURED);
}

// The two-triangle index pattern every quad primitive (filled rect, line, textured rect)
// shares. Assumes the 4 vertices were just appended in top-left/top-right/bottom-right/
// bottom-left order starting at `base`.
void CDrawList::EmitQuadIndices(std::uint32_t base)
{
	m_pIndices[m_nIndexCount + 0] = base + 0;
	m_pIndices[m_nIndexCount + 1] = base + 1;
	m_pIndices[m_nIndexCount + 2] = base + 2;
	m_pIndices[m_nIndexCount + 3] = base + 0;
	m_pIndices[m_nIndexCount + 4] = base + 2;
	m_pIndices[m_nIndexCount + 5] = base + 3;
	m_nIndexCount += 6;
}

// The center-fan index pattern AddRectRoundedFilled/Textured share: `base` is the center
// vertex, `base + 1 .. base + pointCount` are the perimeter points built by
// BuildRoundedRectPoints, in order.
void CDrawList::EmitFanIndices(std::uint32_t base, std::uint32_t pointCount)
{
	for (std::uint32_t i = 0; i < pointCount; i += 1) {
		const std::uint32_t next = (i + 1) % pointCount;
		m_pIndices[m_nIndexCount + 0] = base;
		m_pIndices[m_nIndexCount + 1] = base + 1 + i;
		m_pIndices[m_nIndexCount + 2] = base + 1 + next;
		m_nIndexCount += 3;
	}
}

// Perimeter points, clockwise, grouped by corner (top-left, top-right, bottom-right,
// bottom-left); each corner's arc starts where the previous edge ends and ends where the
// next edge begins, so consecutive points - including across corners - are always
// connected by a straight or curved segment with nothing implicit in between.
void CDrawList::BuildRoundedRectPoints(float x, float y, float w, float h, CornerRadii radii, float outX[],
									   float outY[]) const
{
	const float maxRadius = std::min(w, h) * 0.5f;
	const float radius[4]{
		std::min(radii.TopLeft, maxRadius),
		std::min(radii.TopRight, maxRadius),
		std::min(radii.BottomRight, maxRadius),
		std::min(radii.BottomLeft, maxRadius),
	};
	const float centerX[4]{x + radius[0], x + w - radius[1], x + w - radius[2], x + radius[3]};
	const float centerY[4]{y + radius[0], y + radius[1], y + h - radius[2], y + h - radius[3]};
	const float startAngleDeg[4]{180.0f, 270.0f, 0.0f, 90.0f};

	std::uint32_t pointIndex = 0;
	for (std::uint32_t corner = 0; corner < 4; corner += 1) {
		for (std::uint32_t step = 0; step < kRoundedRectPointsPerCorner; step += 1) {
			const float t = static_cast<float>(step) / static_cast<float>(kCornerSegments);
			const float angle = (startAngleDeg[corner] + 90.0f * t) * (kPi / 180.0f);
			outX[pointIndex] = centerX[corner] + radius[corner] * std::cos(angle);
			outY[pointIndex] = centerY[corner] + radius[corner] * std::sin(angle);
			pointIndex += 1;
		}
	}
}

void CDrawList::Finish()
{
	if (m_bHasOpenCommand && m_nIndexCount > m_nCommandStartIndex) {
		assert(m_nCommandCount < kMaxCommands);
		m_aCommands[m_nCommandCount] = DrawCommand{
			.Kind = m_currentKind,
			.pTexture = m_pCurrentTexture,
			.HasClip = m_bCurrentHasClip,
			.ClipRect = m_currentClipRect,
			.IndexOffset = m_nCommandStartIndex,
			.IndexCount = m_nIndexCount - m_nCommandStartIndex,
			.Glow = m_currentGlow,
			.Progress = m_currentProgress,
		};
		m_nCommandCount += 1;
	}
	m_bHasOpenCommand = false;
}

void CDrawList::AddRectFilled(float x, float y, float w, float h, Color color)
{
	SetTargetTexture(nullptr);

	assert(m_nVertexCount + 4 <= m_nVertexCapacity);
	assert(m_nIndexCount + 6 <= m_nIndexCapacity);

	const std::uint32_t packed = ColorPack(color);
	const std::uint32_t base = m_nVertexCount;

	m_pVertices[base + 0] = {.X = x, .Y = y, .U = 0.0f, .V = 0.0f, .Color = packed};
	m_pVertices[base + 1] = {.X = x + w, .Y = y, .U = 0.0f, .V = 0.0f, .Color = packed};
	m_pVertices[base + 2] = {.X = x + w, .Y = y + h, .U = 0.0f, .V = 0.0f, .Color = packed};
	m_pVertices[base + 3] = {.X = x, .Y = y + h, .U = 0.0f, .V = 0.0f, .Color = packed};
	m_nVertexCount += 4;

	EmitQuadIndices(base);
}

void CDrawList::AddRectOutline(float x, float y, float w, float h, float thickness, Color color)
{
	AddRectFilled(x, y, w, thickness, color);												 // top
	AddRectFilled(x, y + h - thickness, w, thickness, color);								 // bottom
	AddRectFilled(x, y + thickness, thickness, h - 2.0f * thickness, color);				 // left
	AddRectFilled(x + w - thickness, y + thickness, thickness, h - 2.0f * thickness, color); // right
}

void CDrawList::AddRectGradientCorners(float x, float y, float w, float h, Color topLeft, Color topRight,
									   Color bottomLeft, Color bottomRight)
{
	SetTargetTexture(nullptr);

	assert(m_nVertexCount + 4 <= m_nVertexCapacity);
	assert(m_nIndexCount + 6 <= m_nIndexCapacity);

	const std::uint32_t base = m_nVertexCount;
	m_pVertices[base + 0] = {.X = x, .Y = y, .U = 0.0f, .V = 0.0f, .Color = ColorPack(topLeft)};
	m_pVertices[base + 1] = {.X = x + w, .Y = y, .U = 0.0f, .V = 0.0f, .Color = ColorPack(topRight)};
	m_pVertices[base + 2] = {.X = x + w, .Y = y + h, .U = 0.0f, .V = 0.0f, .Color = ColorPack(bottomRight)};
	m_pVertices[base + 3] = {.X = x, .Y = y + h, .U = 0.0f, .V = 0.0f, .Color = ColorPack(bottomLeft)};
	m_nVertexCount += 4;

	EmitQuadIndices(base);
}

void CDrawList::AddRectColorPickerSv(float x, float y, float w, float h, float hueDegrees)
{
	SetTarget(nullptr, EDrawCommandKind::DRAW_COMMAND_KIND_COLOR_PICKER_SV);

	assert(m_nVertexCount + 4 <= m_nVertexCapacity);
	assert(m_nIndexCount + 6 <= m_nIndexCapacity);

	// Vertex color isn't a tint here - its red channel carries hueDegrees/360 (0..1
	// UNORM) to ps_color_picker_sv, which decodes it back to 0..360 and combines it with
	// this quad's UV (saturation/value) to compute the real color per pixel.
	const std::uint32_t packed =
		ColorPack(Color{static_cast<std::uint8_t>(std::clamp(hueDegrees / 360.0f, 0.0f, 1.0f) * 255.0f), 0, 0, 255});
	const std::uint32_t base = m_nVertexCount;
	m_pVertices[base + 0] = {.X = x, .Y = y, .U = 0.0f, .V = 0.0f, .Color = packed};
	m_pVertices[base + 1] = {.X = x + w, .Y = y, .U = 1.0f, .V = 0.0f, .Color = packed};
	m_pVertices[base + 2] = {.X = x + w, .Y = y + h, .U = 1.0f, .V = 1.0f, .Color = packed};
	m_pVertices[base + 3] = {.X = x, .Y = y + h, .U = 0.0f, .V = 1.0f, .Color = packed};
	m_nVertexCount += 4;

	EmitQuadIndices(base);
}

void CDrawList::AddLine(float x0, float y0, float x1, float y1, float thickness, Color color)
{
	const float dx = x1 - x0;
	const float dy = y1 - y0;
	const float length = std::sqrt(dx * dx + dy * dy);
	if (length < 0.0001f) {
		return;
	}

	SetTargetTexture(nullptr);

	const float half = thickness * 0.5f;
	const float nx = -dy / length * half;
	const float ny = dx / length * half;

	assert(m_nVertexCount + 4 <= m_nVertexCapacity);
	assert(m_nIndexCount + 6 <= m_nIndexCapacity);

	const std::uint32_t packed = ColorPack(color);
	const std::uint32_t base = m_nVertexCount;

	m_pVertices[base + 0] = {.X = x0 + nx, .Y = y0 + ny, .U = 0.0f, .V = 0.0f, .Color = packed};
	m_pVertices[base + 1] = {.X = x1 + nx, .Y = y1 + ny, .U = 0.0f, .V = 0.0f, .Color = packed};
	m_pVertices[base + 2] = {.X = x1 - nx, .Y = y1 - ny, .U = 0.0f, .V = 0.0f, .Color = packed};
	m_pVertices[base + 3] = {.X = x0 - nx, .Y = y0 - ny, .U = 0.0f, .V = 0.0f, .Color = packed};
	m_nVertexCount += 4;

	EmitQuadIndices(base);
}

void CDrawList::AddRectRoundedFilled(float x, float y, float w, float h, CornerRadii radii, Color color)
{
	SetTargetTexture(nullptr);

	float pointX[kRoundedRectPointCount];
	float pointY[kRoundedRectPointCount];
	BuildRoundedRectPoints(x, y, w, h, radii, pointX, pointY);

	assert(m_nVertexCount + kRoundedRectPointCount + 1 <= m_nVertexCapacity);
	assert(m_nIndexCount + kRoundedRectPointCount * 3 <= m_nIndexCapacity);

	const std::uint32_t packed = ColorPack(color);
	const std::uint32_t base = m_nVertexCount;

	m_pVertices[base] = {.X = x + w * 0.5f, .Y = y + h * 0.5f, .U = 0.0f, .V = 0.0f, .Color = packed};
	for (std::uint32_t i = 0; i < kRoundedRectPointCount; i += 1) {
		m_pVertices[base + 1 + i] = {.X = pointX[i], .Y = pointY[i], .U = 0.0f, .V = 0.0f, .Color = packed};
	}
	m_nVertexCount += kRoundedRectPointCount + 1;

	EmitFanIndices(base, kRoundedRectPointCount);
}

void CDrawList::AddCircularProgress(float cx, float cy, float outerRadius, float innerRadius, float glowMargin,
									float startAngleDeg, float sweepAngleDeg, float glowStrength, Color color)
{
	const float half = outerRadius + glowMargin;
	const float quadSize = half * 2.0f;

	const CircularProgressParams progress{
		.QuadWidth = quadSize,
		.QuadHeight = quadSize,
		.OuterRadius = outerRadius,
		.InnerRadius = innerRadius,
		.StartAngle = startAngleDeg * (kPi / 180.0f),
		.SweepAngle = sweepAngleDeg * (kPi / 180.0f),
		.GlowStrength = glowStrength,
	};
	SetTarget(nullptr, EDrawCommandKind::DRAW_COMMAND_KIND_CIRCULAR_PROGRESS, BannerGlowParams{}, progress);

	assert(m_nVertexCount + 4 <= m_nVertexCapacity);
	assert(m_nIndexCount + 6 <= m_nIndexCapacity);

	const std::uint32_t packed = ColorPack(color);
	const std::uint32_t base = m_nVertexCount;

	// uv spans the quad's own bounding box (0,0)-(1,1) - the pixel shader reconstructs
	// card-local coordinates from it (angle/radius from center), the same convention
	// AddRectRoundedBannerGlow's uv already uses for its own rounded-box SDF.
	m_pVertices[base + 0] = {.X = cx - half, .Y = cy - half, .U = 0.0f, .V = 0.0f, .Color = packed};
	m_pVertices[base + 1] = {.X = cx + half, .Y = cy - half, .U = 1.0f, .V = 0.0f, .Color = packed};
	m_pVertices[base + 2] = {.X = cx + half, .Y = cy + half, .U = 1.0f, .V = 1.0f, .Color = packed};
	m_pVertices[base + 3] = {.X = cx - half, .Y = cy + half, .U = 0.0f, .V = 1.0f, .Color = packed};
	m_nVertexCount += 4;

	EmitQuadIndices(base);
}

void CDrawList::AddRectRoundedBannerGlow(float cardX, float cardY, float cardW, float cardH, float cardCornerRadius,
										 float glowSize, Color color)
{
	const float x = cardX - glowSize;
	const float y = cardY - glowSize;
	const float w = cardW + glowSize * 2.0f;
	const float h = cardH + glowSize * 2.0f;
	const CornerRadii radii = UniformRadii(cardCornerRadius + glowSize);

	// UniformRadii already zeroes and scales; mirror that here so the shader's own
	// rounded-box SDF (which gets the radius separately, not through CornerRadii) draws a
	// card outline matching the geometry rather than a differently-rounded one.
	const float effectiveCornerRadius = ScaledRadius(cardCornerRadius);
	const BannerGlowParams glow{
		.QuadWidth = w, .QuadHeight = h, .CornerRadius = effectiveCornerRadius, .RingWidth = glowSize};
	SetTarget(nullptr, EDrawCommandKind::DRAW_COMMAND_KIND_BANNER_GLOW, glow);

	float pointX[kRoundedRectPointCount];
	float pointY[kRoundedRectPointCount];
	BuildRoundedRectPoints(x, y, w, h, radii, pointX, pointY);

	assert(m_nVertexCount + kRoundedRectPointCount + 1 <= m_nVertexCapacity);
	assert(m_nIndexCount + kRoundedRectPointCount * 3 <= m_nIndexCapacity);

	const std::uint32_t packed = ColorPack(color);
	const std::uint32_t base = m_nVertexCount;

	// uv spans the rect's bounding box (0,0)-(1,1), same convention as the textured
	// rounded draw - the pixel shader uses it as a "position within the card" signal
	// (angle from center, etc.), not a texture sample coordinate.
	const auto uvFor = [&](float px, float py) { return std::pair<float, float>{(px - x) / w, (py - y) / h}; };

	const auto [centerU, centerV] = uvFor(x + w * 0.5f, y + h * 0.5f);
	m_pVertices[base] = {.X = x + w * 0.5f, .Y = y + h * 0.5f, .U = centerU, .V = centerV, .Color = packed};
	for (std::uint32_t i = 0; i < kRoundedRectPointCount; i += 1) {
		const auto [u, v] = uvFor(pointX[i], pointY[i]);
		m_pVertices[base + 1 + i] = {.X = pointX[i], .Y = pointY[i], .U = u, .V = v, .Color = packed};
	}
	m_nVertexCount += kRoundedRectPointCount + 1;

	EmitFanIndices(base, kRoundedRectPointCount);
}

void CDrawList::AddRectRoundedTexturedUv(float x, float y, float w, float h, CornerRadii radii, float u0, float v0,
										 float u1, float v1, const CTexture *pTexture, Color tint)
{
	SetTargetTexture(pTexture);

	float pointX[kRoundedRectPointCount];
	float pointY[kRoundedRectPointCount];
	BuildRoundedRectPoints(x, y, w, h, radii, pointX, pointY);

	assert(m_nVertexCount + kRoundedRectPointCount + 1 <= m_nVertexCapacity);
	assert(m_nIndexCount + kRoundedRectPointCount * 3 <= m_nIndexCapacity);

	const std::uint32_t packed = ColorPack(tint);
	const std::uint32_t base = m_nVertexCount;

	const auto uvFor = [&](float px, float py) {
		return std::pair<float, float>{u0 + (u1 - u0) * (px - x) / w, v0 + (v1 - v0) * (py - y) / h};
	};

	const auto [centerU, centerV] = uvFor(x + w * 0.5f, y + h * 0.5f);
	m_pVertices[base] = {.X = x + w * 0.5f, .Y = y + h * 0.5f, .U = centerU, .V = centerV, .Color = packed};
	for (std::uint32_t i = 0; i < kRoundedRectPointCount; i += 1) {
		const auto [u, v] = uvFor(pointX[i], pointY[i]);
		m_pVertices[base + 1 + i] = {.X = pointX[i], .Y = pointY[i], .U = u, .V = v, .Color = packed};
	}
	m_nVertexCount += kRoundedRectPointCount + 1;

	EmitFanIndices(base, kRoundedRectPointCount);
}

void CDrawList::AddRectRoundedTextured(float x, float y, float w, float h, CornerRadii radii, const CTexture *pTexture,
									   Color tint)
{
	AddRectRoundedTexturedUv(x, y, w, h, radii, 0.0f, 0.0f, 1.0f, 1.0f, pTexture, tint);
}

UvRect CDrawList::ComputeCoverUv(float boxW, float boxH, float textureW, float textureH)
{
	if (boxW <= 0.0f || boxH <= 0.0f || textureW <= 0.0f || textureH <= 0.0f) {
		return UvRect{0.0f, 0.0f, 1.0f, 1.0f};
	}

	const float boxAspect = boxW / boxH;
	const float textureAspect = textureW / textureH;

	if (textureAspect > boxAspect) {
		// Texture is relatively wider than the box - crop its left/right edges.
		const float visibleWidthFraction = boxAspect / textureAspect;
		const float u0 = (1.0f - visibleWidthFraction) * 0.5f;
		return UvRect{u0, 0.0f, 1.0f - u0, 1.0f};
	}
	if (textureAspect < boxAspect) {
		// Texture is relatively taller than the box - crop its top/bottom edges.
		const float visibleHeightFraction = textureAspect / boxAspect;
		const float v0 = (1.0f - visibleHeightFraction) * 0.5f;
		return UvRect{0.0f, v0, 1.0f, 1.0f - v0};
	}
	return UvRect{0.0f, 0.0f, 1.0f, 1.0f};
}

void CDrawList::AddRectTexturedRotated(float x, float y, float w, float h, float radians, const CTexture *pTexture,
									   Color tint)
{
	SetTargetTexture(pTexture);

	assert(m_nVertexCount + 4 <= m_nVertexCapacity);
	assert(m_nIndexCount + 6 <= m_nIndexCapacity);

	const std::uint32_t packed = ColorPack(tint);
	const std::uint32_t base = m_nVertexCount;

	const float cx = x + w * 0.5f;
	const float cy = y + h * 0.5f;
	const float cosAngle = std::cos(radians);
	const float sinAngle = std::sin(radians);
	const float hw = w * 0.5f;
	const float hh = h * 0.5f;

	// Corner offsets from the center, rotated - same winding (TL, TR, BR, BL) as every
	// other quad here, so EmitQuadIndices below needs no special case.
	const float offsetsX[4] = {-hw, hw, hw, -hw};
	const float offsetsY[4] = {-hh, -hh, hh, hh};
	const float uvs[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
	for (std::uint32_t i = 0; i < 4; i += 1) {
		m_pVertices[base + i] = {
			.X = cx + offsetsX[i] * cosAngle - offsetsY[i] * sinAngle,
			.Y = cy + offsetsX[i] * sinAngle + offsetsY[i] * cosAngle,
			.U = uvs[i][0],
			.V = uvs[i][1],
			.Color = packed,
		};
	}
	m_nVertexCount += 4;

	EmitQuadIndices(base);
}

void CDrawList::AddRectTexturedUv(float x, float y, float w, float h, float u0, float v0, float u1, float v1,
								  const CTexture *pTexture, Color tint)
{
	SetTargetTexture(pTexture);

	assert(m_nVertexCount + 4 <= m_nVertexCapacity);
	assert(m_nIndexCount + 6 <= m_nIndexCapacity);

	const std::uint32_t packed = ColorPack(tint);
	const std::uint32_t base = m_nVertexCount;

	m_pVertices[base + 0] = {.X = x, .Y = y, .U = u0, .V = v0, .Color = packed};
	m_pVertices[base + 1] = {.X = x + w, .Y = y, .U = u1, .V = v0, .Color = packed};
	m_pVertices[base + 2] = {.X = x + w, .Y = y + h, .U = u1, .V = v1, .Color = packed};
	m_pVertices[base + 3] = {.X = x, .Y = y + h, .U = u0, .V = v1, .Color = packed};
	m_nVertexCount += 4;

	EmitQuadIndices(base);
}
