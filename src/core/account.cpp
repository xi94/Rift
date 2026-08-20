#include "core/account.h"

#include <cassert>
#include <cstring>

namespace {
// Shared by CAccount::Init's three fields: zeroes the whole fixed buffer first (so a
// shorter new value doesn't leave a stale tail from whatever was written there
// before - Init is also how an existing account gets overwritten on edit, not just
// how a new one is created), then copies in, relying on the assert to catch an
// over-length value rather than silently truncating it.
void SetField(char *pDest, std::uint64_t capacity, CStringView value)
{
	assert(value.Length < capacity && "field value too long for its fixed buffer");
	std::memset(pDest, 0, capacity);
	std::memcpy(pDest, value.pData, value.Length);
}
} // namespace

void CAccount::Init(CStringView username, CStringView note, CStringView password)
{
	SetField(m_szUsername, sizeof(m_szUsername), username);
	SetField(m_szNote, sizeof(m_szNote), note);
	SetField(m_szPassword, sizeof(m_szPassword), password);
	m_uVisibleBannerMask = 0;
	std::memset(m_aReserved, 0, sizeof(m_aReserved));
}

CStringView CAccount::GetUsername() const
{
	return CStringView{m_szUsername, static_cast<std::uint64_t>(std::strlen(m_szUsername))};
}

CStringView CAccount::GetNote() const
{
	return CStringView{m_szNote, static_cast<std::uint64_t>(std::strlen(m_szNote))};
}

std::uint16_t CAccount::GetEffectiveVisibleMask(std::uint32_t ownBannerIndex) const
{
	if (m_uVisibleBannerMask != 0) {
		return m_uVisibleBannerMask;
	}
	return static_cast<std::uint16_t>(1u << ownBannerIndex);
}
