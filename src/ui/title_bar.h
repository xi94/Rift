#pragma once

// CTitleBar draws Rift's custom title bar - a menu button (top-left), draggable
// caption strip, and minimize/maximize/close glyphs (top-right) - and handles clicks on
// those buttons. Hit-testing for the same geometry lives on CWindow (WM_NCHITTEST needs
// it before any frame is ever drawn), so this only draws/dispatches using that shared
// geometry - visuals and hit-testing can never drift apart.
//
// The one CWidget in this project pushed onto CWidgetStack with alwaysTopmost=true (see
// FORK_WITH_CLASSES.md 5): it must always render on top and always receive input
// regardless of whatever dialog is currently blocking, since minimizing/closing the app
// itself shouldn't require closing popups first. Minimize/Maximize/Close are handled
// directly (they're pure OS window operations, nothing else needs to know); the Menu
// button doesn't reach into CSettingsMenu itself (this widget has no business knowing
// that class exists) - it just latches a polled flag a coordinating owner checks once per
// frame, the same "explicit state polled once per frame, not a callback into an arbitrary
// other module" philosophy the original f4 codebase already followed.

#include "ui/widget.h"

class CWindow;
class CAssetManager;
class CUpdater;
class CFontManager;

// The title bar's own fill - exported so any other chrome strip (a future bottom status
// bar) can share the exact value instead of an independently-tuned constant that reads as
// a "differently colored" bar next to it.
constexpr Color kTitleBarColor{24, 24, 27, 255};

class CTitleBar : public CWidget {
  public:
	// updater is read-only here (GetStage) - purely to decide whether the Update button is
	// currently visible/clickable at all, and what label/color pill to draw it as ("Update
	// Available" in green, "Update Failed" in red, "Updating..." while in flight - see the
	// .cpp's own UpdateStatusLabel). Starting the actual download, or anything else updater-
	// related, is CUpdateOverlay's job, not this one - see ConsumeUpdateClicked. fonts draws
	// that label - the one title bar button with text on it, everything else here is a plain
	// glyph.
	CTitleBar(CWindow &window, CAssetManager &assets, CUpdater &updater, CFontManager &fonts);

	void Update(float deltaSeconds) override;
	void Draw(CDrawList &drawList) override;

	// Consumes (returns true, does nothing else) a press that landed on any of the four
	// buttons - button actions themselves only ever fire on release, matching native
	// title-bar-button behavior (a press-then-drag-off-then-release shouldn't trigger the
	// button). This exists specifically so a press here can never fall through to
	// whatever's underneath: CTitleBar only overrides OnPointerUp for the actual button
	// logic, and since it's the alwaysTopmost widget, an unconsumed press on the Menu
	// button (say) would reach CCarousel with nothing above it blocking yet, starting a
	// card-drag that the *matching* release - consumed here on OnPointerUp before it ever
	// reaches CCarousel - would then never get a chance to end. That exact asymmetry was a
	// real, reported bug: the carousel would keep "dragging" on pure mouse movement, no
	// button held, after opening Settings from the title bar's Menu button.
	bool OnPointerDown(float x, float y) override;
	bool OnPointerUp(float x, float y) override;

	// A hand over any of the four buttons - CWindow::TitleBarHitTest is the same call
	// Draw's own hover background already uses, so this can never disagree with what's
	// visibly highlighted.
	ECursorKind GetDesiredCursor() const override;

	// True at most once per frame, the frame a click on the Menu button landed - a
	// coordinating owner (main.cpp) polls this to toggle whatever settings-menu widget
	// it owns, then the flag clears itself. Named Consume, not Get/Is, to make the
	// one-shot-then-clears contract obvious at the call site.
	bool ConsumeMenuClicked();

	// Same one-shot-poll-then-clears contract as ConsumeMenuClicked - a coordinating owner
	// (main.cpp) polls this to open/close whatever CUpdateOverlay it owns. Only ever latches
	// while the button is actually visible/clickable (see IsUpdateButtonVisible in the
	// .cpp) - a stray click can't fire this while there's nothing to show.
	bool ConsumeUpdateClicked();

  private:
	CWindow &m_window;
	CAssetManager &m_assets;
	CUpdater &m_updater;
	CFontManager &m_fonts;
	bool m_bMenuClickedThisFrame = false;
	bool m_bUpdateClickedThisFrame = false;
};
