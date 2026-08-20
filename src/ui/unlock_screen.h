#pragma once

// The master-password startup gate - now mandatory, not optional (see this file's own
// git history for the earlier "Windows Account" vs "Windows Account + Master Password"
// tier picker this replaced): every vault has a master password, full stop. This one
// widget covers both situations that fact creates:
//
//   - Unlock mode (ActivateForUnlock): CSettings::m_bMasterPasswordEnabled is true but
//     the session's CMasterKey hasn't been unlocked yet (CStorage::Load reports
//     STORAGE_LOAD_LOCKED) - a single password field, exactly the original design.
//   - Setup mode (ActivateForSetup): no master password exists yet (a first run, or an
//     existing install updating from a version where the master password was still
//     optional), or the user asked to reset their password from Settings
//     (CSettingsPanel::ConsumeResetPasswordRequested) - two password fields (password +
//     confirm) with a shared reveal toggle, so a typo in a password nothing else can
//     recover isn't silently locked in. Both first-run and a voluntary reset behave
//     identically here and are both non-cancelable once started - see ActivateForSetup's
//     own comment.
//
// Whichever mode, the whole app is blocked behind this full-screen prompt while active -
// every other widget exists underneath but input doesn't reach it until this widget lets
// it through. Pushed onto CWidgetStack like any other widget, as the topmost *normal*
// entry (above every popup, below only CTitleBar's alwaysTopmost slot) - IsBlocking()
// reporting true unconditionally while active achieves the exact same "nothing
// underneath is reachable" effect through the stack's own mouse-gating cascade, with no
// special-casing needed in main.cpp - only CTitleBar's minimize/maximize/close still
// work while this is active, since CTitleBar dispatches above this regardless of
// blocking state.
//
// The owner (main.cpp) calls ActivateForUnlock() once CStorage::Load reports
// STORAGE_LOAD_LOCKED, or ActivateForSetup() once it sees !CSettings::m_bMasterPasswordEnabled
// or CSettingsPanel::ConsumeResetPasswordRequested() report true. It calls Deactivate()
// plus re-runs CStorage::Load (to backfill account passwords left blank while locked)
// once ConsumeUnlockSucceeded() reports true, or calls Deactivate() plus a full SaveNow
// (both accounts.bin - re-encrypting every account's password under the fresh DEK
// CMasterKey::Set just generated - and settings.bin, to actually persist the new salt/
// wrap/etc fields) once ConsumeSetupSucceeded() reports true - the same one-shot-poll-
// then-clears contract as CTitleBar::ConsumeMenuClicked and CSettingsMenu::ConsumeAction.

#include "font_manager.h"
#include "ui/text_input.h"
#include "ui/widget.h"

class CAssetManager;
class CMasterKey;
struct CSettings;
class CWindow;

class CUnlockScreen : public CWidget {
  public:
	// Stored by reference for this widget's whole lifetime, the same constructor-
	// injection pattern CTitleBar already uses for CWindow - window is needed for the
	// centered card's layout (Draw/OnPointerUp both reason in the full window rect,
	// unlike every popup elsewhere in this project which anchors to some smaller rect).
	// settings is mutable now (unlike the read-only reference this held before setup
	// mode existed): a successful setup writes CSettings::m_bMasterPasswordEnabled and
	// the new salt/opsLimit/memLimit/wrapNonce/wrappedDek fields directly, the same
	// fields CSettingsPanel used to own writing before Reset Password moved here.
	// masterKey is the session state an attempt populates on success either way. assets
	// supplies the setup form's reveal-button icons (see ui/account_modal.h's own
	// DrawEyeGlyph for the sibling copy of this same icon pair).
	CUnlockScreen(CFontManager &fonts, CWindow &window, CSettings &settings, CMasterKey &masterKey,
				 CAssetManager &assets);

	// The existing "type the password you already have" flow - a single field.
	void ActivateForUnlock();

	// The "no master password exists yet, or you asked to replace it" flow - two fields
	// (password + confirm) with a shared reveal toggle. Deliberately not cancelable once
	// active (no Escape-to-abort, unlike the rest of this project's own dismissal
	// conventions) - a first run has nothing to cancel back to, and a voluntary reset
	// (see CSettingsPanel::ConsumeResetPasswordRequested) already closed Settings before
	// this activates, so there's nowhere sensible to return to either; either way, once
	// you're here, you finish it.
	void ActivateForSetup();

