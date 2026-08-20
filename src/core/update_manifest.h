#pragma once

// The plain, parsed shape of update.json - the one file this project's GitHub Releases
// page is expected to publish alongside every release build (see core/updater.h's own file
// comment for the exact URL and the CI-side signing flow). Deliberately not read directly
// off the JSON in-place elsewhere: this is the single, fully-parsed record every other
// piece of the updater (version comparison, the download, the UI) reads from, the same
// "parse once into a plain struct, work from that" split core/riot_client.cpp's own
// RiotClientInstalls.json parsing already draws.
//
// Fixed-capacity char buffers, not std::string: a CUpdateManifest is written on
// CUpdater's worker thread and read from the render thread once GetStage() reports it's
// safe to (see updater.h's own header comment on that contract) - the same "no heap
// ownership crossing the thread boundary" shape CLoginAttempt::m_szMessage already uses.

#include <cstdint>

struct CUpdateManifest {
	char szVersion[32]{};		   // "1.3.0" - compared against RIFT_VERSION_STRING via SemVerParse
	char szMinUpgradeVersion[32]{}; // below this, in-app auto-update is refused - see updater.h
	char szUrl[512]{};			   // the release exe's own download URL (a GitHub Releases asset link)
	char szSha256Hex[65]{};		   // 64 lowercase hex chars + nul - the downloaded exe's expected SHA-256
	char szSignatureBase64[128]{};  // Ed25519 signature (core/update_signing_key.h) over the raw 32-byte
									// SHA-256 digest above, base64-encoded
	char szNotes[1024]{};		   // release notes, shown as-is in CUpdateOverlay
};

// Parses `json` (the raw bytes CUpdater downloaded from the manifest URL) into
// *pOutManifest. False on any parse error or missing/wrong-typed required field (szVersion/
// szUrl/szSha256Hex/szSignatureBase64 - szMinUpgradeVersion and szNotes both default to
// empty/"1.0.0"-equivalent-if-absent rather than being required, since neither is safety-
// critical the way a missing signature would be) - *pOutManifest is left in whatever
// partial state parsing reached, not reset, so a caller must only look at it after checking
// the return value.
bool ParseUpdateManifest(const char *pJson, std::uint64_t jsonLength, CUpdateManifest *pOutManifest);
