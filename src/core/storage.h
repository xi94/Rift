#pragma once

// Local persistence for accounts and settings, split into two independent files -
// %LOCALAPPDATA%\Rift\settings.json (fonts, accent, animation prefs, and the master-
// password's own KEK parameters - salt/opsLimit/memLimit/wrapped DEK, none of which are
// themselves secret, see below) and accounts.vault (every banner's account list -
// usernames, notes, passwords, all of it). Everything this project knows about lives only
// in memory until CStorage round-trips it through these files.
//
// --- Format ---
// Plain, versioned JSON (nlohmann - see third_party), not a raw struct memcpy'd to disk.
// Every field is read individually with an explicit default (nlohmann's own
// json::value(key, default)), so adding, removing, or reordering a field never takes the
// rest of the file down with it the way a fixed-layout struct + "decrypted size must equal
// sizeof(T) exactly" check does - that exact failure mode (a struct size changing, an
// existing file silently treated as absent, CSettings falling back to defaults - which for
// m_bMasterPasswordEnabled means "no password was ever set," while accounts.vault's real
// ciphertext gets read as if it were plaintext) actually happened during development and
// cost a real user's saved account. This format is built specifically so that can't recur:
// a missing or extra key is a no-op for every *other* key, not a reason to discard the
// whole file.
//
// settings.json is never encrypted at the file level - genuinely nothing in it is secret
// on its own. The master-password fields it holds (salt, opsLimit, memLimit, wrapNonce,
// wrappedDek) are exactly what CMasterKey::Unlock needs to even begin deriving a key from a
// typed password in the first place (a real chicken-and-egg constraint: you can't decrypt
// the salt with the key you need the salt to derive), and none of them let anyone recover
// the DEK without the actual password - Argon2id's own memory-hardness plus the AEAD wrap
// is what protects them, not a second encryption layer on top. accounts.vault, in
// contrast, holds real user data (usernames, notes, passwords) and is encrypted as one
// whole blob - XChaCha20-Poly1305, keyed by the master password's own DEK (see
// core/crypto.h/core/master_key.h) - before ever touching disk; the outer JSON envelope
// around it is just nonce/tag/ciphertext, unreadable without that key.
//
// Deliberately not Windows DPAPI (CryptProtectData) anymore, and never was meant to be:
// DPAPI keys off the current Windows login, which ties a vault to one specific Windows
// account/machine and adds nothing a real password-derived key doesn't already cover - the
// master password is mandatory now (see ui/unlock_screen.h), so there is always a proper
// Argon2id-derived key available and no reason to layer an OS-specific mechanism underneath
// it.
//
// --- Why accounts.vault waits for unlock ---
// Unlike the old design (which loaded usernames/notes freely and only blanked passwords
// while locked), this format never even attempts to parse accounts.vault without the DEK -
// the whole blob is opaque ciphertext until then. main.cpp only calls LoadAccounts once
// CMasterKey is actually unlocked; banners simply show zero accounts until that happens,
// which is the literal, complete version of what "locked" is supposed to mean.
//
// --- Durability ---
// Both files are written via CAtomicFile::WriteAtomic (write-temp, flush to disk, rotate
// the previous generation to a `.bak` sibling, atomic rename into place) and read via a
// primary-then-`.bak` fallback - orthogonal to the format itself, and unchanged from
// before: a crash or power loss mid-write can never leave the real path in a partially-
// written state, and even a corrupted primary (bit rot, a bug, manual tampering) recovers
// to the last known-good generation instead of losing everything.

#include <cstddef>
#include <cstdint>

#include "core/master_key.h"
#include "ui/banner.h"
#include "ui/settings.h"

enum class EStorageLoadResult : std::uint8_t {
	STORAGE_LOAD_NO_FILE, // normal first run
	STORAGE_LOAD_FAILED,  // parse/decrypt failure on both the primary file and its .bak (wrong key, corrupted,
						  // tampered) - treat like NO_FILE
	STORAGE_LOAD_OK,	  // fully loaded, including account passwords
	STORAGE_LOAD_LOCKED,  // master password is enabled but CMasterKey isn't unlocked this session - accounts.vault
						  // was never even read; pBanners is left untouched (still whatever it already was, e.g.
						  // the compiled-in demo seed or a previous unlocked session's data)
};

class CStorage {
  public:
	// %LOCALAPPDATA%\Rift into pBuffer - the directory settings.json/accounts.vault
	// actually live in (see this file's own header comment), created (and the one-time
	// f4-rockstar migration run) if it doesn't already exist, same as every other storage
	// path this class resolves. Exists as its own public entry point for a caller that
	// wants the directory itself rather than a file inside it - e.g. a user-facing "open my
	// data folder" action, or core/crash_handler.cpp's own crash-dump folder (a sibling of
	// this one, not this one itself, but resolved the same way).
	static bool GetDataDirectory(char *pBuffer, std::size_t bufferSize);

	// Serializes the given banners' account lists to JSON, encrypts the whole thing with
	// masterKey.m_aDek (XChaCha20-Poly1305, a fresh random nonce every call), and
	// atomically writes the resulting envelope to accounts.vault. Requires
	// masterKey.m_bEnabled - masterPasswordEnabled true with a locked (disabled) key is a
	// no-op returning false, since there is nothing trustworthy in memory to encrypt (see
	// this file's own header comment on why accounts are never loaded at all while locked,
	// so there would be nothing real here to save either).
	static bool SaveAccounts(CBanner *pBanners, std::uint32_t bannerCount, bool masterPasswordEnabled,
							 const CMasterKey &masterKey);

	// Snapshots settings/zoomStop/selectedBanner as plain JSON and atomically writes them
	// to settings.json. Independent of SaveAccounts - a plain settings change never
	// touches accounts.vault.
	static bool SaveSettings(const CSettings &settings, std::int32_t carouselZoomStop,
							 std::int32_t carouselSelectedBanner);

	// Requires masterKey.m_bEnabled (returns STORAGE_LOAD_LOCKED immediately, touching
	// neither the file nor pBanners, otherwise). Reads + decrypts accounts.vault (falling
	// back to its .bak on failure) and, for each persisted banner whose title matches a
	// banner already in pBanners by name, replaces that banner's account list; unmatched
	// banners (a new game added since the file was written) are left as-is. A wrong key
	// fails the AEAD tag check outright (STORAGE_LOAD_FAILED, pBanners untouched) rather
	// than silently producing garbage.
	static EStorageLoadResult LoadAccounts(CBanner *pBanners, std::uint32_t bannerCount, bool masterPasswordEnabled,
										   const CMasterKey &masterKey);

	// Reads settings.json (falling back to its .bak on failure) into settings/
	// outCarouselZoomStop/outCarouselSelectedBanner - never encrypted, so no key is
	// needed. A key genuinely absent from the file (an older save, before some field
	// existed) just leaves that one field at whatever it already held going in - see this
	// file's own header comment for why that per-field tolerance is the entire point of
	// this format.
	static EStorageLoadResult LoadSettings(CSettings &settings, std::int32_t &outCarouselZoomStop,
										   std::int32_t &outCarouselSelectedBanner);
};
