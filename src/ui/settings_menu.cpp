#include "ui/settings_menu.h"

#include "asset_manager.h"
#include "core/animator.h"
#include "core/string_view.h"
#include "ui/draw_list.h"
#include "ui/text.h"
#include "window.h"

namespace {
constexpr float kMenuOpenEaseRate = 20.0f;
constexpr float kMenuX = 8.0f;
constexpr float kMenuWidth = 200.0f;
constexpr float kMenuItemHeight = 32.0f;
constexpr float kMenuPadding = 6.0f;
constexpr float kMenuRadius = 10.0f;
constexpr float kMenuSlidePixels = 6.0f;

constexpr Color kColorBg{30, 30, 34, 255};
constexpr Color kColorBorder{60, 60, 66, 255};
constexpr Color kColorHover{54, 46, 78, 255};
constexpr Color kColorText{220, 220, 224, 255};
// The Settings row's own tint while the vault is still locked - see CSettingsMenu's own
// constructor comment on why that row (and only that row) can be grayed out here.
constexpr Color kColorTextDisabled{100, 100, 106, 255};

constexpr float kMenuIconSize = 16.0f;
constexpr float kMenuIconTextGap = 10.0f;

// Three items (Settings, Open Data Folder, Check for Updates), no footer - the version
// number moved to the app's own bottom status bar (see main.cpp's DrawStatusBarVersion), and
// this row deliberately stays a plain, static label regardless of CUpdater's own state - see
// ui/title_bar.cpp's own Update pill for where that dynamic "Update Available"/"Update
// Failed" feedback actually lives now, so a background check finding something doesn't
// require opening this menu to ever notice.
Rect MenuRect(float openAmount)
{
	const float h = kMenuPadding * 2.0f + kMenuItemHeight * 3.0f;
	const float slide = (1.0f - openAmount) * -kMenuSlidePixels;
	return Rect{kMenuX, kTitleBarHeight + 4.0f + slide, kMenuWidth, h};
}

// The "Settings" row - first item, right below the top padding.
Rect SettingsItemRect(Rect menu)
{
	return Rect{menu.X, menu.Y + kMenuPadding, menu.W, kMenuItemHeight};
}

// The "Open Data Folder" row - stacked directly below SettingsItemRect.
Rect DataFolderItemRect(Rect menu)
{
	return Rect{menu.X, menu.Y + kMenuPadding + kMenuItemHeight, menu.W, kMenuItemHeight};
}

// The "Check for Updates" row - stacked directly below DataFolderItemRect. Always present
// (unlike CTitleBar's own Update button, which only occupies its slot once CUpdater already
// has something to show - see title_bar.cpp) so there's always a discoverable, manual way to
// reach the updater regardless of whether a background check has run yet.
Rect UpdateItemRect(Rect menu)
{
	return Rect{menu.X, menu.Y + kMenuPadding + kMenuItemHeight * 2.0f, menu.W, kMenuItemHeight};
}

} // namespace

CSettingsMenu::CSettingsMenu(CFontManager &fonts, CAssetManager &assets, const bool &appLocked)
	: m_fonts(fonts)
	, m_assets(assets)
	, m_appLocked(appLocked)
{
}

void CSettingsMenu::Open()
{
	m_bOpen = true;
}

// No transient state to reset here (unlike CSettingsPanel), since this menu has no
// fields/popups of its own to leave dirty.
void CSettingsMenu::Close()
{
	m_bOpen = false;
}

void CSettingsMenu::Update(float deltaSeconds)
{
	m_flOpenAmount = CAnimator::EaseToward(m_flOpenAmount, m_bOpen ? 1.0f : 0.0f, kMenuOpenEaseRate, deltaSeconds);
	if (!m_bOpen && m_flOpenAmount < 0.002f) {
		m_flOpenAmount = 0.0f;
	}
}

