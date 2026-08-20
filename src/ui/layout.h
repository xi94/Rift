#pragma once

// The "rectangle method" layout primitives: every widget reasons about a single Rect area
// it was handed, rather than four loose x/y/w/h floats each call site has to keep in sync
// by convention. These free functions are the vocabulary widgets compose layouts from -
// split a rect into strips (the same technique File Pilot's own chrome uses to carve a
// window into title/toolbar/content/status bands), inset it for padding/borders, center a
// fixed-size box inside it, or clamp it against min/max size constraints so a widget can
// make an explicit shrink/hide decision instead of overflowing silently.
//
// Plain stateless math on core/types.h's Rect - free PascalCase functions rather than a
// class, per STYLE.md's own carve-out for math helpers ("there's no reason a math helper
// should look stylistically different from a method just because it isn't attached to a
// class").

#include "core/types.h"

// Shrinks rect by `amount` on all four sides (negative grows it).
Rect RectInset(Rect rect, float amount);
Rect RectInset(Rect rect, float left, float top, float right, float bottom);

// Splits a strip of `amount` off the named edge of `rect` and returns it, shrinking
// `rect` in place to whatever remains - the same "carve off a strip, keep going with the
// remainder" pattern as File Pilot's own layout code. Clamps `amount` to `rect`'s extent
// on that axis so a too-small area degrades to "the strip eats everything, the remainder
// is empty" rather than producing a negative-size rect.
Rect RectSplitTop(Rect &rect, float amount);
Rect RectSplitBottom(Rect &rect, float amount);
Rect RectSplitLeft(Rect &rect, float amount);
Rect RectSplitRight(Rect &rect, float amount);

// A `w`x`h` box centered inside `outer` (used for e.g. centering a fixed-size popup panel
// inside the window rect).
Rect RectCenterIn(Rect outer, float w, float h);

// A widget's size preferences: never smaller than min, never larger than max. 0 means "no
// preference" for MaxW/MaxH (unbounded).
struct LayoutConstraints {
	float MinW;
	float MinH;
	float MaxW;
	float MaxH;
};

// Clamps rect's size to `constraints`, keeping it centered on its original center point.
Rect RectClamp(Rect rect, LayoutConstraints constraints);
