#pragma once

// A horizontally scrolling banner picker: the centered card is the current selection,
// drawn larger with a stronger border; scrolling (wheel, drag, or a direct click on a
// card) animates the selection smoothly rather than snapping. Banner art is a real
// texture (see asset_manager.h); CBanner::Accent is a fallback tint kept for the
// selection glow and anywhere a flat color still reads better than the image.
//
// Three view modes, switched with Ctrl+scroll the way File Pilot's own view-mode slider
// does: Carousel (the above), Grid (a wrapping grid of fixed-size cards, vertically
// scrollable, no horizontal movement), and List (compact full-width rows - a large
// dedicated per-game icon (CBanner::pIcon) + title). Switching crossfades-and-slides
// between whichever two modes are involved rather than snapping, via CAnimator::EaseToward
// like every other animated value in this project.
//
// This is the widget FORK_WITH_CLASSES.md 3.2 specifically calls out as the origin of the
// "a capability lives at one hand-wired call site and every sibling has to remember to
// duplicate it" bug class: the original's Grid/List view modes never got scrollbar-drag
// support because that wiring was free functions a call site had to invoke by hand, and
// its own card-drag-to-scroll was a hand-rolled press/threshold/delta implementation
// independently reinvented from every other drag site. Both are now CScrollable
// (m_wrapScroll) and CDraggable (m_cardDrag) - shared components every consumer in this
// project goes through identically, so a missing wire-up is a much smaller, more visible
// gap than a whole reimplemented feature.
//
// CWidget's OnScroll carries no modifier-key state (window.h's InputEvent doesn't track
// one), so the Ctrl+scroll "change view mode" gesture can't be distinguished from a plain
// wheel notch inside OnScroll itself - the coordinating owner (main.cpp, not yet written)
// is expected to check GetKeyState(VK_CONTROL) itself and call AdjustZoomStop instead of
// OnScroll for that one frame, the same "Win32 state OnScroll's signature can't carry"
// situation, just resolved by a second explicit method rather than a redesigned signature.
//
// Deliberately has no right-click behavior: everything its context menu used to offer
// (Open, Login, Copy Password, Add Account) is already one plain left-click away, on the
// card itself or inside the account list it opens, and a menu that only duplicates the
// primary click is a second thing to maintain for no reach. Right-clicking a card falls
// through to nothing in every view mode.

#include <cstdint>

#include "core/types.h"
#include "font_manager.h"
#include "ui/banner.h"
#include "ui/draggable.h"
#include "ui/scrollable.h"
#include "ui/widget.h"

class CAssetManager;

enum class ECarouselViewMode : std::uint8_t {
	CAROUSEL_VIEW_MODE_CAROUSEL, // the horizontal slide-around view - the original, and the most "zoomed in"
	CAROUSEL_VIEW_MODE_GRID,	 // a wrapping grid of fixed-size cards
	CAROUSEL_VIEW_MODE_LIST,	 // compact full-width rows: thumbnail + title
};
constexpr std::uint32_t kCarouselViewModeCount = 3;

// Carousel, then 3 stops each for Grid and List (base, grown, max) - see m_nZoomStop's own
// comment. 7 stops total; percentages are 100/6 apart (0, 16.67, 33.33, 50, 66.67, 83.33,
// 100) rather than clean whole numbers, the tradeoff for giving Grid and List a symmetric
// 3-level zoom range each.
constexpr std::int32_t kCarouselZoomStopCount = 7;

// One account's real identity within a GetVisibleAccounts query result below - which
// banner actually owns it and its index within that banner's own list. An account visible
// under a second game (CAccount::m_uVisibleBannerMask) is still only ever stored once,
// under its real owner; this is how a query result points back to that one true copy.
struct VisibleAccountRef {
	std::uint32_t BannerIndex;
	std::uint32_t AccountIndex;
};

// The true worst case: every account in every banner could theoretically all be visible
// under one single target banner.
constexpr std::uint32_t kCarouselMaxVisibleAccounts = kCarouselMaxBanners * kCarouselMaxAccountsPerBanner;

struct BannerRect {
	float X;
	float Y;
	float W;
	float H;
};

class CCarousel : public CWidget {
  public:
	// assets supplies the view-mode flyout/chip's own Grid/List/Carousel icons (see
	// DrawModeGlyph in carousel.cpp) - everything else this class draws is either a
	// per-banner texture the caller hands in through AddBanner or a hand-drawn glyph.
	CCarousel(CFontManager &fonts, CAssetManager &assets);

	// Appends a new banner (game). textureAspect is derived from pTexture's real
	// dimensions here, once, so cover-fit cropping never needs to re-query the renderer
	// later.
	void AddBanner(CStringView title, CTexture *pTexture, CTexture *pIcon, Color accent);

