#include "ui/unlock_screen.h"

#include <cstring>

#include <Windows.h>

#include "asset_manager.h"
#include "core/master_key.h"
#include "ui/draw_list.h"
#include "ui/settings.h"
#include "ui/text.h"
#include "window.h"

namespace {
constexpr float kCardWidth = 380.0f;
constexpr float kUnlockCardHeight = 210.0f;
// Title + a 2-line description + password field + confirm field + gap + button + a
// trailing error line - see SetupPasswordFieldRect/SetupConfirmFieldRect/SetupButtonRect
// for the exact stack this sizes.
constexpr float kSetupCardHeight = 366.0f;
constexpr float kCardRadius = 16.0f;
constexpr float kCardPadding = 28.0f;
constexpr float kFieldHeight = 40.0f;
constexpr float kButtonHeight = 40.0f;
constexpr float kGap = 14.0f;
constexpr float kRevealButtonSize = 22.0f;

constexpr Color kColorTextBright{232, 232, 236, 255};
constexpr Color kColorTextDim{150, 150, 156, 255};
constexpr Color kColorError{220, 90, 80, 255};
constexpr Color kColorFieldBg{24, 24, 27, 255};

// The centered prompt card - always centered, unlike the modal/settings panel which
// clamp/scale against the window at larger sizes, since a fixed small card is all this
// screen ever needs. Setup mode's card is taller (see kSetupCardHeight's own comment).
Rect CardRect(float windowW, float windowH, bool setupMode)
{
	const float h = setupMode ? kSetupCardHeight : kUnlockCardHeight;
	return Rect{(windowW - kCardWidth) * 0.5f, (windowH - h) * 0.5f, kCardWidth, h};
}

// --- Unlock mode: a single field, a fixed offset below the card's title/description. ---
Rect FieldRect(Rect card)
{
	return Rect{card.X + kCardPadding, card.Y + 74.0f, card.W - kCardPadding * 2.0f, kFieldHeight};
}

Rect ButtonRect(Rect card)
{
	const Rect field = FieldRect(card);
	return Rect{field.X, field.Y + kFieldHeight + kGap, field.W, kButtonHeight};
}

// --- Setup mode: password + confirm, stacked, with a shared reveal button. ---
Rect SetupPasswordFieldRect(Rect card)
{
	return Rect{card.X + kCardPadding, card.Y + 96.0f, card.W - kCardPadding * 2.0f, kFieldHeight};
}

Rect SetupConfirmFieldRect(Rect card)
{
	const Rect password = SetupPasswordFieldRect(card);
	return Rect{password.X, password.Y + kFieldHeight + kGap, password.W, kFieldHeight};
}

// One reveal button, positioned against whichever field it's actually drawn over -
// both fields show/hide together (see CUnlockScreen::m_bPasswordRevealed's own comment),
// so this is called once per field rather than needing two independent toggle states.
Rect RevealButtonRect(Rect field)
{
	return Rect{field.X + field.W - kRevealButtonSize - 6.0f, field.Y + (field.H - kRevealButtonSize) * 0.5f,
				kRevealButtonSize, kRevealButtonSize};
}

Rect SetupButtonRect(Rect card)
{
	const Rect confirm = SetupConfirmFieldRect(card);
	return Rect{confirm.X, confirm.Y + kFieldHeight + kGap, confirm.W, kButtonHeight};
}

// The reveal button's show/hide icon - EyeVisible/EyeHiddenIcon (asset_manager.h), the
// same pair CAccountModal's own DrawEyeGlyph draws (duplicated rather than shared - this
// project's small per-file drawing helpers, e.g. DrawToggle in ui/settings_panel.cpp,
// generally aren't factored into a shared library either).
void DrawEyeGlyph(CDrawList &drawList, CAssetManager &assets, Rect rect, bool revealed, Color color)
{
	drawList.AddRectRoundedTextured(rect.X, rect.Y, rect.W, rect.H, kCornerRadiiNone,
									revealed ? assets.GetIconEyeVisible() : assets.GetIconEyeHidden(), color);
}

// A focused/unfocused field's fill+border, the same two-rect construction every text
// field in this project draws (see CAccountModal::DrawEditField, CSettingsPanel's Font
// field) - pulled out here since setup mode needs it twice.
void DrawFieldChrome(CDrawList &drawList, Rect field, bool focused, Color accent)
{
	drawList.AddRectRoundedFilled(field.X, field.Y, field.W, field.H, CDrawList::UniformRadii(6.0f),
								  focused ? accent : Color{40, 40, 45, 255});
	drawList.AddRectRoundedFilled(field.X + 1.5f, field.Y + 1.5f, field.W - 3.0f, field.H - 3.0f,
								  CDrawList::UniformRadii(5.0f), kColorFieldBg);
}
} // namespace

