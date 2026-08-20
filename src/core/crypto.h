#pragma once

// libsodium-backed cryptographic primitives (see third_party/libsodium) backing the
// master-password feature: Argon2id key derivation from a user-typed password, and
// XChaCha20-Poly1305 authenticated encryption (ciphertext is always the same size as
// plaintext; the 16-byte auth tag is a separate output - see storage.h's own
// CStorage::SaveAccounts, which encrypts the whole serialized account list as one blob,
// not per-field).
//
// This replaced an earlier BCrypt(Windows CNG)-based PBKDF2-SHA256 + AES-256-GCM design.
// Two real reasons, not just "newer is better":
//   - PBKDF2-SHA256 is cheap to brute-force on a GPU/ASIC compared to Argon2id, which is
//     deliberately memory-hard (see DefaultOpsLimit/DefaultMemLimit) - a stolen vault
//     file's real protection is however expensive guessing the master password is.
//   - AES-GCM's 96-bit nonce is genuinely unsafe to generate randomly at any real save
//     frequency (a collision becomes an XOR-of-two-plaintexts break, not just theoretical
//     risk) - this project's own prior nonce scheme derived it deterministically from
//     (banner title, account index) specifically to avoid that, but that means every
//     re-save of the same account under the same master key reused the exact same nonce,
//     which is exactly as unsafe against two captured on-disk snapshots (a backup, a
//     shadow copy, ...) of the same vault. XChaCha20-Poly1305's 192-bit nonce has no such
//     problem - see Encrypt's own comment.
//
// This is the only encryption layer accounts.vault gets - deliberately not also wrapped in
// Windows DPAPI (CryptProtectData), which an earlier version of this project did. DPAPI
// ties a vault to one specific Windows login/machine and adds nothing a real password-
// derived key doesn't already cover, now that the master password is mandatory (see
// ui/unlock_screen.h) rather than optional - there's no longer a "no master password set"
// case DPAPI would have been the only protection for.

#include <cstddef>
#include <cstdint>

#include "core/string_view.h"

class CCrypto {
  public:
	static constexpr std::uint32_t kKeySize = 32;	// XChaCha20-Poly1305 key size (also this project's DEK size)
	static constexpr std::uint32_t kSaltSize = 16; // crypto_pwhash's Argon2id salt size
	static constexpr std::uint32_t kNonceSize = 24; // crypto_aead_xchacha20poly1305_ietf's nonce size
	static constexpr std::uint32_t kTagSize = 16;	// crypto_aead_xchacha20poly1305_ietf's Poly1305 tag size

	// libsodium's own CSPRNG (randombytes_buf) - the source for a new master-password
	// salt, a fresh DEK, and every AEAD nonce this project generates. Unlike the
	// BCryptGenRandom call this replaced, libsodium's own RNG has no failure mode a caller
	// can meaningfully recover from (it aborts the process itself if the OS entropy source
	// is unavailable), so this doesn't return a status the way the old one did.
	static void RandomBytes(std::uint8_t *pOut, std::uint32_t length);

	// Argon2id (crypto_pwhash) - deliberately memory-hard, unlike the PBKDF2-SHA256 this
	// replaced, so brute-forcing a stolen vault's master password costs real money/time
	// per guess even on custom hardware. opsLimit/memLimit are parameters, not constants,
	// specifically so they can be persisted alongside the salt (see storage.h's own
	// CSettings fields) and re-supplied to every later call - if DefaultOpsLimit/
	// DefaultMemLimit's own values ever change (a future tuning pass, a new libsodium
	// preset), an already-persisted vault must keep using whatever it was actually created
	// with, not whatever "default" means today.
	static bool Argon2idDeriveKey(CStringView password, const std::uint8_t salt[kSaltSize], std::uint64_t opsLimit,
								  std::size_t memLimit, std::uint8_t outKey[kKeySize]);

	// libsodium's own "moderate" Argon2id preset - roughly half a second and a few hundred
	// MiB on typical current hardware, libsodium's own tuning rather than a number picked
	// by hand here. Only used when setting a brand-new master password (see CMasterKey::
	// Set); an existing one keeps whatever opsLimit/memLimit it already persisted, via
	// Argon2idDeriveKey's own parameters.
	static std::uint64_t DefaultOpsLimit();
	static std::size_t DefaultMemLimit();

	// XChaCha20-Poly1305 (crypto_aead_xchacha20poly1305_ietf) - the one AEAD every
	// encrypted field in this project uses. nonce must never repeat under the same key,
	// but at 192 bits it's safe to just RandomBytes a fresh one for every single call
	// (encrypting the same account twice, across two different saves, gets two different
	// nonces) - see this file's own header comment for why that specifically wasn't true
	// of the 96-bit AES-GCM nonce this replaced. Ciphertext is the same length as
	// plaintext; the tag is a separate kTagSize-byte parameter, not appended to the
	// ciphertext buffer.
	static bool Encrypt(const std::uint8_t key[kKeySize], const std::uint8_t nonce[kNonceSize],
					   const std::uint8_t *pPlaintext, std::uint32_t length, std::uint8_t *pCiphertext,
					   std::uint8_t outTag[kTagSize]);

	// Returns false - leaving pOutPlaintext's contents unspecified - if tag doesn't match
	// what the ciphertext actually decrypts to (wrong key, or the ciphertext/tag was
	// corrupted or tampered with).
	static bool Decrypt(const std::uint8_t key[kKeySize], const std::uint8_t nonce[kNonceSize],
					   const std::uint8_t *pCiphertext, std::uint32_t length, const std::uint8_t tag[kTagSize],
					   std::uint8_t *pOutPlaintext);
};
