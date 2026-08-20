#pragma once

// CPU-side geometry builder for the custom UI. Fixed-capacity and arena-backed - one of
// the two places FORK_WITH_CLASSES.md's memory-strategy section (see the original f4
// repo, section 6) keeps the original's bump-allocation reasoning regardless of the
// class-based rewrite: capacity is decided once at Init and never grows, so a UI frame
// never allocates - Clear just resets the counts, the backing storage is reused every
// frame. No OS controls are involved anywhere in this stack - every pixel is drawn by us.
//
// The list batches by texture/kind/clip-rect: Finish closes out the in-progress batch
// into m_aCommands, and the caller submits one IRenderer::Draw2D*/Textured/BannerGlow/
// ColorPickerSv call per command, in order - so draw order (and therefore on-screen
// layering) is preserved across solid/textured calls exactly as issued.

#include <cstdint>

#include "core/types.h"
#include "gfx/renderer.h"

class CMemoryArena;
class CTexture;

// Packs to the byte order Vertex2D::Color expects (R in bits 0-7, A in 24-31).
std::uint32_t ColorPack(Color color);

// Returns `color` with its alpha multiplied by `alpha` / 255 - used throughout ui/ for
// fade animations (backdrop dim, hover states) driven by an eased 0..255 value, composing
// with whatever alpha the color already carried. Deliberately not named to collide with
// core/types.h's ColorWithAlpha, which replaces alpha outright instead of composing it -
// a real, different operation from this one.
Color ColorFadeAlpha(Color color, std::uint8_t alpha);

// Which of the renderer's pixel shaders a command should be submitted with. Solid/
// Textured are the two basic ones; BannerGlow is the animated shimmering border effect on
// the carousel's selected card - same vertex data shape as Solid (flat-colored geometry,
// no texture sample), different pixel shader, so it gets its own kind rather than
// overloading "texture == nullptr".
enum class EDrawCommandKind : std::uint8_t {
	DRAW_COMMAND_KIND_SOLID,
	DRAW_COMMAND_KIND_TEXTURED,
	DRAW_COMMAND_KIND_BANNER_GLOW,
	DRAW_COMMAND_KIND_COLOR_PICKER_SV,	 // CColorPicker's saturation/value square - see AddRectColorPickerSv
	DRAW_COMMAND_KIND_CIRCULAR_PROGRESS, // the account modal's login ring - see AddCircularProgress
};

// Per-draw-call parameters for EDrawCommandKind::DRAW_COMMAND_KIND_BANNER_GLOW's
// rounded-box distance field (see ps_banner_glow in the D3D11 backend): QuadWidth/
// QuadHeight are the full glow quad's logical-pixel size (the card expanded by its glow
// margin on every side), CornerRadius is the card's own corner rounding (not the expanded
// quad's), and RingWidth is that glow margin. Two banner-glow draws with different values
// can't share one DrawCommand - see SetTarget in draw_list.cpp, same reasoning as a
// texture switch.
struct BannerGlowParams {
	float QuadWidth;
	float QuadHeight;
	float CornerRadius;
	float RingWidth;
};

// Per-draw-call parameters for EDrawCommandKind::DRAW_COMMAND_KIND_CIRCULAR_PROGRESS's
// ring shader (see ps_circular_progress in the D3D11 backend): QuadWidth/QuadHeight are
// the full drawn quad's logical-pixel size (the ring expanded by its own glow margin on
// every side, same "bigger quad than the visible shape" convention BannerGlowParams
// uses), OuterRadius/InnerRadius bound the ring band itself, StartAngle/SweepAngle
// (radians, matching BuildRoundedRectPoints' clockwise/Y-down angle convention) place the
// animated sweep - SweepAngle >= 2*pi draws a full, solid ring instead of a comet-tail
// arc - and GlowStrength (0..1) is the outer bloom's intensity, with any breathing pulse
// or fade-in already baked in by the caller. Two circular-progress draws with different
// values can't share one DrawCommand - see SetTarget in draw_list.cpp, same reasoning as
// a texture switch.
struct CircularProgressParams {
	float QuadWidth;
	float QuadHeight;
	float OuterRadius;
	float InnerRadius;
	float StartAngle;
	float SweepAngle;
	float GlowStrength;
	float Padding;
};

