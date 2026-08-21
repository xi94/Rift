#include "ui/carousel.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "asset_manager.h"
#include "core/animator.h"
#include "gfx/texture.h"
#include "ui/draw_list.h"
#include "ui/text.h"
#include "window.h"

namespace {
constexpr float kCardWidth = 220.0f;
constexpr float kCardHeight = 300.0f;
constexpr float kCardSpacing = 36.0f;
// Nominal stride: only used to convert a drag's pixel delta into scroll-offset units
// (OnPointerMove) - an approximate "how many pixels feel like one card" input
// sensitivity, not the actual on-screen spacing (see CumulativeSlotOffset for that).
constexpr float kCardStride = kCardWidth + kCardSpacing;
constexpr float kEaseRate = 12.0f; // higher = snappier settle after drag/click/wheel

constexpr float kViewModeTransitionEaseRate = 16.0f;
constexpr float kViewModeSlidePixels = 18.0f;
constexpr float kZoomPercentEaseRate =
	9.0f; // slower than most easing here on purpose - the List row-icon growth reads best gradual

// Card size continuously scales with m_flZoomPercent across Grid's 3 zoom stops (base,
// grown, max - see GridZoomT) rather than being fixed. Max preserves the base's aspect
// ratio (160:220) exactly - 224/160 == 308/220 == 1.4 - so the card shape doesn't
// distort as it grows.
constexpr float kGridCardWidthBase = 160.0f;
constexpr float kGridCardHeightBase = 220.0f;
constexpr float kGridCardWidthMax = 224.0f;
constexpr float kGridCardHeightMax = 308.0f;
constexpr float kGridGap = 24.0f;
constexpr float kGridPadding = 24.0f;
// How much a Grid card grows at full hover (m_aGridHoverScale == 1) - a noticeable-
// but-not-cartoonish "pop", well under half of kGridGap on a side so a grown card
// doesn't visually collide with its neighbors even mid-transition.
constexpr float kGridHoverScaleAmount = 0.06f;
constexpr float kGridHoverScaleEaseRate = 14.0f;

// Row icon size continuously scales with m_flZoomPercent across List's 3 zoom stops
// (base, grown, max - see ListZoomT) rather than being fixed.
constexpr float kListThumbSizeBase = 56.0f;
constexpr float kListThumbSizeMax = 96.0f;
constexpr float kListRowPaddingV = 14.0f; // above/below the icon within its row, at any size
constexpr float kListPadding = 16.0f;
constexpr float kListGap = 8.0f;

constexpr float kModeSwitcherHoldDuration = 0.7f; // seconds of no hover/activity before the flyout starts fading out
constexpr float kModeSwitcherEaseRate = 18.0f;
constexpr float kModeSwitcherWidth = 150.0f;
constexpr float kModeSwitcherPadding = 6.0f;
constexpr float kModeSwitcherRadius = 10.0f;
constexpr float kModeSwitcherMargin = 16.0f; // from the content area's right/bottom edge, on all sides
constexpr float kModeSwitcherSlidePixels = 8.0f;

// The status bar's own view-mode indicator (icon + "Carousel - Ctrl+Scroll to zoom",
// bottom-right) - see StatusBarContentRect/DrawStatusBarContent, the one place both its
// hit-test geometry and its actual drawing are computed, so they can't disagree.
constexpr float kStatusBarIconSize = 16.0f;
constexpr float kStatusBarIconGap = 8.0f;
constexpr float kStatusBarPadRight = 14.0f;

constexpr Color kModeSwitcherBg{26, 26, 30, 255};
constexpr Color kModeSwitcherBorder{58, 58, 64, 255};
// The active row's fill is a plain neutral grey plus a thin accent-colored bar on its
// left edge, not a solid accent-tinted block - "indicator, not a wash of color."
constexpr Color kModeSwitcherActiveRowFill{52, 52, 58, 255};
constexpr Color kModeSwitcherText{190, 190, 196, 255};
constexpr Color kModeSwitcherTextActive{240, 240, 244, 255};
constexpr float kModeSwitcherRowIconSize = 24.0f;
constexpr float kModeSwitcherRowIconGap = 10.0f;

// The vertical percentage slider running down the flyout's right edge - the live
// percentage text itself is the moving indicator (in a small pill), not a separate
// plain dot plus a separate badge. Bottom of the track is 0% (Carousel), top is 100%
// (List at max zoom) - a down-to-up slider.
constexpr float kModeSwitcherTrackColumnMin = 26.0f;
constexpr float kModeSwitcherTrackWidth = 4.0f;
constexpr float kModeSwitcherIndicatorHeight = 18.0f;
constexpr float kModeSwitcherIndicatorPaddingX = 7.0f;
constexpr Color kModeSwitcherTrackBg{58, 58, 64, 255};
constexpr Color kModeSwitcherIndicatorFill{150, 130, 215, 255};

// Carousel (stop 0) -> Grid (stops 1-3: base, grown, max) -> List (stops 4-6: base,
// grown, max) - see CCarousel::m_nZoomStop's own comment.
constexpr std::int32_t kGridZoomStopFirst = 1;
constexpr std::int32_t kGridZoomStopLast = 3;
constexpr std::int32_t kListZoomStopFirst = 4;
constexpr std::int32_t kListZoomStopLast = kCarouselZoomStopCount - 1;

// The flyout's 3 named rows are listed top-to-bottom in the same direction as the
// slider (bottom = 0% = Carousel, top = 100%+ = List) - List on top, Carousel on the
// bottom - rather than enum declaration order. Shared by DrawModeSwitcher and the
// row-click hit-test so they can't drift apart.
constexpr ECarouselViewMode kModeSwitcherRowMode[3] = {
	ECarouselViewMode::CAROUSEL_VIEW_MODE_LIST,
	ECarouselViewMode::CAROUSEL_VIEW_MODE_GRID,
	ECarouselViewMode::CAROUSEL_VIEW_MODE_CAROUSEL,
};
constexpr std::int32_t kModeSwitcherRowStop[3] = {
	kListZoomStopFirst, // List's own base stop, not a grown one - see the row-click doc comment
	kGridZoomStopFirst, // Grid's own base stop, not its grown one - same reasoning
	0,
};

// Which view mode a given zoom stop falls in (Carousel at 0, Grid across its band,
// List across the rest) - the one mapping SetZoomStop and ApplyZoomStop both use to
// detect when a stop change actually crosses into a different mode.
ECarouselViewMode ZoomStopViewMode(std::int32_t stop)
{
	if (stop <= 0) {
		return ECarouselViewMode::CAROUSEL_VIEW_MODE_CAROUSEL;
	}
	if (stop <= kGridZoomStopLast) {
		return ECarouselViewMode::CAROUSEL_VIEW_MODE_GRID;
	}
	return ECarouselViewMode::CAROUSEL_VIEW_MODE_LIST;
}

// A stop's exact position on the continuous 0..100 zoom scale, evenly spaced across
// kCarouselZoomStopCount stops - what m_flZoomPercent eases toward, and what the
// flyout's slider position/tick marks are derived from.
float ZoomStopPercent(std::int32_t stop)
{
	return static_cast<float>(stop) / static_cast<float>(kCarouselZoomStopCount - 1) * 100.0f;
}

// Widens the track column beyond kModeSwitcherTrackColumnMin if the widest possible
// indicator text ("100%", measured at the active Font Size rather than assumed)
// wouldn't otherwise fit - a fixed pixel column doesn't scale with Font Size, so a
// large enough size would otherwise spill the indicator pill past the panel's border.
float ModeSwitcherTrackColumnWidthFor(const CFont &secondary)
{
	const float indicatorW =
		TextWidth(secondary, StringViewFromCString("100%")) + kModeSwitcherIndicatorPaddingX * 2.0f;
	return std::max(kModeSwitcherTrackColumnMin, indicatorW + 6.0f);
}

// Inverse of the thumb-position math DrawModeSwitcher uses (track.Y is the top / 100%
// end) - nearest stop to wherever the pointer landed in the track.
std::int32_t ZoomStopForTrackPosition(Rect track, float y)
{
	const float t = std::clamp(1.0f - (y - track.Y) / track.H, 0.0f, 1.0f);
	return static_cast<std::int32_t>(std::round(t * static_cast<float>(kCarouselZoomStopCount - 1)));
}

// Grid occupies stops kGridZoomStopFirst..kGridZoomStopLast (base, grown, max); 0 at
// the moment Grid is first entered, 1 at its own max zoom.
float GridZoomT(float zoomPercent)
{
	const float lo = ZoomStopPercent(kGridZoomStopFirst);
	const float hi = ZoomStopPercent(kGridZoomStopLast);
	return std::clamp((zoomPercent - lo) / (hi - lo), 0.0f, 1.0f);
}

float GridCardWidthFor(float zoomPercent)
{
	return kGridCardWidthBase + (kGridCardWidthMax - kGridCardWidthBase) * GridZoomT(zoomPercent);
}

float GridCardHeightFor(float zoomPercent)
{
	return kGridCardHeightBase + (kGridCardHeightMax - kGridCardHeightBase) * GridZoomT(zoomPercent);
}

// List occupies stops kListZoomStopFirst..kListZoomStopLast (base, grown, max).
float ListZoomT(float zoomPercent)
{
	const float lo = ZoomStopPercent(kListZoomStopFirst);
	const float hi = ZoomStopPercent(kListZoomStopLast);
	return std::clamp((zoomPercent - lo) / (hi - lo), 0.0f, 1.0f);
}

float ListThumbSizeFor(float zoomPercent)
{
	return kListThumbSizeBase + (kListThumbSizeMax - kListThumbSizeBase) * ListZoomT(zoomPercent);
}

float ListRowHeightFor(float zoomPercent)
{
	return ListThumbSizeFor(zoomPercent) + kListRowPaddingV * 2.0f;
}

// Falls off to a floor scale within 2 slots either side of center; centered card reads
// clearly larger with a bright border, neighbors recede. Shared by GetBannerRect
// (which needs the scale at an exact slot) and CumulativeSlotOffset (which needs it at
// arbitrary points while integrating).
float CardScaleAtDistance(float distance)
{
	const float closeness = std::max(0.0f, 1.0f - std::min(distance, 2.0f) / 2.0f);
	return 0.90f + 0.28f * closeness;
}

constexpr std::int32_t kOffsetIntegrationSteps = 24;

// Horizontal offset from the center slot (0) to `slot`, accounting for each card's
// actual scaled width rather than a fixed per-slot stride - keeps the edge-to-edge gap
// a constant kCardSpacing regardless of distance from center. CardScaleAtDistance is
// piecewise-linear in distance, so the integral is exactly a trapezoid rule away from
// being exact.
float CumulativeSlotOffset(float slot)
{
	const float sign = slot < 0.0f ? -1.0f : 1.0f;
	const float magnitude = std::fabs(slot);
	if (magnitude < 0.0001f) {
		return 0.0f;
	}

	const float step = magnitude / static_cast<float>(kOffsetIntegrationSteps);
	float integral = 0.0f;
	float previousWidth = kCardWidth * CardScaleAtDistance(0.0f);
	for (std::int32_t i = 1; i <= kOffsetIntegrationSteps; i += 1) {
		const float width = kCardWidth * CardScaleAtDistance(step * static_cast<float>(i));
		integral += (previousWidth + width) * 0.5f * step; // trapezoid rule
		previousWidth = width;
	}

	return sign * (integral + magnitude * kCardSpacing);
}

// How many Grid columns fit across the given width at the given zoom percent - always
// at least 1.
std::uint32_t GridColumnsForWidth(float areaW, float zoomPercent)
{
	const float cardW = GridCardWidthFor(zoomPercent);
	const float usable = areaW - kGridPadding * 2.0f + kGridGap;
	return std::max<std::uint32_t>(1, static_cast<std::uint32_t>(usable / (cardW + kGridGap)));
}

// A card's visual state: neutral (dim gray border, no glow) or highlighted (hover, or
// Carousel mode's centered card - never a persistent "selected" look in Grid/List).
// The border itself is always plain white, never game-colored - only the glow hugging
// the card uses the game's own accent. `strong` (Carousel's centered card) gets a
// slightly larger/brighter glow than a plain hover does.
struct CardVisualState {
	Color BorderColor;
	float BorderThickness;
	float GlowSize; // how many px larger than the card the glow quad extends; 0 = no glow
	std::uint8_t GlowAlpha;
};

constexpr Color kCardNeutralBorder{90, 90, 96, 160};
constexpr Color kCardHighlightBorder{255, 255, 255, 235};

CardVisualState CardVisualStateFor(bool highlighted, bool strong)
{
	if (!highlighted) {
		return CardVisualState{kCardNeutralBorder, 2.0f, 0.0f, 0};
	}
	if (strong) {
		return CardVisualState{kCardHighlightBorder, 2.5f, 18.0f, 255};
	}
	return CardVisualState{kCardHighlightBorder, 2.0f, 14.0f, 225};
}

// The glow quad extends glowSize px past the card on every side - the banner-glow
// shader's rounded-box distance field (computed from the card's own rect) is what
// actually draws the ring-plus-orbiting-beam glow; this just positions/sizes/tints it
// with the game's accent.
void DrawCardGlow(CDrawList &drawList, BannerRect rect, Color color, float glowSize, std::uint8_t glowAlpha,
				  float cornerRadius, std::uint8_t frameAlpha)
{
	if (glowSize <= 0.0f || glowAlpha == 0) {
		return;
	}
	drawList.AddRectRoundedBannerGlow(rect.X, rect.Y, rect.W, rect.H, cornerRadius, glowSize,
									  ColorFadeAlpha(ColorFadeAlpha(color, glowAlpha), frameAlpha));
}

// Display name for a view mode - used by both the flyout's rows and the persistent
// chip so their labels can never drift from each other or from the mode itself.
CStringView ViewModeName(ECarouselViewMode mode)
{
	switch (mode) {
		case ECarouselViewMode::CAROUSEL_VIEW_MODE_CAROUSEL:
			return StringViewFromCString("Carousel");
		case ECarouselViewMode::CAROUSEL_VIEW_MODE_GRID:
			return StringViewFromCString("Grid");
		case ECarouselViewMode::CAROUSEL_VIEW_MODE_LIST:
			return StringViewFromCString("List");
	}
	return StringViewFromCString("");
}

// Grid/List/Carousel's own embedded icons (see asset_manager.h) - used by both the
// flyout's rows and the persistent chip below so the two always agree visually. color
// tints the texture (a plain white-on-transparent source), the same recolor-on-draw
// trick every other embedded icon in this project uses (see title_bar.cpp) rather than
// needing separate active/inactive art.
void DrawModeGlyph(CDrawList &drawList, const CAssetManager &assets, ECarouselViewMode mode, Rect box, Color color)
{
	CTexture *pTexture = nullptr;
	switch (mode) {
		case ECarouselViewMode::CAROUSEL_VIEW_MODE_CAROUSEL:
			pTexture = assets.GetIconCarousel();
			break;
		case ECarouselViewMode::CAROUSEL_VIEW_MODE_GRID:
			pTexture = assets.GetIconGrid();
			break;
		case ECarouselViewMode::CAROUSEL_VIEW_MODE_LIST:
			pTexture = assets.GetIconList();
			break;
	}
	if (pTexture == nullptr) {
		return;
	}
	drawList.AddRectRoundedTextured(box.X, box.Y, box.W, box.H, kCornerRadiiNone, pTexture, color);
}

// Vertically centers a small icon against a text baseline computed the same way
// DrawText calls elsewhere in this file do (baseline = area's vertical center +
// ascent/2) - centering the icon on that same raw geometric center instead reads as
// sitting noticeably above the text (a glyph's cap-height sits above the geometric
// center, not straddling it). ascentValue * 0.15 is an empirical correction, not a
// font-metric-exact one, tuned to match List/Grid's row text.
float IconCenterYFor(float areaCenterY, float ascentValue)
{
	return areaCenterY + ascentValue * 0.15f;
}
} // namespace

