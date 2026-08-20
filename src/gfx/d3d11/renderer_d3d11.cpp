#include "gfx/d3d11/renderer_d3d11.h"

#include <algorithm>
#include <cassert>
#include <cstring>

#include <d3dcompiler.h>

namespace {
using Microsoft::WRL::ComPtr;

// One shared vertex shader; two pixel shaders (solid / textured) selected per draw
// call. tex0/sampler0 are declared unconditionally so both entry points compile from
// the same source - ps_main simply never references them.
const char *const kShaderSource = R"(
		cbuffer Constants : register(b0) {
			float2 viewport_size;
			float2 padding;
		};

		Texture2D tex0 : register(t0);
		SamplerState sampler0 : register(s0);

		struct VS_Input {
			float2 position : POSITION;
			float2 uv : TEXCOORD0;
			float4 color : COLOR0;
		};

		struct PS_Input {
			float4 position : SV_POSITION;
			float2 uv : TEXCOORD0;
			float4 color : COLOR0;
		};

		PS_Input vs_main(VS_Input input)
		{
			PS_Input output;
			float2 ndc = float2(
				(input.position.x / viewport_size.x) * 2.0 - 1.0,
				1.0 - (input.position.y / viewport_size.y) * 2.0);
			output.position = float4(ndc, 0.0, 1.0);
			output.uv = input.uv;
			output.color = input.color;
			return output;
		}

		float4 ps_main(PS_Input input) : SV_TARGET
		{
			return input.color;
		}

		float4 ps_main_textured(PS_Input input) : SV_TARGET
		{
			return tex0.Sample(sampler0, input.uv) * input.color;
		}

		// The carousel/grid hover-and-selected glow: a rounded-box signed distance field
		// locates each pixel relative to the actual card's outline (not the larger quad
		// this glow is drawn on), so the visible glow band hugs the card's real straight
		// edges and rounded corners exactly. A thin, dim always-on ring stays visible the
		// whole time; two brighter "comets" 180 degrees apart continuously orbit the
		// perimeter together on top of it. Soft smoothstep-shaped lobes (not a sharp pow
		// spike) keep the motion reading as a smooth, polished wave rather than a hot dot
		// snapping around. Both are tinted by the game's own accent (input.color, set by
		// the caller, not a global UI color); the card itself gets a plain white border
		// drawn separately on top, which is what keeps this glow contained to a ring
		// instead of washing over the card's own face.
		cbuffer BannerGlowConstants : register(b1) {
			float bg_time_seconds;
			float bg_quad_width;
			float bg_quad_height;
			float bg_corner_radius;
			float bg_ring_width;
			float3 bg_padding;
		};

		// Signed distance from point p (card-local, origin at center) to a rounded-corner
		// box of the given half-size and corner radius - negative inside, positive
		// outside.
		float rounded_box_sdf(float2 p, float2 half_size, float radius)
		{
			float2 q = abs(p) - half_size + radius;
			return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
		}

		// Shortest signed angular distance from `a` to `b`, wrapped to -pi..pi - lets a
		// beam's brightness falloff treat the angle space as a loop instead of snapping
		// at the +-pi seam.
		float angle_delta(float a, float b)
		{
			float d = a - b;
			return atan2(sin(d), cos(d));
		}

		float4 ps_banner_glow(PS_Input input) : SV_TARGET
		{
			float2 local = (input.uv - 0.5) * float2(bg_quad_width, bg_quad_height);
			float2 card_half_size = float2(bg_quad_width, bg_quad_height) * 0.5 - bg_ring_width;
			float ring = max(bg_ring_width, 0.001);

			// 0 right at the card's own edge, 1 at the glow band's outer edge - the sign
			// of rounded_box_sdf is negative inside the card (occluded by the opaque card
			// drawn on top of this, so it doesn't matter what happens there) and positive
			// outside it, which is exactly the band this glow is meant to fill. A steeper
			// curve than a plain sqrt keeps the visible band slim, hugging the card's
			// edge, instead of spreading evenly across the whole margin.
			float d = rounded_box_sdf(local, card_half_size, bg_corner_radius);
			float t = saturate(d / ring);
			float band = pow(saturate(1.0 - t), 1.8);

			float ambient = band * 0.32;

			// Two comets, 180 degrees apart, orbiting together. Normalizing local by
			// card_half_size before the atan2 keeps their travel speed visually even
			// around a non-square card instead of rushing past the short sides.
			float2 norm = local / max(card_half_size, 1.0);
			float angle = atan2(norm.y, norm.x);
			const float pi = 3.14159265;
			float rotation = bg_time_seconds * 1.1;
			float lobe_width = 0.6; // radians - wider reads as a soft wave, not a hard dot
			float lobe_a = 1.0 - smoothstep(0.0, lobe_width, abs(angle_delta(angle, rotation)));
			float lobe_b = 1.0 - smoothstep(0.0, lobe_width, abs(angle_delta(angle, rotation + pi)));
			float comets = max(lobe_a, lobe_b);

			float glow = saturate(ambient + band * comets * 0.85);
			glow *= 1.0 - smoothstep(0.85, 1.0, t); // clean falloff to nothing right at the quad's own edge
			return float4(input.color.rgb * glow, input.color.a * glow);
		}