struct DrawCommand {
	EDrawCommandKind Kind;
	const CTexture *pTexture; // only meaningful when Kind == DRAW_COMMAND_KIND_TEXTURED
	bool HasClip;
	Rect ClipRect; // logical pixels; only meaningful when HasClip
	std::uint32_t IndexOffset;
	std::uint32_t IndexCount;
	BannerGlowParams Glow;			  // only meaningful when Kind == DRAW_COMMAND_KIND_BANNER_GLOW
	CircularProgressParams Progress; // only meaningful when Kind == DRAW_COMMAND_KIND_CIRCULAR_PROGRESS
};

// The UV sub-rectangle that "cover-fits" a textureW x textureH image into a boxW x boxH
// box - scales the image up until it fully covers the box on both axes (like CSS
// background-size: cover) and crops the overflowing axis symmetrically, rather than
// stretching the image to the box's aspect ratio. {0,0,1,1} (the whole texture,
// unstretched) when the aspects already match.
struct UvRect {
	float U0;
	float V0;
	float U1;
	float V1;
};

class CDrawList {
  public:
	static constexpr std::uint32_t kMaxCommands = 256;

	// Center-fan triangulation: kCornerSegments arc points per corner (fewer for a square
	// corner, radius 0 collapses to a single point) plus a center vertex. Radii are
	// clamped to min(w,h)/2 so they can never self-intersect on a thin rect. A "border"
	// isn't a stroke primitive - draw two nested rounded rects, outer then inner, inset by
	// the border thickness. 14 is smooth at every radius this project uses (the close
	// badge, panel corners) without meaningfully growing the vertex/index count (a rounded
	// rect is still under 60 vertices).
	static constexpr std::uint32_t kCornerSegments = 14;

	// Reserves the list's fixed vertex/index/command storage from `arena` once, at
	// startup - this is the only allocation a CDrawList ever does; every frame after this
	// just reuses the same backing storage via Clear.
	void Init(CMemoryArena &arena, std::uint32_t vertexCapacity, std::uint32_t indexCapacity);

	// Resets the list back to empty for a new frame - zeroes the counts and in-progress-
	// command state, frees nothing (the arena-backed storage is reused as-is).
	void Clear();

	// Clips every Add* call until the matching Pop, to `rect` (logical pixels, intersected
	// with the viewport at submit time - a rect that runs off the window edge is fine).
	// Not a stack: a nested Push before a Pop just replaces the active clip rather than
	// intersecting with it - this project doesn't nest scrollable regions today, so this
	// covers every real use (a scrollable list's content) without the bookkeeping a real
	// stack would need. Always pair with PopClipRect once the clipped content is drawn.
	void PushClipRect(Rect rect);
	void PopClipRect();

	// Closes out the in-progress batch into m_aCommands. Call once, after the frame's last
	// Add* call and before submitting to the renderer.
	void Finish();

	const DrawCommand *GetCommands() const
	{
		return m_aCommands;
	}

	std::uint32_t GetCommandCount() const
	{
		return m_nCommandCount;
	}

	const Vertex2D *GetVertices() const
	{
		return m_pVertices;
	}

	// The whole frame's vertex count, not a per-command one - every DrawCommand's index
	// range points into this same shared vertex buffer (only IndexOffset/IndexCount slice
	// per command), matching IRenderer::Draw2D*'s own vertexCount parameter shape.
	std::uint32_t GetVertexCount() const
	{
		return m_nVertexCount;
	}

	const std::uint32_t *GetIndices() const
	{
		return m_pIndices;
	}

	// Toggle-aware corner radii for a uniform rounding: 0 on every corner when Round
	// Corners is off in settings. Deliberately separate from core/types.h's plain
	// CornerRadiiUniform (which always honors the requested radius) - every rounded shape
	// in this project should request its radius through this one instead, so the settings
	// toggle has exactly one place to hook into.
	static CornerRadii UniformRadii(float radius);
	static void SetRoundedCornersEnabled(bool enabled);
	static bool RoundedCornersEnabled();

