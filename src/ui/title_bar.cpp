#include "ui/title_bar.h"

#include <Windows.h>

#include "asset_manager.h"
#include "core/updater.h"
#include "font_manager.h"
#include "ui/draw_list.h"
#include "ui/text.h"
#include "window.h"

namespace {
constexpr Color kColorGlyph{214, 214, 218, 255};
constexpr Color kColorGlyphDim{150, 150, 156, 255};
// Close reads warm/red on hover, the near-universal OS convention; every other button
// gets a plain neutral lighter-grey hover, matching CSettingsMenu's own row hover.
constexpr Color kColorHoverNeutral{255, 255, 255, 18};
constexpr Color kColorHoverClose{232, 17, 35, 255};

// The Update pill's own two "something to say" tints - a muted background plus a brighter
// matching foreground, rather than one flat color for both (a solid bright fill this small
// would read as an alert badge, not a calm, always-there status pill).
constexpr Color kColorUpdateGoodBg{32, 58, 44, 255};
constexpr Color kColorUpdateGoodFg{110, 220, 150, 255};
constexpr Color kColorUpdateBadBg{58, 34, 34, 255};
constexpr Color kColorUpdateBadFg{230, 120, 110, 255};

constexpr float kGlyphLineThickness = 1.5f;
constexpr float kIconSize = 16.0f;

Rect IconRectFor(Rect button)
{
	return Rect{button.X + (button.W - kIconSize) * 0.5f, button.Y + (button.H - kIconSize) * 0.5f, kIconSize,
				kIconSize};
}

// The Update button only ever occupies its reserved title-bar slot (see window.h's own
// comment on TITLE_BAR_BUTTON_UPDATE) once CUpdater actually has something worth a click -
// silent for the ordinary IDLE/CHECKING/UP_TO_DATE/CHECK_FAILED states, exactly matching
// this project's "check quietly in the background, only ever surface it once there's
// something to say" design (see core/updater.h's own file comment).
bool IsUpdateButtonVisible(EUpdateStage stage)
{
	switch (stage) {
		case EUpdateStage::UPDATE_STAGE_AVAILABLE:
		case EUpdateStage::UPDATE_STAGE_MANUAL_UPGRADE_REQUIRED:
		case EUpdateStage::UPDATE_STAGE_DOWNLOADING:
		case EUpdateStage::UPDATE_STAGE_VERIFYING:
		case EUpdateStage::UPDATE_STAGE_INSTALLING:
		case EUpdateStage::UPDATE_STAGE_READY_TO_RELAUNCH:
		case EUpdateStage::UPDATE_STAGE_ERROR:
		case EUpdateStage::UPDATE_STAGE_CANCELLED:
			return true;
		default:
			return false;
	}
}

// The pill's own label - "Update Available" is the one CTitleBar exists to make impossible
// to miss (see this file's own header comment on why it's a labeled pill, not a bare icon);
// the other stages get a plainer, less alarming label of their own rather than reusing that
// same wording for something that isn't actually an available-and-untouched update anymore.
CStringView UpdateStatusLabel(EUpdateStage stage)
{
	switch (stage) {
		case EUpdateStage::UPDATE_STAGE_AVAILABLE:
		case EUpdateStage::UPDATE_STAGE_MANUAL_UPGRADE_REQUIRED:
			return StringViewFromCString("Update Available");
		case EUpdateStage::UPDATE_STAGE_ERROR:
		case EUpdateStage::UPDATE_STAGE_CANCELLED:
			return StringViewFromCString("Update Failed");
		default:
			return StringViewFromCString("Updating...");
	}
}
} // namespace

CTitleBar::CTitleBar(CWindow &window, CAssetManager &assets, CUpdater &updater, CFontManager &fonts)
	: m_window(window)
	, m_assets(assets)
	, m_updater(updater)
	, m_fonts(fonts)
{
}

void CTitleBar::Update(float deltaSeconds)
{
	m_vecBounds = Rect{0.0f, 0.0f, static_cast<float>(m_window.GetWidth()), kTitleBarHeight};
	m_window.SetUpdateButtonVisible(IsUpdateButtonVisible(m_updater.GetStage()));
}

bool CTitleBar::OnPointerDown(float x, float y)
{
	return m_window.TitleBarHitTest(x, y) != ETitleBarButton::TITLE_BAR_BUTTON_NONE;
}