		// CColorPicker's saturation/value square. color(S,V) = V * lerp(white, hue, S) has
		// a genuine S*V cross term, so it is *not* reproducible by interpolating 4 corner
		// colors across a quad's two triangles (that only reconstructs affine functions
		// exactly, not bilinear ones) - a real per-pixel HSV->RGB conversion is the correct
		// fix, not another geometry trick. No cbuffer needed: hue travels in via the
		// vertex color's red channel (0..1 UNORM = 0..360 degrees), set once per quad by
		// CDrawList::AddRectColorPickerSv rather than varying per pixel.
		float3 hsv_to_rgb(float h, float s, float v)
		{
			float3 rgb = saturate(abs(fmod(h / 60.0 + float3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0);
			return v * lerp(float3(1.0, 1.0, 1.0), rgb, s);
		}

		float4 ps_color_picker_sv(PS_Input input) : SV_TARGET
		{
			float hue = input.color.r * 360.0;
			float saturation = input.uv.x;
			float value = 1.0 - input.uv.y;
			return float4(hsv_to_rgb(hue, saturation, value), 1.0);
		}

		// The account modal's login/progress ring. A single quad (see
		// CDrawList::AddCircularProgress) carries input.color as the ring/arc's own tint
		// (alpha already folds in the caller's fade) - everything else about the ring's
		// look is computed per pixel here: a dim always-present track, either a full solid
		// ring or an animated comet-tail arc with true rounded caps, and a soft outward
		// glow halo, so the whole thing stays perfectly smooth at any radius instead of
		// faceting like tessellated geometry would.
		cbuffer CircularProgressConstants : register(b1) {
			float cp_quad_width;
			float cp_quad_height;
			float cp_outer_radius;
			float cp_inner_radius;
			float cp_start_angle;
			float cp_sweep_angle;
			float cp_glow_strength;
			float cp_padding;
		};

		float4 ps_circular_progress(PS_Input input) : SV_TARGET
		{
			float2 local = (input.uv - 0.5) * float2(cp_quad_width, cp_quad_height);
			float r = length(local);
			float angle = atan2(local.y, local.x);
			const float two_pi = 6.28318531;
			const float aa = 1.25; // antialiasing feather, logical pixels

			// The ring band itself, antialiased on both the inner and outer edge.
			float ring = smoothstep(cp_inner_radius - aa, cp_inner_radius + aa, r) *
						(1.0 - smoothstep(cp_outer_radius - aa, cp_outer_radius + aa, r));

			bool solid = cp_sweep_angle >= two_pi - 0.001;

			float3 track_color = float3(0.16, 0.16, 0.19);
			float track_alpha = ring * 0.9;

			float3 arc_rgb;
			float arc_alpha;
			if (solid) {
				arc_rgb = input.color.rgb;
				arc_alpha = ring;
			} else {
				// Wrap the pixel's angle into start-relative space, then clamp to the
				// sweep - this is the arc's own parameter, 0 at the tail end, 1 at the
				// head. Projecting it back onto the centerline and measuring distance from
				// there (rather than a second angular smoothstep) is what gives the arc's
				// two ends real rounded caps for free, the same "distance to nearest point
				// on the shape" idea rounded_box_sdf uses above, just for a curved shape
				// instead of a box.
				float da = angle - cp_start_angle;
				da = da - two_pi * floor(da / two_pi + 0.5);
				float sweep = max(cp_sweep_angle, 0.0001);
				float s = clamp(da, 0.0, sweep);
				float t = s / sweep;

				float centerline_r = (cp_outer_radius + cp_inner_radius) * 0.5;
				float half_thickness = (cp_outer_radius - cp_inner_radius) * 0.5;
				float2 closest = centerline_r * float2(cos(cp_start_angle + s), sin(cp_start_angle + s));
				float dist = length(local - closest);

				float coverage = 1.0 - smoothstep(half_thickness - aa, half_thickness + aa, dist);
				arc_rgb = input.color.rgb * (0.6 + 0.8 * t); // brightens toward the head
				arc_alpha = coverage * t;					  // fades toward the tail
			}

			// The arc composited over the track - both still straight, un-premultiplied
			// alpha.
			float ring_alpha = saturate(arc_alpha + track_alpha * (1.0 - arc_alpha));
			float3 ring_rgb = ring_alpha > 0.0001
				? (arc_rgb * arc_alpha + track_color * track_alpha * (1.0 - arc_alpha)) / ring_alpha
				: track_color;

			// A soft halo bleeding outward past the ring's own edge, plus (while an arc is
			// active) a brighter bloom concentrated at the comet's leading tip so the head
			// reads as an actual light source, not just a brighter pixel.
			float outer_margin = max(max(cp_quad_width, cp_quad_height) * 0.5 - cp_outer_radius, 0.001);
			float glow_t = saturate((r - cp_outer_radius) / outer_margin);
			float glow = cp_glow_strength * pow(saturate(1.0 - glow_t), 2.2);
			if (!solid) {
				float head_angle = cp_start_angle + cp_sweep_angle;
				float2 head_pos =
					((cp_outer_radius + cp_inner_radius) * 0.5) * float2(cos(head_angle), sin(head_angle));
				float head_dist = length(local - head_pos);
				// This Gaussian falloff never truly reaches zero (exp() only decays toward
				// it), unlike the ambient band's pow(saturate(...)) term above, which is
				// forced to exactly 0 right at the quad's own edge. Left unmasked, that
				// residual glow gets hard-clipped by the quad boundary instead of fading out
				// first, which reads as a faint rectangular outline around the ring - so it's
				// tapered by the same saturate(1.0 - glow_t) envelope the ambient term already
				// obeys, forcing it to vanish by the same edge instead of getting cut off.
				float head_glow = cp_glow_strength * 0.9 * exp(-(head_dist * head_dist) / (2.0 * 16.0 * 16.0));
				head_glow *= saturate(1.0 - glow_t);
				glow = saturate(glow + head_glow);
			}
			glow *= 1.0 - ring_alpha; // the halo only shows past the ring's own opaque edge

			float combined_alpha = saturate(ring_alpha + glow);
			float3 final_rgb = combined_alpha > 0.0001 ? (ring_rgb * ring_alpha + input.color.rgb * glow) / combined_alpha
														: track_color;

			return float4(saturate(final_rgb), combined_alpha * input.color.a);
		}
	)";

struct ShaderConstants {
	float ViewportWidth;
	float ViewportHeight;
	float Padding[2];
};
static_assert(sizeof(ShaderConstants) == 16);

// The banner-glow shader's pixel-stage-only cbuffer (register b1, separate from the
// main 2D pipeline's vertex-stage ShaderConstants at b0 - see ps_banner_glow above).
// Field order/count must mirror BannerGlowConstants in kShaderSource exactly (HLSL
// packs this into 2 float4 registers: time/width/height/radius, then ring_width +
// padding).
struct BannerGlowShaderConstants {
	float TimeSeconds;
	float QuadWidth;
	float QuadHeight;
	float CornerRadius;
	float RingWidth;
	float Padding[3];
};
static_assert(sizeof(BannerGlowShaderConstants) == 32);

// The circular-progress ring shader's pixel-stage-only cbuffer (register b1, same slot
// BannerGlowShaderConstants uses - the two are never bound in the same draw call, since
// each draw only ever targets one pixel shader). Field order/count must mirror
// CircularProgressConstants in kShaderSource exactly (HLSL packs this into 2 float4
// registers: width/height/outer_radius/inner_radius, then start/sweep/glow/padding).
struct CircularProgressShaderConstants {
	float QuadWidth;
	float QuadHeight;
	float OuterRadius;
	float InnerRadius;
	float StartAngle;
	float SweepAngle;
	float GlowStrength;
	float Padding;
};
static_assert(sizeof(CircularProgressShaderConstants) == 32);
} // namespace

// Defined here (not in the header) - opaque to everything above IRenderer, only this
// backend knows its real layout. Returned as a `void *` pointing into m_pTexturePool, a
// fixed array, so handles stay valid for the process lifetime.
struct CRendererD3D11::TextureSlot {
	ComPtr<ID3D11Texture2D> Texture;
	ComPtr<ID3D11ShaderResourceView> ShaderResourceView;
	std::uint32_t Width = 0;
	std::uint32_t Height = 0;
};

bool CRendererD3D11::CreateRenderTargetView()
{
	ComPtr<ID3D11Texture2D> backBuffer;
	HRESULT hr = m_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
	if (FAILED(hr)) {
		return false;
	}

	hr = m_pDevice->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_pRenderTargetView);
	return SUCCEEDED(hr);
}