	// The base solid-fill primitive: every other flat-colored shape here (gradient
	// corners, the color-picker SV square, glow quads) shares its two-triangle quad
	// layout, just with different per-vertex colors/UVs/target kind.
	void AddRectFilled(float x, float y, float w, float h, Color color);

	// Four thin filled rects (top/bottom/left/right), not a real stroke primitive - this
	// project has no hollow-rect draw call, so a border/outline is always built from
	// filled geometry, either this or the nested-rounded-rect trick (outer rect in the
	// border color, inner rect in the fill color, inset by the border thickness) used
	// wherever rounding is also needed.
	void AddRectOutline(float x, float y, float w, float h, float thickness, Color color);

	// A solid quad with an independent color per corner, linearly interpolated by the
	// rasterizer within each of the quad's two triangles. Exact for any *1-D* gradient
	// (all 4 corners fall on a single linear ramp - a pure horizontal or vertical
	// gradient, or a gradient between colors that happen to be collinear in RGB space,
	// e.g. CColorPicker's hue strip: red/yellow/green/cyan/blue/magenta/red are exactly
	// collinear segment-by-segment) - but NOT exact for a genuinely bilinear function with
	// a nonzero cross term (the color picker's SV square is V * lerp(white, hue, S), which
	// has an S*V term): triangle-linear interpolation only reconstructs affine functions
	// correctly, so a true 2-D gradient needs a real per-pixel shader instead - see
	// AddRectColorPickerSv for that case specifically.
	void AddRectGradientCorners(float x, float y, float w, float h, Color topLeft, Color topRight, Color bottomLeft,
								Color bottomRight);

	// CColorPicker's saturation/value square: a real per-pixel HSV->RGB conversion
	// (IRenderer::Draw2DColorPickerSv / ps_color_picker_sv), not a geometric trick - see
	// AddRectGradientCorners's doc comment for why one is needed here. hueDegrees travels
	// to the shader packed into the quad's vertex color (u/v span (0,0)-(1,1) across the
	// rect, sampled as saturation/1-value in the shader), so this needs no per-frame
	// renderer state the way the banner-glow shader's time does.
	void AddRectColorPickerSv(float x, float y, float w, float h, float hueDegrees);

	// A thin filled quad along the (x0,y0)-(x1,y1) segment - the only way solid draws can
	// be non-axis-aligned (the title bar's close "X" glyph uses it).
	void AddLine(float x0, float y0, float x1, float y1, float thickness, Color color);

	void AddRectRoundedFilled(float x, float y, float w, float h, CornerRadii radii, Color color);

	// The account modal's login/progress ring - a single quad (big enough to hold the ring
	// plus its outer glow margin) submitted through ps_circular_progress, which computes
	// the track, the animated comet-tail sweep (or a full solid ring when sweepAngleDeg
	// covers a whole circle), its rounded end caps, and an outward glow halo, all
	// per-pixel - a smooth curve at any radius/zoom, unlike a CPU-tessellated fan or
	// strip. `color` is the ring/arc's own tint (its alpha folds in
	// the caller's own fade, same convention as every other primitive here); the shader
	// derives the dim track color and the arc's head/tail brightening from it internally,
	// so no gradient color needs to travel in separately. Angles are in degrees, same
	// clockwise/Y-down convention as BuildRoundedRectPoints; sweepAngleDeg >= 360 draws a
	// full ring instead of an arc.
	void AddCircularProgress(float cx, float cy, float outerRadius, float innerRadius, float glowMargin,
							 float startAngleDeg, float sweepAngleDeg, float glowStrength, Color color);

