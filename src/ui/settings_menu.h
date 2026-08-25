#pragma once

// The small rendered app menu anchored under the title bar's Menu button - a rounded
// popup rather than a native TrackPopupMenu. Two groups of items, split by a hairline:
// Check for Updates on its own (a thing you do to this build), then Settings and Open Data
// Folder (both about this install's configuration). No Exit item - the title bar's own
// close button already does that, so this menu doesn't need a second, redundant way to
// quit - and no version strip either: a number nobody can act on doesn't earn a permanent
// line in a menu.
//
// The item list is a table (see settings_menu.cpp's kMenuItems), not a hand-written rect/
// hover/draw trio per row: this menu grew from three items to four and every one of those
// three had its own copy-pasted hover branch, which is exactly how a fifth ends up
// subtly different from the rest. Adding an item now means adding one row to that table.
//
// While open (IsBlocking), every input event is swallowed regardless of kind or where it
// lands - not just clicks - mirroring the original codebase's own blanket dismiss-on-any-
// input handling for this exact menu. Every CWidget::On* override below returns
// IsBlocking() unconditionally for this reason; see OnPointerUp's own comment for the one
// override that also does real work.

#include <cstdint>

#include "font_manager.h"
#include "ui/widget.h"

class CAssetManager;
struct CSettings;

enum class ESettingsMenuAction : std::uint8_t {
	SETTINGS_MENU_ACTION_NONE,
	SETTINGS_MENU_ACTION_OPEN_SETTINGS,
	SETTINGS_MENU_ACTION_OPEN_DATA_FOLDER,
	// The owner (main.cpp) kicks a fresh CUpdater::CheckForUpdateAsync if one isn't already
	// due, then opens CUpdateOverlay either way, so this row always shows *something* -
	// "Checking...", "Up to date," a real update - rather than being a silent no-op. This
	// row itself stays a plain, static "Check for Updates" label regardless of CUpdater's
	// own state - see ui/title_bar.cpp's own Update pill for where the dynamic "Update
	// Available"/"Update Failed" feedback actually surfaces automatically.
	SETTINGS_MENU_ACTION_CHECK_FOR_UPDATES,
};

class CSettingsMenu : public CWidget {
  public:
	// assets supplies each row's own left-aligned icon - the Settings gear specifically is
	// the same icon the title bar's Menu button used to show itself (see title_bar.cpp); it
	// moved here once that button started opening a menu, not Settings directly. settings is
	// read-only here, purely so the hover highlight and the header's accent bar follow the
	// user's own accent color instead of a hardcoded purple. appLocked is read-only too (a
	// live reference into main()'s own local, the same reference-to-a-longer-lived-local
	// pattern CUnlockScreen's own m_settings/m_masterKey already use) - purely to gray out
	// and disable the Settings row while the vault is still locked (see Draw/OnPointerUp/
	// GetDesiredCursor): this menu itself is reachable on the master-password screen now
	// (see main.cpp's own widget-stack push order), but actually opening CSettingsPanel
	// still isn't, so a click there shouldn't silently do nothing without any visual hint
	// why.
	CSettingsMenu(CFontManager &fonts, CAssetManager &assets, const CSettings &settings, const bool &appLocked);

	void Open();
	void Close();

	// Eases m_flOpenAmount toward its target every frame regardless of whether the menu
	// is open, so a closing menu keeps animating out instead of vanishing instantly, and
	// eases each item's own hover amount so the highlight grows under the cursor rather
	// than snapping between rows.
	void Update(float deltaSeconds) override;
	void Draw(CDrawList &drawList) override;

	bool OnPointerDown(float x, float y) override
	{
		return IsBlocking();
	}
	bool OnPointerMove(float x, float y) override
	{
		return IsBlocking();
	}

	// Any click while the menu is blocking closes it (standard menu dismiss-on-click) and
	// latches whichever item (if any) was actually hit for ConsumeAction to report back.
	bool OnPointerUp(float x, float y) override;

	bool OnScroll(float x, float y, float wheelDelta) override
	{
		return IsBlocking();
	}
	bool OnKeyDown(std::uint32_t keyCode) override
	{
		return IsBlocking();
	}
	bool OnChar(std::uint32_t character) override
	{
		return IsBlocking();
	}

	bool IsBlocking() const override
	{
		return m_flOpenAmount > 0.01f;
	}

	ECursorKind GetDesiredCursor() const override;

	// The action (if any) OnPointerUp's most recent click latched, cleared on read - a
	// coordinating owner (main.cpp) polls this once per frame to open CSettingsPanel or open
	// this app's own data folder in Explorer, the same one-shot-then-clears contract as
	// CTitleBar::ConsumeMenuClicked.
	ESettingsMenuAction ConsumeAction();

	// The cap on settings_menu.cpp's own item table - public only so that table's
	// static_assert can check itself against it.
	static constexpr std::uint64_t kMaxItems = 8;

  private:
	// Whether the item at `index` in settings_menu.cpp's own table can actually be clicked
	// right now - only ever false for Settings while the vault is locked, but expressed as
	// a per-item question so the draw/click/cursor paths all ask it the same way.
	bool IsItemEnabled(std::uint64_t index) const;

	CFontManager &m_fonts;
	CAssetManager &m_assets;
	const CSettings &m_settings;
	const bool &m_appLocked;
	bool m_bOpen = false;
	float m_flOpenAmount = 0.0f;

	// Per-item hover fade, indexed the same as settings_menu.cpp's kMenuItems. Sized off
	// the generous fixed cap above rather than that table's own length, which this header
	// can't see - the static_assert next to the table keeps the two honest.
	float m_aItemHoverAmount[kMaxItems]{};

	ESettingsMenuAction m_pendingAction = ESettingsMenuAction::SETTINGS_MENU_ACTION_NONE;
};
