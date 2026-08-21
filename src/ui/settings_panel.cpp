#include "ui/settings_panel.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <Windows.h>

#include "core/animator.h"
#include "gfx/font.h"
#include "ui/draw_list.h"
#include "ui/layout.h"
#include "ui/text.h"
#include "window.h"

namespace {
constexpr float kPanelOpenEaseRate = 16.0f;
constexpr float kToggleEaseRate = 18.0f;

// Base panel size, tuned at kReferenceBodyPixelHeight - PanelMaxSizeScale grows both
// in lockstep with a larger Font Size setting (see CAccountModal, which does the same
// thing) so bigger text gets more room instead of being clipped/overlapping at the
// original size.
constexpr float kPanelMaxWidthBase = 520.0f;
// A comfortable viewport, not "tall enough to fit every row" - the row list scrolls
// once it overflows this, so it doesn't need to grow every time a row is added.
constexpr float kPanelMaxHeightBase = 460.0f;
// Real baked pixels (CFont::GetPixelHeight already includes CFontManager's display-
// scale factor), matching the default CSettings::m_flFontPixelSize of 16 nominal
// units - see kFontSizeMin/Max below for why the Settings-facing number isn't this.
constexpr float kReferenceBodyPixelHeight = 24.0f;
constexpr float kPanelMargin = 48.0f;
constexpr float kPanelScaleMin = 0.94f;
constexpr float kPanelRadius = 16.0f;
constexpr float kPanelBorderThickness = 1.5f;

constexpr float kRowPaddingX = 24.0f;
constexpr float kCloseSize = 26.0f;

constexpr float kRowLabelTopGap = 8.0f;		// above the title line
constexpr float kRowLabelLineGap = 4.0f;	// between the title and description lines
constexpr float kRowLabelBottomGap = 10.0f; // below the description line

constexpr float kSliderTrackWidth = 130.0f;
constexpr float kSliderTrackVisualHeight = 6.0f;
constexpr float kSliderThumbRadius = 8.0f;
constexpr float kSliderValueLabelGap = 8.0f;
constexpr float kAnimationSpeedMin = 0.25f;
constexpr float kAnimationSpeedMax = 3.0f;

// Nominal Settings-facing units, not literal baked pixels - CFontManager's display-
// scale factor is what turns a value here into the real pixel height stb_truetype
// bakes.
constexpr float kFontSizeMin = 10.0f;
constexpr float kFontSizeMax = 24.0f;

// Same nominal-units convention as kFontSizeMin/Max, independently tunable instead of
// a fixed ratio of Font Size. Narrower range and a lower ceiling than the body
// field's since secondary is meant to stay the smaller, dimmer face labeling body
// content, not grow to rival it.
constexpr float kSecondaryFontSizeMin = 8.0f;
constexpr float kSecondaryFontSizeMax = 18.0f;

constexpr Color kColorBg{26, 26, 29, 255};
constexpr Color kColorBorder{70, 70, 76, 255};
constexpr Color kColorText{224, 224, 228, 255};
constexpr Color kColorTextDim{140, 140, 146, 255};
constexpr Color kColorSeparator{44, 44, 48, 255};
constexpr Color kColorControlBg{40, 40, 45, 255};
constexpr Color kColorToggleOff{70, 70, 76, 255};

// Every size below derives from the active fonts' actual glyph metrics
// (CFont::GetLineHeight: ascent+descent+line_gap, a real baseline-to-baseline
// distance) instead of a fixed pixel constant, so a larger Font Size setting grows
// rows/header/footer to fit instead of the title/description lines overlapping once
// text no longer fits the original small-font layout.
float PanelMaxSizeScale(const CFontManager &fonts)
{
	return std::max(1.0f, fonts.GetBody().GetPixelHeight() / kReferenceBodyPixelHeight);
}

float HeaderHeightFor(const CFontManager &fonts)
{
	return fonts.GetBody().GetLineHeight() + 20.0f;
}

float FooterHeightFor(const CFontManager &fonts)
{
	return std::max(32.0f, fonts.GetSecondary().GetLineHeight() + 16.0f);
}

// A plain row's title-line-plus-description-line stack height - the building block
// RowHeightFor sizes off of.
float RowLabelBlockHeightFor(const CFontManager &fonts)
{
	return kRowLabelTopGap + fonts.GetBody().GetLineHeight() + kRowLabelLineGap + fonts.GetSecondary().GetLineHeight() +
		   kRowLabelBottomGap;
}

// A plain row (label stack only, no extra control height below it) - every row
// except Master Password uses this directly as its ComputeRows height.
float RowHeightFor(const CFontManager &fonts)
{
	return RowLabelBlockHeightFor(fonts);
}

// Body, not secondary: every control this sizes (the font-name field, the master
// password field/button) holds or labels a value the user reads/types, not a dim
// label.
float RowControlHeightFor(const CFontManager &fonts)
{
	return std::max(34.0f, fonts.GetBody().GetLineHeight() + 12.0f);
}

// The vertical center a row's right-aligned control (toggle, stepper, slider, swatch)
// should align to - the midpoint between the title and description text baselines, not
// the row's raw top-to-bottom box center. Centering on the box (this project's original
// approach) pulls every control noticeably closer to the title than the description
// whenever the title line (body font) is taller than the description line (secondary
// font) - the default case, since body is always meant to read larger - which read as
// "the control is hugging the label" once a text-bearing control (the Animation Speed
// slider's own "1.00x" value, sitting right next to the title with barely more gap than
// the title has to the row above it) made the tightness obvious enough to draw a direct
// bug report. A plain toggle/swatch has the exact same asymmetry, just less visually
// obvious without adjacent text on the same row to compare spacing against - so every
// control here switched to this, not just the slider.
float RowControlCenterY(Rect row, const CFontManager &fonts)
{
	const CFont &body = fonts.GetBody();
	const CFont &secondary = fonts.GetSecondary();
	const float titleBaselineY = row.Y + kRowLabelTopGap + body.GetAscent();
	const float descriptionBaselineY =
		row.Y + kRowLabelTopGap + body.GetLineHeight() + kRowLabelLineGap + secondary.GetAscent();
	return (titleBaselineY + descriptionBaselineY) * 0.5f;
}

// Single geometry function draw and click-handling both call - same pattern as
// CAccountModal.
Rect PanelRect(float openAmount, float windowW, float windowH, const CFontManager &fonts)
{
	const float sizeScale = PanelMaxSizeScale(fonts);
	const float panelMaxWidth = kPanelMaxWidthBase * sizeScale;
	const float panelMaxHeight = kPanelMaxHeightBase * sizeScale;

	const float availableW = std::max(0.0f, windowW - kPanelMargin * 2.0f);
	const float availableH = std::max(0.0f, windowH - kPanelMargin * 2.0f);

	float w = std::min(panelMaxWidth, availableW);
	float h = std::min(panelMaxHeight, availableH);

	const float targetAspect = panelMaxWidth / panelMaxHeight;
	if (w / h > targetAspect) {
		w = h * targetAspect;
	} else {
		h = w / targetAspect;
	}

	const float openScale = kPanelScaleMin + (1.0f - kPanelScaleMin) * openAmount;
	w *= openScale;
	h *= openScale;

	return Rect{(windowW - w) * 0.5f, (windowH - h) * 0.5f, w, h};
}

// The X close button's hit rect, top-right of the header - sized off kCloseSize, not
// the header's own height, so it stays a fixed comfortable click target regardless
// of Font Size.
Rect CloseRect(Rect panel, const CFontManager &fonts)
{
	const float headerHeight = HeaderHeightFor(fonts);
	return Rect{panel.X + panel.W - 14.0f - kCloseSize, panel.Y + (headerHeight - kCloseSize) * 0.5f, kCloseSize,
				kCloseSize};
}

// The full panel geometry chain, computed once and shared by draw/click/the pointer
// handlers below instead of each independently recomputing it - same pattern
// CAccountModal's own layout extraction uses. Header/footer are carved off the
// border-inset inner rect via RectSplitTop/Bottom; whatever remains is ScrollRegion,
// the clipped/scrollable strip the row stack lives in.
struct SettingsPanelLayout {
	Rect Panel;
	Rect Inner;
	Rect Header;
	Rect Footer;
	Rect ScrollRegion;
};

SettingsPanelLayout ComputeLayout(float openAmount, float windowW, float windowH, const CFontManager &fonts)
{
	SettingsPanelLayout layout{};
	layout.Panel = PanelRect(openAmount, windowW, windowH, fonts);
	layout.Inner = Rect{
		layout.Panel.X + kPanelBorderThickness,
		layout.Panel.Y + kPanelBorderThickness,
		layout.Panel.W - kPanelBorderThickness * 2.0f,
		layout.Panel.H - kPanelBorderThickness * 2.0f,
	};

	Rect cursor = layout.Inner;
	layout.Header = RectSplitTop(cursor, HeaderHeightFor(fonts));
	layout.Footer = RectSplitBottom(cursor, FooterHeightFor(fonts));
	layout.ScrollRegion = cursor;
	return layout;
}

// The vertical scrollbar's track - a thin strip inset from the scroll region's right
// edge, independent of how many rows currently exist (CScrollable itself decides
// whether there's anything to actually show/hit-test against this track).
Rect ScrollbarTrackRect(Rect scrollRegion)
{
	constexpr float kTrackMargin = 4.0f;
	return Rect{scrollRegion.X + scrollRegion.W - kScrollbarWidth - kTrackMargin, scrollRegion.Y, kScrollbarWidth,
				scrollRegion.H};
}

// True unless row is entirely scrolled out of scrollRegion - a cheap cull before
// drawing/hit-testing a row (the real clip against scrollRegion, CDrawList's push/
// pop clip rect, is what actually stops a partially-visible row from painting
// outside it; this just skips work for rows that are fully off-screen).
bool RowInView(Rect row, Rect scrollRegion)
{
	return !(row.Y + row.H <= scrollRegion.Y || row.Y >= scrollRegion.Y + scrollRegion.H);
}

// The Font row's text-input box, right-aligned in the row at a fixed width wide
// enough for a typical font filename.
Rect FontFieldRect(Rect row, const CFontManager &fonts)
{
	constexpr float kW = 190.0f;
	const float h = RowControlHeightFor(fonts);
	return Rect{row.X + row.W - kRowPaddingX - kW, row.Y + (row.H - h) * 0.5f, kW, h};
}

// A -/value/+ stepper's overall bounding rect (Font Size, Secondary Font Size),
// right-aligned in the row - StepperMinusRect/StepperPlusRect below carve the actual
// button hit rects out of this same rect, so drawing and hit-testing can't disagree.
Rect StepperRect(Rect row, const CFontManager &fonts)
{
	constexpr float kW = 108.0f;
	constexpr float kH = 28.0f;
	return Rect{row.X + row.W - kRowPaddingX - kW, RowControlCenterY(row, fonts) - kH * 0.5f, kW, kH};
}

Rect StepperMinusRect(Rect stepper)
{
	return Rect{stepper.X, stepper.Y, stepper.H, stepper.H};
}

Rect StepperPlusRect(Rect stepper)
{
	return Rect{stepper.X + stepper.W - stepper.H, stepper.Y, stepper.H, stepper.H};
}

// A toggle switch's hit rect (Animations, Round Corners, and Master Password once
// unlocked) - right-aligned in the row.
Rect ToggleRect(Rect row, const CFontManager &fonts)
{
	constexpr float kW = 40.0f;
	constexpr float kH = 22.0f;
	return Rect{row.X + row.W - kRowPaddingX - kW, RowControlCenterY(row, fonts) - kH * 0.5f, kW, kH};
}

// The Accent Color row's color swatch - also doubles as the anchor rect the Color
// Picker popup positions itself relative to.
Rect SwatchRect(Rect row, const CFontManager &fonts)
{
	constexpr float kW = 40.0f;
	constexpr float kH = 24.0f;
	return Rect{row.X + row.W - kRowPaddingX - kW, RowControlCenterY(row, fonts) - kH * 0.5f, kW, kH};
}

// The Master Password row's Reset Password button - right-aligned, same
// RowControlCenterY-centered convention every other row's control uses (a plain row now,
// not the taller custom sub-layout this used to need - see this file's own header
// comment on why resetting no longer happens inline in this panel at all).
Rect MasterPasswordResetButtonRect(Rect row, const CFontManager &fonts)
{
	constexpr float kW = 140.0f;
	const float h = RowControlHeightFor(fonts);
	return Rect{row.X + row.W - kRowPaddingX - kW, RowControlCenterY(row, fonts) - h * 0.5f, kW, h};
}

// The Animation Speed slider's draggable track - right-aligned in the row, at a
// fixed width regardless of Font Size, unlike most other row controls.
Rect SliderTrackRect(Rect row, const CFontManager &fonts)
{
	constexpr float kH = 22.0f; // taller than the visual bar itself - a bigger, easier grab target
	return Rect{row.X + row.W - kRowPaddingX - kSliderTrackWidth, RowControlCenterY(row, fonts) - kH * 0.5f,
				kSliderTrackWidth, kH};
}

float AnimationSpeedToT(float speed)
{
	return std::clamp((speed - kAnimationSpeedMin) / (kAnimationSpeedMax - kAnimationSpeedMin), 0.0f, 1.0f);
}

float AnimationSpeedFromT(float t)
{
	return kAnimationSpeedMin + std::clamp(t, 0.0f, 1.0f) * (kAnimationSpeedMax - kAnimationSpeedMin);
}

// Every row's rect, built by walking a single RectSplitTop cursor down the scroll
// region - each row claims exactly the height it needs and the next one starts
// exactly where it ended, so rows can never overlap or get squeezed regardless of how
// many exist or how tall one is. scrollOffset shifts the whole stack up as the user
// scrolls down, same as CAccountModal's account rows; ContentHeight (the sum of every
// row's height, independent of scrollOffset) is what the scrollbar needs to know
// whether there's anything to scroll at all.
struct SettingsRows {
	Rect Font;
	Rect FontSize;
	Rect SecondaryFontSize;
	Rect Animations;
	Rect RoundedCorners;
	Rect ExcludeFromCapture;
	Rect AnimationSpeed;
	Rect Accent;
	Rect MasterPassword;
	float ContentHeight;
};

// Walks the RectSplitTop cursor described above, in the fixed row order the panel
// always draws in. Every row (including Master Password now - see this file's own
// header comment) is a plain fixed-height row, so this needs no extra state from the
// caller the way it once did to size the old tier sub-layout.
SettingsRows ComputeRows(Rect scrollRegion, float scrollOffset, const CFontManager &fonts)
{
	SettingsRows rows{};
	Rect cursor{scrollRegion.X, scrollRegion.Y - scrollOffset, scrollRegion.W, 1.0e6f};
	const float startY = cursor.Y;

	const float rowHeight = RowHeightFor(fonts);
	rows.Font = RectSplitTop(cursor, rowHeight);
	rows.FontSize = RectSplitTop(cursor, rowHeight);
	rows.SecondaryFontSize = RectSplitTop(cursor, rowHeight);
	rows.Animations = RectSplitTop(cursor, rowHeight);
	rows.RoundedCorners = RectSplitTop(cursor, rowHeight);
	rows.ExcludeFromCapture = RectSplitTop(cursor, rowHeight);
	rows.AnimationSpeed = RectSplitTop(cursor, rowHeight);
	rows.Accent = RectSplitTop(cursor, rowHeight);
	rows.MasterPassword = RectSplitTop(cursor, rowHeight);

	rows.ContentHeight = cursor.Y - startY;
	return rows;
}

// The header's close button glyph - an X built from two crossed lines, since this
// panel has no dedicated close-icon asset (unlike the title bar's embedded icon).
void DrawXGlyph(CDrawList &drawList, Rect rect, Color color)
{
	const float cx = rect.X + rect.W * 0.5f;
	const float cy = rect.Y + rect.H * 0.5f;
	const float r = std::min(rect.W, rect.H) * 0.24f;
	drawList.AddLine(cx - r, cy - r, cx + r, cy + r, 2.0f, color);
	drawList.AddLine(cx - r, cy + r, cx + r, cy - r, 2.0f, color);
}

// The title + dim description line every row starts with - title in body, description
// in secondary, stacked per RowLabelBlockHeightFor's own metrics so the two can't
// drift.
void DrawRowLabel(CDrawList &drawList, const CFontManager &fonts, Rect row, const char *pTitle,
				  const char *pDescription, std::uint8_t alpha)
{
	const CFont &body = fonts.GetBody();
	const CFont &secondary = fonts.GetSecondary();
	const float titleBaselineY = row.Y + kRowLabelTopGap + body.GetAscent();
	const float descriptionBaselineY =
		row.Y + kRowLabelTopGap + body.GetLineHeight() + kRowLabelLineGap + secondary.GetAscent();
	DrawText(drawList, body, row.X + kRowPaddingX, titleBaselineY, StringViewFromCString(pTitle),
			 ColorFadeAlpha(kColorText, alpha));
	DrawText(drawList, secondary, row.X + kRowPaddingX, descriptionBaselineY, StringViewFromCString(pDescription),
			 ColorFadeAlpha(kColorTextDim, alpha));
}

// A pill track + sliding dot, onAmount already eased 0..1 by the caller (see
// CSettingsPanel::Update) so the switch always animates rather than snapping.
void DrawToggle(CDrawList &drawList, Rect rect, float onAmount, Color accent, std::uint8_t alpha)
{
	const Color track = ColorLerp(kColorToggleOff, accent, onAmount);
	drawList.AddRectRoundedFilled(rect.X, rect.Y, rect.W, rect.H, CDrawList::UniformRadii(rect.H * 0.5f),
								  ColorFadeAlpha(track, alpha));

	const float dotSize = rect.H - 6.0f;
	const float dotX = rect.X + 3.0f + (rect.W - rect.H) * onAmount;
	drawList.AddRectRoundedFilled(dotX, rect.Y + 3.0f, dotSize, dotSize, CDrawList::UniformRadii(dotSize * 0.5f),
								  ColorFadeAlpha(Color{245, 245, 248, 255}, alpha));
}

// track is the slider's hit rect (SliderTrackRect) - value's label ("1.35x") is drawn
// to its left in reserved space so it never crowds the track regardless of digit
// count, same reasoning DrawStepper's centered value text uses.
void DrawSlider(CDrawList &drawList, const CFont &font, Rect track, float value, Color accent, std::uint8_t alpha)
{
	char buffer[8];
	const int written = std::snprintf(buffer, sizeof(buffer), "%.2fx", value);
	const CStringView text{buffer, written > 0 ? static_cast<std::uint64_t>(written) : 0};
	const float textW = TextWidth(font, text);
	// Baseline centered on the track's own vertical center via the same ascent+descent
	// formula every other correctly-centered label in this project uses (see e.g.
	// carousel.cpp's own DrawStatusBarContent) - the previous (track.H + ascent) * 0.5f
	// version silently dropped the +descent term, leaving the text sitting visibly low
	// (worse at some Font Size values than others, due to how differently ascent/descent
	// happen to round to real baked pixels at different sizes) instead of centered, which
	// is what read as crowding the row's own description line below it.
	const float trackCenterY = track.Y + track.H * 0.5f;
	DrawText(drawList, font, track.X - kSliderValueLabelGap - textW,
			 trackCenterY + (font.GetAscent() + font.GetDescent()) * 0.5f, text, ColorFadeAlpha(kColorText, alpha));

	const float barY = track.Y + (track.H - kSliderTrackVisualHeight) * 0.5f;
	drawList.AddRectRoundedFilled(track.X, barY, track.W, kSliderTrackVisualHeight,
								  CDrawList::UniformRadii(kSliderTrackVisualHeight * 0.5f),
								  ColorFadeAlpha(kColorToggleOff, alpha));

	const float t = AnimationSpeedToT(value);
	const float fillW = track.W * t;
	if (fillW > 0.0f) {
		drawList.AddRectRoundedFilled(track.X, barY, fillW, kSliderTrackVisualHeight,
									  CDrawList::UniformRadii(kSliderTrackVisualHeight * 0.5f),
									  ColorFadeAlpha(accent, alpha));
	}

	const float thumbCx = track.X + fillW;
	const float thumbCy = track.Y + track.H * 0.5f;
	drawList.AddRectRoundedFilled(thumbCx - kSliderThumbRadius, thumbCy - kSliderThumbRadius, kSliderThumbRadius * 2.0f,
								  kSliderThumbRadius * 2.0f, CDrawList::UniformRadii(kSliderThumbRadius),
								  ColorFadeAlpha(Color{245, 245, 248, 255}, alpha));
}

// The -/value/+ control used by both Font Size and Secondary Font Size - value is
// drawn as a plain integer (both settings only ever step by whole units), centered
// in the gap between the two buttons regardless of digit count.
void DrawStepper(CDrawList &drawList, const CFont &font, Rect rect, float value, std::uint8_t alpha)
{
	const Rect minus = StepperMinusRect(rect);
	const Rect plus = StepperPlusRect(rect);

	drawList.AddRectRoundedFilled(minus.X, minus.Y, minus.W, minus.H, CDrawList::UniformRadii(6.0f),
								  ColorFadeAlpha(kColorControlBg, alpha));
	drawList.AddRectRoundedFilled(plus.X, plus.Y, plus.W, plus.H, CDrawList::UniformRadii(6.0f),
								  ColorFadeAlpha(kColorControlBg, alpha));

	const float mcx = minus.X + minus.W * 0.5f;
	const float mcy = minus.Y + minus.H * 0.5f;
	drawList.AddLine(mcx - 6.0f, mcy, mcx + 6.0f, mcy, 2.0f, ColorFadeAlpha(kColorText, alpha));

	const float pcx = plus.X + plus.W * 0.5f;
	const float pcy = plus.Y + plus.H * 0.5f;
	drawList.AddLine(pcx - 6.0f, pcy, pcx + 6.0f, pcy, 2.0f, ColorFadeAlpha(kColorText, alpha));
	drawList.AddLine(pcx, pcy - 6.0f, pcx, pcy + 6.0f, 2.0f, ColorFadeAlpha(kColorText, alpha));

	char buffer[8];
	const int written = std::snprintf(buffer, sizeof(buffer), "%d", static_cast<int>(value));
	const CStringView text{buffer, written > 0 ? static_cast<std::uint64_t>(written) : 0};
	const float textW = TextWidth(font, text);
	const float middleX = minus.X + minus.W;
	const float middleW = plus.X - middleX;
	DrawText(drawList, font, middleX + (middleW - textW) * 0.5f, rect.Y + (rect.H + font.GetAscent()) * 0.5f, text,
			 ColorFadeAlpha(kColorText, alpha));
}

// CSettings::m_szFontName is the persisted source of truth, so it only moves once a
// bake actually succeeds - never just mirrors whatever's currently typed, or a bad
// in-flight edit would get written to disk.
void SyncAppliedFontName(CSettings &settings, CStringView value)
{
	const std::uint64_t length = std::min<std::uint64_t>(value.Length, sizeof(settings.m_szFontName) - 1);
	std::memcpy(settings.m_szFontName, value.pData, length);
	settings.m_szFontName[length] = '\0';
}
} // namespace

