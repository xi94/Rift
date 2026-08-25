#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <print>

#include <Windows.h>
#include <shellapi.h>
#include <sodium.h>

#include "asset_manager.h"
#include "core/animator.h"
#include "core/crash_handler.h"
#include "core/debug_log.h"
#include "core/master_key.h"
#include "core/memory_arena.h"
#include "core/storage.h"
#include "core/ui_automation.h"
#include "core/updater.h"
#include "core/version.h"
#include "font_manager.h"
#include "gfx/d3d11/renderer_d3d11.h"
#include "gfx/texture.h"
#include "tray.h"
#include "ui/account_modal.h"
#include "ui/carousel.h"
#include "ui/context_menu.h"
#include "ui/draw_list.h"
#include "ui/settings.h"
#include "ui/settings_menu.h"
#include "ui/settings_panel.h"
#include "ui/text.h"
#include "ui/title_bar.h"
#include "ui/unlock_screen.h"
#include "ui/update_overlay.h"
#include "ui/widget_stack.h"
#include "window.h"

namespace {
// Starts the diagnostic log on construction and stops it on destruction. Declared as the very
// first local in main() so that, by C++'s reverse-declaration-order teardown, it is the very
// last one destroyed - which means every other local's destructor still has somewhere to log.
// That matters specifically for CLoginAttempt's own destructor: a worker still wedged inside a
// UI Automation call at shutdown is one of the two ways this app has actually been observed to
// freeze, and a plain DebugLog::Shutdown() before `return 0` would run before that destructor
// and lose the only line that would have said so.
struct SDebugLogSession {
	SDebugLogSession()
	{
		DebugLog::Init();
	}

	~SDebugLogSession()
	{
		DebugLog::Shutdown();
	}

	SDebugLogSession(const SDebugLogSession &) = delete;
	SDebugLogSession &operator=(const SDebugLogSession &) = delete;
};

// One long-lived reservation for the whole process, used for exactly one thing:
// CDrawList's per-frame vertex/index scratch - see core/memory_arena.h's own file
// comment for why this fork keeps that one bump-allocation case and nothing else.
constexpr std::uint64_t kPersistentArenaCapacity = 64ull * 1024 * 1024;
constexpr std::uint32_t kDrawListVertexCapacity = 1 << 16;
constexpr std::uint32_t kDrawListIndexCapacity = (1 << 16) * 3 / 2;

constexpr ColorF kColorBackground{18.0f / 255.0f, 18.0f / 255.0f, 20.0f / 255.0f, 1.0f};
// A thin, slightly-lighter-than-chrome seam between the title bar/status bar and the
// content area - a color-only fix would need a much bigger, uglier brightness jump to
// read as clearly as a 1px line does.
constexpr Color kColorChromeSeam{46, 46, 50, 255};

constexpr std::uint32_t kContextMenuIdOpen = 1;
constexpr std::uint32_t kContextMenuIdLogin = 2;
constexpr std::uint32_t kContextMenuIdCopyPassword = 3;
constexpr std::uint32_t kContextMenuIdAddAccount = 4;
constexpr std::uint32_t kContextMenuIdEditAccount = 5;
constexpr std::uint32_t kContextMenuIdDeleteAccount = 6;

// accounts.bin and settings.bin are independent files (see core/storage.h's own file
// comment for why) with independent atomic writes - SaveAccountsNow/SaveSettingsNow let a
// call site touch only the one that actually changed (e.g. AdjustZoomStop below never
// needs to re-encrypt the whole account list), while SaveNow/LoadNow remain the "do both"
// convenience wrappers for the many call sites that don't have a good reason to be more
// surgical (matching this project's own established "a little more liberal than strictly
// necessary, but simpler" save philosophy - see the comment further down in the main
// loop). CCarousel owns the live CBanner array itself (see ui/carousel.h), so the array
// base is just its first banner's address; CStorage has no reason to know that class
// exists, hence the pointer+count pair instead of a CCarousel&.
void SaveAccountsNow(CCarousel &carousel, const CSettings &settings, const CMasterKey &masterKey)
{
	CStorage::SaveAccounts(&carousel.GetBanner(0), carousel.GetBannerCount(), settings.m_bMasterPasswordEnabled,
						   masterKey);
}

void SaveSettingsNow(const CCarousel &carousel, const CSettings &settings)
{
	CStorage::SaveSettings(settings, carousel.GetZoomStop(), carousel.GetSelectedIndex());
}

void SaveNow(CCarousel &carousel, const CSettings &settings, const CMasterKey &masterKey)
{
	SaveAccountsNow(carousel, settings, masterKey);
	SaveSettingsNow(carousel, settings);
}

// Loading accounts needs to know settings.m_bMasterPasswordEnabled (to know whether the
// persisted passwords are GCM-encrypted at all), so settings must always be loaded first
// - LoadNow below is the only correctly-ordered way to load both from a cold start.
EStorageLoadResult LoadAccountsNow(CCarousel &carousel, const CSettings &settings, const CMasterKey &masterKey)
{
	return CStorage::LoadAccounts(&carousel.GetBanner(0), carousel.GetBannerCount(), settings.m_bMasterPasswordEnabled,
								  masterKey);
}

EStorageLoadResult LoadSettingsNow(CCarousel &carousel, CSettings &settings)
{
	std::int32_t zoomStop = 0;
	std::int32_t selectedBanner = 0;
	const EStorageLoadResult result = CStorage::LoadSettings(settings, zoomStop, selectedBanner);
	carousel.ApplyZoomStop(zoomStop);
	carousel.ApplySelectedIndex(selectedBanner);
	return result;
}

EStorageLoadResult LoadNow(CCarousel &carousel, CSettings &settings, const CMasterKey &masterKey)
{
	LoadSettingsNow(carousel, settings);
	return LoadAccountsNow(carousel, settings, masterKey);
}

// Copies an account's password to the system clipboard as plain Unicode text - the
// tray menu's and carousel context menu's "Copy Password" action.
void CopyPasswordToClipboard(HWND owner, const char *pPassword)
{
	if (!OpenClipboard(owner)) {
		return;
	}
	EmptyClipboard();

	const auto passwordLength = static_cast<int>(std::strlen(pPassword));
	const int wideLength = MultiByteToWideChar(CP_UTF8, 0, pPassword, passwordLength, nullptr, 0);
	const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (static_cast<SIZE_T>(wideLength) + 1) * sizeof(wchar_t));
	if (memory != nullptr) {
		auto *pDestination = static_cast<wchar_t *>(GlobalLock(memory));
		if (pDestination != nullptr) {
			if (wideLength > 0) {
				MultiByteToWideChar(CP_UTF8, 0, pPassword, passwordLength, pDestination, wideLength);
			}
			pDestination[wideLength] = L'\0';
			GlobalUnlock(memory);
			SetClipboardData(CF_UNICODETEXT, memory);
		}
	}