CCarousel::CCarousel(CFontManager &fonts, CAssetManager &assets)
	: m_fonts(fonts)
	, m_assets(assets)
{
}

void CCarousel::AddBanner(CStringView title, CTexture *pTexture, CTexture *pIcon, Color accent)
{
	assert(m_nBannerCount < kCarouselMaxBanners);

	CBanner &banner = m_aBanners[m_nBannerCount];
	banner.Title = title;
	banner.pTexture = pTexture;
	banner.TextureAspect = (pTexture != nullptr && pTexture->GetHeight() > 0)
							   ? static_cast<float>(pTexture->GetWidth()) / static_cast<float>(pTexture->GetHeight())
							   : 1.0f;
	banner.pIcon = pIcon;
	banner.Accent = accent;
	banner.AccountCount = 0;
	m_nBannerCount += 1;
}

void CCarousel::AddAccount(std::uint32_t bannerIndex, CStringView username, CStringView note, CStringView password)
{
	assert(bannerIndex < m_nBannerCount);
	CBanner &banner = m_aBanners[bannerIndex];
	assert(banner.AccountCount < kCarouselMaxAccountsPerBanner);
	banner.Accounts[banner.AccountCount].Init(username, note, password);
	banner.AccountCount += 1;
}

void CCarousel::UpdateAccount(std::uint32_t bannerIndex, std::uint32_t accountIndex, CStringView username,
							  CStringView note, CStringView password)
{
	assert(bannerIndex < m_nBannerCount);
	CBanner &banner = m_aBanners[bannerIndex];
	assert(accountIndex < banner.AccountCount);
	banner.Accounts[accountIndex].Init(username, note, password);
}

