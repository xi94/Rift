#include "asset_manager.h"

#include <print>
#include <thread>
#include <vector>

#include "embeds/banners/2XKO.hpp"
#include "embeds/banners/LeagueOfLegends.hpp"
#include "embeds/banners/Runeterra.hpp"
#include "embeds/banners/TeamfightTactics.hpp"
#include "embeds/banners/Valorant.hpp"
#include "embeds/icons/2XKOIcon.hpp"
#include "embeds/icons/AddIcon.hpp"
#include "embeds/icons/ArrowBack.hpp"
#include "embeds/icons/CarouselIcon.hpp"
#include "embeds/icons/Close.hpp"
#include "embeds/icons/EditIcon.hpp"
#include "embeds/icons/EyeHiddenIcon.hpp"
#include "embeds/icons/EyeVisible.hpp"
#include "embeds/icons/FolderIcon.hpp"
#include "embeds/icons/GridIcon.hpp"
#include "embeds/icons/LeagueIcon.hpp"
#include "embeds/icons/ListArrow.hpp"
#include "embeds/icons/ListIcon.hpp"
#include "embeds/icons/MenuIcon.hpp"
#include "embeds/icons/Minimize.hpp"
#include "embeds/icons/RuneterraIcon.hpp"
#include "embeds/icons/Settings.hpp"
#include "embeds/icons/TFTIcon.hpp"
#include "embeds/icons/UpdateIcon.hpp"
#include "embeds/icons/ValorantIcon.hpp"

#include "stb/stb_image.h"

namespace {
// One decoded image, still CPU-side (a still stb-owned pixel buffer) - the output of the
// parallel decode phase in Load below, consumed by the serial GPU-upload phase right
// after it.
struct DecodedImage {
	unsigned char *pPixels = nullptr; // stbi_image_free's this; null on decode failure
	std::uint32_t Width = 0;
	std::uint32_t Height = 0;
};

// Decodes one embedded PNG/JPEG byte array via stb_image, always forced to RGBA8. Pure
// CPU work against this call's own independent input bytes and its own independent
// output buffer - no shared mutable state, which is exactly what makes it safe for
// Load to fan these out across threads below. One caveat: stb_image's failure-reason
// string (stbi_failure_reason(), read right below on a genuine failure) is a plain
// global in this project's stb_impl.cpp build, not a thread-local one - two decodes
// failing at the exact same instant could each log the other's message. Harmless (these
// are our own known-good embedded assets; a decode failure here means a build-time asset
// problem, not something expected in real use) and not worth a STBI_THREAD_LOCAL build
// change to fully close.
DecodedImage DecodeImage(const std::uint8_t *pBytes, std::uint64_t length, const char *pName)
{
	DecodedImage image;
	int width = 0;
	int height = 0;
	int sourceChannels = 0;
	image.pPixels = stbi_load_from_memory(pBytes, static_cast<int>(length), &width, &height, &sourceChannels, 4);
	if (image.pPixels == nullptr) {
		std::println("Failed to decode embedded asset '{}': {}", pName, stbi_failure_reason());
		return image;
	}
	image.Width = static_cast<std::uint32_t>(width);
	image.Height = static_cast<std::uint32_t>(height);
	return image;
}

// Uploads an already-decoded image as a GPU texture and frees stb's own copy of the
// pixels once it does. Deliberately never fanned out across threads the way DecodeImage
// above is: CRendererD3D11::CreateTexture mutates a shared, unsynchronized texture-pool
// free-list (see its own comment), so every upload has to stay serialized on the calling
// thread, same as every other renderer call in this project.
std::unique_ptr<CTexture> UploadTexture(IRenderer &renderer, const DecodedImage &image)
{
	if (image.pPixels == nullptr) {
		return nullptr;
	}
	auto pTexture = std::make_unique<CTexture>(renderer, image.pPixels, image.Width, image.Height);
	stbi_image_free(image.pPixels);
	return pTexture;
}

// One entry per embedded asset: the bytes to decode, a name for the failure log, and a
// pointer-to-member saying which CAssetManager field the finished texture belongs in -
// the single table both of Load's phases below walk, so decoding and uploading (and,
// previously, the two write-once-per-asset lines duplicating each field name) can never
// end up mismatched against each other the way two independently hand-maintained lists
// could.
struct AssetEntry {
	const std::uint8_t *pBytes;
	std::uint64_t Length;
	const char *pName;
	std::unique_ptr<CTexture> CAssetManager::*pMember;
};
} // namespace

