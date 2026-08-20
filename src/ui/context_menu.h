#pragma once

// An in-app right-click popup: structurally like CSettingsMenu (small rounded panel,
// row-per-item, click-anywhere-closes) but built fresh each open from a caller-supplied
// item list instead of two fixed rows, since what it offers is contextual - right-
// clicking a carousel banner offers Login/Copy Password per account, right-clicking an
// account row in the modal offers Edit/Delete, right-clicking empty carousel space
// offers "Add Account" for the centered banner. No open/close animation (a right-click
// menu reads as instant everywhere else in Windows) and no sub-menus - kMaxItems is
// small enough in every real use that a flat list covers it.
//
// This is the one widget meant to sit above every other popup (a right-click menu reads
// as always-on-top of whatever it was opened over, including an open modal) without
// being the title-bar's special alwaysTopmost case - a coordinating owner achieves that
// simply by pushing this widget onto CWidgetStack last, after every other popup, so push
// order alone makes it the topmost normal entry. Like CSettingsMenu, every input event is
// swallowed while open (IsBlocking), not just clicks landing on a row.

#include <cstdint>

#include "core/string_view.h"
#include "font_manager.h"
#include "ui/widget.h"

constexpr std::uint32_t kContextMenuMaxItems = 12;
constexpr std::uint32_t kContextMenuNoSelection = 0xFFFFFFFFu;

// Caller-defined Id, returned by ConsumeSelection on a hit. Label is a non-owning view -
// every current call site hands in a static string literal; a future caller wanting a
// dynamic per-item label (e.g. "Login as <username>") will need its own owned storage,
// see the original f4 repo's CLAUDE.md for this same caveat.
struct ContextMenuItem {
	CStringView Label;
	std::uint32_t Id;
};

class CContextMenu : public CWidget {
  public:
	explicit CContextMenu(CFontManager &fonts);

	// Opens at (x,y) - the right-click position - clamped so the menu never spills off
	// the window edge. Copies items in; the caller's array doesn't need to outlive the
	// call.
	void Open(float x, float y, const ContextMenuItem *pItems, std::uint32_t itemCount, float windowW, float windowH);
	void Close();

	void Update(float deltaSeconds) override {}
	void Draw(CDrawList &drawList) override;

	bool OnPointerDown(float x, float y) override
	{
		return IsBlocking();
	}
	bool OnPointerMove(float x, float y) override
	{
		return IsBlocking();
	}

	// Always closes the menu (click-anywhere-closes) and latches the clicked item's id
	// for ConsumeSelection, or kContextMenuNoSelection if the click landed outside every
	// row.
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

	// No open/close animation (see the file comment), so blocking is a plain m_bOpen
	// check rather than an eased-amount threshold.
	bool IsBlocking() const override
	{
		return m_bOpen;
	}

	ECursorKind GetDesiredCursor() const override;

	// The id OnPointerUp's most recent click latched, cleared on read - kContextMenuNoSelection
	// if nothing was ever clicked (or the click missed every row) since the last Open.
	std::uint32_t ConsumeSelection();

  private:
	CFontManager &m_fonts;
	bool m_bOpen = false;
	float m_flAnchorX = 0.0f; // top-left corner, already clamped to the window by Open
	float m_flAnchorY = 0.0f;
	ContextMenuItem m_aItems[kContextMenuMaxItems]{};
	std::uint32_t m_nItemCount = 0;
	std::uint32_t m_pendingSelection = kContextMenuNoSelection;
};
