#pragma once

// The D3D11 IRenderer implementation - ported from the original f4's
// gfx/d3d11/Renderer_D3D11.cpp with identifiers restyled per STYLE.md, but every
// algorithm (MSAA setup, the free-list texture pool, the 5 HLSL shaders, scissor-rect
// clipping) unchanged, per FORK_WITH_CLASSES.md section 2 in the original f4 repo
// ("largely as-is... not something that needs re-deriving"). The offscreen
// post-process pass this fork later added on top (an MSAA scene texture + full-screen
// combine shader, solely to drive a login success/error flash tint) was removed again -
// unwanted in practice, and nothing else in this project used that pass for anything.

#include <cstdint>

#include <d3d11.h>
#include <wrl/client.h>

#include "gfx/renderer.h"

class CRendererD3D11 final : public IRenderer {
  public:
	// Calls Shutdown() if Init() ever succeeded and nothing already did - RAII backstop
	// for the common case (nothing explicitly shuts the renderer down; every CTexture
	// still alive at that point is a widget-owned member destructing in the normal
	// reverse-declaration-order teardown, which needs a *working* renderer to call
	// DestroyTexture through). Explicitly calling Shutdown() early is safe too as long as
	// every CTexture-owning object (CFontManager, CAssetManager, ...) is guaranteed to
	// have already been destroyed by then - getting that ordering right by hand was a
	// real crash (access violation in ComPtr::Reset, deep in a CFont/CTexture destructor
	// running after an explicit Shutdown() call had already freed the texture pool), so
	// this destructor exists specifically to make "renderer outlives every texture it
	// issued" the default instead of something every call site has to get right itself.
	~CRendererD3D11() override;

	bool Init(const RendererConfig &config) override;
	void Shutdown() override;
	void Resize(std::uint32_t width, std::uint32_t height, float logicalWidth, float logicalHeight) override;

	void BeginFrame() override;
	void Clear(ColorF color) override;
	void EndFrame() override;

	void Draw2D(const Vertex2D *pVertices, std::uint32_t vertexCount, const std::uint32_t *pIndices,
				std::uint32_t indexCount) override;
	void Draw2DTextured(void *pTextureHandle, const Vertex2D *pVertices, std::uint32_t vertexCount,
						const std::uint32_t *pIndices, std::uint32_t indexCount) override;
	void Draw2DBannerGlow(const Vertex2D *pVertices, std::uint32_t vertexCount, const std::uint32_t *pIndices,
						  std::uint32_t indexCount, float quadWidth, float quadHeight, float cornerRadius,
						  float ringWidth) override;
	void Draw2DColorPickerSv(const Vertex2D *pVertices, std::uint32_t vertexCount, const std::uint32_t *pIndices,
							 std::uint32_t indexCount) override;
	void Draw2DCircularProgress(const Vertex2D *pVertices, std::uint32_t vertexCount, const std::uint32_t *pIndices,
								std::uint32_t indexCount, float quadWidth, float quadHeight, float outerRadius,
								float innerRadius, float startAngle, float sweepAngle, float glowStrength) override;

	void SetEffectTime(float timeSeconds) override;
	void SetClipRect(ClipRect clip) override;

	void *CreateTexture(const std::uint8_t *pRgbaPixels, std::uint32_t width, std::uint32_t height) override;
	void DestroyTexture(void *pHandle) override;

  private:
	template <typename T>
	using ComPtr = Microsoft::WRL::ComPtr<T>;

	// Defined only in the .cpp (opaque to everything above IRenderer) - one GPU texture
	// resource plus the dimensions CreateTexture was given. Returned to CTexture as a
	// `void *` pointing into m_aTexturePool, a fixed array, so handles stay valid for the
	// process lifetime without needing an arena threaded into this class.
	struct TextureSlot;

	static constexpr std::uint32_t kMaxTextures = 32;
	static constexpr std::uint32_t kInitialVertexCapacity = 1024;
	static constexpr std::uint32_t kInitialIndexCapacity = 1536;
	static constexpr UINT kMsaaSampleCount = 4;

	bool CreateRenderTargetView();
	void UpdateViewport();
	bool CompileShader(const char *pSource, const char *pEntryPoint, const char *pTarget, ComPtr<ID3DBlob> &outBlob);
	bool CreatePipeline();
	bool CreateVertexBuffer(std::uint32_t capacity);
	bool CreateIndexBuffer(std::uint32_t capacity);
	void Draw2DInternal(const Vertex2D *pVertices, std::uint32_t vertexCount, const std::uint32_t *pIndices,
						std::uint32_t indexCount, ID3D11PixelShader *pPixelShader,
						ID3D11ShaderResourceView *pShaderResourceView,
						ID3D11Buffer *pPixelExtraConstantBuffer = nullptr);

	ComPtr<ID3D11Device> m_pDevice;
	ComPtr<ID3D11DeviceContext> m_pContext;
	ComPtr<IDXGISwapChain> m_pSwapChain;
	ComPtr<ID3D11RenderTargetView> m_pRenderTargetView;

	ComPtr<ID3D11VertexShader> m_pVertexShader;
	ComPtr<ID3D11PixelShader> m_pPixelShaderSolid;
	ComPtr<ID3D11PixelShader> m_pPixelShaderTextured;
	ComPtr<ID3D11PixelShader> m_pPixelShaderBannerGlow;
	ComPtr<ID3D11PixelShader> m_pPixelShaderColorPickerSv;
	ComPtr<ID3D11PixelShader> m_pPixelShaderCircularProgress;
	ComPtr<ID3D11InputLayout> m_pInputLayout;
	ComPtr<ID3D11Buffer> m_pConstantBuffer;
	ComPtr<ID3D11Buffer> m_pBannerGlowConstantBuffer;
	ComPtr<ID3D11Buffer> m_pCircularProgressConstantBuffer;
	float m_flEffectTimeSeconds = 0.0f;
	ComPtr<ID3D11BlendState> m_pBlendState;
	ComPtr<ID3D11RasterizerState> m_pRasterizerState;
	ComPtr<ID3D11SamplerState> m_pSamplerState;

	ComPtr<ID3D11Buffer> m_pVertexBuffer;
	ComPtr<ID3D11Buffer> m_pIndexBuffer;
	std::uint32_t m_nVertexBufferCapacity = 0;
	std::uint32_t m_nIndexBufferCapacity = 0;

	TextureSlot *m_pTexturePool = nullptr; // kMaxTextures slots, heap-allocated in Init (see .cpp)
	std::uint32_t m_nTexturePoolCount =
		0; // high-water mark: slots [0, m_nTexturePoolCount) have been allocated at least once

	// Slots freed by DestroyTexture, reused by the next CreateTexture before growing
	// m_nTexturePoolCount - without this, repeated create/destroy churn (e.g. a Font Size
	// setting re-baking its atlas) would permanently consume a slot per call and
	// eventually hit the kMaxTextures assert despite nothing being simultaneously alive.
	std::uint32_t m_aFreeTextureIndices[kMaxTextures]{};
	std::uint32_t m_nFreeTextureCount = 0;

	std::uint32_t m_nWidth = 0;	 // physical - swapchain/backbuffer/viewport size
	std::uint32_t m_nHeight = 0; // physical
	float m_flLogicalWidth = 0.0f;
	float m_flLogicalHeight = 0.0f;
	bool m_bInitialized = false;

	ClipRect m_pendingClipRect{}; // SetClipRect's most recent value, applied by the next Draw2DInternal call
};