bool CTitleBar::OnPointerUp(float x, float y)
{
	const ETitleBarButton button = m_window.TitleBarHitTest(x, y);
	if (button == ETitleBarButton::TITLE_BAR_BUTTON_NONE) {
		return false;
	}

	if (button == ETitleBarButton::TITLE_BAR_BUTTON_MENU) {
		m_bMenuClickedThisFrame = true;
	} else if (button == ETitleBarButton::TITLE_BAR_BUTTON_UPDATE) {
		m_bUpdateClickedThisFrame = true;
	} else if (button == ETitleBarButton::TITLE_BAR_BUTTON_MINIMIZE) {
		ShowWindow(m_window.GetHandle(), SW_MINIMIZE);
	} else if (button == ETitleBarButton::TITLE_BAR_BUTTON_MAXIMIZE) {
		ShowWindow(m_window.GetHandle(), IsZoomed(m_window.GetHandle()) ? SW_RESTORE : SW_MAXIMIZE);
	} else if (button == ETitleBarButton::TITLE_BAR_BUTTON_CLOSE) {
		// window.h has no public "request close" setter - PostMessage the same WM_CLOSE
		// the OS caption's own close button would have sent, so CWindow's normal
		// WM_CLOSE handling (ShouldClose) fires exactly the way it already does for
		// Alt+F4 / the taskbar's Close command.
		PostMessageW(m_window.GetHandle(), WM_CLOSE, 0, 0);
	}

	return true;
}

ECursorKind CTitleBar::GetDesiredCursor() const
{
	return m_window.TitleBarHitTest(m_flMouseX, m_flMouseY) == ETitleBarButton::TITLE_BAR_BUTTON_NONE
			   ? ECursorKind::CURSOR_ARROW
			   : ECursorKind::CURSOR_HAND;
}

bool CTitleBar::ConsumeMenuClicked()
{
	const bool clicked = m_bMenuClickedThisFrame;
	m_bMenuClickedThisFrame = false;
	return clicked;
}

bool CTitleBar::ConsumeUpdateClicked()
{
	const bool clicked = m_bUpdateClickedThisFrame;
	m_bUpdateClickedThisFrame = false;
	return clicked;
}

