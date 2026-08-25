#include "ui/game_select_popup.h"

#include <algorithm>
#include <bit>

#include "core/animator.h"
#include "ui/draw_list.h"
#include "ui/text.h"

namespace {
constexpr float kPopupWidth = 220.0f;
constexpr float kPopupPadding = 6.0f;
constexpr float kPopupRadius = 10.0f;
constexpr float kWindowMargin = 8.0f;
constexpr float kAnchorGap = 6.0f; // vertical gap between the chip and the popup below it - see PopupRect
constexpr float kRowPaddingX = 10.0f;
constexpr float kOpenEaseRate = 20.0f; // a quick micro-interaction, similar to a toggle switch's own ease rate

constexpr Color kColorBg{30, 30, 34, 255};
constexpr Color kColorBorder{60, 60, 66, 255};
constexpr Color kColorHover{46, 46, 52, 255};
constexpr Color kColorText{220, 220, 224, 255};
constexpr Color kColorTextDisabled{130, 130, 136, 255};
constexpr Color kColorCheck{130, 200, 140, 255};
constexpr Color kColorCheckDisabled{110, 118, 112, 255};

// Derived from CFont::GetLineHeight rather than a fixed constant - see this file's own
// header comment for why. Deliberately smaller than CCarousel's List view icon (a
// 0.95x ratio vs. List's much larger dedicated thumbnail) since this is still a
// compact multi-select list, not the place to show icons off.
float RowHeightFor(const CFontManager &fonts)
{
	return std::max(30.0f, fonts.GetSecondary().GetLineHeight() + 14.0f);
}

float IconSizeFor(const CFontManager &fonts)
{
	return fonts.GetSecondary().GetLineHeight() * 0.95f;
}

Rect RowRect(Rect popup, const CFontManager &fonts, std::uint32_t index)
{
	const float height = RowHeightFor(fonts);
	return Rect{popup.X, popup.Y + kPopupPadding + static_cast<float>(index) * height, popup.W, height};
}

void DrawCheck(CDrawList &drawList, Rect row, float scale, Color color)
{
	const float cx = row.X + row.W - kRowPaddingX - 8.0f * scale;
	const float cy = row.Y + row.H * 0.5f;
	drawList.AddLine(cx - 7.0f * scale, cy, cx - 2.0f * scale, cy + 5.0f * scale, 2.0f, color);
	drawList.AddLine(cx - 2.0f * scale, cy + 5.0f * scale, cx + 7.0f * scale, cy - 6.0f * scale, 2.0f, color);
}

// The sole remaining checked row can't be unchecked - see this file's header comment
// for why. Shared by OnPointerDown (to refuse the click) and Draw (to render it
// visibly disabled) so the two can't disagree about which row that is.
bool IsLockedRow(std::uint16_t mask, std::uint32_t index)
{
	return (mask & (1u << index)) != 0 && std::popcount(mask) == 1;
}

// The single geometry function every hit-test/draw call below shares. bounds is the
// region the popup must stay fully inside (see Open's own comment) - every clamp below
// is relative to bounds' own edges, not the screen/window origin, so this works
// correctly regardless of where bounds itself sits on screen.
//
// Below the anchor (the chip) and right-aligned to it, so the box hangs off the chip and
// unfolds down-and-left over the form - see this file's own header comment. openAmount
// (0..1, eased - CGameSelectPopup::m_flOpenAmount) animates the height from 0 up to its
// full resting value while X/Y/W stay fixed, so the box visibly "folds out" downward from
// its own top edge; Draw's clip rect (scoped to this same growing rect) is what keeps
// not-yet-revealed rows - always laid out at their final position, never actually moved -
// from poking out past it mid-animation.
//
// Only the resting height is clamped against bounds, never the animating one: clamping the
// growing height would slide the box upward frame by frame as it unfolded, which reads as
// the popup drifting rather than opening.
Rect PopupRect(Rect anchor, std::uint32_t bannerCount, const CFontManager &fonts, Rect bounds, float openAmount)
{
	const float fullHeight =
		kPopupPadding * 2.0f + RowHeightFor(fonts) * static_cast<float>(std::max<std::uint32_t>(1, bannerCount));

	// Right edges flush, then clamped so a narrow column can't push the box off its left
	// side - the popup is wider than the chip, so it always overhangs to the left.
	float x = anchor.X + anchor.W - kPopupWidth;
	x = std::clamp(x, bounds.X + kWindowMargin,
				   std::max(bounds.X + kWindowMargin, bounds.X + bounds.W - kWindowMargin - kPopupWidth));

	const float y = std::clamp(anchor.Y + anchor.H + kAnchorGap, bounds.Y + kWindowMargin,
							   std::max(bounds.Y + kWindowMargin, bounds.Y + bounds.H - kWindowMargin - fullHeight));

	return Rect{x, y, kPopupWidth, fullHeight * openAmount};
}
} // namespace

CGameSelectPopup::CGameSelectPopup(CFontManager &fonts)
	: m_fonts(fonts)
{
}

void CGameSelectPopup::Open(std::uint16_t initialMask, const CBanner *pBanners, std::uint32_t bannerCount, Rect anchor,
							Rect bounds)
{
	m_uMask = initialMask;
	m_pBanners = pBanners;
	m_nBannerCount = bannerCount;
	m_anchor = anchor;
	m_bounds = bounds;
	m_bOpen = true;
}

