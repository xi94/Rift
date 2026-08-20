#include "core/crypto.h"

#include <sodium.h>

void CCrypto::RandomBytes(std::uint8_t *pOut, std::uint32_t length)
{
	randombytes_buf(pOut, length);
}

bool CCrypto::Argon2idDeriveKey(CStringView password, const std::uint8_t salt[kSaltSize], std::uint64_t opsLimit,
								std::size_t memLimit, std::uint8_t outKey[kKeySize])
{
	return crypto_pwhash(outKey, kKeySize, password.pData, static_cast<unsigned long long>(password.Length), salt,
						 opsLimit, memLimit, crypto_pwhash_ALG_ARGON2ID13) == 0;
}

std::uint64_t CCrypto::DefaultOpsLimit()
{
	return crypto_pwhash_OPSLIMIT_MODERATE;
}

std::size_t CCrypto::DefaultMemLimit()
{
	return crypto_pwhash_MEMLIMIT_MODERATE;
}

bool CCrypto::Encrypt(const std::uint8_t key[kKeySize], const std::uint8_t nonce[kNonceSize],
					  const std::uint8_t *pPlaintext, std::uint32_t length, std::uint8_t *pCiphertext,
					  std::uint8_t outTag[kTagSize])
{
	unsigned long long tagLength = 0;
	const int result = crypto_aead_xchacha20poly1305_ietf_encrypt_detached(
		pCiphertext, outTag, &tagLength, pPlaintext, length, nullptr, 0, nullptr, nonce, key);
	return result == 0;
}

bool CCrypto::Decrypt(const std::uint8_t key[kKeySize], const std::uint8_t nonce[kNonceSize],
					  const std::uint8_t *pCiphertext, std::uint32_t length, const std::uint8_t tag[kTagSize],
					  std::uint8_t *pOutPlaintext)
{
	// STATUS_AUTH_TAG_MISMATCH's exact analogue here: a non-zero return on a wrong key or
	// a corrupted/tampered ciphertext or tag - the whole reason this AEAD design replaced
	// unauthenticated/hand-verified encryption in the first place, see crypto.h.
	return crypto_aead_xchacha20poly1305_ietf_decrypt_detached(pOutPlaintext, nullptr, pCiphertext, length, tag,
															   nullptr, 0, nonce, key) == 0;
}