void CTitleBar::Draw(CDrawList &drawList)
{
	const float width = static_cast<float>(m_window.GetWidth());
	drawList.AddRectFilled(0.0f, 0.0f, width, kTitleBarHeight, kTitleBarColor);

	// A flush (non-rounded) hover background, same shape as the native OS title bar
	// buttons this mirrors - Close solid red, everything else a subtle translucent
	// lighten. CWindow::TitleBarHitTest is the same function WM_NCHITTEST itself uses,
	// so hover here can never disagree with what's actually clickable.
	const ETitleBarButton hovered = m_window.TitleBarHitTest(m_flMouseX, m_flMouseY);
	const auto DrawHoverBackground = [&](ETitleBarButton button) {
		if (hovered != button) {
			return;
		}
		const Rect rect = m_window.GetTitleBarButtonRect(button);
		drawList.AddRectFilled(rect.X, rect.Y, rect.W, rect.H,
							   button == ETitleBarButton::TITLE_BAR_BUTTON_CLOSE ? kColorHoverClose
																				 : kColorHoverNeutral);
	};

	// Menu: a hamburger glyph - it opens CSettingsMenu (Settings, Open Data Folder), not
	// Settings directly, so the gear itself now lives on that dropdown's own Settings row
	// instead (see settings_menu.cpp) rather than sitting here.
	{
		DrawHoverBackground(ETitleBarButton::TITLE_BAR_BUTTON_MENU);
		const Rect icon = IconRectFor(m_window.GetTitleBarButtonRect(ETitleBarButton::TITLE_BAR_BUTTON_MENU));
		drawList.AddRectRoundedTextured(icon.X, icon.Y, icon.W, icon.H, kCornerRadiiNone, m_assets.GetIconMenu(),
										kColorGlyph);
	}

	// Update: a labeled, color-coded pill, not a bare icon - "Update Available" in green
	// once one's ready to install (the whole reason TITLE_BAR_BUTTON_UPDATE's slot is wider
	// than every other title bar button - see window.h's own comment), red once something's
	// gone wrong, a plain neutral "Updating..." while a download/verify/install is actually
	// in flight. Only drawn (and only hit-testable, per CWindow::TitleBarHitTest's own
	// m_bUpdateButtonVisible gate this file's Update already set) once IsUpdateButtonVisible
	// says there's actually something to show. Deliberately never opens CUpdateOverlay on
	// its own - the periodic background check (see main.cpp's own comment) stays silent by
	// design; this pill IS that check's one visible effect, clicking it is what opens the
	// overlay.
	if (IsUpdateButtonVisible(m_updater.GetStage())) {
		const EUpdateStage updateStage = m_updater.GetStage();
		const bool isFailure =
			updateStage == EUpdateStage::UPDATE_STAGE_ERROR || updateStage == EUpdateStage::UPDATE_STAGE_CANCELLED;
		const bool isNotable = isFailure || updateStage == EUpdateStage::UPDATE_STAGE_AVAILABLE ||
							   updateStage == EUpdateStage::UPDATE_STAGE_MANUAL_UPGRADE_REQUIRED;

		const Color pillBg = isFailure ? kColorUpdateBadBg : (isNotable ? kColorUpdateGoodBg : kColorHoverNeutral);
		const Color pillFg = isFailure ? kColorUpdateBadFg : (isNotable ? kColorUpdateGoodFg : kColorGlyphDim);

		const Rect button = m_window.GetTitleBarButtonRect(ETitleBarButton::TITLE_BAR_BUTTON_UPDATE);
		constexpr float kPillVerticalInset = 7.0f;
		constexpr float kPillSideInset = 4.0f;
		constexpr float kPillPadding = 10.0f;
		constexpr float kPillIconTextGap = 8.0f;
		const Rect pill{button.X + kPillSideInset, button.Y + kPillVerticalInset, button.W - kPillSideInset * 2.0f,
						button.H - kPillVerticalInset * 2.0f};
		drawList.AddRectRoundedFilled(pill.X, pill.Y, pill.W, pill.H, CDrawList::UniformRadii(pill.H * 0.5f), pillBg);

		const Rect icon{pill.X + kPillPadding, pill.Y + (pill.H - kIconSize) * 0.5f, kIconSize, kIconSize};
		drawList.AddRectRoundedTextured(icon.X, icon.Y, icon.W, icon.H, kCornerRadiiNone, m_assets.GetIconUpdate(),
										pillFg);

		const CFont &secondary = m_fonts.GetSecondary();
		const float baselineY =
			pill.Y + pill.H * 0.5f + (secondary.GetAscent() + secondary.GetDescent()) * 0.5f;
		DrawText(drawList, secondary, icon.X + kIconSize + kPillIconTextGap, baselineY,
				UpdateStatusLabel(updateStage), pillFg);
	}

	// Minimize: the embedded icon.
	{
		DrawHoverBackground(ETitleBarButton::TITLE_BAR_BUTTON_MINIMIZE);
		const Rect icon = IconRectFor(m_window.GetTitleBarButtonRect(ETitleBarButton::TITLE_BAR_BUTTON_MINIMIZE));
		drawList.AddRectRoundedTextured(icon.X, icon.Y, icon.W, icon.H, kCornerRadiiNone, m_assets.GetIconMinimize(),
										hovered == ETitleBarButton::TITLE_BAR_BUTTON_MINIMIZE ? kColorGlyph
																							  : kColorGlyphDim);
	}

	// Maximize / Restore: no embedded icon for this - a square outline, or two
	// overlapping ones once already maximized.
	{
		DrawHoverBackground(ETitleBarButton::TITLE_BAR_BUTTON_MAXIMIZE);
		const Rect rect = m_window.GetTitleBarButtonRect(ETitleBarButton::TITLE_BAR_BUTTON_MAXIMIZE);
		const float centerX = rect.X + rect.W * 0.5f;
		const float centerY = rect.Y + rect.H * 0.5f;
		const Color outlineColor = hovered == ETitleBarButton::TITLE_BAR_BUTTON_MAXIMIZE ? kColorGlyph : kColorGlyphDim;

		if (IsZoomed(m_window.GetHandle())) {
			constexpr float kSize = 8.0f;
			constexpr float kOffset = 3.0f;
			drawList.AddRectOutline(centerX - kSize * 0.5f + kOffset, centerY - kSize * 0.5f - kOffset, kSize, kSize,
									kGlyphLineThickness, outlineColor);
			drawList.AddRectOutline(centerX - kSize * 0.5f - kOffset, centerY - kSize * 0.5f + kOffset, kSize, kSize,
									kGlyphLineThickness, outlineColor);
		} else {
			constexpr float kSize = 10.0f;
			drawList.AddRectOutline(centerX - kSize * 0.5f, centerY - kSize * 0.5f, kSize, kSize, kGlyphLineThickness,
									outlineColor);
		}
	}

	// Close: the embedded icon.
	{
		DrawHoverBackground(ETitleBarButton::TITLE_BAR_BUTTON_CLOSE);
		const Rect icon = IconRectFor(m_window.GetTitleBarButtonRect(ETitleBarButton::TITLE_BAR_BUTTON_CLOSE));
		drawList.AddRectRoundedTextured(icon.X, icon.Y, icon.W, icon.H, kCornerRadiiNone, m_assets.GetIconClose(),
										kColorGlyph);
	}
}
