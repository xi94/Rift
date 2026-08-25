#include "ui/settings_menu.h"

#include <algorithm>

#include "asset_manager.h"
#include "core/animator.h"
#include "core/string_view.h"
#include "ui/draw_list.h"
#include "ui/settings.h"
#include "ui/text.h"
#include "window.h"

namespace {
constexpr float kMenuOpenEaseRate = 20.0f;
constexpr float kItemHoverEaseRate = 22.0f;
constexpr float kMenuX = 8.0f;
constexpr float kMenuWidth = 226.0f;
constexpr float kMenuItemHeight = 34.0f;
constexpr float kMenuPadding = 6.0f;
constexpr float kMenuRadius = 12.0f;
constexpr float kMenuSlidePixels = 6.0f;
// Inset of an item's hover pill from the popup's own edges, and of the header's content
// from the same - one number so the pill, the header text, and the separator all line up
// on the same left edge instead of three hand-tuned ones drifting apart.
constexpr float kMenuInsetX = 5.0f;
constexpr float kMenuContentX = 14.0f;
constexpr float kSeparatorGap = 6.0f;

constexpr Color kColorBg{30, 30, 34, 255};
constexpr Color kColorBorder{60, 60, 66, 255};
constexpr Color kColorSeparator{52, 52, 58, 255};
constexpr Color kColorText{220, 220, 224, 255};
// The Settings row's own tint while the vault is still locked - see CSettingsMenu's own
// constructor comment on why that row (and only that row) can be grayed out here.
constexpr Color kColorTextDisabled{100, 100, 106, 255};

constexpr float kMenuIconSize = 16.0f;
constexpr float kMenuIconTextGap = 10.0f;

// One row of the menu. The whole point of the table (see settings_menu.h's own file
// comment): geometry, hit-testing, hover, and drawing all walk this same list, so a new
// item is one entry here rather than a fourth copy of four near-identical branches.
//
// StartsGroup puts a separator above the item. The grouping is by what the rows have to do
// with each other, not by how often they're used: Check for Updates acts on this build,
// while Settings and Open Data Folder are both "this install's own configuration" - and a
// plain list gave no hint that those were different subjects at all.
struct MenuItem {
	ESettingsMenuAction Action;
	const char *pLabel;
	bool StartsGroup;
};

constexpr MenuItem kMenuItems[]{
	{ESettingsMenuAction::SETTINGS_MENU_ACTION_CHECK_FOR_UPDATES, "Check for Updates", false},
	{ESettingsMenuAction::SETTINGS_MENU_ACTION_OPEN_SETTINGS, "Settings", true},
	{ESettingsMenuAction::SETTINGS_MENU_ACTION_OPEN_DATA_FOLDER, "Open Data Folder", false},
};
constexpr std::uint64_t kMenuItemCount = sizeof(kMenuItems) / sizeof(kMenuItems[0]);
static_assert(kMenuItemCount <= CSettingsMenu::kMaxItems, "grow CSettingsMenu::kMaxItems to match kMenuItems");

// Every row has an embedded icon; the switch is exhaustive so adding an action without one
// is a compile error here rather than a blank square at runtime.
const CTexture *IconForItem(const CAssetManager &assets, std::uint64_t index)
{
	switch (kMenuItems[index].Action) {
		case ESettingsMenuAction::SETTINGS_MENU_ACTION_OPEN_SETTINGS:
			return assets.GetIconSettings();
		case ESettingsMenuAction::SETTINGS_MENU_ACTION_OPEN_DATA_FOLDER:
			return assets.GetIconFolder();
		case ESettingsMenuAction::SETTINGS_MENU_ACTION_CHECK_FOR_UPDATES:
			return assets.GetIconUpdate();
		case ESettingsMenuAction::SETTINGS_MENU_ACTION_NONE:
			break;
	}
	return nullptr;
}

// Vertical space a group separator claims - the hairline itself plus the air on either
// side of it. One number so the height math and the per-item walk can't disagree.
constexpr float kGroupSeparatorBlock = kSeparatorGap * 2.0f + 1.0f;

// The distance from the popup's content top to item `index`, walking the table so that
// every separator above it is accounted for exactly once. This is the only place the item
// stride is expressed; ItemRect and the separator draw both go through it.
float ItemOffsetY(std::uint64_t index)
{
	float offset = 0.0f;
	for (std::uint64_t i = 0; i < index; i += 1) {
		offset += kMenuItemHeight;
		if (i + 1 < kMenuItemCount && kMenuItems[i + 1].StartsGroup) {
			offset += kGroupSeparatorBlock;
		}
	}
	return offset;
}

Rect MenuRect(float openAmount)
{
	const float h = kMenuPadding * 2.0f + ItemOffsetY(kMenuItemCount - 1) + kMenuItemHeight;
	const float slide = (1.0f - openAmount) * -kMenuSlidePixels;
	return Rect{kMenuX, kTitleBarHeight + 4.0f + slide, kMenuWidth, h};
}

Rect ItemRect(Rect menu, std::uint64_t index)
{
	return Rect{menu.X, menu.Y + kMenuPadding + ItemOffsetY(index), menu.W, kMenuItemHeight};
}

// The hairline sitting in the gap above a group-starting item.
Rect GroupSeparatorRect(Rect menu, std::uint64_t index)
{
	const Rect item = ItemRect(menu, index);
	return Rect{menu.X + kMenuContentX, item.Y - kSeparatorGap - 1.0f, menu.W - kMenuContentX * 2.0f, 1.0f};
}

// A row's hover pill - inset from the popup's edges so the highlight reads as a chip
// inside the menu rather than a full-width band cutting it in half.
Rect ItemHighlightRect(Rect item)
{
	return Rect{item.X + kMenuInsetX, item.Y, item.W - kMenuInsetX * 2.0f, item.H};
}

// Baseline for a glyph's own visual center (not just its ascent) to land on the row's
// true center, matching where the icon (a plain box, centered by its own bounds) sits -
// GetAscent() alone puts the baseline too low, since it ignores how far descenders reach
// back up past it (GetDescent() is negative - see its own comment in font.h), which is
// exactly why the text used to read as sitting below the icon rather than level with it.
// kBaselineVisualNudge corrects the last couple pixels of that same gap the ascent/descent
// math alone doesn't quite close - empirical, not font-metric-exact, the same kind of
// tuning carousel.cpp's own IconCenterYFor documents.
float RowBaselineY(Rect row, const CFont &font)
{
	constexpr float kBaselineVisualNudge = 2.0f;
	return row.Y + row.H * 0.5f + (font.GetAscent() + font.GetDescent()) * 0.5f - kBaselineVisualNudge;
}

} // namespace