CUnlockScreen::CUnlockScreen(CFontManager &fonts, CWindow &window, CSettings &settings, CMasterKey &masterKey,
							 CAssetManager &assets)
	: m_fonts(fonts)
	, m_window(window)
	, m_settings(settings)
	, m_masterKey(masterKey)
	, m_assets(assets)
{
}

void CUnlockScreen::ActivateForUnlock()
{
	m_bSetupMode = false;
	m_passwordInput.Init(StringViewFromCString(""));
	m_passwordInput.m_bFocused = true;
	m_bWrongPassword = false;
	m_bPasswordRevealed = false;
	m_bActive = true;
}

void CUnlockScreen::ActivateForSetup()
{
	m_bSetupMode = true;
	m_passwordInput.Init(StringViewFromCString(""));
	m_confirmPasswordInput.Init(StringViewFromCString(""));
	m_passwordInput.m_bFocused = true;
	m_bPasswordRevealed = false;
	m_bPasswordMismatch = false;
	m_bSetupFailed = false;
	m_bActive = true;
}

void CUnlockScreen::Deactivate()
{
	m_bActive = false;
}

bool CUnlockScreen::ConsumeUnlockSucceeded()
{
	const bool bSucceeded = m_bUnlockSucceededThisFrame;
	m_bUnlockSucceededThisFrame = false;
	return bSucceeded;
}

bool CUnlockScreen::ConsumeSetupSucceeded()
{
	const bool bSucceeded = m_bSetupSucceededThisFrame;
	m_bSetupSucceededThisFrame = false;
	return bSucceeded;
}

bool CUnlockScreen::AttemptUnlock()
{
	const bool bUnlocked =
		m_masterKey.Unlock(m_passwordInput.GetValue(), m_settings.m_aMasterPasswordSalt,
						   m_settings.m_masterPasswordOpsLimit, m_settings.m_masterPasswordMemLimit,
						   m_settings.m_aMasterPasswordWrapNonce, m_settings.m_aMasterPasswordWrappedDek);
	if (bUnlocked) {
		m_passwordInput.SetValue(StringViewFromCString(""));
		m_bWrongPassword = false;
		m_bUnlockSucceededThisFrame = true;
	} else {
		m_bWrongPassword = true;
		m_passwordInput.SetValue(StringViewFromCString(""));
		m_passwordInput.m_bFocused = true;
	}

	return bUnlocked;
}

bool CUnlockScreen::AttemptSetup()
{
	if (m_passwordInput.GetValue().Length == 0) {
		return false;
	}

	if (!StringViewEqual(m_passwordInput.GetValue(), m_confirmPasswordInput.GetValue())) {
		m_bPasswordMismatch = true;
		m_bSetupFailed = false;
		m_confirmPasswordInput.SetValue(StringViewFromCString(""));
		m_confirmPasswordInput.m_bFocused = true;
		return false;
	}

	const bool ok = m_masterKey.Set(m_passwordInput.GetValue());
	if (!ok) {
		// Argon2id/the AEAD wrap itself failed - rare (see CCrypto's own comment: mainly
		// an out-of-memory situation under the MODERATE preset's memory cost), but a real
		// possible outcome, not just a hypothetical to leave unhandled.
		m_bSetupFailed = true;
		m_bPasswordMismatch = false;
		return false;
	}

	m_settings.m_bMasterPasswordEnabled = true;
	std::memcpy(m_settings.m_aMasterPasswordSalt, m_masterKey.m_aSalt, sizeof(m_settings.m_aMasterPasswordSalt));
	m_settings.m_masterPasswordOpsLimit = m_masterKey.m_opsLimit;
	m_settings.m_masterPasswordMemLimit = m_masterKey.m_memLimit;
	std::memcpy(m_settings.m_aMasterPasswordWrapNonce, m_masterKey.m_aWrapNonce,
				sizeof(m_settings.m_aMasterPasswordWrapNonce));
	std::memcpy(m_settings.m_aMasterPasswordWrappedDek, m_masterKey.m_aWrappedDek,
				sizeof(m_settings.m_aMasterPasswordWrappedDek));

	m_passwordInput.SetValue(StringViewFromCString(""));
	m_confirmPasswordInput.SetValue(StringViewFromCString(""));
	m_bPasswordMismatch = false;
	m_bSetupFailed = false;
	m_bSetupSucceededThisFrame = true;
	return true;
}