CSettingsPanel::CSettingsPanel(CFontManager &fonts, CSettings &settings, CWindow &window, IRenderer &renderer)
	: m_fonts(fonts)
	, m_settings(settings)
	, m_window(window)
	, m_renderer(renderer)
{
	m_fontNameInput.Init(StringViewFromCString(m_settings.m_szFontName));
}

void CSettingsPanel::Open()
{
	m_bOpen = true;
}

void CSettingsPanel::Close()
{
	m_bOpen = false;
	m_fontNameInput.m_bFocused = false;
	m_colorPicker.Close();
}

void CSettingsPanel::Update(float deltaSeconds)
{
	m_flOpenAmount = CAnimator::EaseToward(m_flOpenAmount, m_bOpen ? 1.0f : 0.0f, kPanelOpenEaseRate, deltaSeconds);
	if (!m_bOpen && m_flOpenAmount < 0.002f) {
		m_flOpenAmount = 0.0f;
	}

	// Naturally snaps instantly too when CSettings::m_bAnimationsEnabled is off, since
	// EaseToward itself respects that flag - no special-casing needed here.
	m_flAnimationsToggleAmount = CAnimator::EaseToward(
		m_flAnimationsToggleAmount, m_settings.m_bAnimationsEnabled ? 1.0f : 0.0f, kToggleEaseRate, deltaSeconds);
	m_flRoundedCornersToggleAmount =
		CAnimator::EaseToward(m_flRoundedCornersToggleAmount, m_settings.m_bRoundedCornersEnabled ? 1.0f : 0.0f,
							  kToggleEaseRate, deltaSeconds);
	m_flExcludeFromCaptureToggleAmount =
		CAnimator::EaseToward(m_flExcludeFromCaptureToggleAmount,
							  m_settings.m_bExcludeAccountListFromCapture ? 1.0f : 0.0f, kToggleEaseRate, deltaSeconds);

	m_fontNameInput.Update(deltaSeconds);
	m_rowsScroll.Update(deltaSeconds);
}

