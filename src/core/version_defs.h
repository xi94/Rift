#pragma once

// Pure preprocessor macros only, nothing else - both core/version.h's C++ constexpr
// declarations and app.rc's VERSIONINFO block (the Windows file-version resource
// Explorer's Properties > Details tab reads) need this same version number, and rc.exe's
// preprocessor pass doesn't understand C++ syntax like `constexpr` - only #define text
// substitution. This file is the one thing both sides can #include without choking on
// the other's syntax, so the number itself only ever gets typed in one place.

#define RIFT_VERSION_MAJOR 1
#define RIFT_VERSION_MINOR 0
#define RIFT_VERSION_PATCH 0

#define RIFT_STRINGIFY_IMPL(x) #x
#define RIFT_STRINGIFY(x) RIFT_STRINGIFY_IMPL(x)

// Adjacent string-literal concatenation (standard C/C++ preprocessing behavior rc.exe's
// own preprocessor shares) - expands to "0" "." "1" "." "0", which both a C++ compiler
// and rc.exe's VALUE statements read as the single string "0.1.0".
#define RIFT_VERSION_STRING \
	RIFT_STRINGIFY(RIFT_VERSION_MAJOR) "." RIFT_STRINGIFY(RIFT_VERSION_MINOR) "." RIFT_STRINGIFY(RIFT_VERSION_PATCH)