bool CAssetManager::Load(IRenderer &renderer)
{
	using namespace kestrel::embed;

	const AssetEntry entries[]{
		{icon::arrow_back_icon.data(), icon::arrow_back_icon.size(), "ArrowBack", &CAssetManager::m_pIconArrowBack},
		{icon::close_icon.data(), icon::close_icon.size(), "Close", &CAssetManager::m_pIconClose},
		{icon::minimize_icon.data(), icon::minimize_icon.size(), "Minimize", &CAssetManager::m_pIconMinimize},
		{icon::settings_icon.data(), icon::settings_icon.size(), "Settings", &CAssetManager::m_pIconSettings},
		{icon::menu_icon.data(), icon::menu_icon.size(), "MenuIcon", &CAssetManager::m_pIconMenu},
		{icon::add_icon.data(), icon::add_icon.size(), "AddIcon", &CAssetManager::m_pIconAdd},
		{icon::edit_icon.data(), icon::edit_icon.size(), "EditIcon", &CAssetManager::m_pIconEdit},
		{icon::folder_icon.data(), icon::folder_icon.size(), "FolderIcon", &CAssetManager::m_pIconFolder},
		{icon::grid_icon.data(), icon::grid_icon.size(), "GridIcon", &CAssetManager::m_pIconGrid},
		{icon::list_icon.data(), icon::list_icon.size(), "ListIcon", &CAssetManager::m_pIconList},
		{icon::carousel_icon.data(), icon::carousel_icon.size(), "CarouselIcon", &CAssetManager::m_pIconCarousel},
		{icon::list_arrow.data(), icon::list_arrow.size(), "ListArrow", &CAssetManager::m_pIconListArrow},
		{icon::eye_visible_icon.data(), icon::eye_visible_icon.size(), "EyeVisible", &CAssetManager::m_pIconEyeVisible},
		{icon::eye_hidden_icon.data(), icon::eye_hidden_icon.size(), "EyeHiddenIcon", &CAssetManager::m_pIconEyeHidden},
		{icon::update_icon.data(), icon::update_icon.size(), "UpdateIcon", &CAssetManager::m_pIconUpdate},
		{icon::league_of_legends_icon.data(), icon::league_of_legends_icon.size(), "LeagueIcon",
		 &CAssetManager::m_pIconLeagueOfLegends},
		{icon::valorant_icon.data(), icon::valorant_icon.size(), "ValorantIcon", &CAssetManager::m_pIconValorant},
		{icon::two_xko_icon.data(), icon::two_xko_icon.size(), "2XKOIcon", &CAssetManager::m_pIconTwoXko},
		{icon::runeterra_icon.data(), icon::runeterra_icon.size(), "RuneterraIcon", &CAssetManager::m_pIconRuneterra},
		{icon::teamfight_tactics_icon.data(), icon::teamfight_tactics_icon.size(), "TFTIcon",
		 &CAssetManager::m_pIconTeamfightTactics},
		{banner::league_of_legends.data(), banner::league_of_legends.size(), "LeagueOfLegends",
		 &CAssetManager::m_pBannerLeagueOfLegends},
		{banner::valorant.data(), banner::valorant.size(), "Valorant", &CAssetManager::m_pBannerValorant},
		{banner::two_xko.data(), banner::two_xko.size(), "2XKO", &CAssetManager::m_pBannerTwoXko},
		{banner::runeterra.data(), banner::runeterra.size(), "Runeterra", &CAssetManager::m_pBannerRuneterra},
		{banner::teamfight_tactics.data(), banner::teamfight_tactics.size(), "TeamfightTactics",
		 &CAssetManager::m_pBannerTeamfightTactics},
	};
	const std::size_t entryCount = sizeof(entries) / sizeof(entries[0]);

	// Phase 1: decode every embedded asset in parallel, one thread per asset - see
	// DecodeImage's own comment for why that's safe. This is where essentially all of
	// Load's real cost lives (five full banner JPEGs plus close to twenty PNG icons, all
	// decoded one at a time before this); a plain one-thread-per-asset fan-out is enough
	// to make a real difference here without needing a persistent thread pool for what's
	// otherwise a one-shot startup cost.
	std::vector<DecodedImage> decoded(entryCount);
	{
		std::vector<std::thread> workers;
		workers.reserve(entryCount);
		for (std::size_t i = 0; i < entryCount; i += 1) {
			workers.emplace_back([&decoded, &entries, i]() {
				decoded[i] = DecodeImage(entries[i].pBytes, entries[i].Length, entries[i].pName);
			});
		}
		for (std::thread &worker : workers) {
			worker.join();
		}
	}

	// Phase 2: upload each decoded image - serialized on this thread, see UploadTexture's
	// own comment for why.
	bool allSucceeded = true;
	for (std::size_t i = 0; i < entryCount; i += 1) {
		this->*entries[i].pMember = UploadTexture(renderer, decoded[i]);
		allSucceeded = allSucceeded && (this->*entries[i].pMember != nullptr);
	}
	return allSucceeded;
}
