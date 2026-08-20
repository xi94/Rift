#pragma once

// RAII ownership of one GPU texture. The original f4 paired a free renderer_create_texture
// with a free renderer_destroy_texture that every caller had to remember to balance by
// hand - and did in fact get this wrong once (a missing free-list meant destroyed slots
// were never reused). CTexture is the fix FORK_WITH_CLASSES.md's memory-strategy section
// (see the original f4 repo) recommends: construction uploads the pixels, destruction
// frees the GPU resource, so "did every create get a matching destroy" is a property the
// compiler enforces via the usual non-copyable-RAII-member rules instead of something a
// future call site can get wrong again.

#include <cstdint>

class IRenderer;

class CTexture {
  public:
	// Uploads pRgbaPixels (width*height*4 bytes, row-major, no padding) via renderer and
	// keeps a reference to it so the destructor can free the same backend resource. The
	// renderer reference must outlive this texture - true for every real use, since
	// IRenderer lives for the whole process and textures are destroyed well before
	// Shutdown().
	CTexture(IRenderer &renderer, const std::uint8_t *pRgbaPixels, std::uint32_t width, std::uint32_t height);
	~CTexture();

	// A bitwise copy would leave two owners freeing the same backend slot on destruction.
	CTexture(const CTexture &) = delete;
	CTexture &operator=(const CTexture &) = delete;

	std::uint32_t GetWidth() const
	{
		return m_nWidth;
	}

	std::uint32_t GetHeight() const
	{
		return m_nHeight;
	}

	// width/height at load time - lets a caller (e.g. CCarousel's banner art) cover-fit an
	// image into a differently-proportioned rect instead of stretching it.
	float GetAspect() const
	{
		return m_flAspect;
	}

	// The opaque backend handle IRenderer::Draw2DTextured expects - only whatever code
	// submits CDrawList's batched commands to the renderer should ever call this.
	void *GetHandle() const
	{
		return m_pHandle;
	}

  private:
	IRenderer &m_renderer;
	void *m_pHandle;
	std::uint32_t m_nWidth;
	std::uint32_t m_nHeight;
	float m_flAspect;
};
