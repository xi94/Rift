#pragma once

// The full-screen "an update is available / is installing" takeover - opened by clicking
// CTitleBar's Update button (visible only once CUpdater actually has something to show; see
// CTitleBar's own file comment on why that button's visibility/glyph reads m_updater
// directly rather than main.cpp polling and pushing state down). Unlike CUnlockScreen, this
// is always dismissible while nothing is actively downloading - an update is a suggestion,
// not a security gate, so nothing here should ever trap the user the way the master-
// password prompt deliberately does.
//
// Pure UI: every real action (starting the download, cancelling, reading progress) calls
// straight through to the CUpdater& this holds - there is no polled Consume* action here
// the way CSettingsMenu/CContextMenu use, because there is nothing for a coordinating owner
// to do in response except what CUpdater itself already exposes. The one exception is the
// relaunch itself (spawning the new process and exiting this one) - that's main.cpp's job,
// via CUpdater::ConsumeReadyToRelaunch, same as every other "the owner needs to do something
// only it can do" signal in this project.

#include "ui/scrollable.h"
#include "ui/widget.h"

class CFontManager;
class CWindow;
class CUpdater;
struct CSettings;

class CUpdateOverlay : public CWidget {
  public:
	CUpdateOverlay(CFontManager &fonts, CWindow &window, CSettings &settings, CUpdater &updater);

	void Open();
	void Close();

	// Also eases m_notesScroll toward its target (see CScrollable::Update) - harmless to
	// call while the notes box isn't even visible (a stage other than AVAILABLE), same as
	// every other per-frame update in this project that doesn't bother gating on its own
	// visibility first.
	void Update(float deltaSeconds) override;
	void Draw(CDrawList &drawList) override;

	bool OnPointerUp(float x, float y) override;
	// Real hit-testing now (not a blanket swallow) - a click on the notes scrollbar's thumb
	// (only present in UPDATE_STAGE_AVAILABLE - see update_overlay.cpp's NotesBoxRect) starts
	// a drag; anything else still just swallows the click, same reasoning as CUnlockScreen's
	// own OnPointerDown/OnScroll/OnRightPointerUp trio - nothing underneath should be
	// reachable through a modal takeover, even a dismissible one.
	bool OnPointerDown(float x, float y) override;
	// Continues a notes-scrollbar drag in progress; a no-op (but still swallows, via the
	// base CWidget contract every other input handler here already follows) otherwise.
	bool OnPointerMove(float x, float y) override;
	// Scrolls the notes box while hovering it; swallows unconditionally either way, same as
	// every other input kind while this is active.
	bool OnScroll(float x, float y, float wheelDelta) override;
	bool OnRightPointerUp(float x, float y) override
	{
		return IsBlocking();
	}

	ECursorKind GetDesiredCursor() const override;

	bool IsBlocking() const override
	{
		return m_bActive;
	}

  private:
	CFontManager &m_fonts;
	CWindow &m_window;
	CSettings &m_settings;
	CUpdater &m_updater;

	bool m_bActive = false;

	// The release-notes box's own scroll state, UPDATE_STAGE_AVAILABLE only - see
	// update_overlay.cpp's NotesBoxRect/NotesScrollbarTrackRect for its geometry. Owned here
	// rather than reset per-Open the way CUnlockScreen resets its own transient state on
	// every Activate*, since re-opening the overlay for the *same* still-pending update
	// (dismiss, then reopen from the menu) reasonably keeps wherever you left the notes
	// scrolled to, the same "don't reset scroll position for no reason" instinct
	// CAccountModal's own m_accountsScroll already follows.
	CScrollable m_notesScroll;
};