void CUnlockScreen::Update(float deltaSeconds)
{
	if (!m_bActive)
		return;

	m_passwordInput.Update(deltaSeconds);
	if (m_bSetupMode) {
		m_confirmPasswordInput.Update(deltaSeconds);
	}
}

bool CUnlockScreen::OnChar(std::uint32_t character)
{
	if (!m_bActive)
		return false;

	// A no-op on an unfocused field, so routing to both unconditionally in setup mode is
	// safe - exactly one of them (or neither) is ever actually focused.
	m_passwordInput.OnChar(character);
	if (m_bSetupMode) {
		m_confirmPasswordInput.OnChar(character);
	}
	return true;
}

bool CUnlockScreen::OnKeyDown(std::uint32_t keyCode)
{
	if (!m_bActive)
		return false;

	if (keyCode == VK_RETURN) {
		if (m_bSetupMode) {
			AttemptSetup();
		} else {
			AttemptUnlock();
		}
		return true;
	}

	// Setup mode only - unlock mode has just the one field, nothing to cycle to. Plain
	// Tab toggles between the two regardless of Shift, since with exactly two fields
	// "forward" and "backward" land on the same place either way.
	if (m_bSetupMode && keyCode == VK_TAB) {
		const bool wasOnPassword = m_passwordInput.m_bFocused;
		m_passwordInput.m_bFocused = !wasOnPassword;
		m_confirmPasswordInput.m_bFocused = wasOnPassword;
		(wasOnPassword ? m_confirmPasswordInput : m_passwordInput).OnKey(VK_END);
		return true;
	}

	if (m_bSetupMode) {
		if (m_passwordInput.m_bFocused) {
			m_passwordInput.OnKey(keyCode);
		} else if (m_confirmPasswordInput.m_bFocused) {
			m_confirmPasswordInput.OnKey(keyCode);
		}
	} else {
		m_passwordInput.OnKey(keyCode);
	}

	return true;
}

bool CUnlockScreen::OnPointerUp(float x, float y)
{
	if (!m_bActive)
		return false;

	const auto windowW = static_cast<float>(m_window.GetWidth());
	const auto windowH = static_cast<float>(m_window.GetHeight());

	if (m_bSetupMode) {
		const Rect card = CardRect(windowW, windowH, true);
		const Rect passwordField = SetupPasswordFieldRect(card);
		const Rect confirmField = SetupConfirmFieldRect(card);

		const bool clickedPassword = RectContainsPoint(passwordField, x, y);
		const bool clickedConfirm = RectContainsPoint(confirmField, x, y);
		if (clickedPassword || clickedConfirm) {
			m_passwordInput.m_bFocused = clickedPassword;
			m_confirmPasswordInput.m_bFocused = clickedConfirm;
			(clickedPassword ? m_passwordInput : m_confirmPasswordInput).OnKey(VK_END);
		}

		// Both fields share one reveal state (see m_bPasswordRevealed's own comment), but
		// each still draws (and must hit-test) its own button - a click on either one
		// toggles the same shared flag.
		if (RectContainsPoint(RevealButtonRect(passwordField), x, y) ||
			RectContainsPoint(RevealButtonRect(confirmField), x, y)) {
			m_bPasswordRevealed = !m_bPasswordRevealed;
		}

		if (RectContainsPoint(SetupButtonRect(card), x, y)) {
			AttemptSetup();
		}
	} else {
		const Rect card = CardRect(windowW, windowH, false);
		const Rect field = FieldRect(card);
		m_passwordInput.m_bFocused = RectContainsPoint(field, x, y);
		if (m_passwordInput.m_bFocused)
			m_passwordInput.OnKey(VK_END); // positions the cursor at the end of the (possibly seeded) value

		if (RectContainsPoint(RevealButtonRect(field), x, y)) {
			m_bPasswordRevealed = !m_bPasswordRevealed;
		}

		if (RectContainsPoint(ButtonRect(card), x, y))
			AttemptUnlock();
	}

	return true;
}