void CCarousel::RemoveAccount(std::uint32_t bannerIndex, std::uint32_t accountIndex)
{
	assert(bannerIndex < m_nBannerCount);
	CBanner &banner = m_aBanners[bannerIndex];
	assert(accountIndex < banner.AccountCount);
	for (std::uint32_t i = accountIndex; i + 1 < banner.AccountCount; i += 1) {
		banner.Accounts[i] = banner.Accounts[i + 1];
	}
	banner.AccountCount -= 1;
}

std::uint32_t CCarousel::GetVisibleAccounts(std::uint32_t bannerIndex, VisibleAccountRef *pOut) const
{
	std::uint32_t count = 0;
	if (bannerIndex >= m_nBannerCount) {
		return count;
	}

	const CBanner &own = m_aBanners[bannerIndex];
	for (std::uint32_t i = 0; i < own.AccountCount; i += 1) {
		pOut[count++] = VisibleAccountRef{bannerIndex, i};
	}

	for (std::uint32_t b = 0; b < m_nBannerCount; b += 1) {
		if (b == bannerIndex) {
			continue;
		}
		const CBanner &other = m_aBanners[b];
		for (std::uint32_t i = 0; i < other.AccountCount; i += 1) {
			const std::uint16_t mask = other.Accounts[i].GetEffectiveVisibleMask(b);
			if ((mask & (1u << bannerIndex)) != 0) {
				pOut[count++] = VisibleAccountRef{b, i};
			}
		}
	}

	assert(count <= kCarouselMaxVisibleAccounts); // true worst case - see the constant's doc comment
	return count;
}

float CCarousel::ClampTarget(float value) const
{
	if (m_nBannerCount == 0) {
		return 0.0f;
	}
	return std::clamp(value, 0.0f, static_cast<float>(m_nBannerCount - 1));
}

BannerRect CCarousel::GridBannerRect(std::uint32_t index) const
{
	const float areaX = m_vecBounds.X;
	const float areaY = m_vecBounds.Y;
	const float areaW = m_vecBounds.W;

	const float cardW = GridCardWidthFor(m_flZoomPercent);
	const float cardH = GridCardHeightFor(m_flZoomPercent);
	const std::uint32_t columns = GridColumnsForWidth(areaW, m_flZoomPercent);
	const std::uint32_t col = index % columns;
	const std::uint32_t row = index / columns;
	const float totalWidth = static_cast<float>(columns) * cardW + static_cast<float>(columns - 1) * kGridGap;
	const float startX = areaX + (areaW - totalWidth) * 0.5f;
	return BannerRect{
		startX + static_cast<float>(col) * (cardW + kGridGap),
		areaY + kGridPadding + static_cast<float>(row) * (cardH + kGridGap) - m_wrapScroll.m_flScrollOffset,
		cardW,
		cardH,
	};
}

float CCarousel::GridContentHeight() const
{
	if (m_nBannerCount == 0) {
		return 0.0f;
	}
	const std::uint32_t columns = GridColumnsForWidth(m_vecBounds.W, m_flZoomPercent);
	const std::uint32_t rows = (m_nBannerCount + columns - 1) / columns;
	const float cardH = GridCardHeightFor(m_flZoomPercent);
	return kGridPadding * 2.0f + static_cast<float>(rows) * cardH + static_cast<float>(rows - 1) * kGridGap;
}

BannerRect CCarousel::ListBannerRect(std::uint32_t index) const
{
	const float rowHeight = ListRowHeightFor(m_flZoomPercent);
	return BannerRect{
		m_vecBounds.X + kListPadding,
		m_vecBounds.Y + kListPadding + static_cast<float>(index) * (rowHeight + kListGap) -
			m_wrapScroll.m_flScrollOffset,
		m_vecBounds.W - kListPadding * 2.0f,
		rowHeight,
	};
}

float CCarousel::ListContentHeight() const
{
	if (m_nBannerCount == 0) {
		return 0.0f;
	}
	const float rowHeight = ListRowHeightFor(m_flZoomPercent);
	return kListPadding * 2.0f + static_cast<float>(m_nBannerCount) * rowHeight +
		   static_cast<float>(m_nBannerCount - 1) * kListGap;
}

float CCarousel::WrapContentHeight(ECarouselViewMode mode) const
{
	if (mode == ECarouselViewMode::CAROUSEL_VIEW_MODE_GRID) {
		return GridContentHeight();
	}
	if (mode == ECarouselViewMode::CAROUSEL_VIEW_MODE_LIST) {
		return ListContentHeight();
	}
	return 0.0f;
}

Rect CCarousel::ScrollbarTrackRect() const
{
	return Rect{m_vecBounds.X + m_vecBounds.W - kScrollbarWidth - 8.0f, m_vecBounds.Y + 8.0f, kScrollbarWidth,
				m_vecBounds.H - 16.0f};
}

float CCarousel::ModeSwitcherTrackColumnWidth() const
{
	return ModeSwitcherTrackColumnWidthFor(m_fonts.GetSecondary());
}

float CCarousel::ModeSwitcherRowsContentHeight() const
{
	return (m_fonts.GetBody().GetLineHeight() + 10.0f) * static_cast<float>(kCarouselViewModeCount);
}

Rect CCarousel::ModeSwitcherPanelRect() const
{
	const float rowsH = ModeSwitcherRowsContentHeight();
	const float h = kModeSwitcherPadding * 2.0f + rowsH;
	const float w = kModeSwitcherWidth + ModeSwitcherTrackColumnWidth();
	const float x = m_vecBounds.X + m_vecBounds.W - w - kModeSwitcherMargin;
	const float y = m_vecBounds.Y + m_vecBounds.H - h - kModeSwitcherMargin;
	return Rect{x, y, w, h};
}

Rect CCarousel::ModeSwitcherRowsRect(Rect panel)
{
	return Rect{panel.X, panel.Y, kModeSwitcherWidth, panel.H};
}

