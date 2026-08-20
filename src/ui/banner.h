#pragma once

// A carousel entry (one game) and the account list under it - shared by CCarousel (the
// widget that scrolls/draws these) and CStorage (which round-trips each banner's account
// list, not the banner itself - texture/accent are compiled in, not user data).

#include <cstdint>

#include "core/account.h"
#include "core/string_view.h"
#include "core/types.h"

class CTexture; // gfx/texture.h - only ever used here as an opaque pointer

constexpr std::uint32_t kCarouselMaxBanners = 16;
constexpr std::uint32_t kCarouselMaxAccountsPerBanner = 32;
constexpr float kCarouselCardCornerRadius = 14.0f;

struct CBanner {
	CStringView Title;
	CTexture *pTexture = nullptr;
	float TextureAspect = 1.0f; // width/height at load time, so cover-fit cropping never needs to re-query the renderer

	// A dedicated square per-game icon, used by List view's row thumbnail and
	// CGameSelectPopup's per-row icon instead of a cover-fit crop of pTexture. May be
	// null (falls back to pTexture/Accent at each call site) for a banner added without
	// a dedicated icon yet.
	CTexture *pIcon = nullptr;

	Color Accent{};
	CAccount Accounts[kCarouselMaxAccountsPerBanner];
	std::uint32_t AccountCount = 0;
};