CSettingsMenu::CSettingsMenu(CFontManager &fonts, CAssetManager &assets, const CSettings &settings,
							 const bool &appLocked)
	: m_fonts(fonts)
	, m_assets(assets)
	, m_settings(settings)
	, m_appLocked(appLocked)
{
}

void CSettingsMenu::Open()
{
	m_bOpen = true;
}

// No transient state to reset here (unlike CSettingsPanel), since this menu has no
// fields/popups of its own to leave dirty - the per-item hover amounts ease themselves
// back down on their own once nothing is hovered.
void CSettingsMenu::Close()
{
	m_bOpen = false;
}

bool CSettingsMenu::IsItemEnabled(std::uint64_t index) const
{
	if (kMenuItems[index].Action == ESettingsMenuAction::SETTINGS_MENU_ACTION_OPEN_SETTINGS) {
		return !m_appLocked;
	}
	return true;
}

void CSettingsMenu::Update(float deltaSeconds)
{
	m_flOpenAmount = CAnimator::EaseToward(m_flOpenAmount, m_bOpen ? 1.0f : 0.0f, kMenuOpenEaseRate, deltaSeconds);
	if (!m_bOpen && m_flOpenAmount < 0.002f) {
		m_flOpenAmount = 0.0f;
	}

	const Rect menu = MenuRect(m_flOpenAmount);
	for (std::uint64_t i = 0; i < kMenuItemCount; i += 1) {
		// No hover highlight for a row that's currently a no-op - see OnPointerUp's own
		// comment.
		const bool hovered =
			IsBlocking() && IsItemEnabled(i) && RectContainsPoint(ItemRect(menu, i), m_flMouseX, m_flMouseY);
		m_aItemHoverAmount[i] =
			CAnimator::EaseToward(m_aItemHoverAmount[i], hovered ? 1.0f : 0.0f, kItemHoverEaseRate, deltaSeconds);
	}
}

