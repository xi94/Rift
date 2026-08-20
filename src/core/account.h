#pragma once

// The base record every account list in this project is built from - one type for the
// modal's account rows and the edit/add form, so there is exactly one layout to reason
// about rather than a view type copied out of this one every frame.

#include <cstdint>

#include "core/string_view.h"

class CAccount {
  public:
	// Copies username/note/password into their fixed buffers - each must fit with room
	// for the null terminator (asserts, doesn't truncate: a field that doesn't fit is a
	// caller bug). Leaves m_uVisibleBannerMask at 0 (see its own comment below).
	void Init(CStringView username, CStringView note, CStringView password);

	// Views into the null-terminated fixed buffers as the non-null-terminated
	// CStringView the rest of this project's UI code deals in - the one seam between
	// CAccount's own null-terminated convention and everywhere else.
	CStringView GetUsername() const;
	CStringView GetNote() const;

	// m_uVisibleBannerMask if it's ever been explicitly set, otherwise a mask with only
	// ownBannerIndex's bit set - the "no explicit choice made yet" default. Every reader
	// of the visibility mask should call this, not read m_uVisibleBannerMask directly, so
	// the zero-means-default convention lives in exactly one place.
	std::uint16_t GetEffectiveVisibleMask(std::uint32_t ownBannerIndex) const;

	// Fields are null-terminated within their fixed buffer, unlike CStringView elsewhere
	// in this project, which is deliberately never null-terminated - these buffers are
	// small, fixed, and rarely scanned, so strlen's cost is irrelevant and null-
	// termination avoids a separate length field per string. Public (not behind
	// accessors) for the same reason f4's own Account struct kept these public: the
	// persistence layer (CStorage) needs to memcpy this whole object as one flat wire
	// record, which a private-field class with only getters can't do without a second
	// serialization type - see CStorage's own comments for why that duplication wasn't
	// worth it there either.
	char m_szUsername[64];
	char m_szNote[32];
	char m_szPassword[128];

	// Bit i set = visible under carousel banner i - which games' account lists this
	// account shows up in, beyond the one it was created under. 0 (Init's default, and
	// every pre-existing persisted account) means "no explicit choice made yet" and is
	// interpreted as "visible only in its own banner" by GetEffectiveVisibleMask above.
	std::uint16_t m_uVisibleBannerMask;

	std::uint8_t m_aReserved[30]; // pad 226 -> 256 (exactly 4 cache lines)
};

static_assert(sizeof(CAccount) == 256);
