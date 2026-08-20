#pragma once

// The settings dialog - opened from CSettingsMenu's "Settings" item. A centered, size-
// clamped panel (same treatment as CAccountModal, so it doesn't balloon on a large
// monitor either) listing every setting this project has: Font (name, text field), Font
// Size / Secondary Font Size (steppers), Animations / Round Corners / Hide From Screen
// Capture (toggles), Animation Speed (slider), Accent Color (swatch, opens CColorPicker's
// popup), Master Password (a plain row with a Reset Password button).
//
// Every setting here has live effect, not just a stored value nobody reads: Animations
// gates CAnimator::EaseToward globally, Animation Speed scales CAnimator's effective rate
// globally, Round Corners gates CDrawList::UniformRadii globally, Font/Font Size re-bake
// CFontManager's body face on the spot, Accent Color feeds CSettings::m_clrAccent
// directly (every other widget already reads it live). This widget edits CSettings& (the
// live persisted-settings object a coordinating owner holds) directly rather than owning
// a copy - so those effects are immediate, not deferred to some later sync step.
//
// The row list is laid out with ui/layout.h's RectSplitTop - each row carves off exactly
// the height it needs from a shared cursor, so rows can never overlap or get squeezed
// regardless of how many exist. The row stack lives inside a scrollable, clipped region
// (CScrollable + CDrawList's push/pop clip rect) below the header and above the footer,
// the same pattern CAccountModal's account list uses.
//
// Unlike every other row, Master Password doesn't edit anything here directly: the
// master password is mandatory now (see ui/unlock_screen.h's own file comment on the
// forced setup flow), so this panel has no "enable/disable" state of its own left to
// hold - resetting one still means typing a brand-new password twice with a live-typo-
// check, which is exactly what that same forced setup screen already does. Clicking
// Reset Password here just closes this panel and requests that the coordinating owner
// (main.cpp) re-activate CUnlockScreen in its setup mode - see ConsumeResetPasswordRequested.
//
// This widget doesn't know CStorage exists: rather than reaching into persistence itself,
// it exposes a consumed OnPointerUp/OnKeyDown return to mean "something changed, save
// now" (mirroring the original's own "every click/Enter while open triggers a save"
// behavior exactly, with no separate flag needed since the return value already carries
// it).

#include <cstdint>

#include "font_manager.h"
#include "ui/color_picker.h"
#include "ui/draggable.h"
#include "ui/scrollable.h"
#include "ui/settings.h"
#include "ui/text_input.h"
#include "ui/widget.h"

class CWindow;
class IRenderer;

class CSettingsPanel : public CWidget {
  public:
	// fonts/renderer are needed to re-bake the body/secondary faces from the Font/Font
	// Size controls (CFontManager::ApplyBody); window supplies the current window size
	// (CWidget's fixed Draw/On* signatures have no room for per-call width/height
	// parameters the way the original's free functions took) and dpi scale that bake
	// needs. settings is the live object this panel edits directly.
	CSettingsPanel(CFontManager &fonts, CSettings &settings, CWindow &window, IRenderer &renderer);

	// Kicks off the open-animation; doesn't touch any field state, so re-opening always
	// resumes where it left off.
	void Open();

	// Kicks off the close-animation and clears every bit of transient interaction state
	// that shouldn't survive a reopen (a focused field, the Accent popup left open) -
	// leaving any of that would read as a stale UI glitch the next time the panel opens.
	void Close();

	void Update(float deltaSeconds) override;
	void Draw(CDrawList &drawList) override;

	// Pointer down/move around the real click handling below, so the Accent Color popup's
	// saturation/value square and hue/alpha strips, the row scrollbar's thumb, and the
	// Animation Speed slider can all be dragged.
	bool OnPointerDown(float x, float y) override;
	bool OnPointerMove(float x, float y) override;

	// Ends any in-progress drag first (scrollbar thumb / color picker / animation speed
	// slider); if none of those were dragging, handles the release as a real click on
	// whatever's underneath (toggles/stepper/swatch/close/backdrop/Reset Password button).
	// Consumed (returns true) for the entirety of a drag-end, and always while the panel
	// is open otherwise - matching the original's "every interaction here triggers a
	// save" behavior, so a coordinating owner should persist settings whenever this
	// returns true.
	bool OnPointerUp(float x, float y) override;

	// Mouse wheel over the panel scrolls the row list - a no-op once every row already
	// fits, same as CScrollable everywhere else. Consumes (returns IsBlocking()) so a
	// wheel event never falls through to the carousel underneath while the panel is open
	// - the original shipped without this wired up once, a real bug fixed later.
	bool OnScroll(float x, float y, float wheelDelta) override;

	// Routes to the font name field if it's focused; harmless no-op otherwise. Swallows
	// input while open either way.
	bool OnChar(std::uint32_t character) override;

	// Escape closes the panel (self-contained here rather than left to a coordinating
	// owner, unlike the original which centralized this in Main.cpp's dispatch chain -
	// this fork's widgets generally own their own dismissal). VK_RETURN applies the typed
	// font name if that field is focused; other keys (backspace, arrows, ...) edit it too.
	// Swallows input while open either way.
	bool OnKeyDown(std::uint32_t keyCode) override;

	bool IsBlocking() const override
	{
		return m_flOpenAmount > 0.01f;
	}

	ECursorKind GetDesiredCursor() const override;

	// True at most once per frame, the frame the Reset Password button was clicked -
	// cleared on read. The owner (main.cpp) should re-activate CUnlockScreen in its setup
	// mode when this reports true - see this file's own header comment on why resetting a
	// password doesn't happen inline in this panel at all anymore.
	bool ConsumeResetPasswordRequested();

  private:
	// Real click handling shared by OnPointerUp (a plain release) - kept private/separate
	// from OnPointerUp itself so the drag-vs-click disambiguation at the top of
	// OnPointerUp stays easy to read.
	bool HandleClick(float x, float y);

	CFontManager &m_fonts;
	CSettings &m_settings;
	CWindow &m_window;
	IRenderer &m_renderer;

	bool m_bOpen = false;
	float m_flOpenAmount = 0.0f;

	CTextInput m_fontNameInput;
	float m_flAnimationsToggleAmount = 1.0f; // animated 0..1, drives the toggle-switch dot
	float m_flRoundedCornersToggleAmount = 1.0f;
	float m_flExcludeFromCaptureToggleAmount = 1.0f;

	// The Accent Color swatch's popup - opened/closed by clicking the swatch (see
	// HandleClick); dragging any of its controls applies the result live into
	// CSettings::m_clrAccent, not just on close.
	CColorPicker m_colorPicker;

	// The row list, once it overflows the panel - same CScrollable + clip-rect pattern
	// CAccountModal's account list uses.
	CScrollable m_rowsScroll;

	// Tracks a drag on the Animation Speed slider - OnPointerDown/Move/Up (already
	// threading pointer events for the color picker's drag and the row scrollbar's thumb)
	// route here too, since the slider needs continuous updates while dragging, not just
	// a single click. CDraggable per FORK_WITH_CLASSES.md 3.2 rather than a hand-rolled
	// bool, though (like CColorPicker) HasMoved() is never read - a plain click on the
	// track should still jump the value there immediately.
	CDraggable m_animationSpeedDrag;

	bool m_bResetPasswordRequestedThisFrame = false;
};
