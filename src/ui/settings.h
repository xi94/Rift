#pragma once

// The plain persisted-settings record - deliberately separate from CSettingsPanel (the
// widget that edits it), the same split the original project draws between its own
// Settings struct and Settings_Panel: CSettings is what CStorage round-trips to disk and
// what every other widget reads live values from (CCarousel's card border color, the
// unlock screen's accent), while CSettingsPanel only owns the transient UI state of the
// dialog that edits it (scroll position, which field is focused, drag state).

#include <cstddef>
#include <cstdint>

#include "core/crypto.h"
#include "core/types.h"

struct CSettings {
	// Last known main window size (logical pixels) - main.cpp reads this before the window
	// even exists to size it on launch, and keeps it in sync with the live window every
	// frame so whatever save happens next (any of them - see main.cpp's own SaveNow call
	// sites) persists whatever size the user last left it at. Defaults match this project's
	// own shipped default size.
	std::uint32_t m_nWindowWidth = 1042;
	std::uint32_t m_nWindowHeight = 675;

	bool m_bAnimationsEnabled = true;
	float m_flAnimationSpeed =
		1.0f; // multiplier on every eased animation's rate - see CAnimator::SetSpeed. 1.0 = normal speed.
	// Multiplies every corner radius the UI asks for (see CDrawList::ScaledRadius) - 1.0 is
	// the radius each widget was designed at, and 0 squares everything off. This replaced a
	// separate on/off toggle, which had become a second control for the value this one
	// already reaches at zero; CStorage still reads that old flag when loading an existing
	// settings.json, so anyone who had turned corners off keeps square corners.
	float m_flCornerRoundness = 1.0f;

	// Nominal display units, not literal baked pixels - see CFont's own comment for why
	// the number a user types here isn't 1:1 with what gets baked.
	float m_flFontPixelSize = 14.0f;
	float m_flSecondaryFontPixelSize = 12.0f;

	Color m_clrAccent{108, 90, 220, 255};
	char m_szFontName[64] = "segoeui.ttf";

	// SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) on the main window while the account
	// modal is open (see main.cpp's main loop) - keeps usernames/notes/revealed passwords out
	// of screenshots and screen shares. Defaults on: this is exactly the kind of thing a user
	// wouldn't think to go looking for a setting to turn on, but would want by default once
	// they realize it exists.
	bool m_bExcludeAccountListFromCapture = true;

	// Closing the window hides it to the tray icon instead of quitting; the tray menu's
	// Exit item is then what actually ends the app. On by default - the tray menu's quick
	// login is the point of having a tray icon at all, and it's only reachable while Rift
	// is still running. Minimizing is unaffected: it always goes to the taskbar.
	bool m_bCloseToTray = true;

	// The master-password KEK/DEK params CMasterKey::Set produced (see its own file
	// comment) - not secret on their own (nobody recovers the DEK from these without the
	// password), persisted so a later session's CMasterKey::Unlock can be re-supplied
	// with exactly what Set actually used. m_opsLimit/m_memLimit specifically must be
	// persisted, not re-read from CCrypto::DefaultOpsLimit/MemLimit at unlock time - see
	// those functions' own comment on why a future tuning change must never invalidate an
	// already-persisted vault.
	bool m_bMasterPasswordEnabled = false;
	std::uint8_t m_aMasterPasswordSalt[CCrypto::kSaltSize]{};
	std::uint64_t m_masterPasswordOpsLimit = 0;
	std::size_t m_masterPasswordMemLimit = 0;
	std::uint8_t m_aMasterPasswordWrapNonce[CCrypto::kNonceSize]{};
	std::uint8_t m_aMasterPasswordWrappedDek[CCrypto::kKeySize + CCrypto::kTagSize]{};
};