bool CSettingsPanel::OnPointerDown(float x, float y)
{
	if (!IsBlocking()) {
		return false;
	}

	const SettingsPanelLayout layout = ComputeLayout(m_flOpenAmount, static_cast<float>(m_window.GetWidth()),
													 static_cast<float>(m_window.GetHeight()), m_fonts);
	const SettingsRows rows = ComputeRows(layout.ScrollRegion, m_rowsScroll.m_flScrollOffset, m_fonts);

	if (m_rowsScroll.OnPointerDown(x, y, ScrollbarTrackRect(layout.ScrollRegion), rows.ContentHeight,
								   layout.ScrollRegion.H)) {
		return true;
	}

	if (m_colorPicker.OnPointerDown(x, y)) {
		m_settings.m_clrAccent = m_colorPicker.GetCurrentColor();
		return true;
	}

	if (RowInView(rows.AnimationSpeed, layout.ScrollRegion)) {
		const Rect slider = SliderTrackRect(rows.AnimationSpeed, m_fonts);
		if (RectContainsPoint(slider, x, y)) {
			m_animationSpeedDrag.Begin(x, y);
			m_settings.m_flAnimationSpeed = AnimationSpeedFromT((x - slider.X) / slider.W);
			CAnimator::SetSpeed(m_settings.m_flAnimationSpeed);
			return true;
		}
	}

	return true; // swallow every press while open, matching the original's blocking behavior
}

