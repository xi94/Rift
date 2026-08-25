#include "ui/tooltip.h"

#include <algorithm>
#include <cstring>

#include "core/animator.h"
#include "font_manager.h"
#include "gfx/font.h"
#include "ui/draw_list.h"
#include "ui/text.h"

namespace {
constexpr float kFadeRate = 22.0f;
constexpr float kPaddingX = 10.0f;
constexpr float kPaddingY = 6.0f;
constexpr float kRadius = 6.0f;
constexpr float kAnchorGap = 8.0f; // between the bubble's edge and the thing it describes
constexpr float kEdgeMargin = 6.0f;
// The bubble rises the last few pixels into place as it fades in - the same "slide a little
// while appearing" treatment CSettingsMenu's own popup uses, so it reads as arriving rather
// than blinking on.
constexpr float kRisePixels = 4.0f;

constexpr Color kColorBg{18, 18, 21, 246};
constexpr Color kColorBorder{72, 72, 80, 255};
constexpr Color kColorText{228, 228, 232, 255};
} // namespace

void CTooltip::Request(CStringView text, Rect anchor)
{
	const std::uint64_t length = std::min<std::uint64_t>(text.Length, kMaxTextLength);
	// A different string restarts the delay (a genuinely new thing to explain), but the
	// same string on a new anchor doesn't - see Request's own comment in tooltip.h.
	if (length != m_nTextLength || std::memcmp(m_szText, text.pData, length) != 0) {
		std::memcpy(m_szText, text.pData, length);
		m_szText[length] = '\0';
		m_nTextLength = length;
		if (m_flVisibleAmount <= 0.01f) {
			m_flHoverSeconds = 0.0f;
		}
	}

	m_anchor = anchor;
	m_bRequestedThisFrame = true;
}

void CTooltip::Update(float deltaSeconds)
{
	if (m_bRequestedThisFrame) {
		m_flHoverSeconds += deltaSeconds;
	} else {
		m_flHoverSeconds = 0.0f;
	}

	const float target = (m_bRequestedThisFrame && m_flHoverSeconds >= kTooltipDelaySeconds) ? 1.0f : 0.0f;
	m_flVisibleAmount = CAnimator::EaseToward(m_flVisibleAmount, target, kFadeRate, deltaSeconds);
	if (target == 0.0f && m_flVisibleAmount < 0.002f) {
		m_flVisibleAmount = 0.0f;
	}

	// Cleared here, not in Draw: Update is the one call every caller makes unconditionally,
	// so a caller that early-outs of its own Draw can't strand this flag set forever.
	m_bRequestedThisFrame = false;
}

void CTooltip::Reset()
{
	m_flHoverSeconds = 0.0f;
	m_flVisibleAmount = 0.0f;
	m_bRequestedThisFrame = false;
	m_nTextLength = 0;
	m_szText[0] = '\0';
}

void CTooltip::Draw(CDrawList &drawList, const CFontManager &fonts, Rect bounds, std::uint8_t alpha) const
{
	if (m_flVisibleAmount <= 0.001f || m_nTextLength == 0) {
		return;
	}

	const CFont &font = fonts.GetSecondary();
	const CStringView text{m_szText, m_nTextLength};
	const float textW = TextWidth(font, text);
	const float w = textW + kPaddingX * 2.0f;
	const float h = font.GetLineHeight() + kPaddingY * 2.0f;

	float x = m_anchor.X + (m_anchor.W - w) * 0.5f;
	float y = m_anchor.Y - h - kAnchorGap + (1.0f - m_flVisibleAmount) * kRisePixels;

	// Above by default, below only when above doesn't fit - a bubble that would paint off
	// the top of the owning panel is worse than one on the other side of its anchor.
	if (y < bounds.Y + kEdgeMargin) {
		y = m_anchor.Y + m_anchor.H + kAnchorGap - (1.0f - m_flVisibleAmount) * kRisePixels;
	}
	x = std::clamp(x, bounds.X + kEdgeMargin, std::max(bounds.X + kEdgeMargin, bounds.X + bounds.W - kEdgeMargin - w));

	const auto fade = static_cast<std::uint8_t>(static_cast<float>(alpha) * m_flVisibleAmount);

	drawList.AddRectRoundedFilled(x, y, w, h, CDrawList::UniformRadii(kRadius), ColorFadeAlpha(kColorBorder, fade));
	drawList.AddRectRoundedFilled(x + 1.0f, y + 1.0f, w - 2.0f, h - 2.0f, CDrawList::UniformRadii(kRadius - 1.0f),
								  ColorFadeAlpha(kColorBg, fade));
	// Same ascent+descent visual centering every other correctly-centered label in this
	// project uses (see ui/settings_menu.cpp's own RowBaselineY) rather than ascent alone,
	// which would sit the text low inside a box this short.
	DrawText(drawList, font, x + kPaddingX, y + h * 0.5f + (font.GetAscent() + font.GetDescent()) * 0.5f, text,
			 ColorFadeAlpha(kColorText, fade));
}
