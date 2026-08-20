#pragma once

// Backend-agnostic rendering interface. Restyled from the original f4's free-function
// gfx::Renderer.h, but the abstraction boundary and pipeline design are unchanged (see
// FORK_WITH_CLASSES.md section 2 in the original f4 repo) - IRenderer is the one real interface
// class this project has (STYLE.md), with exactly one implementation compiled in today
// (gfx/d3d11/renderer_d3d11.h's CRendererD3D11) and a future GL backend slotting in the
// same way. Selected at build time via the RIFT_GFX_BACKEND CMake option, not a runtime
// switch - the interface exists for a clean seam between call sites and the backend, not
// for polymorphism at runtime (there is exactly one IRenderer instance for the process).

#include <cstdint>

#include <Windows.h>

#include "core/types.h"

class IRenderer;

// Window/swapchain setup handed to IRenderer::Init. Width/Height are physical pixels
// (the actual swapchain/backbuffer size); LogicalWidth/LogicalHeight are the space every
// ui:: draw-list coordinate is authored in - see CWindow's own DPI-scale field for why
// these can differ on a scaled monitor.
struct RendererConfig {
	HWND Window;
	std::uint32_t Width;
	std::uint32_t Height;
	float LogicalWidth;
	float LogicalHeight;
};

// A floating-point RGBA color, 0..1 per channel - used only for renderer-internal values
// (the clear color) that get blended/interpolated on the GPU. Deliberately not named
// Color: that name is core/types.h's RGBA8 struct, used everywhere a widget picks a UI
// color - keeping the two distinct avoids a silent precision-lossy conversion at every
// call site that would come from overloading one name for both.
struct ColorF {
	float R;
	float G;
	float B;
	float A;
};

// The GPU-facing vertex layout for all 2D UI drawing (CDrawList builds arrays of these).
// Position is in window client-area logical pixels, origin top-left, y-down - the backend
// handles projection to NDC. Color is RGBA8 packed low-to-high byte (R in bits 0-7, A in
// bits 24-31). U/V are unused (read as 0) by solid-color draws; textured draws sample at
// U/V and multiply by Color, so a texture can still be tinted/faded via vertex alpha.
struct Vertex2D {
	float X;
	float Y;
	float U;
	float V;
	std::uint32_t Color;
};
static_assert(sizeof(Vertex2D) == 20);

// A scissor rect (logical pixels, converted to physical internally) the next Draw2D*
// call is clipped to - Enabled = false disables clipping. Set once per CDrawList command
// by whatever code submits its commands to the renderer, mirroring SetEffectTime's
// "set the pending param, then submit" shape.
struct ClipRect {
	bool Enabled;
	Rect Bounds;
};

// Every renderer_* free function the original f4 had becomes a pure-virtual method here.
// A concrete IRenderer owns everything about turning CDrawList's batched geometry into
// pixels - device/swapchain lifetime, shader compilation, texture upload - callers above
// this interface (CTexture, CFont, the future widget tree) never touch backend types.
class IRenderer {
  public:
	virtual ~IRenderer() = default;

	// One-time setup: creates the device/swapchain/pipeline state against config.Window.
	// Must succeed before any other IRenderer call.
	virtual bool Init(const RendererConfig &config) = 0;

	// Releases everything Init created. Called once at process shutdown.
	virtual void Shutdown() = 0;

	// Must be called after the window's client area changes size (including a DPI change,
	// which resizes the window even if its logical size doesn't change), before the next
	// frame. Width/Height are physical pixels; LogicalWidth/LogicalHeight are what
	// CDrawList's coordinates are authored in - see RendererConfig's own fields.
	virtual void Resize(std::uint32_t width, std::uint32_t height, float logicalWidth, float logicalHeight) = 0;

	// Opens the frame: binds the backbuffer as the render target so the Draw2D* calls
	// below have somewhere to land.
	virtual void BeginFrame() = 0;

	// Fills the just-bound render target with a flat color - the base the frame's 2D draw
	// calls composite on top of.
	virtual void Clear(ColorF color) = 0;

	// Draws one indexed triangle-list batch in screen-space logical-pixel coordinates.
	// Must be called between BeginFrame and EndFrame, after Clear.
	virtual void Draw2D(const Vertex2D *pVertices, std::uint32_t vertexCount, const std::uint32_t *pIndices,
						std::uint32_t indexCount) = 0;