Rect CCarousel::ModeSwitcherTrackRect(Rect panel) const
{
	const float trackColumn = ModeSwitcherTrackColumnWidth();
	return Rect{
		panel.X + panel.W - trackColumn * 0.5f - kModeSwitcherTrackWidth * 0.5f,
		panel.Y + kModeSwitcherPadding + kModeSwitcherIndicatorHeight * 0.5f,
		kModeSwitcherTrackWidth,
		panel.H - kModeSwitcherPadding * 2.0f - kModeSwitcherIndicatorHeight,
	};
}

// The app's own shared bottom chrome strip - see window.h's kStatusBarHeight for why
// this is a plain derivation from m_vecBounds rather than a value passed in every frame
// the way most of this widget's own bounds-relative geometry doesn't need to be:
// m_vecBounds itself already stops exactly where the status bar starts (main.cpp sets
// both from the same window height), so the strip's own rect falls right out of that.
Rect CCarousel::StatusBarRect() const
{
	return Rect{m_vecBounds.X, m_vecBounds.Y + m_vecBounds.H, m_vecBounds.W, kStatusBarHeight};
}

// The status bar's own icon+label bounding box, right-aligned - the single geometry
// function DrawStatusBarContent (drawing) and IsMouseOverModeSwitcher (hit-testing, so
// hovering it opens the flyout) both share, so they can never disagree about where it
// actually is.
Rect CCarousel::StatusBarContentRect() const
{
	const Rect statusBar = StatusBarRect();

	char buffer[48];
	const CStringView name = ViewModeName(m_viewMode);
	const int written = std::snprintf(buffer, sizeof(buffer), "%.*s  -  Ctrl+Scroll to zoom",
									  static_cast<int>(name.Length), name.pData);
	const CStringView text{buffer, written > 0 ? static_cast<std::uint64_t>(written) : 0};
	const float textW = TextWidth(m_fonts.GetSecondary(), text);

	const float contentW = kStatusBarIconSize + kStatusBarIconGap + textW;
	return Rect{statusBar.X + statusBar.W - kStatusBarPadRight - contentW, statusBar.Y, contentW, statusBar.H};
}

Rect CCarousel::ModeSwitcherHoverRect() const
{
	// The union of the flyout panel and the status bar indicator that opens it, spanning
	// the gap between them - once the flyout has started opening, hovering anywhere in
	// this combined region (not just back over the indicator itself) keeps it open, so
	// moving the pointer up from the indicator into the now-visible panel above it never
	// reads as "left the hover zone." See IsMouseOverModeSwitcher for why this is only
	// half the story - before the flyout is visible, only the indicator itself triggers it.
	const Rect panel = ModeSwitcherPanelRect();
	const Rect indicator = StatusBarContentRect();
	const float left = std::min(panel.X, indicator.X);
	const float top = panel.Y;
	const float right = std::max(panel.X + panel.W, indicator.X + indicator.W);
	const float bottom = indicator.Y + indicator.H;
	return Rect{left, top, right - left, bottom - top};
}

bool CCarousel::IsMouseOverModeSwitcher(float mouseX, float mouseY) const
{
	// The status bar indicator alone is the only thing that can *open* the flyout from
	// cold - hovering where the (not yet visible) panel would eventually appear
	// shouldn't summon it out of nowhere. Once it's actually opening/open,
	// ModeSwitcherHoverRect's broader union keeps it that way even while the pointer
	// crosses the gap between the indicator and the panel above it.
	if (RectContainsPoint(StatusBarContentRect(), mouseX, mouseY)) {
		return true;
	}
	return IsModeSwitcherVisible() && RectContainsPoint(ModeSwitcherHoverRect(), mouseX, mouseY);
}

void CCarousel::SetZoomStop(std::int32_t stop)
{
	stop = std::clamp(stop, 0, kCarouselZoomStopCount - 1);
	if (stop != m_nZoomStop) {
		m_nZoomStop = stop;
		const ECarouselViewMode nextMode = ZoomStopViewMode(stop);
		if (nextMode != m_viewMode) {
			m_transitionFromMode = m_viewMode;
			m_viewMode = nextMode;
			m_flTransitionAmount = 1.0f;
			m_wrapScroll =
				CScrollable{}; // the new mode's content height has nothing to do with the old one's scroll position
		}
	}
	m_flModeSwitcherHoldSeconds = kModeSwitcherHoldDuration;
}

void CCarousel::AdjustZoomStop(float wheelDelta)
{
	if (wheelDelta == 0.0f) {
		return;
	}
	// Scroll up moves forward through the zoom stops (Carousel -> Grid -> List -> List
	// grown -> List max); scroll down reverses - starts at Carousel.
	const std::int32_t direction = wheelDelta > 0.0f ? 1 : -1;
	SetZoomStop(m_nZoomStop + direction);
}

void CCarousel::ApplyZoomStop(std::int32_t stop)
{
	stop = std::clamp(stop, 0, kCarouselZoomStopCount - 1);
	m_nZoomStop = stop;
	m_flZoomPercent = ZoomStopPercent(stop);
	m_viewMode = ZoomStopViewMode(stop);
	m_transitionFromMode = m_viewMode;
	m_flTransitionAmount = 0.0f;
}

bool CCarousel::ModeSwitcherOnPointerDown(float x, float y)
{
	if (!IsModeSwitcherVisible()) {
		return false;
	}

	const Rect panel = ModeSwitcherPanelRect();
	if (!RectContainsPoint(panel, x, y)) {
		return false;
	}
	m_bModeSwitcherPointerCaptured = true;

	const Rect rows = ModeSwitcherRowsRect(panel);
	const float rowHeight = m_fonts.GetBody().GetLineHeight() + 10.0f;
	for (std::uint32_t i = 0; i < kCarouselViewModeCount; i += 1) {
		const Rect row{
			rows.X + kModeSwitcherPadding,
			rows.Y + kModeSwitcherPadding + rowHeight * static_cast<float>(i),
			rows.W - kModeSwitcherPadding * 2.0f,
			rowHeight,
		};
		if (RectContainsPoint(row, x, y)) {
			SetZoomStop(kModeSwitcherRowStop[i]);
			return true;
		}
	}

	// Generous grab width around the thin visual track, same reasoning CScrollable's own
	// thumb hit-testing uses - a 4px-wide target is unusable to actually click.
	const Rect track = ModeSwitcherTrackRect(panel);
	const Rect trackGrab{track.X - 10.0f, panel.Y, track.W + 20.0f, panel.H};
	if (RectContainsPoint(trackGrab, x, y)) {
		m_modeSwitcherDrag.Begin(x, y);
		SetZoomStop(ZoomStopForTrackPosition(track, y));
		return true;
	}

	return true; // still somewhere inside the panel (padding) - swallow rather than fall through
}

bool CCarousel::ModeSwitcherOnPointerMove(float x, float y)
{
	if (!m_modeSwitcherDrag.IsPressed()) {
		return false;
	}
	m_modeSwitcherDrag.Update(x, y);
	const Rect panel = ModeSwitcherPanelRect();
	SetZoomStop(ZoomStopForTrackPosition(ModeSwitcherTrackRect(panel), y));
	return true;
}

bool CCarousel::ModeSwitcherOnPointerUp()
{
	const bool wasCaptured = m_bModeSwitcherPointerCaptured;
	m_bModeSwitcherPointerCaptured = false;
	m_modeSwitcherDrag.End();
	return wasCaptured;
}

bool CCarousel::OnPointerDown(float x, float y)
{
	if (ModeSwitcherOnPointerDown(x, y)) {
		return true;
	}

	if (m_viewMode != ECarouselViewMode::CAROUSEL_VIEW_MODE_CAROUSEL) {
		// Grid/List mode has no card-drag-to-scroll (that's Carousel mode's own thing) -
		// the only draggable surface is the vertical scrollbar thumb.
		const float contentHeight = WrapContentHeight(m_viewMode);
		return m_wrapScroll.OnPointerDown(x, y, ScrollbarTrackRect(), contentHeight, m_vecBounds.H);
	}

	m_cardDrag.Begin(x, y);
	m_flDragStartScrollOffset = m_flScrollOffset;
	return true;
}