void CGameSelectPopup::Close()
{
	m_bOpen = false;
}

void CGameSelectPopup::Update(float deltaSeconds)
{
	m_flOpenAmount = CAnimator::EaseToward(m_flOpenAmount, m_bOpen ? 1.0f : 0.0f, kOpenEaseRate, deltaSeconds);
	if (!m_bOpen && m_flOpenAmount < 0.002f) {
		m_flOpenAmount = 0.0f;
	}
}

bool CGameSelectPopup::OnPointerDown(float x, float y)
{
	if (!IsBlocking()) {
		return false;
	}
	const Rect popup = PopupRect(m_anchor, m_nBannerCount, m_fonts, m_bounds, m_flOpenAmount);
	if (!RectContainsPoint(popup, x, y)) {
		return false;
	}

	for (std::uint32_t i = 0; i < m_nBannerCount; i += 1) {
		if (RectContainsPoint(RowRect(popup, m_fonts, i), x, y)) {
			if (!IsLockedRow(m_uMask, i)) {
				m_uMask ^= static_cast<std::uint16_t>(1u << i);
			}
			break;
		}
	}
	return true;
}

ECursorKind CGameSelectPopup::GetDesiredCursor() const
{
	if (!IsBlocking()) {
		return ECursorKind::CURSOR_ARROW;
	}

	const Rect popup = PopupRect(m_anchor, m_nBannerCount, m_fonts, m_bounds, m_flOpenAmount);
	for (std::uint32_t i = 0; i < m_nBannerCount; i += 1) {
		// A locked row's click is silently ignored (see IsLockedRow's own comment) - no
		// hand for it, same as Draw's own hover-highlight skips it.
		if (!IsLockedRow(m_uMask, i) && RectContainsPoint(RowRect(popup, m_fonts, i), m_flMouseX, m_flMouseY)) {
			return ECursorKind::CURSOR_HAND;
		}
	}
	return ECursorKind::CURSOR_ARROW;
}

void CGameSelectPopup::Draw(CDrawList &drawList)
{
	if (!IsBlocking()) {
		return;
	}

	const Rect popup = PopupRect(m_anchor, m_nBannerCount, m_fonts, m_bounds, m_flOpenAmount);
	drawList.AddRectRoundedFilled(popup.X, popup.Y, popup.W, popup.H, CDrawList::UniformRadii(kPopupRadius),
								  kColorBorder);
	drawList.AddRectRoundedFilled(popup.X + 1.0f, popup.Y + 1.0f, popup.W - 2.0f, popup.H - 2.0f,
								  CDrawList::UniformRadii(kPopupRadius - 1.0f), kColorBg);

	// Belt-and-suspenders: PopupRect's own clamp against m_bounds is what actually keeps
	// this positioned correctly, but a real clip rect guarantees no row can ever paint
	// past the popup's own rounded border even in a pathological case (many banners in a
	// short bounds rect) where the position clamp alone can't make everything fit.
	drawList.PushClipRect(popup);

	const float iconSize = IconSizeFor(m_fonts);
	const CFont &secondary = m_fonts.GetSecondary();
	for (std::uint32_t i = 0; i < m_nBannerCount; i += 1) {
		const Rect row = RowRect(popup, m_fonts, i);
		const bool locked = IsLockedRow(m_uMask, i);
		const bool hover = !locked && RectContainsPoint(row, m_flMouseX, m_flMouseY);
		if (hover) {
			drawList.AddRectRoundedFilled(row.X + 4.0f, row.Y, row.W - 8.0f, row.H, CDrawList::UniformRadii(6.0f),
										  kColorHover);
		}

		// A locked row (the sole remaining checked game - see IsLockedRow) reads as
		// disabled: dimmed icon tint, dimmed label, a dimmer check color, and no hover
		// highlight above.
		const Color iconTint = locked ? Color{150, 150, 150, 255} : Color{255, 255, 255, 255};
		const Rect iconRect{row.X + kRowPaddingX, row.Y + (row.H - iconSize) * 0.5f, iconSize, iconSize};
		const CBanner &banner = m_pBanners[i];
		if (banner.pIcon != nullptr) {
			drawList.AddRectRoundedTextured(iconRect.X, iconRect.Y, iconRect.W, iconRect.H,
											CDrawList::UniformRadii(4.0f), banner.pIcon, iconTint);
		} else {
			drawList.AddRectRoundedFilled(iconRect.X, iconRect.Y, iconRect.W, iconRect.H, CDrawList::UniformRadii(4.0f),
										  locked ? ColorFadeAlpha(banner.Accent, 140) : banner.Accent);
		}

		DrawText(drawList, secondary, iconRect.X + iconRect.W + 10.0f,
				 row.Y + row.H * 0.5f + (secondary.GetAscent() + secondary.GetDescent()) * 0.5f, banner.Title,
				 locked ? kColorTextDisabled : kColorText);

		if ((m_uMask & (1u << i)) != 0) {
			DrawCheck(drawList, row, iconSize / 20.0f, locked ? kColorCheckDisabled : kColorCheck);
		}
	}

	drawList.PopClipRect();
}