	// Same batch semantics as Draw2D, but samples the texture at pTextureHandle (an
	// opaque handle from CreateTexture) at each vertex's U/V, tinted by vertex color,
	// instead of drawing flat vertex color.
	virtual void Draw2DTextured(void *pTextureHandle, const Vertex2D *pVertices, std::uint32_t vertexCount,
								const std::uint32_t *pIndices, std::uint32_t indexCount) = 0;

	// Same batch semantics as Draw2D, but through the animated "border beam" glow pixel
	// shader instead of a flat fill - driven by SetEffectTime's most recent value plus the
	// geometry of the card this particular glow quad surrounds: quadWidth/quadHeight are
	// the drawn quad's full logical-pixel size, cornerRadius is the card's own rounding
	// (not the expanded quad's), and ringWidth is the glow margin - together they let the
	// shader compute a rounded-box distance field so the glow hugs the card's real outline.
	virtual void Draw2DBannerGlow(const Vertex2D *pVertices, std::uint32_t vertexCount, const std::uint32_t *pIndices,
								  std::uint32_t indexCount, float quadWidth, float quadHeight, float cornerRadius,
								  float ringWidth) = 0;

	// CColorPicker's saturation/value square - a real per-pixel HSV->RGB conversion, not
	// vertex-color interpolation: that gradient has a genuine saturation*value cross term,
	// which a quad's 2 triangles can't reproduce exactly (only affine functions survive
	// triangle-linear interpolation, and this one isn't affine). No per-frame state to
	// set: hue travels in per-vertex, in the color's red channel, the same way every other
	// draw call already carries a per-vertex tint.
	virtual void Draw2DColorPickerSv(const Vertex2D *pVertices, std::uint32_t vertexCount,
									 const std::uint32_t *pIndices, std::uint32_t indexCount) = 0;

	// The account modal's login/progress ring - a real per-pixel shader (see
	// ps_circular_progress in the D3D11 backend) rather than CPU-tessellated geometry, so
	// the ring, its comet-tail sweep, rounded end caps, and outer glow all stay perfectly
	// smooth at any size. quadWidth/quadHeight are the drawn
	// quad's full logical-pixel size (the ring expanded by its own glow margin, same
	// "bigger quad than the visible shape" convention Draw2DBannerGlow uses);
	// outerRadius/innerRadius bound the ring band; startAngle/sweepAngle are radians
	// (sweepAngle >= 2*pi draws a full, solid ring instead of an animated arc);
	// glowStrength (0..1) is the outer bloom's intensity - any breathing pulse or fade is
	// already baked in by the caller, this shader reads it as a flat value for the frame.
	virtual void Draw2DCircularProgress(const Vertex2D *pVertices, std::uint32_t vertexCount,
										const std::uint32_t *pIndices, std::uint32_t indexCount, float quadWidth,
										float quadHeight, float outerRadius, float innerRadius, float startAngle,
										float sweepAngle, float glowStrength) = 0;

	// Seconds since Init, for the banner-glow shader's animation - set once per frame,
	// read by every Draw2DBannerGlow call until next set.
	virtual void SetEffectTime(float timeSeconds) = 0;

	// Sets the pending scissor rect the next Draw2D*/Textured/BannerGlow call is clipped
	// to - see ClipRect's own doc comment for the "set then submit" call shape.
	virtual void SetClipRect(ClipRect clip) = 0;

	// Uploads a tightly-packed RGBA8 image (width*height*4 bytes, row-major, no padding)
	// as an immutable GPU texture and returns an opaque backend handle - only CTexture
	// (gfx/texture.h) should call this directly; everything else should own a CTexture
	// instead of a raw handle.
	virtual void *CreateTexture(const std::uint8_t *pRgbaPixels, std::uint32_t width, std::uint32_t height) = 0;

	// Frees a texture created by CreateTexture and returns its GPU resource slot to the
	// backend's free list, so a later CreateTexture can reuse it instead of growing the
	// pool unboundedly. Only CTexture's destructor should call this.
	virtual void DestroyTexture(void *pHandle) = 0;

	// Closes the frame: presents the backbuffer.
	virtual void EndFrame() = 0;
};