bool CCarousel::OnPointerMove(float x, float y)
{
	if (ModeSwitcherOnPointerMove(x, y)) {
		return true;
	}

	if (m_viewMode != ECarouselViewMode::CAROUSEL_VIEW_MODE_CAROUSEL) {
		const float contentHeight = WrapContentHeight(m_viewMode);
		m_wrapScroll.OnPointerMove(y, ScrollbarTrackRect(), contentHeight, m_vecBounds.H);
		return m_wrapScroll.IsDragging();
	}
	if (!m_cardDrag.IsPressed()) {
		return false;
	}

	m_cardDrag.Update(x, y);

	// 1:1 tracking with the pointer while dragging; easing only takes over after release.
	const float newOffset = m_flDragStartScrollOffset - m_cardDrag.DeltaX() / kCardStride;
	m_flScrollOffset = newOffset;
	m_flTargetScrollOffset = newOffset;
	return true;
}

bool CCarousel::OnPointerUp(float x, float y)
{
	if (ModeSwitcherOnPointerUp()) {
		return true;
	}

	if (m_viewMode != ECarouselViewMode::CAROUSEL_VIEW_MODE_CAROUSEL) {
		// A release ending a scrollbar-thumb drag must not also be treated as a card click
		// that opens the modal.
		if (m_wrapScroll.IsDragging()) {
			m_wrapScroll.OnPointerUp();
			return true;
		}
		// No drag/selection state to update in Grid/List - a plain hit-test; the caller
		// (main.cpp) opens the modal immediately on any hit in these modes. Still
		// "consumed" either way since this widget owns the whole area.
		m_pendingClickBanner = PendingHitFromHitTest(HitTest(x, y));
		return true;
	}

	if (!m_cardDrag.IsPressed()) {
		return false;
	}
	const bool dragMoved = m_cardDrag.HasMoved();
	m_cardDrag.End();

	if (dragMoved) {
		m_flTargetScrollOffset = ClampTarget(std::round(m_flScrollOffset));
		return true;
	}

	// A click, not a drag: only change the selection if it actually landed on a banner.
	const std::int32_t hitIndex = HitTest(x, y);
	if (hitIndex >= 0) {
		m_flTargetScrollOffset = ClampTarget(static_cast<float>(hitIndex));
	}
	m_pendingClickBanner = PendingHitFromHitTest(hitIndex);
	return true;
}

bool CCarousel::OnRightPointerUp(float x, float y)
{
	if (!m_bIsHovered) {
		return false;
	}
	// A miss is still a meaningful "somewhere in the carousel" result here, not "nothing
	// pending" - see PendingHit's own doc comment in core/types.h.
	m_pendingRightClickBanner = PendingHitFromHitTest(HitTest(x, y));
	return true;
}

PendingHit CCarousel::ConsumePendingRightClick()
{
	const PendingHit pending = m_pendingRightClickBanner;
	m_pendingRightClickBanner = PendingHit{};
	return pending;
}

PendingHit CCarousel::ConsumePendingClick()
{
	const PendingHit pending = m_pendingClickBanner;
	m_pendingClickBanner = PendingHit{};
	return pending;
}

bool CCarousel::OnScroll(float x, float y, float wheelDelta)
{
	(void)x;
	(void)y;
	if (m_viewMode == ECarouselViewMode::CAROUSEL_VIEW_MODE_CAROUSEL) {
		m_flTargetScrollOffset = ClampTarget(m_flTargetScrollOffset - wheelDelta);
		return true;
	}
	const float contentHeight = WrapContentHeight(m_viewMode);
	m_wrapScroll.OnScroll(wheelDelta, contentHeight, m_vecBounds.H);
	return true;
}

ECursorKind CCarousel::GetDesiredCursor() const
{
	if (m_modeSwitcherDrag.IsPressed()) {
		return ECursorKind::CURSOR_DRAG;
	}
	if (m_viewMode != ECarouselViewMode::CAROUSEL_VIEW_MODE_CAROUSEL && m_wrapScroll.IsDragging()) {
		return ECursorKind::CURSOR_DRAG;
	}
	if (m_viewMode == ECarouselViewMode::CAROUSEL_VIEW_MODE_CAROUSEL && m_cardDrag.IsPressed()) {
		return ECursorKind::CURSOR_DRAG;
	}

	if (m_nBannerCount > 0 && RectContainsPoint(StatusBarContentRect(), m_flMouseX, m_flMouseY)) {
		return ECursorKind::CURSOR_HAND;
	}

	if (IsModeSwitcherVisible()) {
		const Rect panel = ModeSwitcherPanelRect();
		if (RectContainsPoint(panel, m_flMouseX, m_flMouseY)) {
			const Rect rows = ModeSwitcherRowsRect(panel);
			const float rowHeight = m_fonts.GetBody().GetLineHeight() + 10.0f;
			for (std::uint32_t i = 0; i < kCarouselViewModeCount; i += 1) {
				const Rect row{
					rows.X + kModeSwitcherPadding,
					rows.Y + kModeSwitcherPadding + rowHeight * static_cast<float>(i),
					rows.W - kModeSwitcherPadding * 2.0f,
					rowHeight,
				};
				if (RectContainsPoint(row, m_flMouseX, m_flMouseY)) {
					return ECursorKind::CURSOR_HAND;
				}
			}
			const Rect track = ModeSwitcherTrackRect(panel);
			const Rect trackGrab{track.X - 10.0f, panel.Y, track.W + 20.0f, panel.H};
			if (RectContainsPoint(trackGrab, m_flMouseX, m_flMouseY)) {
				return ECursorKind::CURSOR_HAND;
			}
		}
	}

	if (HitTest(m_flMouseX, m_flMouseY) >= 0) {
		return ECursorKind::CURSOR_HAND;
	}

	if (m_viewMode != ECarouselViewMode::CAROUSEL_VIEW_MODE_CAROUSEL) {
		const float contentHeight = WrapContentHeight(m_viewMode);
		if (CScrollable::IsVisible(contentHeight, m_vecBounds.H) &&
			RectContainsPoint(ScrollbarTrackRect(), m_flMouseX, m_flMouseY)) {
			return ECursorKind::CURSOR_HAND;
		}
	}

	return ECursorKind::CURSOR_ARROW;
}

void CCarousel::Update(float deltaSeconds)
{
	m_flScrollOffset = CAnimator::EaseToward(m_flScrollOffset, m_flTargetScrollOffset, kEaseRate, deltaSeconds);
	m_flTransitionAmount = CAnimator::EaseToward(m_flTransitionAmount, 0.0f, kViewModeTransitionEaseRate, deltaSeconds);
	if (m_flTransitionAmount < 0.002f) {
		m_flTransitionAmount = 0.0f;
	}
	m_flZoomPercent =
		CAnimator::EaseToward(m_flZoomPercent, ZoomStopPercent(m_nZoomStop), kZoomPercentEaseRate, deltaSeconds);
	m_wrapScroll.Update(deltaSeconds);

	// Grid mode's per-card hover "pop" - only one index (if any) actually hit-tests as
	// hovered, but every card's own scale eases independently toward its target so an
	// outgoing card shrinks back at the same time an incoming one grows, rather than
	// snapping.
	const std::int32_t gridHoveredIndex =
		m_viewMode == ECarouselViewMode::CAROUSEL_VIEW_MODE_GRID ? HitTest(m_flMouseX, m_flMouseY) : -1;
	for (std::uint32_t i = 0; i < m_nBannerCount; i += 1) {
		const float target = static_cast<std::int32_t>(i) == gridHoveredIndex ? 1.0f : 0.0f;
		m_aGridHoverScale[i] =
			CAnimator::EaseToward(m_aGridHoverScale[i], target, kGridHoverScaleEaseRate, deltaSeconds);
	}

	const bool modeSwitcherHovered = IsMouseOverModeSwitcher(m_flMouseX, m_flMouseY);
	if (modeSwitcherHovered) {
		m_flModeSwitcherHoldSeconds = kModeSwitcherHoldDuration;
	} else {
		m_flModeSwitcherHoldSeconds = std::max(0.0f, m_flModeSwitcherHoldSeconds - deltaSeconds);
	}
	const float modeSwitcherTarget = m_flModeSwitcherHoldSeconds > 0.0f ? 1.0f : 0.0f;
	m_flModeSwitcherVisibleAmount =
		CAnimator::EaseToward(m_flModeSwitcherVisibleAmount, modeSwitcherTarget, kModeSwitcherEaseRate, deltaSeconds);
	if (m_flModeSwitcherVisibleAmount < 0.002f) {
		m_flModeSwitcherVisibleAmount = 0.0f;
	}
}

