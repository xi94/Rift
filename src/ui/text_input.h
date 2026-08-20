#pragma once

// A single-line text field: fixed-capacity buffer, a cursor (byte offset, no multi-byte/
// UTF-8 awareness yet - ASCII/Latin-1 typing only), backspace/delete/arrow/home/end, a
// keyboard-driven selection, clipboard (Ctrl+C/X/V), and a blinking caret.
//
// Shift+Left/Right/Home/End extends a selection; typing or Backspace/Delete with an
// active selection replaces/removes it; Ctrl+A selects the whole buffer, Ctrl+C/X/V
// copy/cut/paste through the Windows clipboard as plain text. Ctrl+E is Emacs-style
// end-of-line, added alongside Ctrl+A rather than the more traditional Emacs Ctrl+A
// (beginning-of-line) specifically because Ctrl+A is already claimed by the more
// broadly-expected Windows select-all binding - this asymmetry is deliberate, not an
// oversight.
//
// Per FORK_WITH_CLASSES.md 3.3, this is a plain value type a parent CWidget owns and
// forwards char/key events to directly, not a CWidget itself - it never independently
// needs a place in CWidgetStack's z-order. Only one CTextInput should be focused at a
// time; the owner (CAccountModal, CSettingsPanel) is responsible for routing events to
// whichever one has focus and clearing focus elsewhere - the same "explicit, caller-
// decided" pattern as everything else in this project rather than a global focus-manager
// singleton. m_bFocused is a plain public field for exactly that reason: an owner sets it
// directly, there's no invariant here that needs a setter to protect.

#include <cstdint>

#include "core/string_view.h"
#include "core/types.h"

class CFont;
class CDrawList;

constexpr std::uint32_t kTextInputCapacity = 128;

class CTextInput {
  public:
	// Resets to a fresh, unfocused state seeded with initialValue - the whole object, not
	// just the buffer (focus/caret/selection too), since callers use this both for
	// genuine first-time setup and for re-seeding a field when its owning form (re)opens.
	void Init(CStringView initialValue);

	CStringView GetValue() const
	{
		return CStringView{m_szBuffer, m_nLength};
	}

	// Overwrites the buffer with value (truncated to capacity) and clears cursor/
	// selection - used to program a field's contents from outside (e.g. seeding it from a
	// persisted setting), as opposed to the user-typed-input path (OnChar/OnKey's paste).
	void SetValue(CStringView value);

	bool HasSelection() const
	{
		return m_nSelectionAnchor >= 0 && static_cast<std::uint32_t>(m_nSelectionAnchor) != m_nCursor;
	}

	// Ignores non-printable characters (backspace/enter/etc arrive as WM_CHAR too, with
	// codes below 0x20, and are handled by OnKey instead). No-op while unfocused.
	void OnChar(std::uint32_t character);

	// keyCode is a Win32 VK_* code (see window.h's InputEvent). Handles VK_BACK,
	// VK_DELETE, VK_LEFT, VK_RIGHT, VK_HOME, VK_END, and Ctrl+A/C/E/X/V; anything else is
	// a no-op. No-op while unfocused.
	void OnKey(std::uint32_t keyCode);

	// Advances the blink clock every frame regardless of focus - Draw is what actually
	// decides whether the caret is visible/drawn at all, so an unfocused field's clock
	// just idles harmlessly rather than needing its own gating here.
	void Update(float deltaSeconds);

	// masked=true renders every character as '*' (a password field) while still cursor-
	// positioning/measuring correctly, since the substitution happens before TextWidth,
	// not after.
	void Draw(CDrawList &drawList, const CFont &font, float x, float y, float w, float h, Color textColor,
			  Color caretColor, bool masked) const;

	bool m_bFocused = false;

  private:
	// Removes the active selection (if any) from the buffer, leaving the cursor at its
	// start. Every edit that should "type over" a selection (a typed char, Backspace,
	// Delete, paste) calls this first rather than duplicating the memmove.
	void DeleteSelection();

	char m_szBuffer[kTextInputCapacity]{};
	std::uint32_t m_nLength = 0;
	std::uint32_t m_nCursor = 0;
	float m_flCaretBlinkSeconds = 0.0f;

	// The selection's other end, as a byte offset - -1 means no selection. The selection
	// spans [min(anchor, cursor), max(anchor, cursor)); cursor is always the "live" end,
	// the one further keyboard/mouse input moves.
	std::int32_t m_nSelectionAnchor = -1;
};