ECursorKind CUnlockScreen::GetDesiredCursor() const
{
	if (!m_bActive) {
		return ECursorKind::CURSOR_ARROW;
	}

	const auto windowW = static_cast<float>(m_window.GetWidth());
	const auto windowH = static_cast<float>(m_window.GetHeight());
	const Rect card = CardRect(windowW, windowH, m_bSetupMode);

	if (m_bSetupMode) {
		const Rect passwordField = SetupPasswordFieldRect(card);
		const Rect confirmField = SetupConfirmFieldRect(card);
		// Reveal-button rects sit inside their own field's own right edge (see
		// RevealButtonRect's own comment), so this has to check them first - checking
		// the field first would always win that overlap and the button would never show
		// a hand cursor, only ever an I-beam.
		if (RectContainsPoint(RevealButtonRect(passwordField), m_flMouseX, m_flMouseY) ||
			RectContainsPoint(RevealButtonRect(confirmField), m_flMouseX, m_flMouseY) ||
			RectContainsPoint(SetupButtonRect(card), m_flMouseX, m_flMouseY)) {
			return ECursorKind::CURSOR_HAND;
		}
		if (RectContainsPoint(passwordField, m_flMouseX, m_flMouseY) ||
			RectContainsPoint(confirmField, m_flMouseX, m_flMouseY)) {
			return ECursorKind::CURSOR_IBEAM;
		}
	} else {
		const Rect field = FieldRect(card);
		if (RectContainsPoint(RevealButtonRect(field), m_flMouseX, m_flMouseY) ||
			RectContainsPoint(ButtonRect(card), m_flMouseX, m_flMouseY)) {
			return ECursorKind::CURSOR_HAND;
		}
		if (RectContainsPoint(field, m_flMouseX, m_flMouseY)) {
			return ECursorKind::CURSOR_IBEAM;
		}
	}

	return ECursorKind::CURSOR_ARROW;
}

