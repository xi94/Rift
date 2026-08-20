#include "gfx/font.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <print>

#include "gfx/texture.h"

namespace {
// Bigger atlas for a bigger bake - a fixed 512x512 comfortably fits kCharCount glyphs
// at modest logical sizes and dpiScale 1.0, but a high Font Size setting combined with
// a high-DPI monitor can bake at well over 100 real texels per glyph, which doesn't
// fit 512x512 at 2x2 oversampling. Sized in coarse steps (not exactly-fitted) since
// retrying a failed pack is more complexity than a slightly-oversized atlas costs in
// VRAM.
std::uint32_t AtlasSizeFor(float bakedPixelHeight)
{
	if (bakedPixelHeight <= 24.0f) {
		return 512;
	}
	if (bakedPixelHeight <= 48.0f) {
		return 1024;
	}
	return 2048;
}
} // namespace

// See font.h's own comment: declared out-of-line specifically so these are instantiated
// here, where CTexture (gfx/texture.h, included above) is a complete type.
CFont::CFont() = default;
CFont::~CFont() = default;
CFont::CFont(CFont &&) noexcept = default;
CFont &CFont::operator=(CFont &&) noexcept = default;

bool CFont::LoadFromFile(IRenderer &renderer, const char *pPath, float pixelHeight, float dpiScale)
{
	FILE *pFile = std::fopen(pPath, "rb");
	if (pFile == nullptr) {
		std::println("Failed to open font file: {}", pPath);
		return false;
	}

	std::fseek(pFile, 0, SEEK_END);
	const long fileSize = std::ftell(pFile);
	std::fseek(pFile, 0, SEEK_SET);

	auto *pFontData = static_cast<unsigned char *>(std::malloc(static_cast<std::size_t>(fileSize)));
	const std::size_t readCount = std::fread(pFontData, 1, static_cast<std::size_t>(fileSize), pFile);
	std::fclose(pFile);
	if (readCount != static_cast<std::size_t>(fileSize)) {
		std::println("Failed to read font file: {}", pPath);
		std::free(pFontData);
		return false;
	}

	const float bakedPixelHeight = pixelHeight * dpiScale;
	const std::uint32_t atlasSize = AtlasSizeFor(bakedPixelHeight);

	auto *pAlphaPixels = static_cast<unsigned char *>(std::malloc(atlasSize * atlasSize));

	stbtt_pack_context packContext;
	stbtt_PackBegin(&packContext, pAlphaPixels, static_cast<int>(atlasSize), static_cast<int>(atlasSize), 0, 1,
					nullptr);
	// No supersampling: 2x2 oversampling rasterizes each glyph at 2x linear size and
	// box-filters it back down into the atlas texel, which is a real low-pass blur - fine
	// for text reused at many fractional subpixel offsets, but at a thin face's ~1.5px
	// stroke width it smears the stroke's crisp core into a soft gradient with no sharp
	// center left at all. stb's plain per-pixel coverage rasterization (oversample 1x1)
	// is what crisp UI text actually wants here - sharp at 1:1, same as this atlas is
	// always sampled at (dpiScale bakes 1 atlas texel per logical pixel, see
	// CFont::GetBakeScale).
	stbtt_PackSetOversampling(&packContext, 1, 1);
	const int packedOk = stbtt_PackFontRange(&packContext, pFontData, 0, bakedPixelHeight, static_cast<int>(kFirstChar),
											 static_cast<int>(kCharCount), m_aPackedChars);
	stbtt_PackEnd(&packContext);

	if (!packedOk) {
		std::println("Failed to pack glyph atlas for font: {}", pPath);
		std::free(pAlphaPixels);
		std::free(pFontData);
		return false;
	}

	// stb_truetype's raw coverage mask reads noticeably thin/dim for a light face at
	// typical UI sizes - a mild gamma boost on the alpha channel darkens partially-covered
	// edge pixels toward fully-covered, closer to how the OS's own ClearType/DirectWrite
	// rendering reads, without touching glyph geometry or atlas packing at all.
	constexpr float kAlphaGamma = 0.8f;
	auto *pRgbaPixels = static_cast<unsigned char *>(std::malloc(atlasSize * atlasSize * 4));
	for (std::uint32_t i = 0; i < atlasSize * atlasSize; i += 1) {
		const float coverage = static_cast<float>(pAlphaPixels[i]) / 255.0f;
		const float boosted = std::pow(coverage, kAlphaGamma) * 255.0f;
		pRgbaPixels[i * 4 + 0] = 255;
		pRgbaPixels[i * 4 + 1] = 255;
		pRgbaPixels[i * 4 + 2] = 255;
		pRgbaPixels[i * 4 + 3] = static_cast<unsigned char>(std::min(255.0f, boosted));
	}
	m_pAtlas = std::make_unique<CTexture>(renderer, pRgbaPixels, atlasSize, atlasSize);
	std::free(pRgbaPixels);
	std::free(pAlphaPixels);

	stbtt_fontinfo fontInfo;
	stbtt_InitFont(&fontInfo, pFontData, 0);
	int ascent = 0;
	int descent = 0;
	int lineGap = 0;
	stbtt_GetFontVMetrics(&fontInfo, &ascent, &descent, &lineGap);
	const float scale = stbtt_ScaleForPixelHeight(&fontInfo, bakedPixelHeight);
	m_nAtlasSize = atlasSize;
	m_flPixelHeight = pixelHeight;
	m_flBakeScale = dpiScale;
	m_flAscent = static_cast<float>(ascent) * scale / dpiScale;
	m_flDescent = static_cast<float>(descent) * scale / dpiScale;
	m_flLineGap = static_cast<float>(lineGap) * scale / dpiScale;

	std::free(pFontData);

	return m_pAtlas->GetHandle() != nullptr;
}