	// An animated "border beam" glow hugging the outside of a card-shaped rounded rect:
	// (cardX,cardY,cardW,cardH,cardCornerRadius) describe the card itself, not the larger
	// quad actually drawn - this expands it by glowSize on every side internally and hands
	// the renderer enough geometry info (BannerGlowParams) to compute a rounded-box
	// distance field from the card's real outline, so the visible glow band hugs the
	// card's exact shape (straight edges + rounded corners) instead of reading as a vague
	// circular aura. `color`'s alpha is the glow's overall tint/opacity.
	void AddRectRoundedBannerGlow(float cardX, float cardY, float cardW, float cardH, float cardCornerRadius,
								  float glowSize, Color color);

	// Same shape as AddRectRoundedFilled, but samples `pTexture` (tinted by `tint`)
	// instead of a flat fill - u/v map linearly across the rect's untrimmed bounds
	// regardless of rounding, so the image doesn't distort at the rounded corners.
	void AddRectRoundedTextured(float x, float y, float w, float h, CornerRadii radii, const CTexture *pTexture,
								Color tint);

	// Same shape as AddRectRoundedTextured, but the rect's untrimmed bounds map to the
	// (u0,v0)-(u1,v1) sub-rectangle of the texture instead of the full (0,0)-(1,1) - how
	// this project's banner art cover-fit-crops to a card's aspect ratio instead of
	// stretching it (see ComputeCoverUv).
	void AddRectRoundedTexturedUv(float x, float y, float w, float h, CornerRadii radii, float u0, float v0, float u1,
								  float v1, const CTexture *pTexture, Color tint);

	// A plain axis-aligned textured quad with explicit UV corners (u0,v0)-(u1,v1) - unlike
	// the rounded-textured draw above, UV isn't derived from position within the rect.
	// This is what glyph quads need: stb_truetype hands back atlas UV coordinates
	// directly (see ui/text.cpp), not a 0..1 span across the quad.
	void AddRectTexturedUv(float x, float y, float w, float h, float u0, float v0, float u1, float v1,
						   const CTexture *pTexture, Color tint);

	static UvRect ComputeCoverUv(float boxW, float boxH, float textureW, float textureH);

  private:
	// Closes the in-progress command (if it covers any indices) and opens a new one
	// targeting `pTexture`/`kind`/the active clip/glow/progress params - a no-op if already
	// targeting the same combination, so consecutive draws that don't actually change any
	// of those stay batched into one command.
	void SetTarget(const CTexture *pTexture, EDrawCommandKind kind, BannerGlowParams glow = BannerGlowParams{},
				   CircularProgressParams progress = CircularProgressParams{});
	void SetTargetTexture(const CTexture *pTexture);
	void EmitQuadIndices(std::uint32_t base);
	void EmitFanIndices(std::uint32_t base, std::uint32_t pointCount);
	void BuildRoundedRectPoints(float x, float y, float w, float h, CornerRadii radii, float outX[],
								float outY[]) const;

	Vertex2D *m_pVertices = nullptr;
	std::uint32_t *m_pIndices = nullptr;
	std::uint32_t m_nVertexCapacity = 0;
	std::uint32_t m_nIndexCapacity = 0;
	std::uint32_t m_nVertexCount = 0;
	std::uint32_t m_nIndexCount = 0;

	DrawCommand m_aCommands[kMaxCommands]{};
	std::uint32_t m_nCommandCount = 0;
	EDrawCommandKind m_currentKind = EDrawCommandKind::DRAW_COMMAND_KIND_SOLID;
	const CTexture *m_pCurrentTexture = nullptr;
	bool m_bCurrentHasClip = false;
	Rect m_currentClipRect{};
	BannerGlowParams m_currentGlow{};		 // in-progress command's glow params; see SetTarget in draw_list.cpp
	CircularProgressParams m_currentProgress{}; // in-progress command's ring params; see SetTarget in draw_list.cpp
	std::uint32_t m_nCommandStartIndex = 0;
	bool m_bHasOpenCommand = false;

	// The caller's active clip state (PushClipRect/PopClipRect) - what the *next* Add*
	// call should use. Compared against m_bCurrentHasClip/m_currentClipRect on every call
	// to decide whether the in-progress command needs to close and a new one open, the
	// same way a texture/kind change already does.
	bool m_bActiveClipEnabled = false;
	Rect m_activeClipRect{};
};
