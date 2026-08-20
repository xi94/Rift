#include "core/master_key.h"

#include <cstring>

#include <sodium.h>

CMasterKey::~CMasterKey()
{
	Clear();
}

void CMasterKey::Init()
{
	m_bEnabled = false;
	sodium_memzero(m_aDek, sizeof(m_aDek));
	std::memset(m_aSalt, 0, sizeof(m_aSalt));
	m_opsLimit = 0;
	m_memLimit = 0;
	std::memset(m_aWrapNonce, 0, sizeof(m_aWrapNonce));
	std::memset(m_aWrappedDek, 0, sizeof(m_aWrappedDek));
}

bool CMasterKey::Set(CStringView password)
{
	std::uint8_t salt[CCrypto::kSaltSize];
	CCrypto::RandomBytes(salt, sizeof(salt));

	std::uint8_t dek[CCrypto::kKeySize];
	CCrypto::RandomBytes(dek, sizeof(dek));

	const std::uint64_t opsLimit = CCrypto::DefaultOpsLimit();
	const std::size_t memLimit = CCrypto::DefaultMemLimit();

	std::uint8_t kek[CCrypto::kKeySize];
	if (!CCrypto::Argon2idDeriveKey(password, salt, opsLimit, memLimit, kek)) {
		return false;
	}

	std::uint8_t wrapNonce[CCrypto::kNonceSize];
	CCrypto::RandomBytes(wrapNonce, sizeof(wrapNonce));

	std::uint8_t wrappedDek[CCrypto::kKeySize + CCrypto::kTagSize];
	const bool wrapped =
		CCrypto::Encrypt(kek, wrapNonce, dek, sizeof(dek), wrappedDek, wrappedDek + CCrypto::kKeySize);
	sodium_memzero(kek, sizeof(kek));
	if (!wrapped) {
		return false;
	}

	std::memcpy(m_aDek, dek, sizeof(m_aDek));
	sodium_memzero(dek, sizeof(dek));
	std::memcpy(m_aSalt, salt, sizeof(m_aSalt));
	m_opsLimit = opsLimit;
	m_memLimit = memLimit;
	std::memcpy(m_aWrapNonce, wrapNonce, sizeof(m_aWrapNonce));
	std::memcpy(m_aWrappedDek, wrappedDek, sizeof(m_aWrappedDek));
	m_bEnabled = true;
	return true;
}

bool CMasterKey::Unlock(CStringView password, const std::uint8_t salt[CCrypto::kSaltSize], std::uint64_t opsLimit,
						std::size_t memLimit, const std::uint8_t wrapNonce[CCrypto::kNonceSize],
						const std::uint8_t wrappedDek[CCrypto::kKeySize + CCrypto::kTagSize])
{
	std::uint8_t kek[CCrypto::kKeySize];
	if (!CCrypto::Argon2idDeriveKey(password, salt, opsLimit, memLimit, kek)) {
		return false;
	}

	std::uint8_t dek[CCrypto::kKeySize];
	const bool unwrapped = CCrypto::Decrypt(kek, wrapNonce, wrappedDek, CCrypto::kKeySize,
											wrappedDek + CCrypto::kKeySize, dek);
	sodium_memzero(kek, sizeof(kek));
	if (!unwrapped) {
		return false;
	}

	std::memcpy(m_aDek, dek, sizeof(m_aDek));
	sodium_memzero(dek, sizeof(dek));
	std::memcpy(m_aSalt, salt, sizeof(m_aSalt));
	m_opsLimit = opsLimit;
	m_memLimit = memLimit;
	std::memcpy(m_aWrapNonce, wrapNonce, sizeof(m_aWrapNonce));
	std::memcpy(m_aWrappedDek, wrappedDek, sizeof(m_aWrappedDek));
	m_bEnabled = true;
	return true;
}

void CMasterKey::Clear()
{
	m_bEnabled = false;
	sodium_memzero(m_aDek, sizeof(m_aDek));
}
