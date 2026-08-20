#pragma once

// The single source of truth for Rift's own version string - declared once (as
// core/version_defs.h's own macros, which this just wraps as a real C++ constant) rather
// than hand-typed and inevitably drifting at every place that wants to show it: currently
// CSettingsMenu's footer strip (ui/settings_menu.cpp) and the Windows file-version
// resource Explorer's Properties > Details tab reads (app.rc).

#include "core/version_defs.h"

constexpr const char *kAppVersion = RIFT_VERSION_STRING;

// True only in a build where NDEBUG isn't defined - CMake's own default Debug flags are
// the one configuration that leaves it undefined (see CMakeLists.txt); Release,
// RelWithDebInfo, and MinSizeRel all define it, which is also what disables assert()
// project-wide in those configs. The footer version strip appends "[dev]" only when this
// is true, so a real Release build never shows a stray debug marker.
#ifdef NDEBUG
constexpr bool kIsDebugBuild = false;
#else
constexpr bool kIsDebugBuild = true;
#endif
