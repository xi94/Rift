#include "gfx/texture.h"

#include "gfx/renderer.h"

CTexture::CTexture(IRenderer &renderer, const std::uint8_t *pRgbaPixels, std::uint32_t width, std::uint32_t height)
	: m_renderer(renderer)
	, m_nWidth(width)
	, m_nHeight(height)
	, m_flAspect(height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f)
{
	m_pHandle = m_renderer.CreateTexture(pRgbaPixels, width, height);
}

CTexture::~CTexture()
{
	if (m_pHandle != nullptr) {
		m_renderer.DestroyTexture(m_pHandle);
	}
}