bool CSettingsPanel::OnPointerMove(float x, float y)
{
	if (!IsBlocking()) {
		return false;
	}

	const SettingsPanelLayout layout = ComputeLayout(m_flOpenAmount, static_cast<float>(m_window.GetWidth()),
													 static_cast<float>(m_window.GetHeight()), m_fonts);
	const SettingsRows rows = ComputeRows(layout.ScrollRegion, m_rowsScroll.m_flScrollOffset, m_fonts);

	// CScrollable::OnPointerMove is a no-op unless a thumb drag is actually in progress -
	// same "call unconditionally, the function guards itself" pattern CAccountModal uses.
	m_rowsScroll.OnPointerMove(y, ScrollbarTrackRect(layout.ScrollRegion), rows.ContentHeight, layout.ScrollRegion.H);

	if (m_colorPicker.IsDragging()) {
		m_colorPicker.OnPointerMove(x, y);
		m_settings.m_clrAccent = m_colorPicker.GetCurrentColor();
	}

	if (m_animationSpeedDrag.IsPressed()) {
		m_animationSpeedDrag.Update(x, y);
		const Rect slider = SliderTrackRect(rows.AnimationSpeed, m_fonts);
		m_settings.m_flAnimationSpeed = AnimationSpeedFromT((x - slider.X) / slider.W);
		CAnimator::SetSpeed(m_settings.m_flAnimationSpeed);
	}

	return true;
}

