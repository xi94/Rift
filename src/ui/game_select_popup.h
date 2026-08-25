#pragma once

// The account edit form's "visible in which games" multi-select: one row per carousel
// banner, an icon (or a color-square fallback when a banner has none) on the left and a
// checkmark on the right when that bit is set in the working mask. Opened from a chip
// button in CAccountModal's edit-account form; the working mask is edited live as rows
// are clicked and persists across close/reopen within the same edit session - the caller
// (CAccountModal) reads GetMask() back at Save time, the same "applies live, close is
// just dismissal" pattern CColorPicker already uses.
//
// A row can never be unchecked down to zero games - an account visible in no game is
// unreachable from the carousel entirely ("vanishes into the void"), so whichever row is
// the sole remaining checked one is drawn greyed-out/non-interactive (a click on it is
// silently ignored) until another row is checked first.
//
// Row height/icon size derive from CFont::GetLineHeight (see game_select_popup.cpp's
// RowHeightFor/IconSizeFor) rather than fixed pixel constants, so this popup grows with
// the Font Size setting the same as every other row-based list in this project.
//
// Self-contained like CColorPicker: Open snapshots the banner list, anchor rect, and
// bounds rect to lay out against, rather than taking them fresh on every call the way
// the original's free functions did.
//
// Not a top-level CWidgetStack member (it's a value CAccountModal owns and forwards
// calls to, the plain-member relationship this file's own header doc already describes),
// so nothing calls SetMouseGated on it automatically the way CWidgetStack does for real
// stack members - the owner must do it explicitly once per frame (see
// CAccountModal::Update) or every hover check here silently never fires, since
// CWidget::m_flMouseX/Y default to off-screen and nothing else would ever update them.
//
// Hangs from the chip that opens it: directly below, and right-aligned to the chip's own
// right edge so it grows leftward, over the form rather than away from it. That follows
// the chip itself, which now lives in the top-right of the edit form's header (see
// CAccountModal::VisibilityChipRect) - a menu should drop from the control that opened it,
// and from a top-right anchor the only direction with room is down-and-left.
//
// There is deliberately no flip-above fallback: the anchor sits in the header, so "below"
// always has the whole form beneath it to unfold into, and a popup that sometimes jumped
// above its own chip was this widget's original design and a direct bug report.
// m_flOpenAmount (eased 0 collapsed -> 1 fully open, same CAnimator::EaseToward primitive
// as every other animated reveal in this project) grows the popup's height from the top
// down - "folds out" - while PushClipRect(popup) (see Draw) hides whatever rows the
// not-yet-grown box hasn't reached yet, the same grow-and-clip technique CSettingsPanel's
// protection-tier foldout uses.

#include <cstdint>

#include "font_manager.h"
#include "ui/banner.h"
#include "ui/widget.h"

class CGameSelectPopup : public CWidget {
  public:
	explicit CGameSelectPopup(CFontManager &fonts);

	// initialMask seeds the working mask; pBanners/bannerCount is the row list to offer -
	// the caller's array doesn't need to outlive the call, only individual CBanner
	// pointers within it (pIcon, Accent) are read live by Draw. bounds is the region the
	// popup must stay fully inside - the caller's own content column, not its whole panel
	// or the whole window
	// (clamping against the full window let the popup spill past the account modal's own
	// bottom border whenever its owning panel was smaller than the window and the popup's
	// row count made it tall - a real, screenshotted bug).
	void Open(std::uint16_t initialMask, const CBanner *pBanners, std::uint32_t bannerCount, Rect anchor, Rect bounds);
	void Close();

	std::uint16_t GetMask() const
	{
		return m_uMask;
	}

	void Update(float deltaSeconds) override;
	void Draw(CDrawList &drawList) override;

	// Toggles a row's bit if the click landed on one - except a click on the sole
	// remaining checked row, which is silently ignored (see this file's own doc comment
	// for why). Returns whether the point was anywhere inside the popup at all; unlike
	// CSettingsMenu/CContextMenu, this does NOT self-close on a miss - a false return is
	// the owner's cue to call Close() itself, the same click-anywhere-else-closes
	// contract CColorPicker's OnPointerDown already follows (and for the same reason:
	// both apply their result live/immediately rather than needing a separate confirm
	// step, so there's nothing to lose by leaving the close decision to the owner).
	bool OnPointerDown(float x, float y) override;

	// True through the fold-out AND fold-in animation, not just while m_bOpen - a closing
	// popup still wants input routed to it (and the row/click logic below still reasons in
	// terms of the live m_flOpenAmount) until the fold-in finishes, same as every other
	// animated popup in this project.
	bool IsBlocking() const override
	{
		return m_flOpenAmount > 0.01f;
	}

	// Not auto-polled by CWidgetStack (see this file's own header comment - this isn't a
	// stack member), so the owner (CAccountModal) must consult this explicitly too, the
	// same way it already calls SetMouseGated by hand.
	ECursorKind GetDesiredCursor() const override;

  private:
	CFontManager &m_fonts;
	bool m_bOpen = false;
	float m_flOpenAmount = 0.0f; // eased toward m_bOpen ? 1 : 0 every Update - see this file's own top comment
	std::uint16_t m_uMask = 0;

	const CBanner *m_pBanners = nullptr;
	std::uint32_t m_nBannerCount = 0;

	Rect m_anchor{};
	Rect m_bounds{};
};
