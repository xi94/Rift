#include "ui/context_menu.h"

#include <algorithm>

#include "ui/draw_list.h"
#include "ui/text.h"

namespace {
constexpr float kMenuWidth = 200.0f;
constexpr float kMenuItemHeight = 32.0f;
constexpr float kMenuPadding = 6.0f;
constexpr float kMenuRadius = 10.0f;
constexpr float kWindowMargin = 8.0f;

constexpr Color kColorBg{30, 30, 34, 255};
constexpr Color kColorBorder{60, 60, 66, 255};
constexpr Color kColorHover{54, 46, 78, 255};
constexpr Color kColorText{220, 220, 224, 255};

// The popup's own bounding rect - anchored at the (already window-clamped) open
// position, tall enough for exactly itemCount rows plus padding.
Rect MenuRect(float anchorX, float anchorY, std::uint32_t itemCount)
{
	return Rect{anchorX, anchorY, kMenuWidth, kMenuPadding * 2.0f + kMenuItemHeight * static_cast<float>(itemCount)};
}

// One item row's hit/draw rect, stacked by index below the top padding - shared by
// OnPointerUp and Draw so hit-testing and hover can't drift.
Rect ItemRect(Rect menu, std::uint32_t index)
{
	return Rect{menu.X, menu.Y + kMenuPadding + static_cast<float>(index) * kMenuItemHeight, menu.W, kMenuItemHeight};
}
} // namespace

CContextMenu::CContextMenu(CFontManager &fonts)
	: m_fonts(fonts)
{
}

void CContextMenu::Open(float x, float y, const ContextMenuItem *pItems, std::uint32_t itemCount, float windowW,
						float windowH)
{
	m_nItemCount = std::min(itemCount, kContextMenuMaxItems);
	for (std::uint32_t i = 0; i < m_nItemCount; i += 1) {
		m_aItems[i] = pItems[i];
	}

	const float h = kMenuPadding * 2.0f + kMenuItemHeight * static_cast<float>(m_nItemCount);
	m_flAnchorX = std::clamp(x, kWindowMargin, std::max(kWindowMargin, windowW - kWindowMargin - kMenuWidth));
	m_flAnchorY = std::clamp(y, kWindowMargin, std::max(kWindowMargin, windowH - kWindowMargin - h));
	m_bOpen = true;
}

void CContextMenu::Close()
{
	m_bOpen = false;
}

bool CContextMenu::OnPointerUp(float x, float y)
{
	if (!m_bOpen) {
		return false;
	}

	const Rect rect = MenuRect(m_flAnchorX, m_flAnchorY, m_nItemCount);
	m_pendingSelection = kContextMenuNoSelection;
	for (std::uint32_t i = 0; i < m_nItemCount; i += 1) {
		if (RectContainsPoint(ItemRect(rect, i), x, y)) {
			m_pendingSelection = m_aItems[i].Id;
			break;
		}
	}

	Close(); // click-anywhere-closes, hit or not
	return true;
}

ECursorKind CContextMenu::GetDesiredCursor() const
{
	if (!m_bOpen) {
		return ECursorKind::CURSOR_ARROW;
	}

	const Rect rect = MenuRect(m_flAnchorX, m_flAnchorY, m_nItemCount);
	for (std::uint32_t i = 0; i < m_nItemCount; i += 1) {
		if (RectContainsPoint(ItemRect(rect, i), m_flMouseX, m_flMouseY)) {
			return ECursorKind::CURSOR_HAND;
		}
	}
	return ECursorKind::CURSOR_ARROW;
}

std::uint32_t CContextMenu::ConsumeSelection()
{
	const std::uint32_t selection = m_pendingSelection;
	m_pendingSelection = kContextMenuNoSelection;
	return selection;
}

void CContextMenu::Draw(CDrawList &drawList)
{
	if (!m_bOpen) {
		return;
	}

	const Rect rect = MenuRect(m_flAnchorX, m_flAnchorY, m_nItemCount);
	drawList.AddRectRoundedFilled(rect.X, rect.Y, rect.W, rect.H, CDrawList::UniformRadii(kMenuRadius), kColorBorder);
	drawList.AddRectRoundedFilled(rect.X + 1.0f, rect.Y + 1.0f, rect.W - 2.0f, rect.H - 2.0f,
								  CDrawList::UniformRadii(kMenuRadius - 1.0f), kColorBg);

	const CFont &body = m_fonts.GetBody();
	for (std::uint32_t i = 0; i < m_nItemCount; i += 1) {
		const Rect row = ItemRect(rect, i);
		if (RectContainsPoint(row, m_flMouseX, m_flMouseY)) {
			drawList.AddRectRoundedFilled(row.X + 4.0f, row.Y, row.W - 8.0f, row.H, CDrawList::UniformRadii(6.0f),
										  kColorHover);
		}
		DrawText(drawList, body, row.X + 14.0f, row.Y + (row.H + body.GetAscent()) * 0.5f, m_aItems[i].Label,
				 kColorText);
	}
}