bool CSettingsPanel::OnPointerUp(float x, float y)
{
	if (!IsBlocking()) {
		return false;
	}

	const bool wasDraggingScrollbar = m_rowsScroll.IsDragging();
	m_rowsScroll.OnPointerUp();

	const bool wasDraggingColor = m_colorPicker.IsDragging();
	m_colorPicker.OnPointerUp(x, y);

	const bool wasDraggingSlider = m_animationSpeedDrag.IsPressed();
	m_animationSpeedDrag.End();

	if (wasDraggingScrollbar || wasDraggingColor || wasDraggingSlider) {
		return true;
	}

	return HandleClick(x, y);
}

ECursorKind CSettingsPanel::GetDesiredCursor() const
{
	if (!IsBlocking()) {
		return ECursorKind::CURSOR_ARROW;
	}

	if (m_rowsScroll.IsDragging() || m_colorPicker.IsDragging() || m_animationSpeedDrag.IsPressed()) {
		return ECursorKind::CURSOR_DRAG;
	}

	// Not a CWidgetStack member (see CGameSelectPopup's own comment on the same
	// relationship) - forward explicitly while its popup is open.
	if (m_colorPicker.IsBlocking()) {
		const ECursorKind pickerCursor = m_colorPicker.GetDesiredCursor();
		if (pickerCursor != ECursorKind::CURSOR_ARROW) {
			return pickerCursor;
		}
	}

	const SettingsPanelLayout layout = ComputeLayout(m_flOpenAmount, static_cast<float>(m_window.GetWidth()),
													 static_cast<float>(m_window.GetHeight()), m_fonts);

	if (RectContainsPoint(CloseRect(layout.Panel, m_fonts), m_flMouseX, m_flMouseY)) {
		return ECursorKind::CURSOR_HAND;
	}

	if (!RectContainsPoint(layout.ScrollRegion, m_flMouseX, m_flMouseY)) {
		return ECursorKind::CURSOR_ARROW;
	}

	const SettingsRows rows = ComputeRows(layout.ScrollRegion, m_rowsScroll.m_flScrollOffset, m_fonts);

	if (RowInView(rows.Font, layout.ScrollRegion) &&
		RectContainsPoint(FontFieldRect(rows.Font, m_fonts), m_flMouseX, m_flMouseY)) {
		return ECursorKind::CURSOR_IBEAM;
	}
	if (RowInView(rows.FontSize, layout.ScrollRegion)) {
		const Rect stepper = StepperRect(rows.FontSize, m_fonts);
		if (RectContainsPoint(StepperMinusRect(stepper), m_flMouseX, m_flMouseY) ||
			RectContainsPoint(StepperPlusRect(stepper), m_flMouseX, m_flMouseY)) {
			return ECursorKind::CURSOR_HAND;
		}
	}
	if (RowInView(rows.SecondaryFontSize, layout.ScrollRegion)) {
		const Rect stepper = StepperRect(rows.SecondaryFontSize, m_fonts);
		if (RectContainsPoint(StepperMinusRect(stepper), m_flMouseX, m_flMouseY) ||
			RectContainsPoint(StepperPlusRect(stepper), m_flMouseX, m_flMouseY)) {
			return ECursorKind::CURSOR_HAND;
		}
	}
	if (RowInView(rows.Animations, layout.ScrollRegion) &&
		RectContainsPoint(ToggleRect(rows.Animations, m_fonts), m_flMouseX, m_flMouseY)) {
		return ECursorKind::CURSOR_HAND;
	}
	if (RowInView(rows.RoundedCorners, layout.ScrollRegion) &&
		RectContainsPoint(ToggleRect(rows.RoundedCorners, m_fonts), m_flMouseX, m_flMouseY)) {
		return ECursorKind::CURSOR_HAND;
	}
	if (RowInView(rows.ExcludeFromCapture, layout.ScrollRegion) &&
		RectContainsPoint(ToggleRect(rows.ExcludeFromCapture, m_fonts), m_flMouseX, m_flMouseY)) {
		return ECursorKind::CURSOR_HAND;
	}
	if (RowInView(rows.AnimationSpeed, layout.ScrollRegion) &&
		RectContainsPoint(SliderTrackRect(rows.AnimationSpeed, m_fonts), m_flMouseX, m_flMouseY)) {
		return ECursorKind::CURSOR_HAND;
	}
	if (RowInView(rows.Accent, layout.ScrollRegion) &&
		RectContainsPoint(SwatchRect(rows.Accent, m_fonts), m_flMouseX, m_flMouseY)) {
		return ECursorKind::CURSOR_HAND;
	}
	if (RowInView(rows.MasterPassword, layout.ScrollRegion) &&
		RectContainsPoint(MasterPasswordResetButtonRect(rows.MasterPassword, m_fonts), m_flMouseX, m_flMouseY)) {
		return ECursorKind::CURSOR_HAND;
	}

	if (CScrollable::IsVisible(rows.ContentHeight, layout.ScrollRegion.H) &&
		RectContainsPoint(ScrollbarTrackRect(layout.ScrollRegion), m_flMouseX, m_flMouseY)) {
		return ECursorKind::CURSOR_HAND;
	}

	return ECursorKind::CURSOR_ARROW;
}