void CRendererD3D11::UpdateViewport()
{
	const D3D11_VIEWPORT viewport{
		.TopLeftX = 0,
		.TopLeftY = 0,
		.Width = static_cast<float>(m_nWidth),
		.Height = static_cast<float>(m_nHeight),
		.MinDepth = 0.0f,
		.MaxDepth = 1.0f,
	};
	m_pContext->RSSetViewports(1, &viewport);
}

bool CRendererD3D11::CompileShader(const char *pSource, const char *pEntryPoint, const char *pTarget,
								   ComPtr<ID3DBlob> &outBlob)
{
	UINT compileFlags = 0;
#ifndef NDEBUG
	compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
	ComPtr<ID3DBlob> errorBlob;
	const HRESULT hr = D3DCompile(pSource, std::strlen(pSource), nullptr, nullptr, nullptr, pEntryPoint, pTarget,
								  compileFlags, 0, &outBlob, &errorBlob);
	if (FAILED(hr)) {
		if (errorBlob) {
			OutputDebugStringA(static_cast<const char *>(errorBlob->GetBufferPointer()));
		}
		return false;
	}
	return true;
}

bool CRendererD3D11::CreatePipeline()
{
	ComPtr<ID3DBlob> vsBlob;
	ComPtr<ID3DBlob> psSolidBlob;
	ComPtr<ID3DBlob> psTexturedBlob;
	ComPtr<ID3DBlob> psBannerGlowBlob;
	ComPtr<ID3DBlob> psColorPickerSvBlob;
	ComPtr<ID3DBlob> psCircularProgressBlob;

	if (!CompileShader(kShaderSource, "vs_main", "vs_5_0", vsBlob)) {
		return false;
	}
	if (!CompileShader(kShaderSource, "ps_main", "ps_5_0", psSolidBlob)) {
		return false;
	}
	if (!CompileShader(kShaderSource, "ps_main_textured", "ps_5_0", psTexturedBlob)) {
		return false;
	}
	if (!CompileShader(kShaderSource, "ps_banner_glow", "ps_5_0", psBannerGlowBlob)) {
		return false;
	}
	if (!CompileShader(kShaderSource, "ps_color_picker_sv", "ps_5_0", psColorPickerSvBlob)) {
		return false;
	}
	if (!CompileShader(kShaderSource, "ps_circular_progress", "ps_5_0", psCircularProgressBlob)) {
		return false;
	}

	HRESULT hr =
		m_pDevice->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_pVertexShader);
	if (FAILED(hr)) {
		return false;
	}

	hr = m_pDevice->CreatePixelShader(psSolidBlob->GetBufferPointer(), psSolidBlob->GetBufferSize(), nullptr,
									  &m_pPixelShaderSolid);
	if (FAILED(hr)) {
		return false;
	}

	hr = m_pDevice->CreatePixelShader(psTexturedBlob->GetBufferPointer(), psTexturedBlob->GetBufferSize(), nullptr,
									  &m_pPixelShaderTextured);
	if (FAILED(hr)) {
		return false;
	}

	hr = m_pDevice->CreatePixelShader(psBannerGlowBlob->GetBufferPointer(), psBannerGlowBlob->GetBufferSize(), nullptr,
									  &m_pPixelShaderBannerGlow);
	if (FAILED(hr)) {
		return false;
	}

	hr = m_pDevice->CreatePixelShader(psColorPickerSvBlob->GetBufferPointer(), psColorPickerSvBlob->GetBufferSize(),
									  nullptr, &m_pPixelShaderColorPickerSv);
	if (FAILED(hr)) {
		return false;
	}

	hr = m_pDevice->CreatePixelShader(psCircularProgressBlob->GetBufferPointer(),
									  psCircularProgressBlob->GetBufferSize(), nullptr,
									  &m_pPixelShaderCircularProgress);
	if (FAILED(hr)) {
		return false;
	}

	const D3D11_INPUT_ELEMENT_DESC inputElements[]{
		{"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex2D, X), D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex2D, U), D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(Vertex2D, Color), D3D11_INPUT_PER_VERTEX_DATA, 0},
	};
	hr = m_pDevice->CreateInputLayout(inputElements, ARRAYSIZE(inputElements), vsBlob->GetBufferPointer(),
									  vsBlob->GetBufferSize(), &m_pInputLayout);
	if (FAILED(hr)) {
		return false;
	}

	const D3D11_BUFFER_DESC constantBufferDesc{
		.ByteWidth = sizeof(ShaderConstants),
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_CONSTANT_BUFFER,
	};
	hr = m_pDevice->CreateBuffer(&constantBufferDesc, nullptr, &m_pConstantBuffer);
	if (FAILED(hr)) {
		return false;
	}

	const D3D11_BUFFER_DESC bannerGlowConstantBufferDesc{
		.ByteWidth = sizeof(BannerGlowShaderConstants),
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_CONSTANT_BUFFER,
	};
	hr = m_pDevice->CreateBuffer(&bannerGlowConstantBufferDesc, nullptr, &m_pBannerGlowConstantBuffer);
	if (FAILED(hr)) {
		return false;
	}

	const D3D11_BUFFER_DESC circularProgressConstantBufferDesc{
		.ByteWidth = sizeof(CircularProgressShaderConstants),
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_CONSTANT_BUFFER,
	};
	hr = m_pDevice->CreateBuffer(&circularProgressConstantBufferDesc, nullptr, &m_pCircularProgressConstantBuffer);
	if (FAILED(hr)) {
		return false;
	}

	D3D11_BLEND_DESC blendDesc{};
	blendDesc.RenderTarget[0] = {
		.BlendEnable = TRUE,
		.SrcBlend = D3D11_BLEND_SRC_ALPHA,
		.DestBlend = D3D11_BLEND_INV_SRC_ALPHA,
		.BlendOp = D3D11_BLEND_OP_ADD,
		.SrcBlendAlpha = D3D11_BLEND_ONE,
		.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA,
		.BlendOpAlpha = D3D11_BLEND_OP_ADD,
		.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL,
	};
	hr = m_pDevice->CreateBlendState(&blendDesc, &m_pBlendState);
	if (FAILED(hr)) {
		return false;
	}

	// UI quads carry no meaningful winding order; disable culling rather than depend on
	// getting it right. ScissorEnable is always on - Draw2DInternal sets the scissor rect
	// to the full viewport when no clip is active, so this one rasterizer state covers
	// both clipped and unclipped draws without a second PSO variant.
	const D3D11_RASTERIZER_DESC rasterizerDesc{
		.FillMode = D3D11_FILL_SOLID,
		.CullMode = D3D11_CULL_NONE,
		.ScissorEnable = TRUE,
	};
	hr = m_pDevice->CreateRasterizerState(&rasterizerDesc, &m_pRasterizerState);
	if (FAILED(hr)) {
		return false;
	}

	const D3D11_SAMPLER_DESC samplerDesc{
		.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
		.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP,
		.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP,
		.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP,
		.MaxAnisotropy = 1,
		.MaxLOD = D3D11_FLOAT32_MAX,
	};
	hr = m_pDevice->CreateSamplerState(&samplerDesc, &m_pSamplerState);
	if (FAILED(hr)) {
		return false;
	}

	return true;
}