	CloseClipboard();
}

// Called synchronously from inside CTray's context-menu handler (a right-click on the
// icon) - see tray.h's TrayAccountListCallback type for why this has to be a callback
// rather than something polled once per frame. Named distinctly from that type alias
// (not TrayAccountListCallback) so the two don't collide as identifiers.
std::uint32_t HandleTrayAccountListRequest(void *pUserData, TrayAccountItem *pOutItems, std::uint32_t capacity)
{
	const auto *pCarousel = static_cast<const CCarousel *>(pUserData);

	std::uint32_t written = 0;
	for (std::uint32_t b = 0; b < pCarousel->GetBannerCount() && written < capacity; b += 1) {
		const CBanner &banner = pCarousel->GetBanner(b);
		for (std::uint32_t a = 0; a < banner.AccountCount && written < capacity; a += 1) {
			TrayAccountItem &item = pOutItems[written];
			StringViewCopyToFixed(item.BannerTitle, sizeof(item.BannerTitle), banner.Title);
			StringViewCopyToFixed(item.Username, sizeof(item.Username), banner.Accounts[a].GetUsername());
			item.BannerIndex = static_cast<std::int32_t>(b);
			item.AccountIndex = static_cast<std::int32_t>(a);
			written += 1;
		}
	}
	return written;
}

// Submits one already-Finish()'d CDrawList's batched commands to the renderer, one
// IRenderer::Draw2D*/Textured/BannerGlow/ColorPickerSv call per command, in the order
// CDrawList closed them - preserving on-screen layering exactly as issued.
void SubmitDrawList(IRenderer &renderer, const CDrawList &drawList)
{
	for (std::uint32_t i = 0; i < drawList.GetCommandCount(); i += 1) {
		const DrawCommand &command = drawList.GetCommands()[i];
		renderer.SetClipRect(command.HasClip ? ClipRect{true, command.ClipRect} : ClipRect{false, Rect{}});

		const std::uint32_t *pIndices = drawList.GetIndices() + command.IndexOffset;
		switch (command.Kind) {
			case EDrawCommandKind::DRAW_COMMAND_KIND_SOLID:
				renderer.Draw2D(drawList.GetVertices(), drawList.GetVertexCount(), pIndices, command.IndexCount);
				break;
			case EDrawCommandKind::DRAW_COMMAND_KIND_TEXTURED:
				renderer.Draw2DTextured(command.pTexture != nullptr ? command.pTexture->GetHandle() : nullptr,
										drawList.GetVertices(), drawList.GetVertexCount(), pIndices,
										command.IndexCount);
				break;
			case EDrawCommandKind::DRAW_COMMAND_KIND_BANNER_GLOW:
				renderer.Draw2DBannerGlow(drawList.GetVertices(), drawList.GetVertexCount(), pIndices,
										  command.IndexCount, command.Glow.QuadWidth, command.Glow.QuadHeight,
										  command.Glow.CornerRadius, command.Glow.RingWidth);
				break;
			case EDrawCommandKind::DRAW_COMMAND_KIND_COLOR_PICKER_SV:
				renderer.Draw2DColorPickerSv(drawList.GetVertices(), drawList.GetVertexCount(), pIndices,
											 command.IndexCount);
				break;
			case EDrawCommandKind::DRAW_COMMAND_KIND_CIRCULAR_PROGRESS:
				renderer.Draw2DCircularProgress(drawList.GetVertices(), drawList.GetVertexCount(), pIndices,
												command.IndexCount, command.Progress.QuadWidth,
												command.Progress.QuadHeight, command.Progress.OuterRadius,
												command.Progress.InnerRadius, command.Progress.StartAngle,
												command.Progress.SweepAngle, command.Progress.GlowStrength);
				break;
		}
	}
}

// Everything a frame's render needs, gathered in one place so both the steady-state
// main loop and the live-resize callback (CWindow's ResizeCallback fires synchronously
// from WM_SIZE while the user drags a border, blocking the thread the frame loop runs
// on - the same "Win32 leaves no alternative" situation as ever) can produce the exact
// same pixel-for-pixel frame.
struct RenderContext {
	CWindow *pWindow;
	IRenderer *pRenderer;
	CDrawList *pDrawList;
	CWidgetStack *pStack;
	CCarousel *pCarousel; // for the status bar's own view-mode text - see RenderFrame
	CFontManager *pFonts; // for the status bar's own version text - see DrawStatusBarVersion
};

// The app's own version (core/version.h - not a hand-typed copy that could drift), left-
// aligned in the bottom status bar - moved here from CSettingsMenu's own footer (see that
// class's own file comment) so it's visible without opening the menu at all, the same
// "always there, not tucked behind a click" instinct CCarousel's own view-mode indicator on
// the opposite (right) side of this same bar already follows.
void DrawStatusBarVersion(CDrawList &drawList, CFontManager &fonts, float statusBarY, float statusBarHeight)
{
	const CFont &secondary = fonts.GetSecondary();
	char versionBuffer[48];
	const int written =
		std::snprintf(versionBuffer, sizeof(versionBuffer), "Rift v%s%s", kAppVersion, kIsDebugBuild ? " [dev]" : "");
	const CStringView versionText{versionBuffer, written > 0 ? static_cast<std::uint64_t>(written) : 0};

	constexpr float kStatusBarVersionPadding = 14.0f;
	// Same baseline-centering formula (including the empirical nudge) as
	// CCarousel::DrawStatusBarContent's own comment documents, and the same dim status-bar
	// text color, so both ends of this bar read as one consistent strip rather than two
	// independently-tuned pieces of text.
	constexpr float kBaselineVisualNudge = 2.0f;
	const float baselineY = statusBarY + statusBarHeight * 0.5f +
							 (secondary.GetAscent() + secondary.GetDescent()) * 0.5f - kBaselineVisualNudge;
	DrawText(drawList, secondary, kStatusBarVersionPadding, baselineY, versionText, Color{158, 158, 166, 255});
}