bool CSettingsMenu::OnPointerUp(float x, float y)
{
	if (!IsBlocking()) {
		return false;
	}

	const Rect menu = MenuRect(m_flOpenAmount);
	for (std::uint64_t i = 0; i < kMenuItemCount; i += 1) {
		if (!RectContainsPoint(ItemRect(menu, i), x, y)) {
			continue;
		}
		// A disabled row (Settings, while the vault is still locked) is grayed out and
		// stays a real no-op rather than a mismatched-looking dead click - see Draw and
		// GetDesiredCursor, which both read the same IsItemEnabled.
		if (IsItemEnabled(i)) {
			m_pendingAction = kMenuItems[i].Action;
		}
		break;
	}

	Close(); // any click within the blocking area dismisses the menu
	return true;
}

ECursorKind CSettingsMenu::GetDesiredCursor() const
{
	if (!IsBlocking()) {
		return ECursorKind::CURSOR_ARROW;
	}

	const Rect menu = MenuRect(m_flOpenAmount);
	for (std::uint64_t i = 0; i < kMenuItemCount; i += 1) {
		if (IsItemEnabled(i) && RectContainsPoint(ItemRect(menu, i), m_flMouseX, m_flMouseY)) {
			return ECursorKind::CURSOR_HAND;
		}
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
	const CFont &body = m_fonts.GetBody();

	drawList.AddRectRoundedFilled(rect.X, rect.Y, rect.W, rect.H, CDrawList::UniformRadii(kMenuRadius),
								  ColorWithAlpha(kColorBorder, alpha));
	drawList.AddRectRoundedFilled(rect.X + 1.0f, rect.Y + 1.0f, rect.W - 2.0f, rect.H - 2.0f,
								  CDrawList::UniformRadii(kMenuRadius - 1.0f), ColorWithAlpha(kColorBg, alpha));

	for (std::uint64_t i = 0; i < kMenuItemCount; i += 1) {
		if (kMenuItems[i].StartsGroup && i > 0) {
			const Rect separator = GroupSeparatorRect(rect, i);
			drawList.AddRectFilled(separator.X, separator.Y, separator.W, separator.H,
								   ColorWithAlpha(kColorSeparator, alpha));
		}
	}

	for (std::uint64_t i = 0; i < kMenuItemCount; i += 1) {
		const Rect item = ItemRect(rect, i);
		const bool enabled = IsItemEnabled(i);
		const float hover = m_aItemHoverAmount[i];

		if (hover > 0.001f) {
			// The user's own accent, mixed most of the way back toward the popup's
			// background: a full-strength accent fill behind body text would be louder than
			// anything else on screen, and this menu is not the app's focal point.
			const Color hoverColor = ColorLerp(kColorBg, m_settings.m_clrAccent, 0.28f);
			const Rect highlight = ItemHighlightRect(item);
			drawList.AddRectRoundedFilled(
				highlight.X, highlight.Y, highlight.W, highlight.H, CDrawList::UniformRadii(7.0f),
				ColorWithAlpha(hoverColor, static_cast<std::uint8_t>(static_cast<float>(alpha) * hover)));
		}

		const Color textColor = enabled ? kColorText : kColorTextDisabled;
		// Every row shares the same left padding, so their icons (and their text, shifted
		// over to make room) always land at the same X regardless of which row it is.
		const Rect icon{item.X + kMenuContentX, item.Y + (item.H - kMenuIconSize) * 0.5f, kMenuIconSize, kMenuIconSize};
		drawList.AddRectRoundedTextured(icon.X, icon.Y, icon.W, icon.H, kCornerRadiiNone, IconForItem(m_assets, i),
										ColorWithAlpha(textColor, alpha));

		DrawText(drawList, body, icon.X + kMenuIconSize + kMenuIconTextGap, RowBaselineY(item, body),
				 StringViewFromCString(kMenuItems[i].pLabel), ColorWithAlpha(textColor, alpha));
	}
}
