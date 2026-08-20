#pragma once

// The Ed25519 public key core/updater.h verifies every downloaded update against, before
// it's ever allowed to replace this running exe - see updater.h's own file comment for why
// HTTPS alone (which this project's WinHTTP calls already get for free) isn't enough. The
// matching secret key lives nowhere in this repo; it's the CI-side signing key that signs
// each release's SHA-256 digest, generated once via libsodium's own crypto_sign_keypair and
// handed to whoever configures the release pipeline out of band (see this project's own
// README/release docs for that process, once one exists). Losing the secret key means
// generating a new pair and shipping one more release with this constant updated to match -
// it does not need to be secret to hold up its own end (this file, and the public key in
// it, is meant to be public - "public key," not "secret key").
//
// Regenerate with a small throwaway program: sodium_init(), crypto_sign_keypair(pk, sk),
// print both as hex/byte arrays. Never commit the secret key anywhere, including here.

#include <array>
#include <cstdint>

namespace kestrel::update {
constexpr std::array<std::uint8_t, 32> kEd25519PublicKey{
	0xD0, 0x82, 0xF5, 0xE2, 0x12, 0xC9, 0x37, 0x3F, 0x4E, 0xEE, 0x56, 0x8D, 0xA3, 0x32, 0x8A, 0x64,
	0x3E, 0x73, 0x86, 0xBA, 0x0F, 0xE6, 0x13, 0xC3, 0x7F, 0x70, 0x23, 0x76, 0x63, 0x73, 0xF5, 0xE3,
};
} // namespace kestrel::update
