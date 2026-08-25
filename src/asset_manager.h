#pragma once

// Decodes embeds/**/*.hpp (PNG icons, JPEG banners, embedded as raw byte arrays) into GPU
// textures once at startup via stb_image + CTexture. Nothing else in this project touches
// the embed headers directly - everything downstream just holds a CTexture* handed back
// from here, owned for the process lifetime by this class.

#include <cstdint>
#include <memory>

#include "gfx/texture.h"

class IRenderer;

// The undecoded bytes of an embedded image, for a consumer that needs its own decode rather
// than the GPU texture - core/../tray.cpp builds GDI bitmaps for the tray menu from these.
struct EmbeddedImageBytes {
	const std::uint8_t *pBytes;
	std::uint64_t Length;
};

class CAssetManager {
  public:
	// Per-game icon source bytes, in the same order main() adds the banners.
	static EmbeddedImageBytes IconBytesLeagueOfLegends();
	static EmbeddedImageBytes IconBytesTeamfightTactics();
	static EmbeddedImageBytes IconBytesValorant();
	static EmbeddedImageBytes IconBytesTwoXko();
	static EmbeddedImageBytes IconBytesRuneterra();

	// Requires renderer to already be initialized. Returns false (having logged which
	// asset failed) if any texture fails to decode/upload - a missing/corrupt embed is a
	// build-time asset problem, not something to silently render blank for.
	bool Load(IRenderer &renderer);

	CTexture *GetIconArrowBack() const
	{
		return m_pIconArrowBack.get();
	}
	CTexture *GetIconClose() const
	{
		return m_pIconClose.get();
	}
	CTexture *GetIconMinimize() const
	{
		return m_pIconMinimize.get();
	}
	CTexture *GetIconSettings() const
	{
		return m_pIconSettings.get();
	}
	CTexture *GetIconMenu() const
	{
		return m_pIconMenu.get();
	}
	CTexture *GetIconAdd() const
	{
		return m_pIconAdd.get();
	}
	CTexture *GetIconEdit() const
	{
		return m_pIconEdit.get();
	}
	CTexture *GetIconFolder() const
	{
		return m_pIconFolder.get();
	}

	// The carousel's view-mode flyout rows (see ui/carousel.cpp's DrawModeSwitcher).
	CTexture *GetIconGrid() const
	{
		return m_pIconGrid.get();
	}
	CTexture *GetIconList() const
	{
		return m_pIconList.get();
	}
	CTexture *GetIconCarousel() const
	{
		return m_pIconCarousel.get();
	}

	// The account edit form's "Visible in N games" chip (see ui/account_modal.cpp) -
	// replaced that chip's old hand-drawn grid glyph.
	CTexture *GetIconListArrow() const
	{
		return m_pIconListArrow.get();
	}

	// Password show/hide buttons (CAccountModal's edit form, CUnlockScreen's setup form)
	// - replaced their old hand-drawn ring+pupil glyph.
	CTexture *GetIconEyeVisible() const
	{
		return m_pIconEyeVisible.get();
	}
	CTexture *GetIconEyeHidden() const
	{
		return m_pIconEyeHidden.get();
	}

	// CTitleBar's Update button (see ui/title_bar.cpp) - replaced its old hand-drawn
	// download-arrow glyph.
	CTexture *GetIconUpdate() const
	{
		return m_pIconUpdate.get();
	}

	// Per-game icons (256x256 source) - handed to CCarousel as each banner's own icon,
	// which its List view (large) and CGameSelectPopup (small) both draw instead of the
	// color-square/cover-fit-crop fallbacks they'd otherwise use.
	CTexture *GetIconLeagueOfLegends() const
	{
		return m_pIconLeagueOfLegends.get();
	}
	CTexture *GetIconValorant() const
	{
		return m_pIconValorant.get();
	}
	CTexture *GetIconTwoXko() const
	{
		return m_pIconTwoXko.get();
	}
	CTexture *GetIconRuneterra() const
	{
		return m_pIconRuneterra.get();
	}
	CTexture *GetIconTeamfightTactics() const
	{
		return m_pIconTeamfightTactics.get();
	}

	CTexture *GetBannerLeagueOfLegends() const
	{
		return m_pBannerLeagueOfLegends.get();
	}
	CTexture *GetBannerValorant() const
	{
		return m_pBannerValorant.get();
	}
	CTexture *GetBannerTwoXko() const
	{
		return m_pBannerTwoXko.get();
	}
	CTexture *GetBannerRuneterra() const
	{
		return m_pBannerRuneterra.get();
	}
	CTexture *GetBannerTeamfightTactics() const
	{
		return m_pBannerTeamfightTactics.get();
	}

  private:
	std::unique_ptr<CTexture> m_pIconArrowBack;
	std::unique_ptr<CTexture> m_pIconClose;
	std::unique_ptr<CTexture> m_pIconMinimize;
	std::unique_ptr<CTexture> m_pIconSettings;
	std::unique_ptr<CTexture> m_pIconMenu;
	std::unique_ptr<CTexture> m_pIconAdd;
	std::unique_ptr<CTexture> m_pIconEdit;
	std::unique_ptr<CTexture> m_pIconFolder;
	std::unique_ptr<CTexture> m_pIconGrid;
	std::unique_ptr<CTexture> m_pIconList;
	std::unique_ptr<CTexture> m_pIconCarousel;
	std::unique_ptr<CTexture> m_pIconListArrow;
	std::unique_ptr<CTexture> m_pIconEyeVisible;
	std::unique_ptr<CTexture> m_pIconEyeHidden;
	std::unique_ptr<CTexture> m_pIconUpdate;

	std::unique_ptr<CTexture> m_pIconLeagueOfLegends;
	std::unique_ptr<CTexture> m_pIconValorant;
	std::unique_ptr<CTexture> m_pIconTwoXko;
	std::unique_ptr<CTexture> m_pIconRuneterra;
	std::unique_ptr<CTexture> m_pIconTeamfightTactics;

	std::unique_ptr<CTexture> m_pBannerLeagueOfLegends;
	std::unique_ptr<CTexture> m_pBannerValorant;
	std::unique_ptr<CTexture> m_pBannerTwoXko;
	std::unique_ptr<CTexture> m_pBannerRuneterra;
	std::unique_ptr<CTexture> m_pBannerTeamfightTactics;
};