std::int32_t CCarousel::GetSelectedIndex() const
{
	return static_cast<std::int32_t>(ClampTarget(std::round(m_flTargetScrollOffset)));
}

BannerRect CCarousel::GetBannerRect(std::uint32_t index) const
{
	const float centerX = m_vecBounds.X + m_vecBounds.W * 0.5f;
	const float slot = static_cast<float>(index) - m_flScrollOffset;
	const float distance = std::fabs(slot);

	const float scale = CardScaleAtDistance(distance);
	const float w = kCardWidth * scale;
	const float h = kCardHeight * scale;
	const float cx = centerX + CumulativeSlotOffset(slot);

	return BannerRect{
		cx - w * 0.5f,
		m_vecBounds.Y + (m_vecBounds.H - h) * 0.5f,
		w,
		h,
	};
}

std::int32_t CCarousel::HitTest(float x, float y) const
{
	const float areaX = m_vecBounds.X;
	const float areaY = m_vecBounds.Y;
	const float areaW = m_vecBounds.W;
	const float areaH = m_vecBounds.H;

	if (m_viewMode != ECarouselViewMode::CAROUSEL_VIEW_MODE_CAROUSEL) {
		for (std::uint32_t i = 0; i < m_nBannerCount; i += 1) {
			const BannerRect rect =
				m_viewMode == ECarouselViewMode::CAROUSEL_VIEW_MODE_GRID ? GridBannerRect(i) : ListBannerRect(i);
			if (x < rect.X || x >= rect.X + rect.W || y < rect.Y || y >= rect.Y + rect.H) {
				continue;
			}
			if (y < areaY || y >= areaY + areaH) {
				continue; // scrolled out of the visible area
			}
			return static_cast<std::int32_t>(i);
		}
		return -1;
	}

	std::int32_t bestIndex = -1;
	float bestDistance = 0.0f;

	for (std::uint32_t i = 0; i < m_nBannerCount; i += 1) {
		const BannerRect rect = GetBannerRect(i);
		if (x < rect.X || x >= rect.X + rect.W || y < rect.Y || y >= rect.Y + rect.H) {
			continue;
		}

		const float distance = std::fabs(static_cast<float>(i) - m_flScrollOffset);
		if (bestIndex < 0 || distance < bestDistance) {
			bestIndex = static_cast<std::int32_t>(i);
			bestDistance = distance;
		}
	}

	(void)areaX;
	(void)areaW;
	return bestIndex;
}

void CCarousel::DrawCarouselMode(CDrawList &drawList, std::uint8_t alpha, float yOffset, float mouseX,
								 float mouseY) const
{
	const float areaX = m_vecBounds.X;
	const float areaW = m_vecBounds.W;

	for (std::uint32_t i = 0; i < m_nBannerCount; i += 1) {
		BannerRect rect = GetBannerRect(i);
		rect.Y += yOffset;
		if (rect.X + rect.W < areaX || rect.X > areaX + areaW) {
			continue;
		}

		const CBanner &banner = m_aBanners[i];
		const float slot = static_cast<float>(i) - m_flScrollOffset;
		const bool isSelected = std::fabs(slot) < 0.5f;
		const bool isHovered =
			!isSelected && mouseX >= rect.X && mouseX < rect.X + rect.W && mouseY >= rect.Y && mouseY < rect.Y + rect.H;
		const CardVisualState state = CardVisualStateFor(isSelected || isHovered, isSelected);

		// Border-then-image, both rounded: a rounded stroke isn't a primitive this project
		// has, so the border is a full-size rounded rect in BorderColor with the (smaller-
		// radius) image rounded rect drawn on top, inset by the border thickness.
		const auto radii = CDrawList::UniformRadii(kCarouselCardCornerRadius);
		DrawCardGlow(drawList, rect, banner.Accent, state.GlowSize, state.GlowAlpha, kCarouselCardCornerRadius, alpha);
		drawList.AddRectRoundedFilled(rect.X, rect.Y, rect.W, rect.H, radii, ColorFadeAlpha(state.BorderColor, alpha));
		if (banner.pTexture != nullptr) {
			const auto innerRadii = CDrawList::UniformRadii(kCarouselCardCornerRadius - state.BorderThickness);
			const float innerW = rect.W - state.BorderThickness * 2.0f;
			const float innerH = rect.H - state.BorderThickness * 2.0f;
			// Cover-fit crop instead of stretch - see ComputeCoverUv's doc comment.
			const UvRect uv = CDrawList::ComputeCoverUv(innerW, innerH, banner.TextureAspect, 1.0f);
			drawList.AddRectRoundedTexturedUv(rect.X + state.BorderThickness, rect.Y + state.BorderThickness, innerW,
											  innerH, innerRadii, uv.U0, uv.V0, uv.U1, uv.V1, banner.pTexture,
											  ColorFadeAlpha(Color{255, 255, 255, 255}, alpha));
		}
	}

	// m_vecBounds spans the full window width with no side margin, so a card mid-scroll
	// touches the window's left/right edge exactly instead of stopping short of it - a
	// gradient strip from the window's own clear color (main.cpp's kColorBackground) down
	// to transparent reads as an intentional vignette instead of a card visibly bleeding
	// into the border.
	constexpr float kEdgeFadeWidth = 64.0f;
	constexpr Color kEdgeFadeColor{18, 18, 20, 255};
	const Color fadeOpaque = ColorFadeAlpha(kEdgeFadeColor, alpha);
	const Color fadeClear = ColorFadeAlpha(kEdgeFadeColor, 0);
	drawList.AddRectGradientCorners(areaX, m_vecBounds.Y, kEdgeFadeWidth, m_vecBounds.H, fadeOpaque, fadeClear,
									fadeOpaque, fadeClear);
	drawList.AddRectGradientCorners(areaX + areaW - kEdgeFadeWidth, m_vecBounds.Y, kEdgeFadeWidth, m_vecBounds.H,
									fadeClear, fadeOpaque, fadeClear, fadeOpaque);
}