void RenderFrame(RenderContext &context)
{
	  CWindow &window = *context.pWindow;
	  const float width = static_cast<float>(window.GetWidth());
	  const float height = static_cast<float>(window.GetHeight());

	  context.pDrawList->Clear();

	  // The title bar/status bar chrome - including the version number (DrawStatusBarVersion)
	  // - stays visible even on the master-password screen now, not just once unlocked: only
	  // the carousel's own content (its cards, and the view-mode text this same bar's right
	  // side shows - see DrawStatusBarContent) actually waits on m_bVisible, which main.cpp
	  // keeps in sync with the unlock state independently of this chrome.
	  context.pDrawList->AddRectFilled(0.0f, kTitleBarHeight, width, 1.0f, kColorChromeSeam);
	  context.pDrawList->AddRectFilled(0.0f, height - kStatusBarHeight - 1.0f, width, 1.0f, kColorChromeSeam);
	  context.pDrawList->AddRectFilled(0.0f, height - kStatusBarHeight, width, kStatusBarHeight, kTitleBarColor);
	  DrawStatusBarVersion(*context.pDrawList, *context.pFonts, height - kStatusBarHeight, kStatusBarHeight);
	  if (context.pCarousel->m_bVisible) {
		    context.pCarousel->DrawStatusBarContent(*context.pDrawList);
	  }

	  context.pStack->Draw(*context.pDrawList);
	  context.pDrawList->Finish();

	  context.pRenderer->BeginFrame();
	  context.pRenderer->Clear(kColorBackground);
	  SubmitDrawList(*context.pRenderer, *context.pDrawList);
	  context.pRenderer->EndFrame();
}

// Called synchronously from WM_SIZE while a border drag is in progress.
void OnWindowResize(void *pUserData)
{
    auto *pContext = static_cast<RenderContext *>(pUserData);
    // hmmmmm
    pContext->pRenderer->Resize(pContext->pWindow->GetPhysicalWidth(), pContext->pWindow->GetPhysicalHeight(), static_cast<float>(pContext->pWindow->GetWidth()), static_cast<float>(pContext->pWindow->GetHeight()));
	  RenderFrame(*pContext);
}

// Bundles the extra state OnWindowDpiChanged needs beyond RenderContext - the font
// re-bake needs CFontManager/IRenderer/CSettings, none of which RenderContext itself
// carries (it only needs enough to redraw, not enough to re-bake a font atlas).
struct DpiChangeContext {
	CWindow *pWindow;
	IRenderer *pRenderer;
	CFontManager *pFonts;
	const CSettings *pSettings;
};

// Called synchronously from WM_DPICHANGED, before the SetWindowPos that message
// triggers repaints the window at its new physical size - re-bakes both font atlases
// at the new monitor's scale first so that repaint never samples a stale-resolution
// atlas.
void OnWindowDpiChanged(void *pUserData)
{
	auto *pContext = static_cast<DpiChangeContext *>(pUserData);
	pContext->pFonts->ApplyBody(*pContext->pRenderer, StringViewFromCString(pContext->pSettings->m_szFontName),
								pContext->pSettings->m_flFontPixelSize, pContext->pSettings->m_flSecondaryFontPixelSize,
								pContext->pWindow->GetDpiScale());
}
} // namespace

