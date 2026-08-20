#pragma once

// A baked glyph atlas: one GPU texture (alpha coverage expanded to RGBA8 so it samples
// through the same tinted-texture pipeline every other textured draw uses) plus stb's
// packed glyph metrics for ASCII 32..126.
//
// DPI-aware by construction, not by an afterthought scale factor sprinkled at call
// sites: LoadFromFile bakes the atlas at pixelHeight * dpiScale real texels (so glyphs
// stay crisp - sampling a low-res atlas stretched across more physical pixels is exactly
// the blur a DPI-unaware app gets), but every metric this class exposes (GetPixelHeight,
// GetAscent, GetDescent, GetLineGap/GetLineHeight) and everything ui::TextWidth /
// ui::DrawText return or accept is in the *same logical-pixel space as the rest of the
// UI* - callers never need to know or care what dpiScale a CFont was baked with.
// GetBakeScale/GetPackedChars/GetAtlasSize exist only for ui/text.cpp's glyph-walking
// code, which is the one place that does need the baked/physical unit space - nothing
// else should call them.
//
// A CFont can be re-loaded (re-baked) in place any number of times (used by both a Font
// Size setting and a live DPI change) - LoadFromFile always bakes into a fresh CTexture
// and destroys whichever one this CFont held before, so a caller re-baking into an
// already-live CFont never leaks the previous atlas.

#include <cstdint>
#include <memory>

#include "core/string_view.h"

#include "stb/stb_truetype.h"

class IRenderer;
class CTexture;

class CFont {
  public:
	// Declared (not defaulted inline) and defined out-of-line in font.cpp, the standard
	// fix for a unique_ptr<CTexture> member when CTexture is only forward-declared here:
	// an implicitly-generated destructor/move-assignment would need CTexture's complete
	// type at every call site that destroys or reassigns a CFont (font_manager.cpp
	// included), not just here where it's only forward-declared.
	// The default constructor is also declared out-of-line, not just defaulted inline:
	// MSVC generates exception-unwind cleanup for a constructor's member-initializer list
	// that needs unique_ptr<CTexture>'s destructor even when nothing in it can actually
	// throw, so it hits the same incomplete-type problem the destructor/move-assignment
	// do.
	CFont();
	~CFont();
	CFont(CFont &&) noexcept;
	CFont &operator=(CFont &&) noexcept;
	CFont(const CFont &) = delete;
	CFont &operator=(const CFont &) = delete;

	static constexpr std::uint32_t kFirstChar = 32;
	static constexpr std::uint32_t kCharCount = 95; // ASCII 32..126

	// pPath is a plain C string (a filesystem path, not one of CStringView's
	// non-null-terminated views) since it's handed straight to the CRT's fopen.
	// pixelHeight is the logical size callers should reason about everywhere else (row
	// layout, TextWidth); the atlas itself is baked at pixelHeight * dpiScale real texels
	// so glyphs stay crisp when the whole UI is later upscaled onto a higher-DPI physical
	// backbuffer.
	bool LoadFromFile(IRenderer &renderer, const char *pPath, float pixelHeight, float dpiScale);

	float GetPixelHeight() const
	{
		return m_flPixelHeight;
	}

	float GetAscent() const
	{
		return m_flAscent;
	}

	float GetDescent() const
	{
		return m_flDescent;
	}

	float GetLineGap() const
	{
		return m_flLineGap;
	}

	// Baseline-to-baseline distance for two consecutive lines set in this font - ascent
	// plus the (positive) magnitude of descent plus line gap. UI layout that stacks text
	// uses this instead of a fixed pixel constant so it grows along with the font instead
	// of overlapping once the user picks a larger size.
	float GetLineHeight() const
	{
		return m_flAscent - m_flDescent + m_flLineGap;
	}

	const CTexture *GetAtlas() const
	{
		return m_pAtlas.get();
	}

	// Baked-unit-space accessors for ui/text.cpp's glyph walk only - see this class's own
	// file comment.
	const stbtt_packedchar *GetPackedChars() const
	{
		return m_aPackedChars;
	}

	std::uint32_t GetAtlasSize() const
	{
		return m_nAtlasSize;
	}

	float GetBakeScale() const
	{
		return m_flBakeScale;
	}

  private:
	std::unique_ptr<CTexture> m_pAtlas;
	stbtt_packedchar m_aPackedChars[kCharCount]{};
	std::uint32_t m_nAtlasSize = 0; // this bake's atlas texture is m_nAtlasSize x m_nAtlasSize texels
	float m_flPixelHeight = 0.0f;	// logical
	float m_flBakeScale = 1.0f;		// atlas texels per logical pixel - the dpiScale this was baked with
	float m_flAscent = 0.0f;		// logical
	float m_flDescent = 0.0f;		// logical
	float m_flLineGap = 0.0f;		// logical
};