bool CSettingsPanel::HandleClick(float x, float y)
{
	const SettingsPanelLayout layout = ComputeLayout(m_flOpenAmount, static_cast<float>(m_window.GetWidth()),
													 static_cast<float>(m_window.GetHeight()), m_fonts);

	if (RectContainsPoint(CloseRect(layout.Panel, m_fonts), x, y) || !RectContainsPoint(layout.Panel, x, y)) {
		Close();
		return true;
	}

	// Clicks outside the scrollable row area (header/footer) never hit a row - same
	// clickInScrollRegion guard CAccountModal's account list uses.
	const bool clickInScrollRegion = RectContainsPoint(layout.ScrollRegion, x, y);
	const SettingsRows rows = ComputeRows(layout.ScrollRegion, m_rowsScroll.m_flScrollOffset, m_fonts);

	const bool clickedFontField = clickInScrollRegion && RowInView(rows.Font, layout.ScrollRegion) &&
								  RectContainsPoint(FontFieldRect(rows.Font, m_fonts), x, y);
	m_fontNameInput.m_bFocused = clickedFontField;
	if (clickedFontField) {
		m_fontNameInput.SetValue(
			m_fontNameInput.GetValue()); // click-to-position not implemented yet; end is a reasonable default
	}

	if (clickInScrollRegion && RowInView(rows.FontSize, layout.ScrollRegion)) {
		const Rect stepper = StepperRect(rows.FontSize, m_fonts);
		if (RectContainsPoint(StepperMinusRect(stepper), x, y)) {
			m_settings.m_flFontPixelSize = std::max(kFontSizeMin, m_settings.m_flFontPixelSize - 1.0f);
			if (m_fonts.ApplyBody(m_renderer, m_fontNameInput.GetValue(), m_settings.m_flFontPixelSize,
								  m_settings.m_flSecondaryFontPixelSize, m_window.GetDpiScale())) {
				SyncAppliedFontName(m_settings, m_fontNameInput.GetValue());
			}
		} else if (RectContainsPoint(StepperPlusRect(stepper), x, y)) {
			m_settings.m_flFontPixelSize = std::min(kFontSizeMax, m_settings.m_flFontPixelSize + 1.0f);
			if (m_fonts.ApplyBody(m_renderer, m_fontNameInput.GetValue(), m_settings.m_flFontPixelSize,
								  m_settings.m_flSecondaryFontPixelSize, m_window.GetDpiScale())) {
				SyncAppliedFontName(m_settings, m_fontNameInput.GetValue());
			}
		}
	}

	if (clickInScrollRegion && RowInView(rows.SecondaryFontSize, layout.ScrollRegion)) {
		const Rect stepper = StepperRect(rows.SecondaryFontSize, m_fonts);
		if (RectContainsPoint(StepperMinusRect(stepper), x, y)) {
			m_settings.m_flSecondaryFontPixelSize =
				std::max(kSecondaryFontSizeMin, m_settings.m_flSecondaryFontPixelSize - 1.0f);
			if (m_fonts.ApplyBody(m_renderer, m_fontNameInput.GetValue(), m_settings.m_flFontPixelSize,
								  m_settings.m_flSecondaryFontPixelSize, m_window.GetDpiScale())) {
				SyncAppliedFontName(m_settings, m_fontNameInput.GetValue());
			}
		} else if (RectContainsPoint(StepperPlusRect(stepper), x, y)) {
			m_settings.m_flSecondaryFontPixelSize =
				std::min(kSecondaryFontSizeMax, m_settings.m_flSecondaryFontPixelSize + 1.0f);
			if (m_fonts.ApplyBody(m_renderer, m_fontNameInput.GetValue(), m_settings.m_flFontPixelSize,
								  m_settings.m_flSecondaryFontPixelSize, m_window.GetDpiScale())) {
				SyncAppliedFontName(m_settings, m_fontNameInput.GetValue());
			}
		}
	}

	if (clickInScrollRegion && RowInView(rows.Animations, layout.ScrollRegion) &&
		RectContainsPoint(ToggleRect(rows.Animations, m_fonts), x, y)) {
		m_settings.m_bAnimationsEnabled = !m_settings.m_bAnimationsEnabled;
		CAnimator::SetEnabled(m_settings.m_bAnimationsEnabled);
	}

	if (clickInScrollRegion && RowInView(rows.RoundedCorners, layout.ScrollRegion) &&
		RectContainsPoint(ToggleRect(rows.RoundedCorners, m_fonts), x, y)) {
		m_settings.m_bRoundedCornersEnabled = !m_settings.m_bRoundedCornersEnabled;
		CDrawList::SetRoundedCornersEnabled(m_settings.m_bRoundedCornersEnabled);
	}

	if (clickInScrollRegion && RowInView(rows.ExcludeFromCapture, layout.ScrollRegion) &&
		RectContainsPoint(ToggleRect(rows.ExcludeFromCapture, m_fonts), x, y)) {
		m_settings.m_bExcludeAccountListFromCapture = !m_settings.m_bExcludeAccountListFromCapture;
	}

	// Animation Speed's slider is dragged, not clicked - see OnPointerDown/Move, which
	// need continuous updates a single click can't express.

	const Rect swatch = SwatchRect(rows.Accent, m_fonts);
	if (clickInScrollRegion && RowInView(rows.Accent, layout.ScrollRegion) && RectContainsPoint(swatch, x, y)) {
		if (m_colorPicker.IsBlocking()) {
			m_colorPicker.Close();
		} else {
			m_colorPicker.Open(m_settings.m_clrAccent, swatch, static_cast<float>(m_window.GetWidth()),
							   static_cast<float>(m_window.GetHeight()));
		}
		return true;
	}
	if (m_colorPicker.IsBlocking() && !RectContainsPoint(swatch, x, y)) {
		// Click-anywhere-else-closes, same pattern CSettingsMenu already uses - the color
		// was already applied live while dragging, so closing here needs no separate
		// "confirm." (CColorPicker's own OnPointerDown already consumed any click that
		// actually landed inside its popup, before HandleClick ever runs - this only
		// fires for a click that missed the popup entirely.)
		m_colorPicker.Close();
		return true;
	}

	if (clickInScrollRegion && RowInView(rows.MasterPassword, layout.ScrollRegion) &&
		RectContainsPoint(MasterPasswordResetButtonRect(rows.MasterPassword, m_fonts), x, y)) {
		// Closing here, not just latching the flag, is deliberate - see this file's own
		// header comment: the coordinating owner is about to take over the whole app with
		// CUnlockScreen's setup mode, so there's nothing left for this panel to still be
		// open for.
		m_bResetPasswordRequestedThisFrame = true;
		Close();
		return true;
	}

	return true;
}