	void Deactivate();

	void Update(float deltaSeconds) override;
	void Draw(CDrawList &drawList) override;

	// Routes to whichever field is currently focused (there's always exactly one, in
	// either mode) - harmless no-op otherwise.
	bool OnChar(std::uint32_t character) override;

	// VK_RETURN attempts an unlock (unlock mode) or a submit (setup mode, from either
	// field - both need the same "do both fields agree" check regardless of which one
	// was just edited); everything else routes into whichever field is focused. No
	// VK_ESCAPE handling in either mode - see ActivateForSetup's own comment for why
	// setup mode in particular is deliberately not cancelable; unlock mode never
	// supported cancelling either (there's nothing to cancel back to while locked).
	bool OnKeyDown(std::uint32_t keyCode) override;

	// Focuses whichever field the click landed on (positioning the cursor at the end),
	// toggles the reveal state if it landed on the reveal button (setup mode only), or
	// attempts unlock/submit if it landed on the button. Consumes every click while
	// active - see this file's own comment for why nothing should fall through to
	// whatever's underneath.
	bool OnPointerUp(float x, float y) override;

	// Pointer-down, wheel, and right-click carry no visible affordance of their own on
	// this screen, but must still be swallowed while active rather than falling through
	// to whatever's underneath (gated to an off-screen mouse position already, but a
	// wheel notch doesn't need real cursor coordinates to scroll something invisible -
	// the original's app_locked branch swallowed every event type unconditionally, and
	// these three exist only to match that rather than leave a narrow, easy-to-miss gap).
	bool OnPointerDown(float x, float y) override
	{
		return IsBlocking();
	}
	bool OnScroll(float x, float y, float wheelDelta) override
	{
		return IsBlocking();
	}
	bool OnRightPointerUp(float x, float y) override
	{
		return IsBlocking();
	}

	bool IsBlocking() const override
	{
		return m_bActive;
	}

	ECursorKind GetDesiredCursor() const override;

	// True at most once per frame, the frame an unlock attempt actually succeeded -
	// cleared on read. The owner should call Deactivate() and re-run CStorage::Load when
	// this reports true.
	bool ConsumeUnlockSucceeded();

	// True at most once per frame, the frame a new/reset master password was actually
	// set - cleared on read. The owner should call Deactivate() and re-run a full
	// SaveNow when this reports true - see this file's own header comment for why that's
	// a different follow-up than ConsumeUnlockSucceeded's.
	bool ConsumeSetupSucceeded();

  private:
	// Unlock mode's Enter-in-the-field / Unlock-button handler - tries the typed
	// password against m_settings' persisted salt/wrap/etc fields, clears the field
	// either way, and sets m_bWrongPassword so a failed attempt shows an inline error
	// and keeps focus for a retry.
	bool AttemptUnlock();

	// Setup mode's Enter-in-either-field / Create-button handler - validates both
	// fields are non-empty and agree with each other before ever calling
	// CMasterKey::Set, so a genuine derivation failure (see its own comment - rare, but
	// real) is never confused with "you mistyped the confirmation."
	bool AttemptSetup();

	CFontManager &m_fonts;
	CWindow &m_window;
	CSettings &m_settings;
	CMasterKey &m_masterKey;
	CAssetManager &m_assets;

	bool m_bActive = false;
	bool m_bSetupMode = false;

	CTextInput m_passwordInput;
	bool m_bWrongPassword = false; // unlock mode's inline error, shown until the next attempt

	// Both modes' reveal toggle - unlock mode's one field, or setup mode's two sharing a
	// single flag (so revealing one reveals both, since they represent the same value
	// being double-checked). Reset false on every Activate* so a previous mode's reveal
	// state never carries over into the next.
	bool m_bPasswordRevealed = false;

	// Setup mode only.
	CTextInput m_confirmPasswordInput;
	bool m_bPasswordMismatch = false; // password/confirm disagreed on the last submit attempt
	bool m_bSetupFailed = false;	  // CMasterKey::Set itself failed (rare - see AttemptSetup)

	bool m_bUnlockSucceededThisFrame = false;
	bool m_bSetupSucceededThisFrame = false;
};