void CCarousel::DrawGridMode(CDrawList &drawList, std::uint8_t alpha, float yOffset, float mouseX, float mouseY) const
{
	const Rect region = m_vecBounds;

	drawList.PushClipRect(region);
	for (std::uint32_t i = 0; i < m_nBannerCount; i += 1) {
		BannerRect rect = GridBannerRect(i);
		rect.Y += yOffset;
		if (rect.Y + rect.H < region.Y || rect.Y > region.Y + region.H) {
			continue; // cull; the clip rect above also guards correctness
		}

		const CBanner &banner = m_aBanners[i];
		// No persistent "selected" look here - only ever the actively-hovered card gets
		// highlighted.
		const bool isHovered =
			mouseX >= rect.X && mouseX < rect.X + rect.W && mouseY >= rect.Y && mouseY < rect.Y + rect.H;
		const CardVisualState state = CardVisualStateFor(isHovered, false);

		// Grows around the card's own center - see kGridHoverScaleAmount's doc comment for
		// why this can't collide with neighboring cards even mid-transition.
		const float hoverScale = 1.0f + kGridHoverScaleAmount * m_aGridHoverScale[i];
		if (hoverScale != 1.0f) {
			const float cx = rect.X + rect.W * 0.5f;
			const float cy = rect.Y + rect.H * 0.5f;
			rect.W *= hoverScale;
			rect.H *= hoverScale;
			rect.X = cx - rect.W * 0.5f;
			rect.Y = cy - rect.H * 0.5f;
		}

		const auto radii = CDrawList::UniformRadii(kCarouselCardCornerRadius);
		DrawCardGlow(drawList, rect, banner.Accent, state.GlowSize, state.GlowAlpha, kCarouselCardCornerRadius, alpha);
		drawList.AddRectRoundedFilled(rect.X, rect.Y, rect.W, rect.H, radii, ColorFadeAlpha(state.BorderColor, alpha));
		if (banner.pTexture != nullptr) {
			const auto innerRadii = CDrawList::UniformRadii(kCarouselCardCornerRadius - state.BorderThickness);
			const float innerW = rect.W - state.BorderThickness * 2.0f;
			const float innerH = rect.H - state.BorderThickness * 2.0f;
			const UvRect uv = CDrawList::ComputeCoverUv(innerW, innerH, banner.TextureAspect, 1.0f);
			drawList.AddRectRoundedTexturedUv(rect.X + state.BorderThickness, rect.Y + state.BorderThickness, innerW,
											  innerH, innerRadii, uv.U0, uv.V0, uv.U1, uv.V1, banner.pTexture,
											  ColorFadeAlpha(Color{255, 255, 255, 255}, alpha));
		}
	}
	drawList.PopClipRect();

	const float contentHeight = GridContentHeight();
	m_wrapScroll.DrawEdgeFade(drawList, region, contentHeight, region.H, ColorFadeAlpha(Color{18, 18, 20, 255}, alpha));
	m_wrapScroll.Draw(drawList, ScrollbarTrackRect(), contentHeight, region.H,
					  ColorFadeAlpha(Color{160, 160, 168, 200}, alpha), mouseX, mouseY);
}

void CCarousel::DrawListMode(CDrawList &drawList, std::uint8_t alpha, float yOffset, float mouseX, float mouseY) const
{
	const Rect region = m_vecBounds;
	const CFont &body = m_fonts.GetBody();

	drawList.PushClipRect(region);
	for (std::uint32_t i = 0; i < m_nBannerCount; i += 1) {
		BannerRect rect = ListBannerRect(i);
		rect.Y += yOffset;
		if (rect.Y + rect.H < region.Y || rect.Y > region.Y + region.H) {
			continue;
		}

		const CBanner &banner = m_aBanners[i];
		// No persistent "selected" row here either.
		const bool isHovered =
			mouseX >= rect.X && mouseX < rect.X + rect.W && mouseY >= rect.Y && mouseY < rect.Y + rect.H;
		const Color rowBg = isHovered ? Color{42, 42, 47, 220} : Color{32, 32, 36, 220};
		drawList.AddRectRoundedFilled(rect.X, rect.Y, rect.W, rect.H, CDrawList::UniformRadii(10.0f),
									  ColorFadeAlpha(rowBg, alpha));

		const float thumbSize = ListThumbSizeFor(m_flZoomPercent);
		const Rect thumb{rect.X + 12.0f, rect.Y + (rect.H - thumbSize) * 0.5f, thumbSize, thumbSize};

		if (banner.pIcon != nullptr) {
			// Already square - a plain 0..1 UV, not ComputeCoverUv (that's for cropping the
			// wide banner art, which this isn't).
			drawList.AddRectRoundedTextured(thumb.X, thumb.Y, thumb.W, thumb.H, CDrawList::UniformRadii(10.0f),
											banner.pIcon, ColorFadeAlpha(Color{255, 255, 255, 255}, alpha));
		} else if (banner.pTexture != nullptr) {
			// Falls back to a cover-fit crop of the banner art for a banner added without a
			// dedicated icon yet.
			const UvRect uv = CDrawList::ComputeCoverUv(thumb.W, thumb.H, banner.TextureAspect, 1.0f);
			drawList.AddRectRoundedTexturedUv(thumb.X, thumb.Y, thumb.W, thumb.H, CDrawList::UniformRadii(10.0f), uv.U0,
											  uv.V0, uv.U1, uv.V1, banner.pTexture,
											  ColorFadeAlpha(Color{255, 255, 255, 255}, alpha));
		}

		const float textX = thumb.X + thumb.W + 16.0f;
		DrawText(drawList, body, textX, rect.Y + (rect.H + body.GetAscent()) * 0.5f, banner.Title,
				 ColorFadeAlpha(Color{232, 232, 236, 255}, alpha));
	}
	drawList.PopClipRect();

	const float contentHeight = ListContentHeight();
	m_wrapScroll.DrawEdgeFade(drawList, region, contentHeight, region.H, ColorFadeAlpha(Color{18, 18, 20, 255}, alpha));
	m_wrapScroll.Draw(drawList, ScrollbarTrackRect(), contentHeight, region.H,
					  ColorFadeAlpha(Color{160, 160, 168, 200}, alpha), mouseX, mouseY);
}

void CCarousel::DrawMode(CDrawList &drawList, ECarouselViewMode mode, std::uint8_t alpha, float yOffset, float mouseX,
						 float mouseY) const
{
	switch (mode) {
		case ECarouselViewMode::CAROUSEL_VIEW_MODE_CAROUSEL:
			DrawCarouselMode(drawList, alpha, yOffset, mouseX, mouseY);
			break;
		case ECarouselViewMode::CAROUSEL_VIEW_MODE_GRID:
			DrawGridMode(drawList, alpha, yOffset, mouseX, mouseY);
			break;
		case ECarouselViewMode::CAROUSEL_VIEW_MODE_LIST:
			DrawListMode(drawList, alpha, yOffset, mouseX, mouseY);
			break;
	}
}

void CCarousel::DrawStatusBarContent(CDrawList &drawList) const
{
	if (m_nBannerCount == 0) {
		return; // nothing to zoom in/out of yet - same gate Draw's own flyout call used to have
	}

	char buffer[48];
	const CStringView name = ViewModeName(m_viewMode);
	// %.*s with an explicit precision, not %s - name.pData isn't guaranteed null-
	// terminated, only name.Length bytes are valid to read. Plain ASCII only: CFont only
	// bakes glyphs 32..126, so an em dash or other non-ASCII byte would just be skipped.
	const int written = std::snprintf(buffer, sizeof(buffer), "%.*s  -  Ctrl+Scroll to zoom",
									  static_cast<int>(name.Length), name.pData);
	const CStringView text{buffer, written > 0 ? static_cast<std::uint64_t>(written) : 0};

	// No background pill of its own, unlike the floating overlay chip this replaced -
	// the status bar underneath it (main.cpp's own background fill) is the only backing
	// this needs.
	const CFont &secondary = m_fonts.GetSecondary();
	const Rect content = StatusBarContentRect();

	const Rect iconBox{content.X, content.Y + (content.H - kStatusBarIconSize) * 0.5f, kStatusBarIconSize,
					   kStatusBarIconSize};
	DrawModeGlyph(drawList, m_assets, m_viewMode, iconBox, Color{175, 175, 182, 255});

	// Baseline for the text's own visual center (not just its ascent) to land on the
	// status bar's true center, matching where the icon (a plain box, centered by its own
	// bounds) sits - see settings_menu.cpp's own comment on this same ascent/descent math
	// and its kBaselineVisualNudge for why GetAscent() alone isn't enough, and why even
	// ascent+descent alone still reads a couple pixels low against a plain geometrically-
	// centered icon like this one.
	constexpr float kBaselineVisualNudge = 2.0f;
	const float baselineY =
		content.Y + content.H * 0.5f + (secondary.GetAscent() + secondary.GetDescent()) * 0.5f - kBaselineVisualNudge;
	DrawText(drawList, secondary, iconBox.X + kStatusBarIconSize + kStatusBarIconGap, baselineY, text,
			 Color{158, 158, 166, 255});
}