bool CSettingsPanel::OnScroll(float x, float y, float wheelDelta)
{
	(void)x;
	(void)y;
	if (!IsBlocking()) {
		return false;
	}

	const SettingsPanelLayout layout = ComputeLayout(m_flOpenAmount, static_cast<float>(m_window.GetWidth()),
													 static_cast<float>(m_window.GetHeight()), m_fonts);
	const SettingsRows rows = ComputeRows(layout.ScrollRegion, m_rowsScroll.m_flScrollOffset, m_fonts);
	m_rowsScroll.OnScroll(wheelDelta, rows.ContentHeight, layout.ScrollRegion.H);
	return true;
}

bool CSettingsPanel::OnChar(std::uint32_t character)
{
	if (!IsBlocking()) {
		return false;
	}

	// A no-op on an unfocused field, so this is safe to call unconditionally.
	m_fontNameInput.OnChar(character);
	return true;
}

bool CSettingsPanel::OnKeyDown(std::uint32_t keyCode)
{
	if (!IsBlocking()) {
		return false;
	}

	if (keyCode == VK_ESCAPE) {
		Close();
		return true;
	}

	if (m_fontNameInput.m_bFocused) {
		if (keyCode == VK_RETURN) {
			if (m_fonts.ApplyBody(m_renderer, m_fontNameInput.GetValue(), m_settings.m_flFontPixelSize,
								  m_settings.m_flSecondaryFontPixelSize, m_window.GetDpiScale())) {
				SyncAppliedFontName(m_settings, m_fontNameInput.GetValue());
			}
			return true;
		}
		m_fontNameInput.OnKey(keyCode);
		return true;
	}

	return true;
}

bool CSettingsPanel::ConsumeResetPasswordRequested()
{
	const bool requested = m_bResetPasswordRequestedThisFrame;
	m_bResetPasswordRequestedThisFrame = false;
	return requested;
}