bool CSettingsMenu::OnPointerUp(float x, float y)
{
	if (!IsBlocking()) {
		return false;
	}

	const Rect rect = MenuRect(m_flOpenAmount);
	if (RectContainsPoint(SettingsItemRect(rect), x, y)) {
		// Grayed out (see Draw/GetDesiredCursor) while the vault is still locked - opening
		// CSettingsPanel doesn't make sense yet, and it isn't even reachable from here (see
		// main.cpp's own widget-stack push order), so this is a real no-op, not a
		// mismatched-looking dead click.
		if (!m_appLocked) {
			m_pendingAction = ESettingsMenuAction::SETTINGS_MENU_ACTION_OPEN_SETTINGS;
		}
	} else if (RectContainsPoint(DataFolderItemRect(rect), x, y)) {
		m_pendingAction = ESettingsMenuAction::SETTINGS_MENU_ACTION_OPEN_DATA_FOLDER;
	} else if (RectContainsPoint(UpdateItemRect(rect), x, y)) {
		m_pendingAction = ESettingsMenuAction::SETTINGS_MENU_ACTION_CHECK_FOR_UPDATES;
	}

	Close(); // any click within the blocking area dismisses the menu
	return true;
}

ECursorKind CSettingsMenu::GetDesiredCursor() const
{
	if (!IsBlocking()) {
		return ECursorKind::CURSOR_ARROW;
	}

	const Rect rect = MenuRect(m_flOpenAmount);
	if ((!m_appLocked && RectContainsPoint(SettingsItemRect(rect), m_flMouseX, m_flMouseY)) ||
		RectContainsPoint(DataFolderItemRect(rect), m_flMouseX, m_flMouseY) ||
		RectContainsPoint(UpdateItemRect(rect), m_flMouseX, m_flMouseY)) {
		return ECursorKind::CURSOR_HAND;
	}
	return ECursorKind::CURSOR_ARROW;
}

ESettingsMenuAction CSettingsMenu::ConsumeAction()
{
	const ESettingsMenuAction action = m_pendingAction;
	m_pendingAction = ESettingsMenuAction::SETTINGS_MENU_ACTION_NONE;
	return action;
}