	// Asserts rather than validates - both indices are always caller-controlled (UI-driven
	// selection, not user-typed input).
	void AddAccount(std::uint32_t bannerIndex, CStringView username, CStringView note, CStringView password);

	// Overwrites an existing account row in place (used by an edit form) rather than
	// remove+add, so its position in the list doesn't change.
	void UpdateAccount(std::uint32_t bannerIndex, std::uint32_t accountIndex, CStringView username, CStringView note,
					   CStringView password);

	// Shifts every following account down by one slot and decrements the count - the
	// fixed-capacity array has no gaps to leave behind.
	void RemoveAccount(std::uint32_t bannerIndex, std::uint32_t accountIndex);

	// Every account visible under bannerIndex's account list: that banner's own accounts
	// first, in original order, then every other banner's account that has opted into this
	// banner via CAccount::GetEffectiveVisibleMask, in banner-then-account order. pOut must
	// have room for kCarouselMaxVisibleAccounts entries; returns how many were written.
	std::uint32_t GetVisibleAccounts(std::uint32_t bannerIndex, VisibleAccountRef *pOut) const;

	std::uint32_t GetBannerCount() const
	{
		return m_nBannerCount;
	}
	const CBanner &GetBanner(std::uint32_t index) const
	{
		return m_aBanners[index];
	}
	CBanner &GetBanner(std::uint32_t index)
	{
		return m_aBanners[index];
	}

	// The banner index Carousel mode is currently centered/settling on (its *target*, not
	// the mid-animation scroll position) - what a caller uses to know which banner counts
	// as "selected," since Grid/List have no persistent selected-card concept of their own.
	std::int32_t GetSelectedIndex() const;

	// On-screen rect for one banner at its current (possibly mid-animation) scale/
	// position, in Carousel mode specifically - shared by Draw and HitTest so what's drawn
	// and what's clickable can never drift apart. Grid/List geometry is internal (nothing
	// outside this class needs those rects individually).
	BannerRect GetBannerRect(std::uint32_t index) const;

	// Index of the banner under (x,y) in whichever mode is currently active, or -1 if the
	// point isn't over any banner. In Carousel mode, when cards overlap (the centered one
	// is drawn larger than its slot), the most-centered candidate wins, matching what's
	// visually on top; Grid/List cards never overlap, so first-hit-wins there.
	std::int32_t HitTest(float x, float y) const;

	// Ctrl+scroll (see this class's own file comment for why this isn't just OnScroll):
	// cycles the view mode one zoom stop per notch, clamped (not wrapped) at either end.
	// Scrolling up moves forward through the stops (Carousel -> Grid -> List -> List grown
	// -> List max); scrolling down reverses.
	void AdjustZoomStop(float wheelDelta);

	// Snaps the zoom stop (and the view mode/percent it implies) directly to `stop`, no
	// easing or crossfade - the "apply a persisted value before the first frame" step
	// CStorage's Load needs, the same role CFontManager::ApplyBody/CAnimator::SetEnabled
	// play for other persisted settings a coordinating owner applies right after load.
	void ApplyZoomStop(std::int32_t stop);

	// Snaps Carousel mode's centered banner directly to `index`, no easing - the same
	// "apply a persisted value before the first frame" role ApplyZoomStop plays just
	// above, for the banner CStorage's Load remembers Carousel mode was last centered on.
	// Independent of whichever view mode ApplyZoomStop above restores: Grid/List never
	// touch m_flScrollOffset/m_flTargetScrollOffset, so this is exactly the value Carousel
	// mode resumes on whenever the user next zooms/switches back into it.
	void ApplySelectedIndex(std::int32_t index);

	std::int32_t GetZoomStop() const
	{
		return m_nZoomStop;
	}
	ECarouselViewMode GetViewMode() const
	{
		return m_viewMode;
	}

	// The area (m_vecBounds, set once per frame by whoever owns this widget - see
	// CWidget::m_vecBounds) drives every geometry computation below; nothing here takes a
	// separate area rect parameter the way the original's free functions had to.
	void Update(float deltaSeconds) override;
	void Draw(CDrawList &drawList) override;

	// The current view mode's name + icon ("Carousel - Ctrl+Scroll to zoom", the same
	// glyph DrawModeSwitcher's own flyout rows use), bottom-right of the app's shared
	// bottom chrome strip (window.h's kStatusBarHeight - main.cpp draws that strip's own
	// background; this only draws into it, at the same derived position
	// StatusBarContentRect computes for hit-testing) rather than this widget's own
	// floating overlay chip the way it used to be. Plain text/icon, no background pill of
	// its own - the status bar underneath it is the only backing it needs. Also the
	// mode-switcher flyout's own open trigger now - see IsMouseOverModeSwitcher.
	void DrawStatusBarContent(CDrawList &drawList) const;