void CUnlockScreen::Draw(CDrawList &drawList)
{
	if (!m_bActive)
		return;

	const auto windowW = static_cast<float>(m_window.GetWidth());
	const auto windowH = static_cast<float>(m_window.GetHeight());
	const Color accent = m_settings.m_clrAccent;
	const CFont &body = m_fonts.GetBody();
	const CFont &secondary = m_fonts.GetSecondary();

	// Stops short of the bottom status bar rather than covering the whole window - main.cpp
	// draws that strip (background, seams, the version number) before this widget's own
	// Draw ever runs, so an opaque backdrop reaching all the way down used to fully paint
	// over it the instant the vault was locked, hiding it completely.
	drawList.AddRectFilled(0.0f, 0.0f, windowW, windowH - kStatusBarHeight, Color{12, 12, 14, 255});

	const Rect card = CardRect(windowW, windowH, m_bSetupMode);
	drawList.AddRectRoundedFilled(card.X, card.Y, card.W, card.H, CDrawList::UniformRadii(kCardRadius),
								  Color{26, 26, 30, 255});

	if (!m_bSetupMode) {
		DrawText(drawList, body, card.X + kCardPadding, card.Y + kCardPadding + body.GetAscent(),
				 StringViewFromCString("Master Password"), kColorTextBright);
		DrawText(drawList, secondary, card.X + kCardPadding,
				 card.Y + kCardPadding + body.GetLineHeight() + 4.0f + secondary.GetAscent(),
				 StringViewFromCString("Enter your master password to continue."), kColorTextDim);

		const Rect field = FieldRect(card);
		DrawFieldChrome(drawList, field, m_passwordInput.m_bFocused, accent);
		m_passwordInput.Draw(drawList, body, field.X, field.Y, field.W, field.H, kColorTextBright, accent,
							 !m_bPasswordRevealed);
		const Rect reveal = RevealButtonRect(field);
		DrawEyeGlyph(drawList, m_assets, reveal, m_bPasswordRevealed,
					 RectContainsPoint(reveal, m_flMouseX, m_flMouseY) ? kColorTextBright : kColorTextDim);

		const Rect button = ButtonRect(card);
		drawList.AddRectRoundedFilled(button.X, button.Y, button.W, button.H, CDrawList::UniformRadii(8.0f), accent);
		DrawCenteredText(drawList, body, button.X, button.Y, button.W, button.H, StringViewFromCString("Unlock"),
						 ColorForegroundOn(accent));

		if (m_bWrongPassword) {
			const float messageY = button.Y + button.H + kGap + secondary.GetAscent();
			DrawText(drawList, secondary, card.X + kCardPadding, messageY, StringViewFromCString("Incorrect password."),
					 kColorError);
		}
		return;
	}

	// Setup mode.
	DrawText(drawList, body, card.X + kCardPadding, card.Y + kCardPadding + body.GetAscent(),
			 StringViewFromCString("Create a Master Password"), kColorTextBright);
	const float descriptionY1 = card.Y + kCardPadding + body.GetLineHeight() + 4.0f + secondary.GetAscent();
	DrawText(drawList, secondary, card.X + kCardPadding, descriptionY1,
			 StringViewFromCString("This encrypts your saved account passwords. Choose"), kColorTextDim);
	DrawText(drawList, secondary, card.X + kCardPadding, descriptionY1 + secondary.GetLineHeight(),
			 StringViewFromCString("something memorable - it can't be recovered if lost."), kColorTextDim);

	const Rect passwordField = SetupPasswordFieldRect(card);
	DrawFieldChrome(drawList, passwordField, m_passwordInput.m_bFocused, accent);
	m_passwordInput.Draw(drawList, body, passwordField.X, passwordField.Y, passwordField.W, passwordField.H,
						 kColorTextBright, accent, !m_bPasswordRevealed);
	const Rect passwordReveal = RevealButtonRect(passwordField);
	DrawEyeGlyph(drawList, m_assets, passwordReveal, m_bPasswordRevealed,
				 RectContainsPoint(passwordReveal, m_flMouseX, m_flMouseY) ? kColorTextBright : kColorTextDim);

	const Rect confirmField = SetupConfirmFieldRect(card);
	DrawFieldChrome(drawList, confirmField, m_confirmPasswordInput.m_bFocused, accent);
	m_confirmPasswordInput.Draw(drawList, body, confirmField.X, confirmField.Y, confirmField.W, confirmField.H,
								kColorTextBright, accent, !m_bPasswordRevealed);
	const Rect confirmReveal = RevealButtonRect(confirmField);
	DrawEyeGlyph(drawList, m_assets, confirmReveal, m_bPasswordRevealed,
				 RectContainsPoint(confirmReveal, m_flMouseX, m_flMouseY) ? kColorTextBright : kColorTextDim);

	const Rect button = SetupButtonRect(card);
	drawList.AddRectRoundedFilled(button.X, button.Y, button.W, button.H, CDrawList::UniformRadii(8.0f), accent);
	DrawCenteredText(drawList, body, button.X, button.Y, button.W, button.H, StringViewFromCString("Create Password"),
					 ColorForegroundOn(accent));

	if (m_bPasswordMismatch || m_bSetupFailed) {
		const float messageY = button.Y + button.H + kGap + secondary.GetAscent();
		DrawText(
			drawList, secondary, card.X + kCardPadding, messageY,
			StringViewFromCString(m_bPasswordMismatch ? "Passwords don't match." : "Something went wrong - try again."),
			kColorError);
	}
}
