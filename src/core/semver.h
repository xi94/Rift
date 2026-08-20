#pragma once

// Parses "X.Y.Z" version strings into a comparable (major, minor, patch) tuple for the
// updater (core/updater.h) to compare the running build against a fetched manifest's own
// version field. A plain lexicographic string compare gets this wrong - "1.10.0" < "1.9.0"
// character-by-character, since '1' < '9' - comparing the parsed integers instead gets it
// right regardless of digit count.

#include <cstdint>

#include "core/string_view.h"

struct SemVer {
	std::uint32_t Major = 0;
	std::uint32_t Minor = 0;
	std::uint32_t Patch = 0;
};

// False if `text` doesn't start with at least one digit (genuinely unparseable - an empty
// or garbage manifest field). A partial version like "1.2" still succeeds (Patch stays 0).
// Anything from the first non-digit-non-dot character onward (a "-beta.1" pre-release
// suffix, a "+build5" metadata tag) is ignored - this project's manifest schema has no use
// for either, so treating "1.3.0-rc1" as plain 1.3.0 is the right amount of parsing, not a
// missing feature.
bool SemVerParse(CStringView text, SemVer &outVersion);

inline bool operator==(const SemVer &a, const SemVer &b)
{
	return a.Major == b.Major && a.Minor == b.Minor && a.Patch == b.Patch;
}
inline bool operator<(const SemVer &a, const SemVer &b)
{
	if (a.Major != b.Major) {
		return a.Major < b.Major;
	}
	if (a.Minor != b.Minor) {
		return a.Minor < b.Minor;
	}
	return a.Patch < b.Patch;
}
inline bool operator!=(const SemVer &a, const SemVer &b)
{
	return !(a == b);
}
inline bool operator>(const SemVer &a, const SemVer &b)
{
	return b < a;
}
inline bool operator<=(const SemVer &a, const SemVer &b)
{
	return !(b < a);
}
inline bool operator>=(const SemVer &a, const SemVer &b)
{
	return !(a < b);
}