	bool OnPointerDown(float x, float y) override;
	bool OnPointerMove(float x, float y) override;
	bool OnPointerUp(float x, float y) override;
	bool OnScroll(float x, float y, float wheelDelta) override;

	// The banner a genuine left-click (not a drag, not a mode-switcher click, not the
	// release that ends a scrollbar-thumb drag) just resolved to via OnPointerUp -
	// PENDING_HIT_KIND_MISS on a click that missed every banner, PENDING_HIT_KIND_NONE on
	// any OnPointerUp that wasn't one of these three excluded cases. A coordinating owner
	// decides whether a hit should actually open the account modal (Grid/List: any hit
	// opens immediately; Carousel mode: only a click on the already-centered/selected card
	// does, since a click on any other card just re-centers it - GetSelectedIndex() read
	// *before* dispatching the click is what "already centered" compares against). Kept as
	// an explicit signal rather than a coordinating owner re-deriving it from HitTest after
	// the fact specifically because that re-derivation can't tell a real card click apart
	// from the release ending an unrelated drag - both can land over a card - which is
	// exactly the class of bug this method exists to make impossible to reintroduce.
	PendingHit ConsumePendingClick();

	// Whether the mouse is currently over the view-mode flyout's hover region (its panel,
	// once visible, plus the persistent chip beneath it). Update calls this itself every
	// frame using the already-gated m_flMouseX/m_flMouseY CWidget::SetMouseGated set just
	// before Update runs - unlike the original free-function version, this class already
	// has its own current mouse position as persistent state, so there's no need for a
	// coordinating owner to compute and thread this through by hand. Exposed publicly
	// anyway since it's a genuinely useful standalone query (e.g. for a future cursor
	// change), not because Update needs a caller to supply it.
	bool IsMouseOverModeSwitcher(float mouseX, float mouseY) const;

	// A hand over a card/row (Carousel/Grid/List alike - HitTest already unifies the three)
	// or the flyout's own rows/track; SIZEALL while a card drag, wrap-scrollbar drag, or
	// flyout track drag is actually in progress - mirrors OnPointerDown/Move/Up's own
	// per-mode priority order so this can never show a cursor for an interaction that
	// wouldn't actually happen on that click.
	ECursorKind GetDesiredCursor() const override;

  private:
	// --- Geometry (pure functions of current state + m_vecBounds, no side effects) ---
	// Kept as private methods rather than free functions (the original's own shape) since
	// C++ has no friend-less way for an anonymous-namespace free function to reach a
	// class's private members - the class-based rewrite's answer to the same "shared
	// geometry function, draw and hit-testing both call" pattern the original's file-local
	// helpers already used.
	BannerRect GridBannerRect(std::uint32_t index) const;
	float GridContentHeight() const;
	BannerRect ListBannerRect(std::uint32_t index) const;
	float ListContentHeight() const;
	float WrapContentHeight(ECarouselViewMode mode) const;
	Rect ScrollbarTrackRect() const;
	float ModeSwitcherTrackColumnWidth() const;
	float ModeSwitcherRowsContentHeight() const;
	Rect ModeSwitcherPanelRect() const;
	static Rect ModeSwitcherRowsRect(Rect panel);
	Rect ModeSwitcherTrackRect(Rect panel) const;
	Rect ModeSwitcherHoverRect() const;
	Rect StatusBarRect() const;
	Rect StatusBarContentRect() const;
	bool IsModeSwitcherVisible() const
	{
		return m_flModeSwitcherVisibleAmount > 0.01f;
	}
	float ClampTarget(float value) const;

	// Applies `stop` (clamped) as the new target: updates m_viewMode/starts the crossfade
	// transition only when the implied mode actually changes (not on every zoom-stop
	// change - moving between List's three zoom stops doesn't re-trigger it), and always
	// refreshes the flyout's hold timer, the same "show it right away" effect hovering it
	// has. Shared by AdjustZoomStop (wheel) and the flyout's own row-click/track-drag
	// handling below so all three input paths change state identically.
	void SetZoomStop(std::int32_t stop);

	// --- View-mode flyout pointer handling, called from OnPointerDown/Move/Up before
	// falling through to card/scrollbar handling - see this class's own file comment for
	// why "swallow the click, wider than the drag state itself" needs m_bModeSwitcherPointerCaptured. ---
	bool ModeSwitcherOnPointerDown(float x, float y);
	bool ModeSwitcherOnPointerMove(float x, float y);
	bool ModeSwitcherOnPointerUp();

