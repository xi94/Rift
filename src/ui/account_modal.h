#pragma once

// The account list popup: opens when a carousel banner is clicked (see main.cpp),
// animates open/close (fade + scale, via CAnimator::EaseToward), and dims/blocks
// everything behind it while open (IsBlocking).
//
// The panel scales with the main window's own size (kPanelWidthFraction/HeightFraction in
// account_modal.cpp), capped by kPanelMaxWidthBase/HeightBase so it doesn't turn enormous
// on a large/1440p+ monitor.
//
// Three modes, same panel (only the inner content crossfades - position/size/backdrop
// don't re-animate on a mode switch): ACCOUNT_LIST (pick/edit/remove an account, or add a
// new one), LOGIN_PROGRESS (a CLoginAttempt's status, from clicking Login), and
// EDIT_ACCOUNT (add/edit one row's username/note/password, from the header's + button or
// a row's pencil button).
//
// Data ownership: unlike the directive that originally shaped this port, CCarousel (see
// ui/carousel.h, already landed) turned out to own the live CBanner array itself, not a
// separate array main.cpp holds - so this class takes a CCarousel& and reads/mutates
// banner/account data entirely through CCarousel's own public surface
// (GetBanner/AddAccount/UpdateAccount/RemoveAccount/GetVisibleAccounts) rather than a raw
// CBanner*/count pair. This avoids duplicating CCarousel's already-correct account-array
// mutation logic and its VisibleAccountRef query.
//
// Every mutating call site resolves a "query index" (a position in
// CCarousel::GetVisibleAccounts' result - what m_nSelectedAccountIndex/
// m_nEditAccountIndex actually store, since CLAUDE.md roadmap phase 24 lets an account
// visible in a second game show up in more than one banner's rendered list) back to its
// real owning (bannerIndex, accountIndex) through ONE shared resolver,
// ResolveVisibleAccount, before ever touching CCarousel's storage - this is the exact
// mechanism that fixed a real cross-banner data-corruption bug in the original codebase,
// so every Save/Delete/remove-button/right-click-Delete path goes through it, never a raw
// index into one banner's own array.

#include <cstdint>

#include "core/login_attempt.h"
#include "core/types.h"
#include "font_manager.h"
#include "ui/carousel.h"
#include "ui/game_select_popup.h"
#include "ui/scrollable.h"
#include "ui/text_input.h"
#include "ui/widget.h"

class CAssetManager;
class CWindow;
struct CSettings;

enum class EAccountModalMode : std::uint8_t {
	ACCOUNT_MODAL_MODE_ACCOUNT_LIST,
	ACCOUNT_MODAL_MODE_LOGIN_PROGRESS,
	ACCOUNT_MODAL_MODE_EDIT_ACCOUNT, // add (m_nEditAccountIndex == -1) or edit (>= 0) one account row
};

class CAccountModal : public CWidget {
  public:
	CAccountModal(CFontManager &fonts, CCarousel &carousel, CWindow &window, const CSettings &settings,
				  CAssetManager &assets);

	// Leaves m_nSelectedAccountIndex at -1 (none) - no account is pre-selected on open,
	// the user picks a row explicitly (see OnPointerUp's row-click and click-off
	// handling).
	void Open(std::int32_t bannerIndex);

	// Starts the close animation (Update drives m_flOpenAmount back to 0) - doesn't
	// forget m_nBannerIndex/m_mode immediately, so the closing panel still has something
	// to draw itself with while it fades out.
	void Close();

	// Opens the modal pre-selected to one account and immediately starts a login
	// attempt - the same sequence clicking a row then the Login button triggers,
	// collapsed into one call. Used by the tray's quick-access menu so a right-click
	// quick-login lands straight on the progress view instead of requiring the modal
	// plus a click.
	void OpenForQuickLogin(std::int32_t bannerIndex, std::int32_t accountIndex);

	// Switches to EDIT_ACCOUNT with all three fields blank, ready to fill in a brand new
	// row - Save (see OnPointerUp) calls CCarousel::AddAccount.
	void StartAddAccount();

	// Switches to EDIT_ACCOUNT pre-filled from an existing row - Save calls
	// CCarousel::UpdateAccount in place instead of adding a new one. queryIndex is a
	// CCarousel::GetVisibleAccounts query index (see this class's own file comment),
	// resolved internally to the account's real owning banner - this works correctly
	// even when the row being edited is visible here but actually owned by a different
	// banner. Also the entry point for main.cpp's right-click "Edit" context-menu
	// action.
	void StartEditAccount(std::uint32_t queryIndex);