void CCarousel::DrawModeSwitcher(CDrawList &drawList) const
{
	if (m_flModeSwitcherVisibleAmount <= 0.001f) {
		return;
	}

	const auto alpha = static_cast<std::uint8_t>(255.0f * m_flModeSwitcherVisibleAmount);
	const Rect panel = ModeSwitcherPanelRect();
	const float slide = (1.0f - m_flModeSwitcherVisibleAmount) * kModeSwitcherSlidePixels;
	const Rect shiftedPanel{panel.X, panel.Y + slide, panel.W, panel.H};

	// A soft drop shadow beneath the panel - 3 layered, progressively larger/dimmer
	// rounded rects offset down - gives the flyout a lifted, elevated read against the
	// carousel instead of looking pasted flat onto it.
	for (std::int32_t i = 3; i >= 1; i -= 1) {
		const float t = static_cast<float>(i) / 3.0f;
		const float offset = 7.0f * t;
		const auto layerAlpha =
			static_cast<std::uint8_t>(30.0f * (1.0f - t * 0.6f) * (static_cast<float>(alpha) / 255.0f));
		if (layerAlpha == 0) {
			continue;
		}
		drawList.AddRectRoundedFilled(shiftedPanel.X - offset * 0.3f, shiftedPanel.Y + offset,
									  shiftedPanel.W + offset * 0.6f, shiftedPanel.H + offset,
									  CDrawList::UniformRadii(kModeSwitcherRadius + offset * 0.3f),
									  Color{0, 0, 0, layerAlpha});
	}

	drawList.AddRectRoundedFilled(shiftedPanel.X, shiftedPanel.Y, shiftedPanel.W, shiftedPanel.H,
								  CDrawList::UniformRadii(kModeSwitcherRadius),
								  ColorFadeAlpha(kModeSwitcherBorder, alpha));
	drawList.AddRectRoundedFilled(shiftedPanel.X + 1.0f, shiftedPanel.Y + 1.0f, shiftedPanel.W - 2.0f,
								  shiftedPanel.H - 2.0f, CDrawList::UniformRadii(kModeSwitcherRadius - 1.0f),
								  ColorFadeAlpha(kModeSwitcherBg, alpha));

	const CFont &body = m_fonts.GetBody();
	const CFont &secondary = m_fonts.GetSecondary();

	const Rect rows = ModeSwitcherRowsRect(shiftedPanel);
	const float rowHeight = body.GetLineHeight() + 10.0f;
	for (std::uint32_t i = 0; i < kCarouselViewModeCount; i += 1) {
		const ECarouselViewMode mode = kModeSwitcherRowMode[i];
		const Rect row{
			rows.X + kModeSwitcherPadding,
			rows.Y + kModeSwitcherPadding + rowHeight * static_cast<float>(i),
			rows.W - kModeSwitcherPadding * 2.0f,
			rowHeight,
		};
		const bool active = mode == m_viewMode;
		// A fixed left inset (12px) for the icon/text column, well clear of the active
		// row's 3px accent bar either way.
		const float contentX = row.X + 12.0f;
		if (active) {
			drawList.AddRectRoundedFilled(row.X, row.Y, row.W, row.H, CDrawList::UniformRadii(6.0f),
										  ColorFadeAlpha(kModeSwitcherActiveRowFill, alpha));
			drawList.AddRectRoundedFilled(row.X + 2.0f, row.Y + 3.0f, 3.0f, row.H - 6.0f, CDrawList::UniformRadii(1.5f),
										  ColorFadeAlpha(kModeSwitcherIndicatorFill, alpha));
		}
		const Rect iconBox{
			contentX,
			IconCenterYFor(row.Y + row.H * 0.5f, body.GetAscent()) - kModeSwitcherRowIconSize * 0.5f,
			kModeSwitcherRowIconSize,
			kModeSwitcherRowIconSize,
		};
		DrawModeGlyph(drawList, m_assets, mode, iconBox,
					  ColorFadeAlpha(active ? kModeSwitcherTextActive : kModeSwitcherText, alpha));
		DrawText(drawList, body, contentX + kModeSwitcherRowIconSize + kModeSwitcherRowIconGap,
				 row.Y + (row.H + body.GetAscent()) * 0.5f, ViewModeName(mode),
				 ColorFadeAlpha(active ? kModeSwitcherTextActive : kModeSwitcherText, alpha));
	}

	// The percentage slider: a thin track, bottom = 0% (Carousel) to top = 100% (List at
	// max zoom) - a down-to-up readout of m_flZoomPercent. The moving indicator is the
	// live percentage text itself, in a small pill. Tick marks at every zoom stop (not
	// just the 3 named modes) make the discrete steps underneath the continuous indicator
	// position visible.
	const Rect track = ModeSwitcherTrackRect(shiftedPanel);
	drawList.AddRectRoundedFilled(track.X, track.Y, track.W, track.H, CDrawList::UniformRadii(track.W * 0.5f),
								  ColorFadeAlpha(kModeSwitcherTrackBg, alpha));
	for (std::int32_t stop = 0; stop < kCarouselZoomStopCount; stop += 1) {
		const float st = ZoomStopPercent(stop) / 100.0f;
		const float ty = track.Y + track.H * (1.0f - st);
		drawList.AddRectFilled(track.X - 3.0f, ty - 0.75f, track.W + 6.0f, 1.5f,
							   ColorFadeAlpha(Color{118, 118, 126, 190}, alpha));
	}

	char percentBuffer[8];
	const int written =
		std::snprintf(percentBuffer, sizeof(percentBuffer), "%d%%", static_cast<int>(m_flZoomPercent + 0.5f));
	const CStringView percentText{percentBuffer, written > 0 ? static_cast<std::uint64_t>(written) : 0};
	const float percentTextW = TextWidth(secondary, percentText);
	const float indicatorW = percentTextW + kModeSwitcherIndicatorPaddingX * 2.0f;

	const float t = std::clamp(m_flZoomPercent / 100.0f, 0.0f, 1.0f);
	const float indicatorCy = track.Y + track.H * (1.0f - t); // 0% at the bottom, 100% at the top
	const Rect indicator{
		track.X + track.W * 0.5f - indicatorW * 0.5f,
		indicatorCy - kModeSwitcherIndicatorHeight * 0.5f,
		indicatorW,
		kModeSwitcherIndicatorHeight,
	};
	// A thin dark border ring plus the fill, so the pill reads as a real control sitting on
	// the track rather than a flat patch of color.
	drawList.AddRectRoundedFilled(indicator.X - 1.0f, indicator.Y - 1.0f, indicator.W + 2.0f, indicator.H + 2.0f,
								  CDrawList::UniformRadii(indicator.H * 0.5f + 1.0f),
								  ColorFadeAlpha(Color{16, 14, 22, 255}, alpha));
	drawList.AddRectRoundedFilled(indicator.X, indicator.Y, indicator.W, indicator.H,
								  CDrawList::UniformRadii(indicator.H * 0.5f),
								  ColorFadeAlpha(kModeSwitcherIndicatorFill, alpha));
	DrawText(drawList, secondary, indicator.X + kModeSwitcherIndicatorPaddingX,
			 indicator.Y + (indicator.H + secondary.GetAscent()) * 0.5f - 1.0f, percentText,
			 ColorFadeAlpha(Color{20, 18, 26, 255}, alpha));
}

void CCarousel::Draw(CDrawList &drawList)
{
	if (m_flTransitionAmount > 0.001f) {
		const auto outgoingAlpha = static_cast<std::uint8_t>(255.0f * m_flTransitionAmount);
		const auto incomingAlpha = static_cast<std::uint8_t>(255.0f * (1.0f - m_flTransitionAmount));
		const float slide = m_flTransitionAmount * kViewModeSlidePixels;
		DrawMode(drawList, m_transitionFromMode, outgoingAlpha, -slide, m_flMouseX, m_flMouseY);
		DrawMode(drawList, m_viewMode, incomingAlpha, slide, m_flMouseX, m_flMouseY);
	} else {
		DrawMode(drawList, m_viewMode, 255, 0.0f, m_flMouseX, m_flMouseY);
	}

	if (m_nBannerCount > 0) {
		DrawModeSwitcher(drawList);
	}
}