int main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

	// Before literally anything else, including InstallCrashHandler below - see
	// CUpdater::RunStartupRecoveryAndMaybeExit's own comment. A true return means this
	// process instance has nothing further to do (it either just relaunched a repaired copy
	// of itself, or there's nothing for it to do at all) - main() ends here, full stop.
	if (CUpdater::RunStartupRecoveryAndMaybeExit()) {
		  return 0;
	}

	// Before literally anything else - see crash_handler.h's own file comment. Every mechanism
	// it installs is process-wide, so this one call covers every thread this process ever
	// creates (including CLoginAttempt's own worker threads), not just this one.
	InstallCrashHandler();

	// Right after the crash handler and before anything that could hang: this is what turns
	// "it froze" into a log naming the exact call that never returned, plus a minidump of every
	// thread taken while it is still stuck. On by default in a Debug build, and switched on in
	// any build by setting the RIFT_DEBUG_LOG environment variable - see core/debug_log.h.
	const SDebugLogSession debugLogSession;
	if (DebugLog::IsEnabled() && DebugLog::GetFilePath()[0] != '\0') {
		std::println("Diagnostic log: {}", DebugLog::GetFilePath());
	}

	// Before the first login attempt ever creates one - holds this process's multi-threaded
	// apartment open so the per-attempt CoInitializeEx/CoUninitialize pairs stop building and
	// tearing down the whole apartment (and UI Automation's state inside it) every time. See
	// CUiAutomation::KeepProcessMtaAlive's own comment; safe here because it joins no apartment
	// itself, so the render thread stays apartment-free.
	CUiAutomation::KeepProcessMtaAlive();

	// Must succeed before any CCrypto/CMasterKey/CStorage call - libsodium picks its own
	// fastest-available implementation of every primitive at runtime (AES-NI vs a portable
	// fallback, etc.) here, once, rather than on first use. Safe to call exactly once, this
	// early, before any worker thread exists (see sodium_init's own docs: it's not safe to
	// call concurrently with any other libsodium function, only with itself).
	if (sodium_init() < 0) {
	  	std::println("Failed to initialize libsodium.");
		  return 1;
	}

	CMemoryArena persistentArena;
	if (!persistentArena.Init(kPersistentArenaCapacity)) {
		  std::println("Failed to reserve persistent arena.");
		  return 1;
	}

	// Read just the window size ahead of everything else settings-related below (CSettings
	// itself, the real LoadNow call) - those all need pCarousel, which doesn't exist yet,
	// but window.Create() needs a size right now. Falls back to CSettings's own defaults
	// (a no-op CStorage::LoadSettings, e.g. first run) rather than the old hardcoded
	// 1042x675 literal.
	CSettings bootSettings;
	std::int32_t bootZoomStopUnused = 0;
	std::int32_t bootSelectedBannerUnused = 0;
	CStorage::LoadSettings(bootSettings, bootZoomStopUnused, bootSelectedBannerUnused);

	CWindow window;
	if (!window.Create(L"Rift", bootSettings.m_nWindowWidth, bootSettings.m_nWindowHeight)) {
		  std::println("Failed to create window.");
		  return 1;
	}

	CRendererD3D11 renderer;
	const RendererConfig rendererConfig{window.GetHandle(), window.GetPhysicalWidth(), window.GetPhysicalHeight(),
										static_cast<float>(window.GetWidth()), static_cast<float>(window.GetHeight())};
	if (!renderer.Init(rendererConfig)) {
		  std::println("Failed to initialize renderer.");
		  return 1;
	}

	CAssetManager assets;
	if (!assets.Load(renderer)) {
		  std::println("Failed to load one or more embedded assets.");
		  return 1;
	}

	CFontManager fonts;
	if (!fonts.Load(renderer, window.GetDpiScale())) {
		  std::println("Failed to load the UI font.");
		  return 1;
	}

	CTray tray;
	if (!tray.Create(L"Rift")) {
		  std::println("Failed to create tray icon.");
	}

	CDrawList drawList;
	drawList.Init(persistentArena, kDrawListVertexCapacity, kDrawListIndexCapacity);

	CWidgetStack stack;

	// Every CWidget subclass is heap-allocated and owned by the stack (per
	// FORK_WITH_CLASSES.md section 6's memory-strategy recommendation - the widget tree
	// is a one-time startup cost, not a per-frame one, so ordinary unique_ptr ownership is
	// the right call here) - a raw pointer captured right after construction stays valid
	// for the stack's whole lifetime (moving a unique_ptr into the stack's vector doesn't
	// relocate the object it owns), so main() keeps typed access to call each widget's
	// own methods alongside the generic CWidget interface CWidgetStack itself uses.
	auto pCarouselOwned = std::make_unique<CCarousel>(fonts, assets);
	CCarousel *pCarousel = pCarouselOwned.get();

	// Demo accounts until real persistence overwrites them below - fixed per-title brand
	// colors, not user-customizable (the global Accent Color setting only ever reaches
	// genuinely global chrome, never a specific game's own identity).
	pCarousel->AddBanner(StringViewFromCString("League of Legends"), assets.GetBannerLeagueOfLegends(),
						 assets.GetIconLeagueOfLegends(), Color{210, 175, 55, 255}); // golden yellow
	pCarousel->AddBanner(StringViewFromCString("Teamfight Tactics"), assets.GetBannerTeamfightTactics(),
						 assets.GetIconTeamfightTactics(), Color{70, 140, 190, 255}); // teal-blue
	pCarousel->AddBanner(StringViewFromCString("Valorant"), assets.GetBannerValorant(), assets.GetIconValorant(),
						 Color{210, 55, 60, 255}); // red
	pCarousel->AddBanner(StringViewFromCString("2XKO"), assets.GetBannerTwoXko(), assets.GetIconTwoXko(),
						 Color{45, 205, 210, 255}); // cyan
	pCarousel->AddBanner(StringViewFromCString("Legends of Runeterra"), assets.GetBannerRuneterra(),
						 assets.GetIconRuneterra(), Color{140, 90, 200, 255}); // purple

	// pCarousel's address is stable for the rest of main's scope (the stack owns it, and
	// nothing removes it before shutdown); the callback itself only runs later,
	// synchronously from a right-click, by which point LoadNow below has already updated
	// the account lists it reads.
	tray.SetAccountListCallback(HandleTrayAccountListRequest, pCarousel);

	CSettings settings;
	CMasterKey masterKey;
	masterKey.Init();

	// One check, right at startup, full stop - no delay, no periodic recheck. The worker
	// thread this spawns does the actual network I/O (see core/updater.h's own file
	// comment), so this doesn't block window creation/asset loading below it.
	CUpdater updater;
	updater.Init();
	updater.CheckForUpdateAsync(kAppVersion);

	// Overwrites the demo accounts seeded above (for banners with a saved account list)
	// and the settings defaults from %LOCALAPPDATA%\Rift\accounts.bin, if it
	// exists. No file yet (first run) just leaves the in-memory defaults as-is.
	// STORAGE_LOAD_LOCKED (a master password is set but hasn't been unlocked this
	// session) still applies everything except account passwords, which Load already
	// left blank - the user unlocks later via CUnlockScreen.
	const EStorageLoadResult loadResult = LoadNow(*pCarousel, settings, masterKey);
	if (loadResult == EStorageLoadResult::STORAGE_LOAD_OK || loadResult == EStorageLoadResult::STORAGE_LOAD_LOCKED) {
		CAnimator::SetEnabled(settings.m_bAnimationsEnabled);
		CAnimator::SetSpeed(settings.m_flAnimationSpeed);
		CDrawList::SetRoundedCornersEnabled(settings.m_bRoundedCornersEnabled);
		fonts.ApplyBody(renderer, StringViewFromCString(settings.m_szFontName), settings.m_flFontPixelSize,
						settings.m_flSecondaryFontPixelSize, window.GetDpiScale());
	}

	// The master password is mandatory now (see ui/unlock_screen.h's own file comment) -
	// either it's never been set at all (a genuine first run, or an existing install
	// updating from a version where it was still optional - !m_bMasterPasswordEnabled
	// covers both identically), which forces setup mode, or one was set but hasn't been
	// unlocked this session (STORAGE_LOAD_LOCKED), which forces the ordinary unlock prompt.
	// Computed here, before any widget construction (rather than after, the way this used to
	// read), since CSettingsMenu needs a live reference to this exact local - see its own
	// constructor comment on why the menu itself stays reachable on the master-password
	// screen now, unlike CSettingsPanel/CContextMenu, which still don't (see the push order
	// below).
	bool appLocked = !settings.m_bMasterPasswordEnabled || loadResult == EStorageLoadResult::STORAGE_LOAD_LOCKED;
	pCarousel->m_bVisible = !appLocked;

	auto pModalOwned = std::make_unique<CAccountModal>(fonts, *pCarousel, window, settings, assets);
	CAccountModal *pModal = pModalOwned.get();

	auto pSettingsMenuOwned = std::make_unique<CSettingsMenu>(fonts, assets, appLocked);
	CSettingsMenu *pSettingsMenu = pSettingsMenuOwned.get();

	auto pSettingsPanelOwned = std::make_unique<CSettingsPanel>(fonts, settings, window, renderer);
	CSettingsPanel *pSettingsPanel = pSettingsPanelOwned.get();

	auto pContextMenuOwned = std::make_unique<CContextMenu>(fonts);
	CContextMenu *pContextMenu = pContextMenuOwned.get();

	auto pUnlockScreenOwned = std::make_unique<CUnlockScreen>(fonts, window, settings, masterKey, assets);
	CUnlockScreen *pUnlockScreen = pUnlockScreenOwned.get();
	if (!settings.m_bMasterPasswordEnabled) {
		pUnlockScreen->ActivateForSetup();
	} else if (loadResult == EStorageLoadResult::STORAGE_LOAD_LOCKED) {
		pUnlockScreen->ActivateForUnlock();
	}

	auto pUpdateOverlayOwned = std::make_unique<CUpdateOverlay>(fonts, window, settings, updater);
	CUpdateOverlay *pUpdateOverlay = pUpdateOverlayOwned.get();

	auto pTitleBarOwned = std::make_unique<CTitleBar>(window, assets, updater, fonts);
	CTitleBar *pTitleBar = pTitleBarOwned.get();

	// Push order is z-order, bottom to top - matching the original's own layering
	// (Carousel < Modal < Settings_Panel < Context_Menu). CUnlockScreen goes on top of
	// Modal/SettingsPanel/ContextMenu: while active it consumes every input event
	// unconditionally, so nothing below it is reachable - see unlock_screen.h's own file
	// comment for why this needs no special-casing here the way the original's app_locked
	// branch did. CSettingsMenu and CUpdateOverlay both sit above even CUnlockScreen,
	// unlike every dialog below it - CTitleBar (which opens both, and dispatches above
	// everything else regardless of lock state) reaches them the same way whether the vault
	// is locked or not, so neither should require unlocking first (CSettingsMenu's own
	// Settings row still gates CSettingsPanel specifically - see that class's own
	// constructor comment). CTitleBar is the one alwaysTopmost entry - it must always
	// render on top and always receive input regardless of any dialog's blocking state.
	stack.Push(std::move(pCarouselOwned));
	stack.Push(std::move(pModalOwned));
	stack.Push(std::move(pSettingsPanelOwned));
	stack.Push(std::move(pContextMenuOwned));
	stack.Push(std::move(pUnlockScreenOwned));
	stack.Push(std::move(pSettingsMenuOwned));
	stack.Push(std::move(pUpdateOverlayOwned));
	stack.Push(std::move(pTitleBarOwned), true);

	float mouseX = -1.0f;
	float mouseY = -1.0f;

	// Feeds the banner-glow shader's shimmer (see the main-loop's own
	// renderer.SetEffectTime call) - computed once per main-loop iteration below.
	const auto appStartTime = std::chrono::steady_clock::now();

	// SetWindowDisplayAffinity's own last-applied state (see the main loop's own call) -
	// tracked so that call only actually happens on a real transition, not every single
	// frame regardless of whether anything changed.
	bool bWindowExcludedFromCapture = false;

	// Two right-click targets, disambiguated by which is currently >= 0 (opening one
	// clears the other) rather than encoding the target into the context-menu item id
	// itself - set whenever a right-click opens CContextMenu, read back once a selection
	// comes in.
	std::int32_t contextMenuTargetBannerIndex = -1;
	std::int32_t contextMenuTargetAccountIndex = -1;

	RenderContext renderContext{&window, &renderer, &drawList, &stack, pCarousel, &fonts};
	window.SetResizeCallback(OnWindowResize, &renderContext);

	DpiChangeContext dpiContext{&window, &renderer, &fonts, &settings};
	window.SetDpiChangedCallback(OnWindowDpiChanged, &dpiContext);

	// One real frame, drawn and presented while the window is still hidden, so Show()
	// reveals actual UI instead of whatever the swapchain's backbuffer started with - see
	// CWindow::Create's own comment for why this has to happen in this order.
	RenderFrame(renderContext);
	window.Show();

	auto previousTime = std::chrono::steady_clock::now();

	while (!window.ShouldClose()) {
		window.PumpMessages();

		const ETrayEventType trayEvent = tray.TakeEvent();
		if (trayEvent == ETrayEventType::TRAY_EVENT_EXIT_REQUESTED) {
			PostMessageW(window.GetHandle(), WM_CLOSE, 0, 0);
		} else if (trayEvent == ETrayEventType::TRAY_EVENT_SHOW_WINDOW) {
			ShowWindow(window.GetHandle(), SW_RESTORE);
			SetForegroundWindow(window.GetHandle());
		} else if (trayEvent == ETrayEventType::TRAY_EVENT_QUICK_LOGIN ||
				   trayEvent == ETrayEventType::TRAY_EVENT_COPY_PASSWORD) {
			const std::int32_t bannerIndex = tray.GetPendingBannerIndex();
			const std::int32_t accountIndex = tray.GetPendingAccountIndex();
			const bool valid = bannerIndex >= 0 &&
							   static_cast<std::uint32_t>(bannerIndex) < pCarousel->GetBannerCount() &&
							   accountIndex >= 0 &&
							   static_cast<std::uint32_t>(accountIndex) <
								   pCarousel->GetBanner(static_cast<std::uint32_t>(bannerIndex)).AccountCount;
			if (valid && trayEvent == ETrayEventType::TRAY_EVENT_QUICK_LOGIN) {
				ShowWindow(window.GetHandle(), SW_RESTORE);
				SetForegroundWindow(window.GetHandle());
				pModal->OpenForQuickLogin(bannerIndex, accountIndex);
			} else if (valid) {
				const CBanner &banner = pCarousel->GetBanner(static_cast<std::uint32_t>(bannerIndex));
				CopyPasswordToClipboard(window.GetHandle(), banner.Accounts[accountIndex].m_szPassword);
			}
		}

		const float width = static_cast<float>(window.GetWidth());
		const float height = static_cast<float>(window.GetHeight());

		// Kept live so whatever SaveSettingsNow/SaveNow call happens next persists the
		// current size - guarded against 0 since a minimized window reports a zero client
		// size (WM_SIZE), which would otherwise overwrite the real saved size with garbage.
		if (window.GetWidth() > 0 && window.GetHeight() > 0) {
			settings.m_nWindowWidth = window.GetWidth();
			settings.m_nWindowHeight = window.GetHeight();
		}

		// CCarousel is the one widget whose whole layout derives from an externally-set
		// m_vecBounds (see carousel.h's own file comment) - every other widget queries its
		// own CWindow& reference directly, so nothing else needs this treatment.
		pCarousel->m_vecBounds = Rect{0.0f, kTitleBarHeight, width, height - kTitleBarHeight - kStatusBarHeight};

		for (std::uint32_t i = 0; i < window.GetInputEventCount(); i += 1) {
			const InputEvent &event = window.GetInputEvents()[i];

			if (event.Type == EInputEventType::INPUT_EVENT_MOUSE_MOVE) {
				mouseX = event.X;
				mouseY = event.Y;
			}
			stack.SetRealMousePosition(mouseX, mouseY);

			bool consumed = false;
			switch (event.Type) {
				case EInputEventType::INPUT_EVENT_MOUSE_DOWN:
					consumed = stack.DispatchPointerDown(event.X, event.Y);
					break;

				case EInputEventType::INPUT_EVENT_MOUSE_MOVE:
					consumed = stack.DispatchPointerMove(event.X, event.Y);
					break;

				case EInputEventType::INPUT_EVENT_MOUSE_UP: {
					// "Already-centered card opens the modal" needs the selection as it
					// stood *before* this click, since a click on a different card just
					// re-centers it rather than opening anything - captured before
					// dispatch runs (dispatch may itself change the selection).
					const std::int32_t previouslySelectedBanner = pCarousel->GetSelectedIndex();
					consumed = stack.DispatchPointerUp(event.X, event.Y);

					// ConsumePendingClick is only ever non-PENDING_HIT_KIND_NONE immediately
					// after this exact dispatch (CCarousel::OnPointerUp is the only place
					// that sets it), so polling it here - rather than unconditionally every
					// event like the right-click Consume*s below - keeps the cause and the
					// effect next to each other.
					const PendingHit clickedBanner = pCarousel->ConsumePendingClick();
					const bool openImmediately =
						pCarousel->GetViewMode() != ECarouselViewMode::CAROUSEL_VIEW_MODE_CAROUSEL;

					if (clickedBanner.Kind == EPendingHitKind::PENDING_HIT_KIND_INDEX &&
						(openImmediately || clickedBanner.Index == previouslySelectedBanner)) {
						pModal->Open(clickedBanner.Index);
					}

					break;
				}

				case EInputEventType::INPUT_EVENT_RIGHT_MOUSE_UP:
					consumed = stack.DispatchRightPointerUp(event.X, event.Y);
					break;

				case EInputEventType::INPUT_EVENT_MOUSE_WHEEL: {
					// Ctrl+scroll cycles the carousel's view mode (File Pilot's own view-
					// mode slider is the reference) - the same modifier this project uses
					// nowhere else, so there's no conflict to resolve. Only live when
					// nothing above the carousel is currently blocking, matching the
					// original's own "Ctrl+scroll only does this while the carousel is
					// actually the foreground surface" behavior.
					const bool ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
					const bool anythingAboveCarouselBlocking =
						pModal->IsBlocking() || pSettingsMenu->IsBlocking() || pSettingsPanel->IsBlocking() ||
						pContextMenu->IsBlocking() || pUnlockScreen->IsBlocking();

					if (ctrlDown && !anythingAboveCarouselBlocking) {
						pCarousel->AdjustZoomStop(event.WheelDelta);
						// Zoom stop lives in settings.bin, not accounts.bin - narrowing to
						// SaveSettingsNow means a Ctrl+scroll notch (which can fire many
						// times in a row) never re-encrypts/rewrites the account list.
						SaveSettingsNow(*pCarousel, settings);
						consumed = true;
					} else {
						consumed = stack.DispatchScroll(event.X, event.Y, event.WheelDelta);
					}

					break;
				}
				case EInputEventType::INPUT_EVENT_KEY_DOWN:
					consumed = stack.DispatchKeyDown(event.KeyCode);
					break;

				case EInputEventType::INPUT_EVENT_CHAR_TYPED:
					consumed = stack.DispatchChar(event.KeyCode);
					break;
			}

			// Title bar Menu button toggles CSettingsMenu; CSettingsMenu's own row clicks
			// open CSettingsPanel or request the app close. Cheap to poll every event
			// (each Consume* returns a "nothing happened" sentinel almost always).
			if (pTitleBar->ConsumeMenuClicked()) {
				if (pSettingsMenu->IsBlocking()) {
					pSettingsMenu->Close();
				} else {
					pSettingsMenu->Open();
				}
			}

			if (pTitleBar->ConsumeUpdateClicked()) {
				if (pUpdateOverlay->IsBlocking()) {
					pUpdateOverlay->Close();
				} else {
					pUpdateOverlay->Open();
				}
			}

			const ESettingsMenuAction settingsMenuAction = pSettingsMenu->ConsumeAction();
			if (settingsMenuAction == ESettingsMenuAction::SETTINGS_MENU_ACTION_OPEN_SETTINGS) {
				pSettingsPanel->Open();
			} else if (settingsMenuAction == ESettingsMenuAction::SETTINGS_MENU_ACTION_CHECK_FOR_UPDATES) {
				// Only kicks a fresh check if the last result isn't still something worth
				// showing as-is (an in-flight download, a not-yet-acted-on AVAILABLE) -
				// otherwise this would restart a perfectly good in-progress download or
				// throw away a manifest the user hasn't decided on yet, just because they
				// reopened the menu.
				const EUpdateStage stage = updater.GetStage();
				if (stage == EUpdateStage::UPDATE_STAGE_IDLE || stage == EUpdateStage::UPDATE_STAGE_UP_TO_DATE ||
					stage == EUpdateStage::UPDATE_STAGE_CHECK_FAILED) {
					updater.CheckForUpdateAsync(kAppVersion);
				}
				pUpdateOverlay->Open();
			} else if (settingsMenuAction == ESettingsMenuAction::SETTINGS_MENU_ACTION_OPEN_DATA_FOLDER) {
				// "open", not "explore": lets Explorer reuse an existing window for this
				// folder rather than always spawning a new one - the normal double-click-a-
				// folder behavior. CStorage::GetDataDirectory also creates the directory (and
				// runs its one-time migration) if it doesn't exist yet, so this always has
				// somewhere real to point Explorer at.
				char dataDirectory[MAX_PATH];
				if (CStorage::GetDataDirectory(dataDirectory, sizeof(dataDirectory))) {
					ShellExecuteA(nullptr, "open", dataDirectory, nullptr, nullptr, SW_SHOWNORMAL);
				}
			}

			// CSettingsPanel::HandleClick already closed the panel itself before latching
			// this - see its own comment for why. Setup mode (not Unlock) is always the
			// right one to activate here: this only ever fires while already unlocked
			// (Settings isn't reachable otherwise), and a reset always means "create a new
			// password," never "prove you know the current one" (the unlocked session
			// already established that).
			if (pSettingsPanel->ConsumeResetPasswordRequested()) {
				pUnlockScreen->ActivateForSetup();
				appLocked = true;
				pCarousel->m_bVisible = false;
			}

			// Right-click routing: neither CCarousel nor CAccountModal can open
			// CContextMenu itself (they don't own that class) - poll whichever one just
			// latched a hit and build/open the actual menu. PENDING_HIT_KIND_MISS from
			// CCarousel still means "hit the carousel but no specific banner" (right-
			// clicking empty space), which resolves to the centered banner, matching the
			// original's own behavior; PENDING_HIT_KIND_NONE means nothing happened at all.
			const PendingHit rightClickedRow = pModal->ConsumePendingRightClickRow();
			const PendingHit rightClickedBanner = pCarousel->ConsumePendingRightClick();
			if (rightClickedRow.Kind == EPendingHitKind::PENDING_HIT_KIND_INDEX) {
				contextMenuTargetBannerIndex = -1;
				contextMenuTargetAccountIndex = rightClickedRow.Index;
				const ContextMenuItem items[]{
					{StringViewFromCString("Edit"), kContextMenuIdEditAccount},
					{StringViewFromCString("Delete"), kContextMenuIdDeleteAccount},
				};

				pContextMenu->Open(event.X, event.Y, items, 2, width, height);
			} else if (rightClickedBanner.Kind != EPendingHitKind::PENDING_HIT_KIND_NONE) {
				const std::int32_t targetBanner = rightClickedBanner.Kind == EPendingHitKind::PENDING_HIT_KIND_INDEX
													  ? rightClickedBanner.Index
													  : pCarousel->GetSelectedIndex();
				if (targetBanner >= 0) {
					contextMenuTargetBannerIndex = targetBanner;
					contextMenuTargetAccountIndex = -1;
					const CBanner &banner = pCarousel->GetBanner(static_cast<std::uint32_t>(targetBanner));
					const bool hasAccounts = banner.AccountCount > 0;

					ContextMenuItem items[4];
					std::uint32_t itemCount = 0;
					items[itemCount++] = {StringViewFromCString("Open"), kContextMenuIdOpen};
					if (hasAccounts) {
						items[itemCount++] = {StringViewFromCString("Login"), kContextMenuIdLogin};
						items[itemCount++] = {StringViewFromCString("Copy Password"), kContextMenuIdCopyPassword};
					}

					items[itemCount++] = {StringViewFromCString("Add Account"), kContextMenuIdAddAccount};
					pContextMenu->Open(event.X, event.Y, items, itemCount, width, height);
				}
			}

			const std::uint32_t selected = pContextMenu->ConsumeSelection();
			if (selected != kContextMenuNoSelection) {
				if (selected == kContextMenuIdOpen && contextMenuTargetBannerIndex >= 0) {
					pModal->Open(contextMenuTargetBannerIndex);
				} else if (selected == kContextMenuIdLogin && contextMenuTargetBannerIndex >= 0) {
					const CBanner &banner =
						pCarousel->GetBanner(static_cast<std::uint32_t>(contextMenuTargetBannerIndex));
					if (banner.AccountCount > 0) {
						pModal->OpenForQuickLogin(contextMenuTargetBannerIndex, 0);
					}
				} else if (selected == kContextMenuIdCopyPassword && contextMenuTargetBannerIndex >= 0) {
					const CBanner &banner =
						pCarousel->GetBanner(static_cast<std::uint32_t>(contextMenuTargetBannerIndex));
					if (banner.AccountCount > 0) {
						CopyPasswordToClipboard(window.GetHandle(), banner.Accounts[0].m_szPassword);
					}
				} else if (selected == kContextMenuIdAddAccount && contextMenuTargetBannerIndex >= 0) {
					pModal->Open(contextMenuTargetBannerIndex);
					pModal->StartAddAccount();
				} else if (selected == kContextMenuIdEditAccount && contextMenuTargetAccountIndex >= 0) {
					pModal->StartEditAccount(static_cast<std::uint32_t>(contextMenuTargetAccountIndex));
				} else if (selected == kContextMenuIdDeleteAccount && contextMenuTargetAccountIndex >= 0) {
					pModal->RemoveAccountRow(static_cast<std::uint32_t>(contextMenuTargetAccountIndex));
					SaveNow(*pCarousel, settings, masterKey);
				}
			}

			// Everything that actually changed persisted state funnels through the
			// ordinary "consumed -> save" rule below, matching the original's own liberal
			// "every interaction while a dialog is open triggers a save" behavior - a
			// little more liberal than strictly necessary (e.g. also saving on a plain
			// carousel card click), but CStorage::Save is cheap and idempotent, and
			// precisely tracking which sub-interaction actually touched persisted state
			// isn't worth the complexity it would add here.
			//
			// CUnlockScreen's own two success signals need different follow-ups, not just
			// a plain save: ConsumeUnlockSucceeded only needs a *reload* (the in-memory
			// account passwords are already blank from an earlier locked Load, and
			// nothing else changed that isn't already on disk); ConsumeSetupSucceeded
			// needs a full *save* instead (every account's password gets re-encrypted
			// under the fresh DEK CMasterKey::Set just generated, and settings.bin needs
			// the new salt/wrap/etc fields actually persisted, not just held in memory).
			if (event.Type == EInputEventType::INPUT_EVENT_MOUSE_UP ||
				event.Type == EInputEventType::INPUT_EVENT_KEY_DOWN ||
				event.Type == EInputEventType::INPUT_EVENT_RIGHT_MOUSE_UP) {
				const bool unlockSucceeded = pUnlockScreen->ConsumeUnlockSucceeded();
				const bool setupSucceeded = pUnlockScreen->ConsumeSetupSucceeded();

				if (unlockSucceeded) {
					LoadAccountsNow(*pCarousel, settings, masterKey);
					appLocked = false;
					pCarousel->m_bVisible = true;
					pUnlockScreen->Deactivate();
				} else if (setupSucceeded) {
					SaveNow(*pCarousel, settings, masterKey);
					appLocked = false;
					pCarousel->m_bVisible = true;
					pUnlockScreen->Deactivate();
				} else if (consumed) {
					SaveNow(*pCarousel, settings, masterKey);
				}
			}
		}

		const auto currentTime = std::chrono::steady_clock::now();
		const float deltaSeconds = std::chrono::duration<float>(currentTime - previousTime).count();
		previousTime = currentTime;

		stack.SetRealMousePosition(mouseX, mouseY);
		stack.Update(deltaSeconds);
		// Skipped over the resize border: forcing the app cursor there every frame would
		// fight the OS's own resize arrows, which WM_SETCURSOR already sets correctly.
		if (!window.IsMouseOverResizeBorder()) {
			window.SetCursorKind(stack.GetDesiredCursor());
		}

		// Joins a finished worker exactly once it's actually done (see CUpdater::Update's
		// own comment) - safe, and necessary, to call every frame regardless of whether
		// anything updater-related is currently happening.
		updater.Update();

		// A verified update has been swapped into this exe's own path and is ready to run -
		// save everything, spawn a fresh process at that same path (now the new build), and
		// cleanly exit this one through the normal WM_CLOSE/ShouldClose path rather than
		// calling ExitProcess directly, so the usual end-of-scope teardown (SaveNow below,
		// CRendererD3D11's own GPU-resource release order) still runs.
		if (updater.ConsumeReadyToRelaunch()) {
			SaveNow(*pCarousel, settings, masterKey);

			wchar_t exePath[MAX_PATH];
			if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
				STARTUPINFOW startupInfo{sizeof(startupInfo)};
				PROCESS_INFORMATION processInfo{};
				if (CreateProcessW(exePath, nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startupInfo,
								   &processInfo)) {
					CloseHandle(processInfo.hProcess);
					CloseHandle(processInfo.hThread);
				}
			}

			PostMessageW(window.GetHandle(), WM_CLOSE, 0, 0);
		}

		// Excludes the window from screenshots/screen recordings/screen shares while the
		// account modal (account list, edit/add forms - anywhere a username, note, or
		// revealed password is actually on screen) is open, if the user wants that - see
		// CSettings::m_bExcludeAccountListFromCapture's own comment. Only calls
		// SetWindowDisplayAffinity on an actual transition, not every frame; the OS keeps
		// enforcing whatever affinity was last set on its own, no per-frame call needed to
		// sustain it. WDA_NONE (not WDA_EXCLUDEFROMCAPTURE) restores normal capture the
		// moment the modal closes or the setting is turned off, rather than leaving the
		// whole app excluded indefinitely - this is scoped to exactly the view that
		// actually shows sensitive data, not a blanket "never capturable" toggle.
		const bool shouldExcludeFromCapture = settings.m_bExcludeAccountListFromCapture && pModal->IsBlocking();
		if (shouldExcludeFromCapture != bWindowExcludedFromCapture) {
			SetWindowDisplayAffinity(window.GetHandle(), shouldExcludeFromCapture ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
			bWindowExcludedFromCapture = shouldExcludeFromCapture;
		}

		const float timeSeconds = std::chrono::duration<float>(currentTime - appStartTime).count();

		// Drives the banner-glow shader's shimmer (the carousel's selected-card border) -
		// read back by the renderer whenever the carousel's batched geometry actually
		// gets submitted below.
		renderer.SetEffectTime(timeSeconds);

		RenderFrame(renderContext);

		// Last thing in the frame, so it only ticks once a frame has actually been presented -
		// the watchdog reads this to tell "the whole app is frozen" apart from "a login worker
		// is stuck but the UI is fine", which look identical from outside the process.
		DebugLog::MarkUiThreadAlive();
	}

	SaveNow(*pCarousel, settings, masterKey);

	// No explicit renderer.Shutdown() here on purpose: CCarousel/CAccountModal/etc (via
	// `stack`), CAssetManager, and CFontManager all still own live CTexture objects at
	// this point, and each one's destructor calls back into the renderer to release its
	// GPU resource - shutting the renderer down first would leave those calls running
	// against an already-torn-down device (a real access violation this project hit
	// once). CRendererD3D11's own destructor now handles this automatically, and normal
	// C++ scope-exit destroys every local here in reverse declaration order - `renderer`
	// is declared before `assets`/`fonts`/`stack`, so it's guaranteed to outlive them.
	return 0;
}