	// Resolves queryIndex (a CCarousel::GetVisibleAccounts index into m_nBannerIndex's
	// current row list) to its real owning banner and removes it there, adjusting
	// m_nSelectedAccountIndex the same way a plain row click's remove button does.
	// Shared by this class's own row-remove-button handling and main.cpp's right-click
	// "Delete" context-menu action. A no-op if queryIndex doesn't resolve to a real
	// account.
	void RemoveAccountRow(std::uint32_t queryIndex);

	void Update(float deltaSeconds) override;
	void Draw(CDrawList &drawList) override;

	// Handles a click if the modal is blocking: backdrop/close-badge closes it (except
	// during LOGIN_PROGRESS - Cancel is the only way out of an in-flight login), a row
	// selects it, Login starts a background attempt and switches to LOGIN_PROGRESS, the
	// header's + button starts adding a row, a row's pencil/x buttons start editing/
	// delete it, and (in EDIT_ACCOUNT) Save/Cancel/Delete mutate m_carousel directly.
	// Always returns true while blocking - the standard "a modal dialog owns all input
	// while open" contract every popup CWidget in this project already follows.
	bool OnPointerDown(float x, float y) override;
	bool OnPointerMove(float x, float y) override;
	bool OnPointerUp(float x, float y) override;

	// Hit-tests which account row (if any) was right-clicked and latches it for
	// ConsumePendingRightClickRow - this class can't open CContextMenu itself (it
	// doesn't own that class), so a coordinating owner polls the result and builds/
	// opens the actual menu (Edit/Delete), the same one-shot-then-clears contract
	// CCarousel::ConsumePendingRightClick already uses for its own right-click support.
	bool OnRightPointerUp(float x, float y) override;

	// Mouse wheel over the account list scrolls the row list - a no-op once every row
	// already fits, same as CScrollable everywhere else. Still consumes (returns true)
	// whenever blocking, regardless of mode, so a wheel notch over an open modal never
	// reaches the carousel underneath.
	bool OnScroll(float x, float y, float wheelDelta) override;

	// Routes to whichever of m_editUsername/m_editNote/m_editPassword is focused; Tab
	// cycles focus between them. VK_ESCAPE backs EDIT_ACCOUNT out to ACCOUNT_LIST one
	// level, or (in ACCOUNT_LIST) clears the row selection first and only closes the
	// whole modal on a second press with nothing selected - both originally lived as
	// special-cased branches in the original codebase's main-loop input dispatch;
	// consolidated here now that the modal is a self-contained widget that already
	// knows its own mode/selection, which is exactly the kind of duplication
	// FORK_WITH_CLASSES.md's architecture is meant to make unnecessary. Always
	// consumes while blocking.
	bool OnKeyDown(std::uint32_t keyCode) override;
	bool OnChar(std::uint32_t character) override;

	bool IsBlocking() const override
	{
		return m_flOpenAmount > 0.01f;
	}

	ECursorKind GetDesiredCursor() const override;

	// The query index (see this class's own file comment) OnRightPointerUp's most recent
	// hit-test latched: PENDING_HIT_KIND_MISS means the right-click landed inside the
	// modal but not on any specific row, PENDING_HIT_KIND_NONE means nothing pending. Only
	// meaningful the frame after OnRightPointerUp returned true; cleared on read.
	PendingHit ConsumePendingRightClickRow();

  private:
	// --- Layout: the full panel geometry chain, computed once per call and shared by
	// every hit-test/draw function below instead of each independently re-deriving it
	// (mirrors CCarousel's own "private methods, not free functions, since C++ has no
	// friend-less way for an anonymous-namespace function to reach private members"
	// reasoning). ---
	struct Layout {
		Rect Panel;
		Rect Inner;
		Rect Content;
		Rect Left;
		Rect Right;
		Rect Footer;
	};
	Layout ComputeLayout() const;
	Rect PanelRect() const;
	Rect AccountsScrollRegionRect(Rect right) const;

	// A note-less account's row is shorter than one with a note (see RowHeightForAccount in
	// the .cpp), so each row's position is a running sum of the real heights before it, not
	// a flat index * uniform-height multiply - ComputeRowLayout is that sum, computed once
	// and shared by every caller that needs row geometry (draw, click/hover hit-testing,
	// scroll content height) so they can never disagree with each other about where a row
	// actually is. outTops/outHeights must each have room for count entries (callers already
	// size these off kCarouselMaxVisibleAccounts, matching refs itself).
	void ComputeRowLayout(const VisibleAccountRef *refs, std::uint32_t count, float *outTops,
						  float *outHeights) const;
	float AccountsContentHeight(const float *tops, const float *heights, std::uint32_t count) const;
	Rect AccountsScrollbarTrackRect(Rect scrollRegion) const;
	Rect CloseBadgeRect(Rect left) const;
	Rect LoginButtonRect(Rect footer) const;
	Rect RowRect(Rect right, float top, float height) const;
	Rect RowRemoveButtonRect(Rect row) const;
	Rect RowEditButtonRect(Rect row) const;
	Rect AddAccountButtonRect(Rect right) const;
	float EditFormContentOffsetY(Rect right) const;
	Rect EditFieldBlockRect(Rect right, std::uint32_t index, float yOffset) const;
	Rect EditFieldInputRect(Rect block) const;
	Rect VisibilityChipRect(Rect right, float yOffset) const;
	Rect EditCancelButtonRect(Rect save) const;
	Rect EditDeleteButtonRect(Rect footer) const;
	Rect PasswordRevealButtonRect(Rect passwordField) const;

