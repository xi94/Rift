#pragma once

// Session state for the optional master-password feature: an additional XChaCha20-
// Poly1305 encryption layer CStorage applies to each account's password field before the
// whole file goes through DPAPI, so a locally-running process under the same Windows user
// - which DPAPI alone doesn't protect against - still can't read stored passwords without
// this password too.
//
// KEK/DEK, not "encrypt everything directly with a password-derived key": the typed
// password only ever derives a Key-Encryption-Key (via Argon2id - see CCrypto), which
// does nothing but unwrap a separate, randomly-generated Data-Encryption-Key - m_aDek is
// what actually encrypts every account's password field. Changing the master password
// then only means re-deriving the KEK and re-wrapping the same DEK (a few dozen bytes),
// not re-encrypting every already-stored account; it also leaves room for a future second
// unlock path (a recovery key, a different password) to just be another wrapped copy of
// the same DEK, without touching the encrypted accounts at all.
//
// No separate "verifier" hash either, unlike the PBKDF2/SHA-256 design this replaced:
// Unlock's own AEAD tag check on the wrapped DEK already fails outright on a wrong
// password (or corrupted/tampered persisted data) - a dedicated verifier alongside that
// would just be a second thing that could, in principle, disagree with the first.
//
// m_aDek lives only in memory for the current session - never the raw typed password, and
// never written to disk itself; only the wrapped/encrypted copy is, via CSettings (see
// storage.h's own CStorage::SaveSettings, which writes it into settings.json's own
// "master_password" object), which is safe to persist as-is (nobody recovers the DEK from
// it without the password).

#include <cstddef>
#include <cstdint>

#include "core/crypto.h"
#include "core/string_view.h"

class CMasterKey {
  public:
	// Zeroes every field, including m_aDek - see Clear's own comment.
	~CMasterKey();

	// Zeroes to the at-rest, disabled state - the default before any password has been
	// set or unlocked this session.
	void Init();

	// Sets a brand-new master password: generates a fresh salt and a fresh random DEK,
	// derives the KEK (CCrypto::Argon2idDeriveKey, using CCrypto::DefaultOpsLimit/
	// MemLimit for a brand-new password) and wraps the DEK with it
	// (CCrypto::Encrypt). Returns false (leaving this object untouched) if key derivation
	// or wrapping fails. On success, every field below is valid and the caller (see
	// CUnlockScreen::AttemptSetup) is responsible for persisting them into CSettings.
	bool Set(CStringView password);

	// Re-derives the KEK from a typed password against previously-persisted salt/
	// opsLimit/memLimit/wrapNonce/wrappedDek, then tries to unwrap wrappedDek - the AEAD
	// tag check inside CCrypto::Decrypt is what actually rejects a wrong password (see
	// this file's own header comment on why there's no separate verifier). Returns false
	// (leaving this object untouched) either way; a caller can't distinguish "wrong
	// password" from "corrupted persisted data" from this alone, same as the design it
	// replaced.
	bool Unlock(CStringView password, const std::uint8_t salt[CCrypto::kSaltSize], std::uint64_t opsLimit,
			   std::size_t memLimit, const std::uint8_t wrapNonce[CCrypto::kNonceSize],
			   const std::uint8_t wrappedDek[CCrypto::kKeySize + CCrypto::kTagSize]);

	// Turns the feature off for this session: zeroes m_aDek (via sodium_memzero, not a
	// plain memset a compiler could optimize away as a dead store - this is the one field
	// here that's an actual secret). CStorage stops encrypting account passwords on the
	// next save once CSettings::m_bMasterPasswordEnabled is also cleared by the caller.
	void Clear();

	bool m_bEnabled = false; // a password has been set or unlocked this session; the fields below are valid
	std::uint8_t m_aDek[CCrypto::kKeySize]{};

	// Not secret (nobody recovers m_aDek from these without the password) - persisted
	// into CSettings as-is by the caller after Set(), and re-supplied to Unlock() on a
	// later session from whatever CStorage loaded.
	std::uint8_t m_aSalt[CCrypto::kSaltSize]{};
	std::uint64_t m_opsLimit = 0;
	std::size_t m_memLimit = 0;
	std::uint8_t m_aWrapNonce[CCrypto::kNonceSize]{};
	std::uint8_t m_aWrappedDek[CCrypto::kKeySize + CCrypto::kTagSize]{};
};
