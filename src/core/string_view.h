#pragma once

// A non-owning, non-null-terminated string view - the same role f4's own Str plays.
// Kept as a bare struct (no C prefix, see STYLE.md) since it's pure data with no
// behavior of its own beyond the free functions below - matching the project's own rule
// that plain data doesn't get promoted to a "class" just for the sake of it.
//
// Owned strings in this fork are either a plain fixed char[] embedded directly in a POD
// record (an account's username, a persisted banner title - the same fixed-buffer
// treatment f4 itself uses for exactly this reason: a record type shouldn't own a heap
// allocation per instance), or std::string for genuinely one-shot, dynamically-sized
// values built up during init/settings handling. There's no CFixedString wrapper type -
// see FORK_WITH_CLASSES.md's memory-strategy section for why this fork doesn't carry
// f4's arena-backed Str_Buffer forward: once general heap allocation is back on the
// table for one-shot values, a hand-rolled fixed-capacity string type adds a second way
// to do the same thing std::string already does correctly.

#include <cstdint>
#include <cstring>

struct CStringView {
	const char *pData;
	std::uint64_t Length;
};

static_assert(sizeof(CStringView) == 16);

// Wraps a null-terminated C string (a literal, a CRT/Win32 API result) as a CStringView -
// the one place strlen gets called to bridge into this project's own non-null-terminated
// convention.
inline CStringView StringViewFromCString(const char *cstr)
{
	return CStringView{cstr, static_cast<std::uint64_t>(std::strlen(cstr))};
}

// Byte-for-byte comparison - a CStringView carries no encoding assumption beyond "bytes",
// so this is exactly memcmp gated by a length check, not a locale-aware comparison.
inline bool StringViewEqual(CStringView a, CStringView b)
{
	return a.Length == b.Length && std::memcmp(a.pData, b.pData, a.Length) == 0;
}

// Copies into a fixed, null-terminated destination buffer, truncating (not asserting) on
// overflow - used wherever a CStringView needs to become a real C string for a Win32/CRT
// call, or land in a fixed-size POD record field.
inline void StringViewCopyToFixed(char *pDest, std::uint64_t destCapacity, CStringView src)
{
	const std::uint64_t length = src.Length < destCapacity - 1 ? src.Length : destCapacity - 1;
	std::memcpy(pDest, src.pData, length);
	pDest[length] = '\0';
}