bool CRendererD3D11::CreateVertexBuffer(std::uint32_t capacity)
{
	const D3D11_BUFFER_DESC desc{
		.ByteWidth = capacity * sizeof(Vertex2D),
		.Usage = D3D11_USAGE_DYNAMIC,
		.BindFlags = D3D11_BIND_VERTEX_BUFFER,
		.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
	};
	const HRESULT hr = m_pDevice->CreateBuffer(&desc, nullptr, &m_pVertexBuffer);
	if (FAILED(hr)) {
		return false;
	}
	m_nVertexBufferCapacity = capacity;
	return true;
}

bool CRendererD3D11::CreateIndexBuffer(std::uint32_t capacity)
{
	const D3D11_BUFFER_DESC desc{
		.ByteWidth = capacity * sizeof(std::uint32_t),
		.Usage = D3D11_USAGE_DYNAMIC,
		.BindFlags = D3D11_BIND_INDEX_BUFFER,
		.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
	};
	const HRESULT hr = m_pDevice->CreateBuffer(&desc, nullptr, &m_pIndexBuffer);
	if (FAILED(hr)) {
		return false;
	}
	m_nIndexBufferCapacity = capacity;
	return true;
}

// The one real draw path every Draw2D*/Textured/BannerGlow/ColorPickerSv entry point
// funnels through: uploads this batch's vertices/indices (growing either buffer first if
// it's outgrown its current capacity), binds whichever pixel shader/texture/extra
// constant buffer this particular draw kind needs, applies the pending scissor rect, and
// issues the indexed draw call.
void CRendererD3D11::Draw2DInternal(const Vertex2D *pVertices, std::uint32_t vertexCount, const std::uint32_t *pIndices,
									std::uint32_t indexCount, ID3D11PixelShader *pPixelShader,
									ID3D11ShaderResourceView *pShaderResourceView,
									ID3D11Buffer *pPixelExtraConstantBuffer)
{
	assert(m_bInitialized);
	if (vertexCount == 0 || indexCount == 0) {
		return;
	}

	if (vertexCount > m_nVertexBufferCapacity) {
		const bool ok = CreateVertexBuffer(vertexCount * 2);
		assert(ok);
		(void)ok;
	}
	if (indexCount > m_nIndexBufferCapacity) {
		const bool ok = CreateIndexBuffer(indexCount * 2);
		assert(ok);
		(void)ok;
	}

	D3D11_MAPPED_SUBRESOURCE mapped{};

	m_pContext->Map(m_pVertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	std::memcpy(mapped.pData, pVertices, vertexCount * sizeof(Vertex2D));
	m_pContext->Unmap(m_pVertexBuffer.Get(), 0);

	m_pContext->Map(m_pIndexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	std::memcpy(mapped.pData, pIndices, indexCount * sizeof(std::uint32_t));
	m_pContext->Unmap(m_pIndexBuffer.Get(), 0);

	const ShaderConstants constants{.ViewportWidth = m_flLogicalWidth, .ViewportHeight = m_flLogicalHeight};
	m_pContext->UpdateSubresource(m_pConstantBuffer.Get(), 0, nullptr, &constants, 0, 0);

	const UINT stride = sizeof(Vertex2D);
	const UINT offset = 0;
	ID3D11Buffer *pVertexBuffer = m_pVertexBuffer.Get();
	m_pContext->IASetVertexBuffers(0, 1, &pVertexBuffer, &stride, &offset);
	m_pContext->IASetIndexBuffer(m_pIndexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
	m_pContext->IASetInputLayout(m_pInputLayout.Get());
	m_pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	m_pContext->VSSetShader(m_pVertexShader.Get(), nullptr, 0);
	ID3D11Buffer *pConstantBuffer = m_pConstantBuffer.Get();
	m_pContext->VSSetConstantBuffers(0, 1, &pConstantBuffer);
	m_pContext->PSSetShader(pPixelShader, nullptr, 0);
	m_pContext->PSSetShaderResources(0, 1, &pShaderResourceView);
	ID3D11SamplerState *pSampler = m_pSamplerState.Get();
	m_pContext->PSSetSamplers(0, 1, &pSampler);
	if (pPixelExtraConstantBuffer != nullptr) {
		m_pContext->PSSetConstantBuffers(1, 1, &pPixelExtraConstantBuffer);
	}

	m_pContext->OMSetBlendState(m_pBlendState.Get(), nullptr, 0xFFFFFFFF);
	m_pContext->RSSetState(m_pRasterizerState.Get());

	// Logical -> physical pixels, same scale Resize derives from width/logicalWidth
	// elsewhere - the scissor rect operates on the actual render target, which is always
	// sized in physical pixels regardless of DPI.
	const float scale = m_flLogicalWidth > 0.0f ? static_cast<float>(m_nWidth) / m_flLogicalWidth : 1.0f;
	D3D11_RECT scissor{
		.left = 0, .top = 0, .right = static_cast<LONG>(m_nWidth), .bottom = static_cast<LONG>(m_nHeight)};
	if (m_pendingClipRect.Enabled) {
		const Rect &clip = m_pendingClipRect.Bounds;
		scissor.left = std::clamp(static_cast<LONG>(clip.X * scale), 0L, static_cast<LONG>(m_nWidth));
		scissor.top = std::clamp(static_cast<LONG>(clip.Y * scale), 0L, static_cast<LONG>(m_nHeight));
		scissor.right =
			std::clamp(static_cast<LONG>((clip.X + clip.W) * scale), scissor.left, static_cast<LONG>(m_nWidth));
		scissor.bottom =
			std::clamp(static_cast<LONG>((clip.Y + clip.H) * scale), scissor.top, static_cast<LONG>(m_nHeight));
	}
	m_pContext->RSSetScissorRects(1, &scissor);

	m_pContext->DrawIndexed(indexCount, 0, 0);
}

bool CRendererD3D11::Init(const RendererConfig &config)
{
	assert(!m_bInitialized);
	assert(config.Window != nullptr);

	DXGI_SWAP_CHAIN_DESC swapChainDesc{
		.BufferDesc =
			{
				.Width = config.Width,
				.Height = config.Height,
				.RefreshRate = {.Numerator = 0, .Denominator = 1},
				.Format = DXGI_FORMAT_R8G8B8A8_UNORM,
			},
		// MSAA directly on the swapchain's own backbuffer - DXGI_SWAP_EFFECT_DISCARD
		// (unlike the FLIP_* effects) supports this and resolves it implicitly on
		// Present, so no explicit ResolveSubresource is needed for this target. 4x MSAA
		// for every render-target-capable format, including this one, is mandatory
		// hardware support at feature level 10.1+ (this app already requires 11.0), so no
		// runtime capability query is needed either.
		.SampleDesc = {.Count = kMsaaSampleCount, .Quality = 0},
		.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
		.BufferCount = 2,
		.OutputWindow = config.Window,
		.Windowed = TRUE,
		.SwapEffect = DXGI_SWAP_EFFECT_DISCARD,
	};

	UINT deviceFlags = 0;
#ifndef NDEBUG
	deviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

	const D3D_FEATURE_LEVEL requestedFeatureLevel = D3D_FEATURE_LEVEL_11_0;
	D3D_FEATURE_LEVEL obtainedFeatureLevel;

	const HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, deviceFlags,
													 &requestedFeatureLevel, 1, D3D11_SDK_VERSION, &swapChainDesc,
													 &m_pSwapChain, &m_pDevice, &obtainedFeatureLevel, &m_pContext);
	if (FAILED(hr)) {
		return false;
	}

	if (!CreateRenderTargetView()) {
		return false;
	}
	if (!CreatePipeline()) {
		return false;
	}
	if (!CreateVertexBuffer(kInitialVertexCapacity)) {
		return false;
	}
	if (!CreateIndexBuffer(kInitialIndexCapacity)) {
		return false;
	}

	m_pTexturePool = new TextureSlot[kMaxTextures];

	m_nWidth = config.Width;
	m_nHeight = config.Height;
	m_flLogicalWidth = config.LogicalWidth;
	m_flLogicalHeight = config.LogicalHeight;
	UpdateViewport();

	m_bInitialized = true;
	return true;
}

CRendererD3D11::~CRendererD3D11()
{
	if (m_bInitialized) {
		Shutdown();
	}
}

void CRendererD3D11::Shutdown()
{
	delete[] m_pTexturePool;
	m_pTexturePool = nullptr;
	*this = CRendererD3D11();
}

void CRendererD3D11::Resize(std::uint32_t width, std::uint32_t height, float logicalWidth, float logicalHeight)
{
	assert(m_bInitialized);
	if (width == 0 || height == 0) {
		return;
	}

	m_pRenderTargetView.Reset();

	const HRESULT hr = m_pSwapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
	assert(SUCCEEDED(hr));

	const bool ok = CreateRenderTargetView();
	assert(ok);
	(void)ok;

	m_nWidth = width;
	m_nHeight = height;
	m_flLogicalWidth = logicalWidth;
	m_flLogicalHeight = logicalHeight;
	UpdateViewport();
}

void CRendererD3D11::BeginFrame()
{
	assert(m_bInitialized);
	ID3D11RenderTargetView *pRenderTargetView = m_pRenderTargetView.Get();
	m_pContext->OMSetRenderTargets(1, &pRenderTargetView, nullptr);
}

void CRendererD3D11::Clear(ColorF color)
{
	assert(m_bInitialized);
	const float rgba[4]{color.R, color.G, color.B, color.A};
	m_pContext->ClearRenderTargetView(m_pRenderTargetView.Get(), rgba);
}

void CRendererD3D11::Draw2D(const Vertex2D *pVertices, std::uint32_t vertexCount, const std::uint32_t *pIndices,
							std::uint32_t indexCount)
{
	Draw2DInternal(pVertices, vertexCount, pIndices, indexCount, m_pPixelShaderSolid.Get(), nullptr);
}

void CRendererD3D11::Draw2DTextured(void *pTextureHandle, const Vertex2D *pVertices, std::uint32_t vertexCount,
									const std::uint32_t *pIndices, std::uint32_t indexCount)
{
	assert(pTextureHandle != nullptr);
	auto *pSlot = static_cast<TextureSlot *>(pTextureHandle);
	Draw2DInternal(pVertices, vertexCount, pIndices, indexCount, m_pPixelShaderTextured.Get(),
				   pSlot->ShaderResourceView.Get());
}

void CRendererD3D11::Draw2DBannerGlow(const Vertex2D *pVertices, std::uint32_t vertexCount,
									  const std::uint32_t *pIndices, std::uint32_t indexCount, float quadWidth,
									  float quadHeight, float cornerRadius, float ringWidth)
{
	assert(m_bInitialized);
	const BannerGlowShaderConstants constants{
		.TimeSeconds = m_flEffectTimeSeconds,
		.QuadWidth = quadWidth,
		.QuadHeight = quadHeight,
		.CornerRadius = cornerRadius,
		.RingWidth = ringWidth,
	};
	m_pContext->UpdateSubresource(m_pBannerGlowConstantBuffer.Get(), 0, nullptr, &constants, 0, 0);
	Draw2DInternal(pVertices, vertexCount, pIndices, indexCount, m_pPixelShaderBannerGlow.Get(), nullptr,
				   m_pBannerGlowConstantBuffer.Get());
}

void CRendererD3D11::Draw2DColorPickerSv(const Vertex2D *pVertices, std::uint32_t vertexCount,
										 const std::uint32_t *pIndices, std::uint32_t indexCount)
{
	assert(m_bInitialized);
	// No cbuffer: hue travels per-vertex in the vertex color's red channel.
	Draw2DInternal(pVertices, vertexCount, pIndices, indexCount, m_pPixelShaderColorPickerSv.Get(), nullptr);
}

void CRendererD3D11::Draw2DCircularProgress(const Vertex2D *pVertices, std::uint32_t vertexCount,
											const std::uint32_t *pIndices, std::uint32_t indexCount, float quadWidth,
											float quadHeight, float outerRadius, float innerRadius, float startAngle,
											float sweepAngle, float glowStrength)
{
	assert(m_bInitialized);
	const CircularProgressShaderConstants constants{
		.QuadWidth = quadWidth,
		.QuadHeight = quadHeight,
		.OuterRadius = outerRadius,
		.InnerRadius = innerRadius,
		.StartAngle = startAngle,
		.SweepAngle = sweepAngle,
		.GlowStrength = glowStrength,
	};
	m_pContext->UpdateSubresource(m_pCircularProgressConstantBuffer.Get(), 0, nullptr, &constants, 0, 0);
	Draw2DInternal(pVertices, vertexCount, pIndices, indexCount, m_pPixelShaderCircularProgress.Get(), nullptr,
				   m_pCircularProgressConstantBuffer.Get());
}

void CRendererD3D11::SetEffectTime(float timeSeconds)
{
	m_flEffectTimeSeconds = timeSeconds;
}

void CRendererD3D11::SetClipRect(ClipRect clip)
{
	m_pendingClipRect = clip;
}

void *CRendererD3D11::CreateTexture(const std::uint8_t *pRgbaPixels, std::uint32_t width, std::uint32_t height)
{
	assert(m_bInitialized);

	// Prefer a freed slot over growing the high-water mark - see m_aFreeTextureIndices.
	std::uint32_t index;
	if (m_nFreeTextureCount > 0) {
		m_nFreeTextureCount -= 1;
		index = m_aFreeTextureIndices[m_nFreeTextureCount];
	} else {
		assert(m_nTexturePoolCount < kMaxTextures);
		index = m_nTexturePoolCount;
		m_nTexturePoolCount += 1;
	}

	TextureSlot &slot = m_pTexturePool[index];

	const D3D11_TEXTURE2D_DESC textureDesc{
		.Width = width,
		.Height = height,
		.MipLevels = 1,
		.ArraySize = 1,
		.Format = DXGI_FORMAT_R8G8B8A8_UNORM,
		.SampleDesc = {.Count = 1, .Quality = 0},
		.Usage = D3D11_USAGE_IMMUTABLE,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE,
	};
	const D3D11_SUBRESOURCE_DATA initialData{
		.pSysMem = pRgbaPixels,
		.SysMemPitch = width * 4,
	};
	HRESULT hr = m_pDevice->CreateTexture2D(&textureDesc, &initialData, &slot.Texture);
	if (FAILED(hr)) {
		m_aFreeTextureIndices[m_nFreeTextureCount] = index; // give the slot back; nothing was created
		m_nFreeTextureCount += 1;
		return nullptr;
	}

	hr = m_pDevice->CreateShaderResourceView(slot.Texture.Get(), nullptr, &slot.ShaderResourceView);
	if (FAILED(hr)) {
		slot.Texture.Reset();
		m_aFreeTextureIndices[m_nFreeTextureCount] = index;
		m_nFreeTextureCount += 1;
		return nullptr;
	}

	slot.Width = width;
	slot.Height = height;
	return &slot;
}

void CRendererD3D11::DestroyTexture(void *pHandle)
{
	assert(pHandle != nullptr);
	auto *pSlot = static_cast<TextureSlot *>(pHandle);
	pSlot->ShaderResourceView.Reset();
	pSlot->Texture.Reset();

	const auto index = static_cast<std::uint32_t>(pSlot - m_pTexturePool);
	assert(index < m_nTexturePoolCount);
	assert(m_nFreeTextureCount < kMaxTextures);
	m_aFreeTextureIndices[m_nFreeTextureCount] = index;
	m_nFreeTextureCount += 1;
}

void CRendererD3D11::EndFrame()
{
	assert(m_bInitialized);
	m_pSwapChain->Present(1, 0);
}
