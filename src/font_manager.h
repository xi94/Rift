#pragma once

// Loads the default UI font once at startup, Segoe UI straight from the system Fonts
// directory. Two baked sizes:
//
//   - Body: anything the user reads or types as *content* - account usernames and notes,
//     a text field's typed value (font name, master password, an edit form's username/
//     note/password), dialog titles, button labels. If it's the thing someone came to
//     this screen to look at or edit, it's body.
//   - Secondary: static chrome around that content - row labels ("Username", "Password"),
//     one-line descriptions/hints, footer text. If it's scaffolding a field's label
//     rather than the value inside it, it's secondary - never a value the user actually
//     typed.
//
// A genuinely smaller face reads better than shrinking the body face's quads would, so
// secondary is its own independently-sized, independently-persisted value rather than a
// fixed ratio of body.
//
// Both are DPI-aware by construction (see gfx/font.h) - every consumer of GetBody/
// GetSecondary keeps reasoning in logical pixels regardless of the monitor's scale
// factor, so this class is the only place dpiScale needs to be threaded in.

#include "core/string_view.h"
#include "gfx/font.h"

class IRenderer;

class CFontManager {
  public:
	// The very first bake, at startup, before CSettings has been loaded from disk - uses
	// this class's own hardcoded nominal defaults, which ApplyBody then immediately
	// overrides with the persisted/default Settings values.
	bool Load(IRenderer &renderer, float dpiScale);

	// Re-bakes both fonts from a font file name (e.g. "segoeui.ttf", resolved against the
	// system Fonts directory) - bodyPixelSize/secondaryPixelSize are independent nominal
	// Settings values (see font_manager.cpp's kFontSizeDisplayScale for what "nominal"
	// means), not a fixed ratio of each other. Leaves both untouched if either bake
	// fails (bad filename, corrupt/missing file, degenerate size): bakes into temporaries
	// first and only swaps them in once both succeed, so a bad value typed into the
	// settings field - or a transient failure re-baking after a live DPI change - never
	// leaves the UI without a working font. Returns whether it succeeded.
	bool ApplyBody(IRenderer &renderer, CStringView fontFileName, float bodyPixelSize, float secondaryPixelSize,
				   float dpiScale);

	const CFont &GetBody() const
	{
		return m_body;
	}
	const CFont &GetSecondary() const
	{
		return m_secondary;
	}

  private:
	CFont m_body;
	CFont m_secondary;
};
