#include "ui/settings_panel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <Windows.h>

#include "asset_manager.h"
#include "core/animator.h"
#include "gfx/font.h"
#include "ui/draw_list.h"
#include "ui/layout.h"
#include "ui/text.h"
#include "window.h"

namespace {
constexpr float kPanelOpenEaseRate = 16.0f;
constexpr float kToggleEaseRate = 18.0f;
// How fast the eased display copies of the numeric/color settings chase the real value -
// see CSettingsPanel's own m_flFontSizeDisplay. Close to the toggle rate on purpose: a
// row's control and its toggle-switch neighbours should settle at the same pace.
constexpr float kValueEaseRate = 16.0f;

// Base panel size, tuned at kReferenceBodyPixelHeight - PanelMaxSizeScale grows both
// in lockstep with a larger Font Size setting (see CAccountModal, which does the same
// thing) so bigger text gets more room instead of being clipped/overlapping at the
// original size.
// Wider than the 520 this shipped at: every row gained a reset button to the left of
// its control (see ResetButtonRect), and the label column shouldn't pay for it.
constexpr float kPanelMaxWidthBase = 570.0f;
// A comfortable viewport, not "tall enough to fit every row" - the row list scrolls
// once it overflows this, so it doesn't need to grow every time a row is added.
constexpr float kPanelMaxHeightBase = 480.0f;
// Real baked pixels (CFont::GetPixelHeight already includes CFontManager's display-
// scale factor), matching the default CSettings::m_flFontPixelSize of 16 nominal
// units - see kFontSizeMin/Max below for why the Settings-facing number isn't this.
constexpr float kReferenceBodyPixelHeight = 24.0f;
// The same real-baked-pixels figure for the secondary face, matching the default
// CSettings::m_flSecondaryFontPixelSize of 12 nominal units - see PanelMaxSizeScale.
constexpr float kReferenceSecondaryPixelHeight = 20.0f;
constexpr float kPanelMargin = 48.0f;
constexpr float kPanelScaleMin = 0.94f;
constexpr float kPanelRadius = 16.0f;
constexpr float kPanelBorderThickness = 1.5f;

constexpr float kRowPaddingX = 26.0f;
constexpr float kCloseSize = 26.0f;

// All three were tightened-up numbers (8/4/10) that left rows reading as one dense block
// of text with the controls crowded against it. Every one of them is a gap *between*
// things rather than a fixed row height, so raising them spaces the list out without any
// row ever clipping its own contents at a larger Font Size - RowHeightFor is the sum of
// these plus the two real line heights, and every control centers off the same metrics
// (see RowControlCenterY).
constexpr float kRowLabelTopGap = 14.0f;	// above the title line
constexpr float kRowLabelLineGap = 6.0f;	// between the title and description lines
constexpr float kRowLabelBottomGap = 16.0f; // below the description line

// The hover highlight painted behind whichever row the cursor is over - inset from the
// row's own padding so it reads as a band around the content rather than a full-bleed
// stripe touching the panel's edges.
constexpr float kRowHighlightInsetX = 12.0f;
constexpr float kRowHighlightRadius = 8.0f;

// A row's "restore this setting's default" button: a square icon button sitting just left
// of that row's own control (matching FilePilot's settings list, which is where this
// affordance comes from), so it never collides with the label text on the left or the
// control on the right.
constexpr float kResetButtonSize = 28.0f;
constexpr float kResetButtonGap = 10.0f;
constexpr float kResetIconSize = 18.0f;
constexpr float kResetAppearRate = 20.0f;
// How fast a clicked button's spin unwinds - deliberately slower than the fade so the
// turn stays legible rather than being over before the eye lands on it.
constexpr float kResetSpinRate = 6.0f;
constexpr float kTwoPi = 6.28318530717958647692f;

// Section headings ("Appearance", "Motion", ...) grouping the row list - a dim secondary-
// font label plus the breathing room above it that actually does the grouping work.
constexpr float kSectionHeaderTopGap = 22.0f;
constexpr float kSectionHeaderBottomGap = 8.0f;

constexpr float kSliderTrackWidth = 130.0f;
constexpr float kSliderTrackVisualHeight = 6.0f;
constexpr float kSliderThumbRadius = 8.0f;
constexpr float kSliderValueLabelGap = 8.0f;
// Reserved for the slider's own "1.00x" readout, left of the track - a fixed reservation
// rather than the measured text width so the reset button left of it (see
// SliderControlRect) doesn't shuffle sideways as the value's digit count changes.
constexpr float kSliderValueLabelWidth = 48.0f;
constexpr float kAnimationSpeedMin = 0.25f;
constexpr float kAnimationSpeedMax = 3.0f;

// 1.0 is the radius every widget in this project was drawn at, and 0 squares everything
// off. The ceiling is 1.5 rather than something larger because the rounded-rect builder
// clamps every radius to half the shape's shorter side (see BuildRoundedRectPoints): a
// control already drawn as a pill - a toggle track, a slider thumb, a circular badge - is
// at that limit by definition and cannot get rounder no matter what this says, and past
// roughly 1.5 most of the remaining shapes hit it too, which made the top of a longer
// slider feel dead.
constexpr float kCornerRoundnessMin = 0.0f;
constexpr float kCornerRoundnessMax = 1.5f;

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
constexpr Color kColorSeparator{58, 58, 64, 255};
constexpr Color kColorControlBg{40, 40, 45, 255};
constexpr Color kColorToggleOff{70, 70, 76, 255};
constexpr Color kColorRowHover{36, 36, 41, 255};
// A reset button only exists while it does something (see UpdateResetButtons), so there's
// no dimmed-but-present state to color for - just the resting icon and its hover, which is
// a real step lighter than the row behind it rather than the barely-there tint it was.
constexpr Color kColorResetIdle{150, 150, 158, 255};
constexpr Color kColorResetHoverBg{64, 64, 72, 255};

// Every size below derives from the active fonts' actual glyph metrics
// (CFont::GetLineHeight: ascent+descent+line_gap, a real baseline-to-baseline
// distance) instead of a fixed pixel constant, so a larger Font Size setting grows
// rows/header/footer to fit instead of the title/description lines overlapping once
// text no longer fits the original small-font layout.
// Both faces, not just the body one: a row's description line is set in the secondary
// face, and Secondary Font Size is independently adjustable (see kSecondaryFontSizeMax), so
// scaling off body alone left the panel exactly as narrow as before while the longest text
// on every row grew - which is how a description ended up running into the control beside
// it. The secondary face is compared against its own smaller reference height so that a
// default-sized secondary contributes a scale of 1.0 the same way a default body does.
float PanelMaxSizeScale(const CFontManager &fonts)
{
	const float bodyScale = fonts.GetBody().GetPixelHeight() / kReferenceBodyPixelHeight;
	const float secondaryScale = fonts.GetSecondary().GetPixelHeight() / kReferenceSecondaryPixelHeight;
	return std::max(1.0f, std::max(bodyScale, secondaryScale));
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

// A section heading strip ("Appearance", "Motion", ...) - the same metrics-derived
// sizing every other height here uses, so headings grow with Font Size too.
float SectionHeaderHeightFor(const CFontManager &fonts)
{
	return kSectionHeaderTopGap + fonts.GetSecondary().GetLineHeight() + kSectionHeaderBottomGap;
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

// The slider's full visual footprint - the draggable track plus the reserved space its
// "1.00x" readout occupies to the left of it. SliderTrackRect alone is the hit rect; this
// is what the reset button positions itself against, so the button lands left of the
// readout instead of on top of it.
Rect SliderControlRect(Rect row, const CFontManager &fonts)
{
	const Rect track = SliderTrackRect(row, fonts);
	const float reserved = kSliderValueLabelGap + kSliderValueLabelWidth;
	return Rect{track.X - reserved, track.Y, track.W + reserved, track.H};
}

// A row's reset button, immediately left of that row's own control. Deriving it from the
// control (rather than pinning it to a fixed column) is what keeps it adjacent to the
// thing it restores on every row, whether that control is a wide text field or a narrow
// toggle - the arrangement FilePilot's own settings list uses.
Rect ResetButtonRect(Rect control, Rect row, const CFontManager &fonts)
{
	return Rect{control.X - kResetButtonGap - kResetButtonSize, RowControlCenterY(row, fonts) - kResetButtonSize * 0.5f,
				kResetButtonSize, kResetButtonSize};
}

float AnimationSpeedToT(float speed)
{
	return std::clamp((speed - kAnimationSpeedMin) / (kAnimationSpeedMax - kAnimationSpeedMin), 0.0f, 1.0f);
}

float AnimationSpeedFromT(float t)
{
	return kAnimationSpeedMin + std::clamp(t, 0.0f, 1.0f) * (kAnimationSpeedMax - kAnimationSpeedMin);
}

float CornerRoundnessToT(float roundness)
{
	return std::clamp((roundness - kCornerRoundnessMin) / (kCornerRoundnessMax - kCornerRoundnessMin), 0.0f, 1.0f);
}

float CornerRoundnessFromT(float t)
{
	return kCornerRoundnessMin + std::clamp(t, 0.0f, 1.0f) * (kCornerRoundnessMax - kCornerRoundnessMin);
}

// Every row's rect, built by walking a single RectSplitTop cursor down the scroll
// region - each row claims exactly the height it needs and the next one starts
// exactly where it ended, so rows can never overlap or get squeezed regardless of how
// many exist or how tall one is. scrollOffset shifts the whole stack up as the user
// scrolls down, same as CAccountModal's account rows; ContentHeight (the sum of every
// row's height, independent of scrollOffset) is what the scrollbar needs to know
// whether there's anything to scroll at all.
struct SettingsRows {
	Rect SectionAppearance;
	Rect Font;
	Rect FontSize;
	Rect SecondaryFontSize;
	Rect Accent;
	Rect CornerRoundness;
	Rect SectionMotion;
	Rect Animations;
	Rect AnimationSpeed;
	Rect SectionPrivacy;
	Rect ExcludeFromCapture;
	Rect CloseToTray;
	Rect SectionSecurity;
	Rect MasterPassword;
	float ContentHeight;
};

// Walks the RectSplitTop cursor described above, in the fixed order the panel always
// draws in. Every row (including Master Password now - see this file's own header
// comment) is a plain fixed-height row, so this needs no extra state from the caller the
// way it once did to size the old tier sub-layout. Rows are grouped under section
// headings rather than listed in one undifferentiated stack: what a setting actually
// affects (how the app looks, how it moves, what it keeps private) is the only ordering a
// reader can navigate by, and each heading carves its own strip off this same cursor, so a
// heading can no more overlap a row than two rows can overlap each other. The first
// heading gets a smaller top gap than the rest - there's no preceding group for it to
// separate from, just the header rule right above it.
SettingsRows ComputeRows(Rect scrollRegion, float scrollOffset, const CFontManager &fonts)
{
	SettingsRows rows{};
	Rect cursor{scrollRegion.X, scrollRegion.Y - scrollOffset, scrollRegion.W, 1.0e6f};
	const float startY = cursor.Y;

	const float rowHeight = RowHeightFor(fonts);
	const float sectionHeight = SectionHeaderHeightFor(fonts);

	rows.SectionAppearance = RectSplitTop(cursor, sectionHeight - kSectionHeaderTopGap * 0.5f);
	rows.Font = RectSplitTop(cursor, rowHeight);
	rows.FontSize = RectSplitTop(cursor, rowHeight);
	rows.SecondaryFontSize = RectSplitTop(cursor, rowHeight);
	rows.Accent = RectSplitTop(cursor, rowHeight);
	rows.CornerRoundness = RectSplitTop(cursor, rowHeight);

	rows.SectionMotion = RectSplitTop(cursor, sectionHeight);
	rows.Animations = RectSplitTop(cursor, rowHeight);
	rows.AnimationSpeed = RectSplitTop(cursor, rowHeight);

	rows.SectionPrivacy = RectSplitTop(cursor, sectionHeight);
	rows.ExcludeFromCapture = RectSplitTop(cursor, rowHeight);
	rows.CloseToTray = RectSplitTop(cursor, rowHeight);

	rows.SectionSecurity = RectSplitTop(cursor, sectionHeight);
	rows.MasterPassword = RectSplitTop(cursor, rowHeight);

	rows.ContentHeight = cursor.Y - startY;
	return rows;
}

// The row a reset target lives on. Every switch over ESettingsResetTarget in this file is
// deliberately exhaustive with no default label, so adding a target is a compile error
// here (and in ResetTargetControlRect, and in the panel's own IsTargetAtDefault/
// ResetTargetToDefault) rather than a button that silently does nothing.
Rect ResetTargetRowRect(const SettingsRows &rows, ESettingsResetTarget target)
{
	switch (target) {
		case ESettingsResetTarget::SETTINGS_RESET_FONT:
			return rows.Font;
		case ESettingsResetTarget::SETTINGS_RESET_FONT_SIZE:
			return rows.FontSize;
		case ESettingsResetTarget::SETTINGS_RESET_SECONDARY_FONT_SIZE:
			return rows.SecondaryFontSize;
		case ESettingsResetTarget::SETTINGS_RESET_ACCENT:
			return rows.Accent;
		case ESettingsResetTarget::SETTINGS_RESET_CORNER_ROUNDNESS:
			return rows.CornerRoundness;
		case ESettingsResetTarget::SETTINGS_RESET_ANIMATIONS:
			return rows.Animations;
		case ESettingsResetTarget::SETTINGS_RESET_ANIMATION_SPEED:
			return rows.AnimationSpeed;
		case ESettingsResetTarget::SETTINGS_RESET_EXCLUDE_FROM_CAPTURE:
			return rows.ExcludeFromCapture;
		case ESettingsResetTarget::SETTINGS_RESET_CLOSE_TO_TRAY:
			return rows.CloseToTray;
		case ESettingsResetTarget::SETTINGS_RESET_COUNT:
			break;
	}
	return Rect{};
}

// The control a reset target's button sits next to - see ResetButtonRect.
Rect ResetTargetControlRect(const SettingsRows &rows, ESettingsResetTarget target, const CFontManager &fonts)
{
	switch (target) {
		case ESettingsResetTarget::SETTINGS_RESET_FONT:
			return FontFieldRect(rows.Font, fonts);
		case ESettingsResetTarget::SETTINGS_RESET_FONT_SIZE:
			return StepperRect(rows.FontSize, fonts);
		case ESettingsResetTarget::SETTINGS_RESET_SECONDARY_FONT_SIZE:
			return StepperRect(rows.SecondaryFontSize, fonts);
		case ESettingsResetTarget::SETTINGS_RESET_ACCENT:
			return SwatchRect(rows.Accent, fonts);
		case ESettingsResetTarget::SETTINGS_RESET_CORNER_ROUNDNESS:
			return SliderControlRect(rows.CornerRoundness, fonts);
		case ESettingsResetTarget::SETTINGS_RESET_ANIMATIONS:
			return ToggleRect(rows.Animations, fonts);
		case ESettingsResetTarget::SETTINGS_RESET_ANIMATION_SPEED:
			return SliderControlRect(rows.AnimationSpeed, fonts);
		case ESettingsResetTarget::SETTINGS_RESET_EXCLUDE_FROM_CAPTURE:
			return ToggleRect(rows.ExcludeFromCapture, fonts);
		case ESettingsResetTarget::SETTINGS_RESET_CLOSE_TO_TRAY:
			return ToggleRect(rows.CloseToTray, fonts);
		case ESettingsResetTarget::SETTINGS_RESET_COUNT:
			break;
	}
	return Rect{};
}

// Where a row's label column has to stop. Always leaves room for the reset button whether
// or not one is currently showing: the button comes and goes as the value moves on and off
// its default, and a label that re-flowed every time it did would be far more distracting
// than the few pixels of column this costs.
float LabelRightEdge(Rect control, Rect row, const CFontManager &fonts)
{
	constexpr float kLabelControlGap = 16.0f;
	return ResetButtonRect(control, row, fonts).X - kLabelControlGap;
}

// Both of the above at once, since every caller (hover, cursor, click, draw) wants the
// button's own rect and nothing else.
Rect ResetTargetButtonRect(const SettingsRows &rows, ESettingsResetTarget target, const CFontManager &fonts)
{
	return ResetButtonRect(ResetTargetControlRect(rows, target, fonts), ResetTargetRowRect(rows, target), fonts);
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
//
// labelRightEdge is where the label column actually ends: the left edge of whatever this
// row's own control (or its reset button, when one is showing) occupies, minus a gap.
// Both lines are ellipsized to it rather than being drawn at full length, because at a
// large Font Size / Secondary Font Size they genuinely do reach the control - and text
// painted straight through a stepper or a toggle is the worst of the available outcomes.
// The caller passes this rather than DrawRowLabel deriving it, since only the caller knows
// which control this row has.
void DrawRowLabel(CDrawList &drawList, const CFontManager &fonts, Rect row, const char *pTitle,
				  const char *pDescription, float labelRightEdge, std::uint8_t alpha)
{
	const CFont &body = fonts.GetBody();
	const CFont &secondary = fonts.GetSecondary();
	const float titleBaselineY = row.Y + kRowLabelTopGap + body.GetAscent();
	const float descriptionBaselineY =
		row.Y + kRowLabelTopGap + body.GetLineHeight() + kRowLabelLineGap + secondary.GetAscent();
	const float labelX = row.X + kRowPaddingX;
	const float maxWidth = labelRightEdge - labelX;
	DrawTextEllipsized(drawList, body, labelX, titleBaselineY, StringViewFromCString(pTitle), maxWidth,
					   ColorFadeAlpha(kColorText, alpha));
	DrawTextEllipsized(drawList, secondary, labelX, descriptionBaselineY, StringViewFromCString(pDescription), maxWidth,
					   ColorFadeAlpha(kColorTextDim, alpha));
}

// A section heading - the group's name and a hairline running out to the row padding on
// the right, and nothing else: an accent bar on the left made the headings compete with
// the rows they were only supposed to be labelling. Bottom-aligned within its strip rather
// than top-aligned, so the first heading (whose strip is deliberately shorter - see
// ComputeRows) still sits the same distance above its own first row as every other one.
//
// The rule lands on the text's real visual center: baseline minus half of (ascent +
// descent), since descent is negative (see font.h). The obvious-looking (descent - ascent)
// form is a different quantity entirely - half the glyph height rather than the offset to
// its middle - and it sat the rule visibly high of the text it runs beside.
void DrawSectionHeader(CDrawList &drawList, const CFontManager &fonts, Rect rect, const char *pTitle,
					   std::uint8_t alpha)
{
	const CFont &secondary = fonts.GetSecondary();
	const CStringView title = StringViewFromCString(pTitle);
	const float baselineY =
		rect.Y + rect.H - kSectionHeaderBottomGap - (secondary.GetLineHeight() - secondary.GetAscent());
	const float centerY = baselineY - (secondary.GetAscent() + secondary.GetDescent()) * 0.5f;

	// A short lead-in rule, the label, then the long rule out to the padding - "--- Motion
	// --------". The stub on the left is what makes the heading read as part of the rule
	// rather than as a line that happens to start after some text, and it gives every
	// heading the same left edge as the rows beneath it.
	constexpr float kTextRuleGap = 10.0f;
	constexpr float kLeadRuleWidth = 16.0f;
	const float leadX = rect.X + kRowPaddingX;
	drawList.AddRectFilled(leadX, centerY, kLeadRuleWidth, 1.0f, ColorFadeAlpha(kColorSeparator, alpha));

	const float textX = leadX + kLeadRuleWidth + kTextRuleGap;
	DrawText(drawList, secondary, textX, baselineY, title, ColorFadeAlpha(kColorText, alpha));

	const float ruleX = textX + TextWidth(secondary, title) + kTextRuleGap;
	const float ruleRight = rect.X + rect.W - kRowPaddingX;
	if (ruleRight > ruleX) {
		drawList.AddRectFilled(ruleX, centerY, ruleRight - ruleX, 1.0f, ColorFadeAlpha(kColorSeparator, alpha));
	}
}

// A row's reset button - present only while its row is actually off its default, so the
// button appearing *is* the signal that there's something to restore. appearAmount fades
// it in; spinAmount is 1.0 the instant it's clicked and eases back to 0, drawn here as a
// full turn, so the icon winds back while the value it restored animates back and the two
// read as one event.
void DrawResetButton(CDrawList &drawList, const CTexture *pIcon, Rect rect, float appearAmount, float spinAmount,
					 bool hovered, std::uint8_t alpha)
{
	if (appearAmount <= 0.01f || pIcon == nullptr) {
		return;
	}

	const auto fade = static_cast<std::uint8_t>(static_cast<float>(alpha) * appearAmount);
	if (hovered) {
		drawList.AddRectRoundedFilled(rect.X, rect.Y, rect.W, rect.H, CDrawList::UniformRadii(7.0f),
									  ColorFadeAlpha(kColorResetHoverBg, fade));
	}

	// Counter-clockwise: this restores a previous value, and a spin that reads as winding
	// back is the whole point of animating it at all.
	const float radians = -spinAmount * kTwoPi;
	const float iconSize = kResetIconSize;
	drawList.AddRectTexturedRotated(rect.X + (rect.W - iconSize) * 0.5f, rect.Y + (rect.H - iconSize) * 0.5f, iconSize,
									iconSize, radians, pIcon,
									ColorFadeAlpha(hovered ? kColorText : kColorResetIdle, fade));
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

// track is the slider's hit rect (SliderTrackRect); t is where the thumb sits (0..1) and
// valueText is the readout drawn to its left in reserved space, so it never crowds the
// track regardless of digit count - same reasoning DrawStepper's centered value text uses.
// Both are passed in rather than derived here, because the two sliders this serves measure
// different things ("1.35x" of speed, "120%" of roundness) and neither range belongs in a
// drawing function.
void DrawSlider(CDrawList &drawList, const CFont &font, Rect track, float t, CStringView valueText, Color accent,
				std::uint8_t alpha)
{
	const CStringView text = valueText;
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

	const float fillW = track.W * std::clamp(t, 0.0f, 1.0f);
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

	// Rounded, not truncated: `value` is an eased display copy (see CSettingsPanel's own
	// m_flFontSizeDisplay), so it spends most of a reset animation between two integers,
	// and truncating would make the readout lag the control by a whole step.
	char buffer[8];
	const int written = std::snprintf(buffer, sizeof(buffer), "%d", static_cast<int>(std::lround(value)));
	const CStringView text{buffer, written > 0 ? static_cast<std::uint64_t>(written) : 0};
	const float textW = TextWidth(font, text);
	const float middleX = minus.X + minus.W;
	const float middleW = plus.X - middleX;
	DrawText(drawList, font, middleX + (middleW - textW) * 0.5f,
			 rect.Y + rect.H * 0.5f + (font.GetAscent() + font.GetDescent()) * 0.5f, text,
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

CSettingsPanel::CSettingsPanel(CFontManager &fonts, CSettings &settings, CWindow &window, IRenderer &renderer,
							   CAssetManager &assets)
	: m_fonts(fonts)
	, m_settings(settings)
	, m_window(window)
	, m_renderer(renderer)
	, m_assets(assets)
{
	m_fontNameInput.Init(StringViewFromCString(m_settings.m_szFontName));

	// Seeded from the real values, not left at zero: these only exist to make a *change*
	// animate, so the panel's very first frame must already be showing the truth rather
	// than easing up to it from nothing.
	m_flFontSizeDisplay = m_settings.m_flFontPixelSize;
	m_flSecondaryFontSizeDisplay = m_settings.m_flSecondaryFontPixelSize;
	m_flAnimationSpeedDisplay = m_settings.m_flAnimationSpeed;
	m_flCornerRoundnessDisplay = m_settings.m_flCornerRoundness;
	m_flAccentDisplayR = static_cast<float>(m_settings.m_clrAccent.R);
	m_flAccentDisplayG = static_cast<float>(m_settings.m_clrAccent.G);
	m_flAccentDisplayB = static_cast<float>(m_settings.m_clrAccent.B);
}

bool CSettingsPanel::IsTargetAtDefault(ESettingsResetTarget target) const
{
	// The shipped defaults, straight off a default-constructed record rather than a second
	// hand-maintained table here - CSettings' own member initializers are the one place
	// this project states what a setting starts as, and a copy of them here would drift
	// the first time one changed.
	const CSettings defaults;
	constexpr float kEpsilon = 0.001f;

	switch (target) {
		case ESettingsResetTarget::SETTINGS_RESET_FONT:
			// Both the applied name and whatever is currently typed into the field: an
			// un-applied edit sitting in the box is exactly the kind of thing a reset is
			// for, so the button stays live until the box itself reads the default too.
			return std::strcmp(m_settings.m_szFontName, defaults.m_szFontName) == 0 &&
				   m_fontNameInput.GetValue().Length == std::strlen(defaults.m_szFontName) &&
				   std::memcmp(m_fontNameInput.GetValue().pData, defaults.m_szFontName,
							   m_fontNameInput.GetValue().Length) == 0;
		case ESettingsResetTarget::SETTINGS_RESET_FONT_SIZE:
			return std::fabs(m_settings.m_flFontPixelSize - defaults.m_flFontPixelSize) < kEpsilon;
		case ESettingsResetTarget::SETTINGS_RESET_SECONDARY_FONT_SIZE:
			return std::fabs(m_settings.m_flSecondaryFontPixelSize - defaults.m_flSecondaryFontPixelSize) < kEpsilon;
		case ESettingsResetTarget::SETTINGS_RESET_ACCENT:
			return m_settings.m_clrAccent.R == defaults.m_clrAccent.R &&
				   m_settings.m_clrAccent.G == defaults.m_clrAccent.G &&
				   m_settings.m_clrAccent.B == defaults.m_clrAccent.B &&
				   m_settings.m_clrAccent.A == defaults.m_clrAccent.A;
		case ESettingsResetTarget::SETTINGS_RESET_CORNER_ROUNDNESS:
			return std::fabs(m_settings.m_flCornerRoundness - defaults.m_flCornerRoundness) < kEpsilon;
		case ESettingsResetTarget::SETTINGS_RESET_ANIMATIONS:
			return m_settings.m_bAnimationsEnabled == defaults.m_bAnimationsEnabled;
		case ESettingsResetTarget::SETTINGS_RESET_ANIMATION_SPEED:
			return std::fabs(m_settings.m_flAnimationSpeed - defaults.m_flAnimationSpeed) < kEpsilon;
		case ESettingsResetTarget::SETTINGS_RESET_EXCLUDE_FROM_CAPTURE:
			return m_settings.m_bExcludeAccountListFromCapture == defaults.m_bExcludeAccountListFromCapture;
		case ESettingsResetTarget::SETTINGS_RESET_CLOSE_TO_TRAY:
			return m_settings.m_bCloseToTray == defaults.m_bCloseToTray;
		case ESettingsResetTarget::SETTINGS_RESET_COUNT:
			break;
	}
	return true;
}

void CSettingsPanel::ResetTargetToDefault(ESettingsResetTarget target)
{
	const CSettings defaults;

	switch (target) {
		case ESettingsResetTarget::SETTINGS_RESET_FONT:
			m_fontNameInput.SetValue(StringViewFromCString(defaults.m_szFontName));
			// Only mirrored into settings if the bake actually succeeds, same rule every
			// other font edit here follows (see SyncAppliedFontName) - the default face is
			// as capable of being missing on a given machine as a typed one.
			if (m_fonts.ApplyBody(m_renderer, m_fontNameInput.GetValue(), m_settings.m_flFontPixelSize,
								  m_settings.m_flSecondaryFontPixelSize, m_window.GetDpiScale())) {
				SyncAppliedFontName(m_settings, m_fontNameInput.GetValue());
			}
			break;
		case ESettingsResetTarget::SETTINGS_RESET_FONT_SIZE:
			m_settings.m_flFontPixelSize = defaults.m_flFontPixelSize;
			if (m_fonts.ApplyBody(m_renderer, m_fontNameInput.GetValue(), m_settings.m_flFontPixelSize,
								  m_settings.m_flSecondaryFontPixelSize, m_window.GetDpiScale())) {
				SyncAppliedFontName(m_settings, m_fontNameInput.GetValue());
			}
			break;
		case ESettingsResetTarget::SETTINGS_RESET_SECONDARY_FONT_SIZE:
			m_settings.m_flSecondaryFontPixelSize = defaults.m_flSecondaryFontPixelSize;
			if (m_fonts.ApplyBody(m_renderer, m_fontNameInput.GetValue(), m_settings.m_flFontPixelSize,
								  m_settings.m_flSecondaryFontPixelSize, m_window.GetDpiScale())) {
				SyncAppliedFontName(m_settings, m_fontNameInput.GetValue());
			}
			break;
		case ESettingsResetTarget::SETTINGS_RESET_ACCENT:
			m_settings.m_clrAccent = defaults.m_clrAccent;
			// The picker popup, if it happens to be open, is showing the color that just
			// stopped being current - closing it is the honest outcome, and reopening it
			// picks up the restored value the way it always does.
			m_colorPicker.Close();
			break;
		case ESettingsResetTarget::SETTINGS_RESET_CORNER_ROUNDNESS:
			// No SetCornerRoundnessScale here on purpose - applying it now is exactly what
			// made every corner in the app snap to the default while the slider was still
			// gliding back to it. Update drives the real scale from the eased display copy,
			// so leaving this alone is what animates it.
			m_settings.m_flCornerRoundness = defaults.m_flCornerRoundness;
			break;
		case ESettingsResetTarget::SETTINGS_RESET_ANIMATIONS:
			m_settings.m_bAnimationsEnabled = defaults.m_bAnimationsEnabled;
			CAnimator::SetEnabled(m_settings.m_bAnimationsEnabled);
			break;
		case ESettingsResetTarget::SETTINGS_RESET_ANIMATION_SPEED:
			m_settings.m_flAnimationSpeed = defaults.m_flAnimationSpeed;
			CAnimator::SetSpeed(m_settings.m_flAnimationSpeed);
			break;
		case ESettingsResetTarget::SETTINGS_RESET_EXCLUDE_FROM_CAPTURE:
			m_settings.m_bExcludeAccountListFromCapture = defaults.m_bExcludeAccountListFromCapture;
			break;
		case ESettingsResetTarget::SETTINGS_RESET_CLOSE_TO_TRAY:
			m_settings.m_bCloseToTray = defaults.m_bCloseToTray;
			break;
		case ESettingsResetTarget::SETTINGS_RESET_COUNT:
			return;
	}

	m_aResetSpinAmount[static_cast<std::uint64_t>(target)] = 1.0f;
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
	// Nothing left on screen for a bubble to point at - without this it would hang over
	// whatever is behind the panel for the length of its own fade.
	m_tooltip.Reset();
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
	m_flCloseToTrayToggleAmount = CAnimator::EaseToward(
		m_flCloseToTrayToggleAmount, m_settings.m_bCloseToTray ? 1.0f : 0.0f, kToggleEaseRate, deltaSeconds);
	m_flExcludeFromCaptureToggleAmount =
		CAnimator::EaseToward(m_flExcludeFromCaptureToggleAmount,
							  m_settings.m_bExcludeAccountListFromCapture ? 1.0f : 0.0f, kToggleEaseRate, deltaSeconds);

	// Eased display copies of everything whose control would otherwise snap - see the
	// members' own comment in settings_panel.h. The Animation Speed slider is the one
	// exception while it's actually being dragged: easing there would make the thumb trail
	// the cursor, which reads as lag, not as animation.
	m_flFontSizeDisplay =
		CAnimator::EaseToward(m_flFontSizeDisplay, m_settings.m_flFontPixelSize, kValueEaseRate, deltaSeconds);
	m_flSecondaryFontSizeDisplay = CAnimator::EaseToward(
		m_flSecondaryFontSizeDisplay, m_settings.m_flSecondaryFontPixelSize, kValueEaseRate, deltaSeconds);
	if (m_animationSpeedDrag.IsPressed()) {
		m_flAnimationSpeedDisplay = m_settings.m_flAnimationSpeed;
	} else {
		m_flAnimationSpeedDisplay = CAnimator::EaseToward(m_flAnimationSpeedDisplay, m_settings.m_flAnimationSpeed,
														  kValueEaseRate, deltaSeconds);
	}
	if (m_cornerRoundnessDrag.IsPressed()) {
		m_flCornerRoundnessDisplay = m_settings.m_flCornerRoundness;
	} else {
		m_flCornerRoundnessDisplay = CAnimator::EaseToward(m_flCornerRoundnessDisplay, m_settings.m_flCornerRoundness,
														   kValueEaseRate, deltaSeconds);
	}
	// Corner Roundness is the one display copy that also drives the real thing: the whole
	// UI's corners follow the eased value rather than the settled one, so a reset travels
	// back exactly as if the slider were being dragged there. Every other display copy here
	// only feeds its own control's drawing, because nothing else on this panel is a live
	// property of every shape on screen. CSettings still holds the settled value - that's
	// what persists - and this runs every frame whether the panel is open or not, so the
	// two are never left disagreeing.
	CDrawList::SetCornerRoundnessScale(m_flCornerRoundnessDisplay);

	m_flAccentDisplayR = CAnimator::EaseToward(m_flAccentDisplayR, static_cast<float>(m_settings.m_clrAccent.R),
											   kValueEaseRate, deltaSeconds);
	m_flAccentDisplayG = CAnimator::EaseToward(m_flAccentDisplayG, static_cast<float>(m_settings.m_clrAccent.G),
											   kValueEaseRate, deltaSeconds);
	m_flAccentDisplayB = CAnimator::EaseToward(m_flAccentDisplayB, static_cast<float>(m_settings.m_clrAccent.B),
											   kValueEaseRate, deltaSeconds);

	UpdateResetButtons(deltaSeconds);

	m_fontNameInput.Update(deltaSeconds);
	m_rowsScroll.Update(deltaSeconds);
	m_tooltip.Update(deltaSeconds);
}

// Hover-driven state for the per-row reset buttons, plus the tooltip request for whichever
// one the cursor is resting on. Lives in Update rather than Draw so the fade starts on the
// same frame the hover does and Draw stays a pure paint - see ui/tooltip.h's own note on
// why Request belongs here.
void CSettingsPanel::UpdateResetButtons(float deltaSeconds)
{
	const SettingsPanelLayout layout = ComputeLayout(m_flOpenAmount, static_cast<float>(m_window.GetWidth()),
													 static_cast<float>(m_window.GetHeight()), m_fonts);
	const SettingsRows rows = ComputeRows(layout.ScrollRegion, m_rowsScroll.m_flScrollOffset, m_fonts);

	// A closed panel has no hover to speak of, and the color picker's popup covers rows it
	// would otherwise look like the cursor is over.
	const bool pointerLive =
		IsBlocking() && !m_colorPicker.IsBlocking() && RectContainsPoint(layout.ScrollRegion, m_flMouseX, m_flMouseY);

	for (std::uint64_t i = 0; i < static_cast<std::uint64_t>(ESettingsResetTarget::SETTINGS_RESET_COUNT); i += 1) {
		const auto target = static_cast<ESettingsResetTarget>(i);
		const Rect row = ResetTargetRowRect(rows, target);
		const bool inView = RowInView(row, layout.ScrollRegion);
		const bool atDefault = IsTargetAtDefault(target);

		// Shown only while it would actually do something. A button that appears on hover
		// even when the row is already at its default is a control that does nothing when
		// clicked, and no amount of dimming makes that read as anything but broken - so the
		// button's presence is the signal, and its absence means "this is the default."
		const float appearTarget = (IsBlocking() && !atDefault) ? 1.0f : 0.0f;
		m_aResetAppearAmount[i] =
			CAnimator::EaseToward(m_aResetAppearAmount[i], appearTarget, kResetAppearRate, deltaSeconds);
		m_aResetSpinAmount[i] = CAnimator::EaseToward(m_aResetSpinAmount[i], 0.0f, kResetSpinRate, deltaSeconds);
		if (m_aResetSpinAmount[i] < 0.002f) {
			m_aResetSpinAmount[i] = 0.0f;
		}

		if (pointerLive && inView && !atDefault) {
			const Rect button = ResetTargetButtonRect(rows, target, m_fonts);
			if (RectContainsPoint(button, m_flMouseX, m_flMouseY)) {
				m_tooltip.Request(StringViewFromCString("Restore default setting."), button);
			}
		}
	}
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

	if (RowInView(rows.CornerRoundness, layout.ScrollRegion)) {
		const Rect slider = SliderTrackRect(rows.CornerRoundness, m_fonts);
		if (RectContainsPoint(slider, x, y)) {
			m_cornerRoundnessDrag.Begin(x, y);
			m_settings.m_flCornerRoundness = CornerRoundnessFromT((x - slider.X) / slider.W);
			CDrawList::SetCornerRoundnessScale(m_settings.m_flCornerRoundness);
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

	if (m_cornerRoundnessDrag.IsPressed()) {
		m_cornerRoundnessDrag.Update(x, y);
		const Rect slider = SliderTrackRect(rows.CornerRoundness, m_fonts);
		m_settings.m_flCornerRoundness = CornerRoundnessFromT((x - slider.X) / slider.W);
		// Applied live while dragging, like every other setting here - the whole UI rounds
		// off under the cursor, which is the only useful way to pick this value.
		CDrawList::SetCornerRoundnessScale(m_settings.m_flCornerRoundness);
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

	const bool wasDraggingSlider = m_animationSpeedDrag.IsPressed() || m_cornerRoundnessDrag.IsPressed();
	m_animationSpeedDrag.End();
	m_cornerRoundnessDrag.End();

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

	if (m_rowsScroll.IsDragging() || m_colorPicker.IsDragging() || m_animationSpeedDrag.IsPressed() ||
		m_cornerRoundnessDrag.IsPressed()) {
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

	// Same order as HandleClick's own dispatch: a reset button overlaps no other control,
	// but it does sit inside its row, so it answers first for the same reason.
	for (std::uint64_t i = 0; i < static_cast<std::uint64_t>(ESettingsResetTarget::SETTINGS_RESET_COUNT); i += 1) {
		const auto target = static_cast<ESettingsResetTarget>(i);
		if (!RowInView(ResetTargetRowRect(rows, target), layout.ScrollRegion) || IsTargetAtDefault(target)) {
			continue;
		}
		if (RectContainsPoint(ResetTargetButtonRect(rows, target, m_fonts), m_flMouseX, m_flMouseY)) {
			return ECursorKind::CURSOR_HAND;
		}
	}

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
	if (RowInView(rows.ExcludeFromCapture, layout.ScrollRegion) &&
		RectContainsPoint(ToggleRect(rows.ExcludeFromCapture, m_fonts), m_flMouseX, m_flMouseY)) {
		return ECursorKind::CURSOR_HAND;
	}
	if (RowInView(rows.CloseToTray, layout.ScrollRegion) &&
		RectContainsPoint(ToggleRect(rows.CloseToTray, m_fonts), m_flMouseX, m_flMouseY)) {
		return ECursorKind::CURSOR_HAND;
	}
	if (RowInView(rows.AnimationSpeed, layout.ScrollRegion) &&
		RectContainsPoint(SliderTrackRect(rows.AnimationSpeed, m_fonts), m_flMouseX, m_flMouseY)) {
		return ECursorKind::CURSOR_HAND;
	}
	if (RowInView(rows.CornerRoundness, layout.ScrollRegion) &&
		RectContainsPoint(SliderTrackRect(rows.CornerRoundness, m_fonts), m_flMouseX, m_flMouseY)) {
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

	// Reset buttons are tested before anything else on the row: one sits directly left of
	// its row's own control, and whichever branch below owns that control would otherwise
	// have to know to exclude it. A button on a row already at its default is deliberately
	// inert (it's drawn dimmed - see DrawResetButton), so clicking it does nothing rather
	// than replaying an animation for a value that never moved.
	if (clickInScrollRegion && !m_colorPicker.IsBlocking()) {
		for (std::uint64_t i = 0; i < static_cast<std::uint64_t>(ESettingsResetTarget::SETTINGS_RESET_COUNT); i += 1) {
			const auto target = static_cast<ESettingsResetTarget>(i);
			if (!RowInView(ResetTargetRowRect(rows, target), layout.ScrollRegion) || IsTargetAtDefault(target)) {
				continue;
			}
			if (RectContainsPoint(ResetTargetButtonRect(rows, target, m_fonts), x, y)) {
				ResetTargetToDefault(target);
				return true;
			}
		}
	}

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

	if (clickInScrollRegion && RowInView(rows.ExcludeFromCapture, layout.ScrollRegion) &&
		RectContainsPoint(ToggleRect(rows.ExcludeFromCapture, m_fonts), x, y)) {
		m_settings.m_bExcludeAccountListFromCapture = !m_settings.m_bExcludeAccountListFromCapture;
	}

	if (clickInScrollRegion && RowInView(rows.CloseToTray, layout.ScrollRegion) &&
		RectContainsPoint(ToggleRect(rows.CloseToTray, m_fonts), x, y)) {
		m_settings.m_bCloseToTray = !m_settings.m_bCloseToTray;
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
			 layout.Header.Y + layout.Header.H * 0.5f + (body.GetAscent() + body.GetDescent()) * 0.5f,
			 StringViewFromCString("Settings"), ColorFadeAlpha(kColorText, alpha));

	const Rect close = CloseRect(layout.Panel, m_fonts);
	const bool hoverClose = RectContainsPoint(close, m_flMouseX, m_flMouseY);
	DrawXGlyph(drawList, close, ColorFadeAlpha(hoverClose ? kColorText : kColorTextDim, alpha));

	drawList.AddRectFilled(layout.Header.X + kRowPaddingX, layout.Header.Y + layout.Header.H,
						   layout.Header.W - kRowPaddingX * 2.0f, 1.0f, ColorFadeAlpha(kColorSeparator, alpha));

	const SettingsRows rows = ComputeRows(layout.ScrollRegion, m_rowsScroll.m_flScrollOffset, m_fonts);

	// Real GPU-side clipping so a row that's scrolled halfway behind the header genuinely
	// can't paint outside ScrollRegion - same pattern CAccountModal's account list uses.
	drawList.PushClipRect(layout.ScrollRegion);

	// The hovered row's band, painted before any row content so every label and control
	// lands on top of it. Deliberately not eased: a highlight that fades behind a moving
	// cursor trails the thing it's supposed to be marking.
	const Rect hoverableRows[]{rows.CornerRoundness, rows.Font,			 rows.FontSize,		  rows.SecondaryFontSize,
							   rows.Accent,			 rows.Animations,	 rows.AnimationSpeed, rows.ExcludeFromCapture,
							   rows.CloseToTray,	 rows.MasterPassword};
	if (IsBlocking() && !m_colorPicker.IsBlocking() && RectContainsPoint(layout.ScrollRegion, m_flMouseX, m_flMouseY)) {
		for (const Rect &row : hoverableRows) {
			if (RowInView(row, layout.ScrollRegion) && RectContainsPoint(row, m_flMouseX, m_flMouseY)) {
				drawList.AddRectRoundedFilled(row.X + kRowHighlightInsetX, row.Y, row.W - kRowHighlightInsetX * 2.0f,
											  row.H, CDrawList::UniformRadii(kRowHighlightRadius),
											  ColorFadeAlpha(kColorRowHover, alpha));
				break;
			}
		}
	}

	const Rect sectionRows[]{rows.SectionAppearance, rows.SectionMotion, rows.SectionPrivacy, rows.SectionSecurity};
	const char *const sectionTitles[]{"Appearance", "Motion", "Privacy", "Security"};
	for (std::uint64_t i = 0; i < sizeof(sectionRows) / sizeof(sectionRows[0]); i += 1) {
		if (RowInView(sectionRows[i], layout.ScrollRegion)) {
			DrawSectionHeader(drawList, m_fonts, sectionRows[i], sectionTitles[i], alpha);
		}
	}

	if (RowInView(rows.Font, layout.ScrollRegion)) {
		DrawRowLabel(drawList, m_fonts, rows.Font, "Font", "The system font file applied to the UI.",
					 LabelRightEdge(FontFieldRect(rows.Font, m_fonts), rows.Font, m_fonts), alpha);
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
		DrawRowLabel(drawList, m_fonts, rows.FontSize, "Font Size", "Determines the scale of the whole UI.",
					 LabelRightEdge(StepperRect(rows.FontSize, m_fonts), rows.FontSize, m_fonts), alpha);
		DrawStepper(drawList, body, StepperRect(rows.FontSize, m_fonts), m_flFontSizeDisplay, alpha);
	}

	if (RowInView(rows.SecondaryFontSize, layout.ScrollRegion)) {
		DrawRowLabel(drawList, m_fonts, rows.SecondaryFontSize, "Secondary Font Size",
					 "Scale of labels/hints and other small text.",
					 LabelRightEdge(StepperRect(rows.SecondaryFontSize, m_fonts), rows.SecondaryFontSize, m_fonts),
					 alpha);
		DrawStepper(drawList, body, StepperRect(rows.SecondaryFontSize, m_fonts), m_flSecondaryFontSizeDisplay, alpha);
	}

	if (RowInView(rows.Animations, layout.ScrollRegion)) {
		DrawRowLabel(drawList, m_fonts, rows.Animations, "Animations",
					 "Applies animations to popups, scrolling, and the caret.",
					 LabelRightEdge(ToggleRect(rows.Animations, m_fonts), rows.Animations, m_fonts), alpha);
		DrawToggle(drawList, ToggleRect(rows.Animations, m_fonts), m_flAnimationsToggleAmount, m_settings.m_clrAccent,
				   alpha);
	}

	if (RowInView(rows.CornerRoundness, layout.ScrollRegion)) {
		DrawRowLabel(
			drawList, m_fonts, rows.CornerRoundness, "Corner Roundness", "How round those corners actually are.",
			LabelRightEdge(SliderControlRect(rows.CornerRoundness, m_fonts), rows.CornerRoundness, m_fonts), alpha);
		// A percentage, not a multiplier: this scales a length nobody knows the pixel value
		// of, so "120%" says everything "1.20x" would and reads as a proportion of the
		// design's own rounding, which is exactly what it is.
		char roundnessBuffer[8];
		const int roundnessWritten =
			std::snprintf(roundnessBuffer, sizeof(roundnessBuffer), "%.0f%%", m_flCornerRoundnessDisplay * 100.0f);
		DrawSlider(
			drawList, body, SliderTrackRect(rows.CornerRoundness, m_fonts),
			CornerRoundnessToT(m_flCornerRoundnessDisplay),
			CStringView{roundnessBuffer, roundnessWritten > 0 ? static_cast<std::uint64_t>(roundnessWritten) : 0},
			m_settings.m_clrAccent, alpha);
	}

	if (RowInView(rows.CloseToTray, layout.ScrollRegion)) {
		DrawRowLabel(drawList, m_fonts, rows.CloseToTray, "Close To Tray",
					 "Closing hides Rift to the system tray instead of quitting.",
					 LabelRightEdge(ToggleRect(rows.CloseToTray, m_fonts), rows.CloseToTray, m_fonts), alpha);
		DrawToggle(drawList, ToggleRect(rows.CloseToTray, m_fonts), m_flCloseToTrayToggleAmount, m_settings.m_clrAccent,
				   alpha);
	}

	if (RowInView(rows.AnimationSpeed, layout.ScrollRegion)) {
		DrawRowLabel(drawList, m_fonts, rows.AnimationSpeed, "Animation Speed",
					 "How fast popups, scrolling, and toggles animate.",
					 LabelRightEdge(SliderControlRect(rows.AnimationSpeed, m_fonts), rows.AnimationSpeed, m_fonts),
					 alpha);
		char speedBuffer[8];
		const int speedWritten = std::snprintf(speedBuffer, sizeof(speedBuffer), "%.2fx", m_flAnimationSpeedDisplay);
		DrawSlider(drawList, body, SliderTrackRect(rows.AnimationSpeed, m_fonts),
				   AnimationSpeedToT(m_flAnimationSpeedDisplay),
				   CStringView{speedBuffer, speedWritten > 0 ? static_cast<std::uint64_t>(speedWritten) : 0},
				   m_settings.m_clrAccent, alpha);
	}

	if (RowInView(rows.Accent, layout.ScrollRegion)) {
		DrawRowLabel(drawList, m_fonts, rows.Accent, "Accent Color", "Click to pick the selection/button accent color.",
					 LabelRightEdge(SwatchRect(rows.Accent, m_fonts), rows.Accent, m_fonts), alpha);
		const Rect swatch = SwatchRect(rows.Accent, m_fonts);
		// The eased display copy, so restoring the default accent travels there instead of
		// cutting - every other surface in the app reads m_clrAccent directly and changes
		// instantly, which is the correct behavior for them and the wrong one here.
		const Color swatchColor{
			static_cast<std::uint8_t>(std::lround(m_flAccentDisplayR)),
			static_cast<std::uint8_t>(std::lround(m_flAccentDisplayG)),
			static_cast<std::uint8_t>(std::lround(m_flAccentDisplayB)),
			m_settings.m_clrAccent.A,
		};
		drawList.AddRectRoundedFilled(swatch.X, swatch.Y, swatch.W, swatch.H, CDrawList::UniformRadii(6.0f),
									  ColorFadeAlpha(swatchColor, alpha));
	}

	if (RowInView(rows.MasterPassword, layout.ScrollRegion)) {
		DrawRowLabel(
			drawList, m_fonts, rows.MasterPassword, "Master Password", "Encrypts your saved account passwords.",
			LabelRightEdge(MasterPasswordResetButtonRect(rows.MasterPassword, m_fonts), rows.MasterPassword, m_fonts),
			alpha);

		const Rect button = MasterPasswordResetButtonRect(rows.MasterPassword, m_fonts);
		const bool hoverButton = RectContainsPoint(button, m_flMouseX, m_flMouseY);
		drawList.AddRectRoundedFilled(
			button.X, button.Y, button.W, button.H, CDrawList::UniformRadii(6.0f),
			ColorFadeAlpha(hoverButton ? ColorLighten(kColorControlBg, 10) : kColorControlBg, alpha));
		DrawCenteredText(drawList, body, button.X, button.Y, button.W, button.H,
						 StringViewFromCString("Reset Password"), ColorFadeAlpha(kColorText, alpha));
	}

	// After every row's own content, still inside the clip: a button belongs to its row and
	// should scroll out of view with it.
	for (std::uint64_t i = 0; i < static_cast<std::uint64_t>(ESettingsResetTarget::SETTINGS_RESET_COUNT); i += 1) {
		const auto target = static_cast<ESettingsResetTarget>(i);
		const Rect row = ResetTargetRowRect(rows, target);
		if (!RowInView(row, layout.ScrollRegion)) {
			continue;
		}
		const Rect button = ResetTargetButtonRect(rows, target, m_fonts);
		const bool hovered = !m_colorPicker.IsBlocking() &&
							 RectContainsPoint(layout.ScrollRegion, m_flMouseX, m_flMouseY) &&
							 RectContainsPoint(button, m_flMouseX, m_flMouseY);
		DrawResetButton(drawList, m_assets.GetIconReset(), button, m_aResetAppearAmount[i], m_aResetSpinAmount[i],
						hovered, alpha);
	}

	drawList.PopClipRect();

	m_rowsScroll.DrawEdgeFade(drawList, layout.ScrollRegion, rows.ContentHeight, layout.ScrollRegion.H,
							  ColorFadeAlpha(kColorBg, alpha));
	m_rowsScroll.Draw(drawList, ScrollbarTrackRect(layout.ScrollRegion), rows.ContentHeight, layout.ScrollRegion.H,
					  ColorFadeAlpha(Color{120, 120, 128, 190}, alpha), m_flMouseX, m_flMouseY);

	drawList.AddRectFilled(layout.Footer.X + kRowPaddingX, layout.Footer.Y, layout.Footer.W - kRowPaddingX * 2.0f, 1.0f,
						   ColorFadeAlpha(kColorSeparator, alpha));
	DrawText(drawList, secondary, layout.Footer.X + kRowPaddingX,
			 layout.Footer.Y + layout.Footer.H * 0.5f + (secondary.GetAscent() + secondary.GetDescent()) * 0.5f,
			 StringViewFromCString("Escape to dismiss"), ColorFadeAlpha(kColorTextDim, alpha));

	// Drawn last so it layers over every row above, including the footer - needs the
	// Accent row's current on-screen position, which only exists while that row's still
	// inside the clipped scroll region; if it's scrolled out of view there's nothing
	// sensible to anchor the popup to, so it's skipped too.
	if (RowInView(rows.Accent, layout.ScrollRegion)) {
		m_colorPicker.Draw(drawList);
	}

	// Absolutely last, over even the color picker - a tooltip that can be painted over by
	// the thing it's explaining isn't a tooltip. Clamped to the panel rather than the
	// window so a bubble never floats off the dialog it belongs to.
	m_tooltip.Draw(drawList, m_fonts, layout.Panel, alpha);
}