void CSettingsPanel::Draw(CDrawList &drawList)
{
	if (m_flOpenAmount <= 0.001f) {
		return;
	}

	const float windowW = static_cast<float>(m_window.GetWidth());
	const float windowH = static_cast<float>(m_window.GetHeight());

	const auto alpha = static_cast<std::uint8_t>(255.0f * m_flOpenAmount);
	const SettingsPanelLayout layout = ComputeLayout(m_flOpenAmount, windowW, windowH, m_fonts);

	drawList.AddRectFilled(0.0f, 0.0f, windowW, windowH,
						   Color{0, 0, 0, static_cast<std::uint8_t>(140.0f * m_flOpenAmount)});

	drawList.AddRectRoundedFilled(layout.Panel.X, layout.Panel.Y, layout.Panel.W, layout.Panel.H,
								  CDrawList::UniformRadii(kPanelRadius), ColorFadeAlpha(kColorBorder, alpha));
	drawList.AddRectRoundedFilled(
		layout.Panel.X + kPanelBorderThickness, layout.Panel.Y + kPanelBorderThickness,
		layout.Panel.W - kPanelBorderThickness * 2.0f, layout.Panel.H - kPanelBorderThickness * 2.0f,
		CDrawList::UniformRadii(kPanelRadius - kPanelBorderThickness), ColorFadeAlpha(kColorBg, alpha));

	const CFont &body = m_fonts.GetBody();
	const CFont &secondary = m_fonts.GetSecondary();

	DrawText(drawList, body, layout.Header.X + kRowPaddingX,
			 layout.Header.Y + (layout.Header.H + body.GetAscent()) * 0.5f, StringViewFromCString("Settings"),
			 ColorFadeAlpha(kColorText, alpha));

	const Rect close = CloseRect(layout.Panel, m_fonts);
	const bool hoverClose = RectContainsPoint(close, m_flMouseX, m_flMouseY);
	DrawXGlyph(drawList, close, ColorFadeAlpha(hoverClose ? kColorText : kColorTextDim, alpha));

	drawList.AddRectFilled(layout.Header.X + kRowPaddingX, layout.Header.Y + layout.Header.H,
						   layout.Header.W - kRowPaddingX * 2.0f, 1.0f, ColorFadeAlpha(kColorSeparator, alpha));

	const SettingsRows rows = ComputeRows(layout.ScrollRegion, m_rowsScroll.m_flScrollOffset, m_fonts);

	// Real GPU-side clipping so a row that's scrolled halfway behind the header genuinely
	// can't paint outside ScrollRegion - same pattern CAccountModal's account list uses.
	drawList.PushClipRect(layout.ScrollRegion);

	if (RowInView(rows.Font, layout.ScrollRegion)) {
		DrawRowLabel(drawList, m_fonts, rows.Font, "Font", "The system font file applied to the UI.", alpha);
		const Rect field = FontFieldRect(rows.Font, m_fonts);
		// A genuine accent-colored border ring when focused (not just a subtly-tinted
		// fill, which read as ambiguous about which field was actually active).
		drawList.AddRectRoundedFilled(
			field.X, field.Y, field.W, field.H, CDrawList::UniformRadii(6.0f),
			ColorFadeAlpha(m_fontNameInput.m_bFocused ? m_settings.m_clrAccent : kColorControlBg, alpha));
		drawList.AddRectRoundedFilled(
			field.X + 1.5f, field.Y + 1.5f, field.W - 3.0f, field.H - 3.0f, CDrawList::UniformRadii(5.0f),
			ColorFadeAlpha(m_fontNameInput.m_bFocused ? ColorLerp(kColorBg, m_settings.m_clrAccent, 0.25f) : kColorBg,
						   alpha));
		// Body, not secondary - the typed font-name value, same reasoning as
		// CAccountModal's edit fields.
		m_fontNameInput.Draw(drawList, body, field.X, field.Y, field.W, field.H, ColorFadeAlpha(kColorText, alpha),
							 ColorFadeAlpha(m_settings.m_clrAccent, alpha), false);
	}

	if (RowInView(rows.FontSize, layout.ScrollRegion)) {
		DrawRowLabel(drawList, m_fonts, rows.FontSize, "Font Size", "Determines the scale of the whole UI.", alpha);
		DrawStepper(drawList, body, StepperRect(rows.FontSize, m_fonts), m_settings.m_flFontPixelSize, alpha);
	}

	if (RowInView(rows.SecondaryFontSize, layout.ScrollRegion)) {
		DrawRowLabel(drawList, m_fonts, rows.SecondaryFontSize, "Secondary Font Size",
					 "Scale of labels/hints and other small text.", alpha);
		DrawStepper(drawList, body, StepperRect(rows.SecondaryFontSize, m_fonts), m_settings.m_flSecondaryFontPixelSize,
					alpha);
	}

	if (RowInView(rows.Animations, layout.ScrollRegion)) {
		DrawRowLabel(drawList, m_fonts, rows.Animations, "Animations",
					 "Applies animations to popups, scrolling, and the caret.", alpha);
		DrawToggle(drawList, ToggleRect(rows.Animations, m_fonts), m_flAnimationsToggleAmount, m_settings.m_clrAccent,
				   alpha);
	}

	if (RowInView(rows.RoundedCorners, layout.ScrollRegion)) {
		DrawRowLabel(drawList, m_fonts, rows.RoundedCorners, "Round Corners",
					 "Applies rounding to popups, banners, and buttons.", alpha);
		DrawToggle(drawList, ToggleRect(rows.RoundedCorners, m_fonts), m_flRoundedCornersToggleAmount,
				   m_settings.m_clrAccent, alpha);
	}

	if (RowInView(rows.ExcludeFromCapture, layout.ScrollRegion)) {
		DrawRowLabel(drawList, m_fonts, rows.ExcludeFromCapture, "Hide From Screen Capture",
					 "Excludes the account list from screenshots and screen sharing.", alpha);
		DrawToggle(drawList, ToggleRect(rows.ExcludeFromCapture, m_fonts), m_flExcludeFromCaptureToggleAmount,
				   m_settings.m_clrAccent, alpha);
	}

	if (RowInView(rows.AnimationSpeed, layout.ScrollRegion)) {
		DrawRowLabel(drawList, m_fonts, rows.AnimationSpeed, "Animation Speed",
					 "How fast popups, scrolling, and toggles animate.", alpha);
		DrawSlider(drawList, body, SliderTrackRect(rows.AnimationSpeed, m_fonts), m_settings.m_flAnimationSpeed,
				   m_settings.m_clrAccent, alpha);
	}

	if (RowInView(rows.Accent, layout.ScrollRegion)) {
		DrawRowLabel(drawList, m_fonts, rows.Accent, "Accent Color", "Click to pick the selection/button accent color.",
					 alpha);
		const Rect swatch = SwatchRect(rows.Accent, m_fonts);
		drawList.AddRectRoundedFilled(swatch.X, swatch.Y, swatch.W, swatch.H, CDrawList::UniformRadii(6.0f),
									  ColorFadeAlpha(m_settings.m_clrAccent, alpha));
	}

	if (RowInView(rows.MasterPassword, layout.ScrollRegion)) {
		DrawRowLabel(drawList, m_fonts, rows.MasterPassword, "Master Password",
					 "Encrypts your saved account passwords.", alpha);

		const Rect button = MasterPasswordResetButtonRect(rows.MasterPassword, m_fonts);
		const bool hoverButton = RectContainsPoint(button, m_flMouseX, m_flMouseY);
		drawList.AddRectRoundedFilled(button.X, button.Y, button.W, button.H, CDrawList::UniformRadii(6.0f),
									  ColorFadeAlpha(hoverButton ? ColorLighten(kColorControlBg, 10) : kColorControlBg,
													alpha));
		DrawCenteredText(drawList, body, button.X, button.Y, button.W, button.H,
						 StringViewFromCString("Reset Password"), ColorFadeAlpha(kColorText, alpha));
	}

	drawList.PopClipRect();

	m_rowsScroll.DrawEdgeFade(drawList, layout.ScrollRegion, rows.ContentHeight, layout.ScrollRegion.H,
							  ColorFadeAlpha(kColorBg, alpha));
	m_rowsScroll.Draw(drawList, ScrollbarTrackRect(layout.ScrollRegion), rows.ContentHeight, layout.ScrollRegion.H,
					  ColorFadeAlpha(Color{120, 120, 128, 190}, alpha), m_flMouseX, m_flMouseY);

	drawList.AddRectFilled(layout.Footer.X + kRowPaddingX, layout.Footer.Y, layout.Footer.W - kRowPaddingX * 2.0f, 1.0f,
						   ColorFadeAlpha(kColorSeparator, alpha));
	DrawText(drawList, secondary, layout.Footer.X + kRowPaddingX,
			 layout.Footer.Y + (layout.Footer.H + secondary.GetAscent()) * 0.5f,
			 StringViewFromCString("Escape to dismiss"), ColorFadeAlpha(kColorTextDim, alpha));

	// Drawn last so it layers over every row above, including the footer - needs the
	// Accent row's current on-screen position, which only exists while that row's still
	// inside the clipped scroll region; if it's scrolled out of view there's nothing
	// sensible to anchor the popup to, so it's skipped too.
	if (RowInView(rows.Accent, layout.ScrollRegion)) {
		m_colorPicker.Draw(drawList);
	}
}
