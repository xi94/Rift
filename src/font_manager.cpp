#include "font_manager.h"

#include <cstdio>
#include <cstring>
#include <print>

#include <Windows.h>

namespace {
// The Settings panel's "Font Size" field is a friendly display number, not a literal
// pixel count fed straight to stb_truetype - text at a given Font Size read noticeably
// smaller than what that number implies in basically every other app. Rather than
// chase whether that's a "real" bug, scale the number up into the pixel height stb
// actually bakes: kFontSizeDisplayScale is the one place that conversion happens,
// applied every time a nominal pixel size reaches CFont::LoadFromFile (both Load's
// startup bake and ApplyBody's re-bake) so it can't drift between the two call sites.
// CFont::GetPixelHeight ends up holding the real (post-scale) baked size, so every
// layout/metric consumer downstream already reasons in real pixels with no separate
// propagation needed - this is the only choke point.
constexpr float kFontSizeDisplayScale = 1.5f;

// Nominal Settings units (see kFontSizeDisplayScale) - the startup bake before
// CSettings is loaded/applied. Secondary is its own independent value, not a fixed
// ratio of body.
constexpr float kBodyPixelHeight = 16.0f;
constexpr float kSecondaryPixelHeight = 12.0f;
constexpr const char *kDefaultFontFileName = "segoeui.ttf";

// Resolves a font filename fragment (e.g. "segoeui.ttf") against the system Fonts
// directory into a full path CFont::LoadFromFile can fopen. pName is a null-terminated
// C string, not a CStringView - it's built from/fed into CRT path functions, not
// rendered.
bool FontPathFor(const char *pName, char *pBuffer, std::size_t bufferSize)
{
	char windowsDir[MAX_PATH];
	const UINT length = GetWindowsDirectoryA(windowsDir, MAX_PATH);
	if (length == 0 || length >= MAX_PATH) {
		return false;
	}

	return std::snprintf(pBuffer, bufferSize, "%s\\Fonts\\%s", windowsDir, pName) > 0;
}
} // namespace

bool CFontManager::Load(IRenderer &renderer, float dpiScale)
{
	char path[MAX_PATH + 64];
	if (!FontPathFor(kDefaultFontFileName, path, sizeof(path))) {
		std::println("Failed to resolve the system Fonts directory.");
		return false;
	}

	if (!m_body.LoadFromFile(renderer, path, kBodyPixelHeight * kFontSizeDisplayScale, dpiScale)) {
		return false;
	}

	if (!m_secondary.LoadFromFile(renderer, path, kSecondaryPixelHeight * kFontSizeDisplayScale, dpiScale)) {
		return false;
	}

	return true;
}

bool CFontManager::ApplyBody(IRenderer &renderer, CStringView fontFileName, float bodyPixelSize,
							 float secondaryPixelSize, float dpiScale)
{
	char name[128];
	if (fontFileName.Length == 0 || fontFileName.Length >= sizeof(name)) {
		return false;
	}
	std::memcpy(name, fontFileName.pData, fontFileName.Length);
	name[fontFileName.Length] = '\0';

	char path[MAX_PATH + 64];
	if (!FontPathFor(name, path, sizeof(path))) {
		return false;
	}

	// Both sizes are baked into temporaries first and only swapped in once both succeed:
	// a bad typed font name (or an out-of-range size) must never leave the UI with one
	// face updated and the other stale.
	CFont bodyCandidate;
	if (!bodyCandidate.LoadFromFile(renderer, path, bodyPixelSize * kFontSizeDisplayScale, dpiScale)) {
		return false;
	}

	CFont secondaryCandidate;
	if (!secondaryCandidate.LoadFromFile(renderer, path, secondaryPixelSize * kFontSizeDisplayScale, dpiScale)) {
		return false;
	}

	m_body = std::move(bodyCandidate);
	m_secondary = std::move(secondaryCandidate);
	return true;
}
