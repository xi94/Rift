#pragma once

// Crash/power-loss-safe file writes: WriteAtomic writes to a `<path>.tmp` sibling, flushes
// it to disk (FlushFileBuffers, not just the OS page cache - the whole point is surviving
// a power loss, not just a process crash), rotates whatever was already at `path` to
// `<path>.bak` (replacing any older backup), then atomically renames the temp file into
// `path`'s place. The real path is therefore never observable in a partially-written
// state, and the previous good generation survives even a crash that lands mid-write -
// CStorage's Load path retries against BackupPathFor(path) whenever its own decrypt/parse
// step rejects the primary file, which is the actual "not prone to data loss" guarantee:
// worst case, a crash loses the single most recent save, never everything.
//
// Pure file I/O - no knowledge of what the bytes mean (encryption, struct layout,
// versioning all stay the caller's concern; see CStorage).

#include <cstddef>
#include <cstdint>

class CAtomicFile {
  public:
	// Returns false, leaving whatever was already on disk (both `path` and its `.bak`)
	// untouched, if any step before the final rename fails - a failed save must never
	// destroy what's already durably there.
	static bool WriteAtomic(const char *pPath, const void *pData, std::size_t length);

	// Reads `path` fully into a heap buffer (std::malloc'd, sized exactly to the file -
	// see CStorage's own comment on why large structs are heap-, not stack-, allocated
	// here) via *ppOutData/*pOutLength. Returns false (freeing anything it allocated) if
	// the file doesn't exist or can't be read start to finish. Caller owns the buffer and
	// must std::free it; validating/decrypting the bytes is the caller's job.
	static bool ReadFile(const char *pPath, std::uint8_t **ppOutData, std::size_t *pOutLength);

	// Builds `path + ".bak"` into pOutBuffer. Exposed (not just an internal helper) so
	// CStorage's Load path can compute the same fallback path WriteAtomic rotates into.
	static bool BackupPathFor(const char *pPath, char *pOutBuffer, std::size_t bufferSize);
};