void CSettingsMenu::Draw(CDrawList &drawList)
{
	if (m_flOpenAmount <= 0.001f) {
		return;
	}

	const auto alpha = static_cast<std::uint8_t>(255.0f * m_flOpenAmount);
	const Rect rect = MenuRect(m_flOpenAmount);

	drawList.AddRectRoundedFilled(rect.X, rect.Y, rect.W, rect.H, CDrawList::UniformRadii(kMenuRadius),
								  ColorWithAlpha(kColorBorder, alpha));
	drawList.AddRectRoundedFilled(rect.X + 1.0f, rect.Y + 1.0f, rect.W - 2.0f, rect.H - 2.0f,
								  CDrawList::UniformRadii(kMenuRadius - 1.0f), ColorWithAlpha(kColorBg, alpha));

	const Rect settingsRow = SettingsItemRect(rect);
	const Rect dataFolderRow = DataFolderItemRect(rect);
	const Rect updateRow = UpdateItemRect(rect);
	// No hover highlight for a row that's currently a no-op - see OnPointerUp's own comment.
	const bool hoverSettings = !m_appLocked && RectContainsPoint(settingsRow, m_flMouseX, m_flMouseY);
	const bool hoverDataFolder = RectContainsPoint(dataFolderRow, m_flMouseX, m_flMouseY);
	const bool hoverUpdate = RectContainsPoint(updateRow, m_flMouseX, m_flMouseY);
	const Color settingsColor = m_appLocked ? kColorTextDisabled : kColorText;

	if (hoverSettings) {
		drawList.AddRectRoundedFilled(settingsRow.X + 4.0f, settingsRow.Y, settingsRow.W - 8.0f, settingsRow.H,
									  CDrawList::UniformRadii(6.0f), ColorWithAlpha(kColorHover, alpha));
	}
	if (hoverDataFolder) {
		drawList.AddRectRoundedFilled(dataFolderRow.X + 4.0f, dataFolderRow.Y, dataFolderRow.W - 8.0f,
									  dataFolderRow.H, CDrawList::UniformRadii(6.0f), ColorWithAlpha(kColorHover, alpha));
	}
	if (hoverUpdate) {
		drawList.AddRectRoundedFilled(updateRow.X + 4.0f, updateRow.Y, updateRow.W - 8.0f, updateRow.H,
									  CDrawList::UniformRadii(6.0f), ColorWithAlpha(kColorHover, alpha));
	}

	const CFont &body = m_fonts.GetBody();

	// Settings gets the same gear the title bar's own Menu button used to show before
	// that button started opening this menu rather than Settings directly (see
	// title_bar.cpp) - it belongs here now, on the row that actually opens it.
	const Rect settingsIcon{settingsRow.X + 14.0f, settingsRow.Y + (settingsRow.H - kMenuIconSize) * 0.5f,
							kMenuIconSize, kMenuIconSize};
	const Rect dataFolderIcon{dataFolderRow.X + 14.0f, dataFolderRow.Y + (dataFolderRow.H - kMenuIconSize) * 0.5f,
							  kMenuIconSize, kMenuIconSize};
	const Rect updateIcon{updateRow.X + 14.0f, updateRow.Y + (updateRow.H - kMenuIconSize) * 0.5f, kMenuIconSize,
						  kMenuIconSize};
	drawList.AddRectRoundedTextured(settingsIcon.X, settingsIcon.Y, settingsIcon.W, settingsIcon.H, kCornerRadiiNone,
									m_assets.GetIconSettings(), ColorWithAlpha(settingsColor, alpha));
	drawList.AddRectRoundedTextured(dataFolderIcon.X, dataFolderIcon.Y, dataFolderIcon.W, dataFolderIcon.H,
									kCornerRadiiNone, m_assets.GetIconFolder(), ColorWithAlpha(kColorText, alpha));
	drawList.AddRectRoundedTextured(updateIcon.X, updateIcon.Y, updateIcon.W, updateIcon.H, kCornerRadiiNone,
									m_assets.GetIconUpdate(), ColorWithAlpha(kColorText, alpha));

	// All three rows share the same left padding, so their icons (and their text, shifted
	// over to make room) always land at the same X regardless of which row it is.
	const float textX = settingsIcon.X + kMenuIconSize + kMenuIconTextGap;
	// Baseline for a glyph's own visual center (not just its ascent) to land on the row's
	// true center, matching where the icon (a plain box, centered by its own bounds) sits
	// - GetAscent() alone puts the baseline too low, since it ignores how far descenders
	// reach back up past it (GetDescent() is negative - see its own comment in font.h),
	// which is exactly why the text used to read as sitting below the icon rather than
	// level with it. kBaselineVisualNudge corrects the last couple pixels of that same
	// gap the ascent/descent math alone doesn't quite close - empirical, not font-metric-
	// exact, the same kind of tuning carousel.cpp's own IconCenterYFor documents.
	constexpr float kBaselineVisualNudge = 2.0f;
	const auto RowBaselineY = [&](Rect row) {
		return row.Y + row.H * 0.5f + (body.GetAscent() + body.GetDescent()) * 0.5f - kBaselineVisualNudge;
	};
	DrawText(drawList, body, textX, RowBaselineY(settingsRow), StringViewFromCString("Settings"),
			 ColorWithAlpha(settingsColor, alpha));
	DrawText(drawList, body, textX, RowBaselineY(dataFolderRow), StringViewFromCString("Open Data Folder"),
			 ColorWithAlpha(kColorText, alpha));
	DrawText(drawList, body, textX, RowBaselineY(updateRow), StringViewFromCString("Check for Updates"),
			 ColorWithAlpha(kColorText, alpha));
}