	// --- Draw ---
	void DrawCarouselMode(CDrawList &drawList, std::uint8_t alpha, float yOffset, float mouseX, float mouseY) const;
	void DrawGridMode(CDrawList &drawList, std::uint8_t alpha, float yOffset, float mouseX, float mouseY) const;
	void DrawListMode(CDrawList &drawList, std::uint8_t alpha, float yOffset, float mouseX, float mouseY) const;
	void DrawMode(CDrawList &drawList, ECarouselViewMode mode, std::uint8_t alpha, float yOffset, float mouseX,
				  float mouseY) const;
	void DrawModeSwitcher(CDrawList &drawList) const;

	CFontManager &m_fonts;
	CAssetManager &m_assets;

	CBanner m_aBanners[kCarouselMaxBanners]{};
	std::uint32_t m_nBannerCount = 0;

	float m_flScrollOffset = 0.0f;		 // animated, in card-slot units - Carousel mode only
	float m_flTargetScrollOffset = 0.0f; // where m_flScrollOffset eases toward

	CDraggable m_cardDrag; // Carousel mode's card-drag-to-scroll
	float m_flDragStartScrollOffset = 0.0f;

	// View-mode switching, driven by a single continuous 0..100 "zoom" position - File
	// Pilot's own icon-size slider is the reference: Carousel (0%) -> Grid, and both Grid
	// and List keep going once entered, growing their card/row-icon size continuously
	// through two more stops of their own (base, grown, max - 3 total each) rather than
	// either being a dead end. m_nZoomStop is the target stop index (0..
	// kCarouselZoomStopCount-1, one Ctrl+scroll notch moves exactly one stop);
	// m_flZoomPercent eases toward that stop's exact percentage every frame, driving both
	// the flyout's slider thumb and - once m_nZoomStop lands in the Grid or List band -
	// that mode's card/row-icon size continuously, so growing across several notches reads
	// as smooth motion rather than snapping between fixed sizes.
	std::int32_t m_nZoomStop = 0;
	float m_flZoomPercent = 0.0f;

	// m_viewMode is the current target; while m_flTransitionAmount > 0 (eases 1 -> 0 right
	// after a switch), m_transitionFromMode is still being drawn too, fading/sliding out as
	// m_viewMode fades/slides in - see Draw. Updated whenever m_nZoomStop crosses into a
	// different mode's band, not on every zoom-stop change (moving between List's three
	// zoom stops doesn't re-trigger this crossfade).
	ECarouselViewMode m_viewMode = ECarouselViewMode::CAROUSEL_VIEW_MODE_CAROUSEL;
	ECarouselViewMode m_transitionFromMode = ECarouselViewMode::CAROUSEL_VIEW_MODE_CAROUSEL;
	float m_flTransitionAmount = 0.0f;

	// Grid/List modes' vertical scroll - reset on every mode switch since the two modes'
	// content heights are unrelated to each other.
	CScrollable m_wrapScroll;

	// Bottom-right view-mode flyout - File Pilot's own Ctrl+scroll slider shows a similar
	// flyout on hover, listing every mode with the current one highlighted.
	// m_flModeSwitcherHoldSeconds counts down from kModeSwitcherHoldDuration whenever the
	// pointer leaves the flyout's hover region and no mode change just happened;
	// m_flModeSwitcherVisibleAmount eases toward 1 while it's > 0, toward 0 once it's
	// spent.
	float m_flModeSwitcherHoldSeconds = 0.0f;
	float m_flModeSwitcherVisibleAmount = 0.0f;

	// The flyout's track/thumb press-and-hold state (a plain press tracker, not a click-vs-
	// drag disambiguation - a track press always jumps to that position immediately and
	// starts tracking, there's no "was this a click or a drag" question the way the card
	// scroll above has one).
	CDraggable m_modeSwitcherDrag;

	// True from a pointer-down the flyout consumed (a row click or a track drag-start)
	// until the matching pointer-up - wider than m_modeSwitcherDrag.IsPressed() on purpose:
	// a plain row click never starts m_modeSwitcherDrag (it resolves entirely on the down),
	// but its paired release still needs to be swallowed rather than reaching the carousel
	// underneath (a real click-through bug the original fixed once).
	bool m_bModeSwitcherPointerCaptured = false;

	// Grid mode's per-card hover "pop" - eases toward 1 for whichever card is actually
	// hovered this frame and toward 0 for every other, so a card grows smoothly on hover
	// and shrinks back smoothly when the pointer moves to a different card or leaves
	// entirely (a fixed-size array, not a single "which index" scalar, so the outgoing and
	// incoming card can ease simultaneously instead of snapping).
	float m_aGridHoverScale[kCarouselMaxBanners]{};

	PendingHit m_pendingClickBanner;
};