	// Resolves a query index to its real (bannerIndex, accountIndex) - see this file's
	// own header comment for why every mutation must go through this.
	bool ResolveVisibleAccount(std::uint32_t bannerIndex, std::uint32_t queryIndex, VisibleAccountRef &out) const;

	// Resolves queryIndex through ResolveVisibleAccount and starts m_login with that
	// account's username/password plus bannerIndex's own Title as the launch-product target
	// (deliberately bannerIndex - the game view the click came from - not the resolved
	// ref.BannerIndex the account record actually lives under; see this method's own .cpp
	// comment for why a cross-visible account makes those two genuinely different things) -
	// the one place both LOGIN_PROGRESS entry points (a row's own Login button, and the
	// tray's quick-login) build what CLoginAttempt::Start needs, so they can't drift apart.
	// A no-op (m_login never starts) if queryIndex doesn't resolve to a real account.
	void StartLoginFor(std::uint32_t bannerIndex, std::uint32_t queryIndex);

	// --- Draw sections ---
	void DrawAccountList(CDrawList &drawList, Rect right, std::uint8_t alpha) const;
	void DrawAccountRow(CDrawList &drawList, Rect right, Rect row, const CAccount &account, bool isSelected,
						std::uint8_t alpha) const;
	void DrawLoginProgress(CDrawList &drawList, Rect right, std::uint8_t alpha) const;
	void DrawEditAccount(CDrawList &drawList, Rect right, std::uint8_t alpha);
	void DrawEditField(CDrawList &drawList, Rect block, const char *pLabel, const CTextInput &input, bool masked,
					   std::uint8_t alpha) const;
	void DrawFooter(CDrawList &drawList, Rect footer, std::uint8_t alpha);
	void DrawSectionTitle(CDrawList &drawList, Rect right, float headerHeight, CStringView title,
						  std::uint8_t alpha) const;
	void DrawAddAccountButton(CDrawList &drawList, Rect right, std::uint8_t alpha) const;

	CFontManager &m_fonts;
	CCarousel &m_carousel;
	CWindow &m_window;
	const CSettings &m_settings;
	CAssetManager &m_assets;

	bool m_bIsOpen = false;			  // target state
	float m_flOpenAmount = 0.0f;	  // animated 0 (closed) .. 1 (fully open)
	std::int32_t m_nBannerIndex = -1; // which banner's accounts are shown; -1 when none

	// Which row is highlighted / would log in; -1 = none. A CCarousel::GetVisibleAccounts
	// query index - see this file's own header comment.
	std::int32_t m_nSelectedAccountIndex = -1;

	EAccountModalMode m_mode = EAccountModalMode::ACCOUNT_MODAL_MODE_ACCOUNT_LIST;
	CLoginAttempt m_login;
	float m_flLoginElapsedSeconds = 0.0f; // drives the in-progress indicator's pulse animation

	// EDIT_ACCOUNT's form fields. Only one is focused at a time - same "explicit,
	// caller-routes-input" pattern as CSettingsPanel's single font-name field, extended
	// to three since a row genuinely has three editable values.
	CTextInput m_editUsername;
	CTextInput m_editNote;
	CTextInput m_editPassword;
	// -1 = adding a new account; >= 0 = editing that row, again as a query index, not
	// necessarily an index into m_nBannerIndex's own accounts array.
	std::int32_t m_nEditAccountIndex = -1;
	bool m_bEditPasswordRevealed =
		false; // the eye button next to the password field; reset to false on every add/edit start

	// Which other games this account is visible in - the form's "Visible in N games"
	// chip opens this; its working mask is written into the saved account on Save.
	CGameSelectPopup m_gameSelect;

	// ACCOUNT_LIST's row list, once it overflows the panel. Only relevant in that one
	// mode; harmless (CScrollable::IsVisible is false) everywhere else.
	CScrollable m_accountsScroll;

	PendingHit m_pendingRightClickRow;
};
