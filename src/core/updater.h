#pragma once

// The in-app updater: checks a manifest published alongside this project's GitHub
// Releases, downloads and cryptographically verifies a newer build if one exists, and
// swaps it into place for the app to relaunch into - all off the render thread, the same
// worker-thread/atomic-status shape core/login_attempt.h already established (see that
// file's own header comment for the reasoning this mirrors: single-producer/single-
// consumer atomics, no mutex, Update() joins a finished worker exactly once).
//
// --- The manifest ---
// GET https://github.com/xi94/Rift/releases/latest/download/update.json - GitHub's own
// stable "latest release" redirect (github.com -> a signed objects.githubusercontent.com
// URL), not the rate-limited api.github.com REST API, so this needs no auth token and hits
// no rate limit. See core/update_manifest.h for the parsed schema (version, url, sha256,
// signature, min_upgrade_version, notes) - this project's own CI is expected to publish
// that file as a release asset on every tagged release, alongside the exe itself.
//
// --- Trust ---
// HTTPS proves the bytes came from GitHub unmodified; it does NOT prove nobody with repo
// write access (or a compromised CI runner) swapped the release asset for something else,
// and it does nothing against a machine with a corporate MITM root CA installed. So this
// checks two independent things before ever touching the running exe's file, in order:
//   1. SHA-256(downloaded bytes) == manifest.sha256 (integrity - catches a truncated or
//      corrupted download)
//   2. Ed25519-verify(SHA-256 digest, manifest.signature, core/update_signing_key.h's own
//      embedded public key) (authenticity - catches anything not signed by whoever holds
//      the matching CI secret key, including a tampered manifest.sha256 itself, since the
//      signature covers the digest, not the file bytes directly)
// Either failing aborts the update with UPDATE_STAGE_ERROR; nothing downloaded is ever
// executed or moved into place until both pass.
//
// --- Applying ---
// See ApplyDownloadedExe's own comment in the .cpp for the full reasoning; short version: a
// single MoveFileExW(newExe, selfPath, MOVEFILE_REPLACE_EXISTING) is one atomic NTFS rename
// - there is no window where selfPath doesn't exist - so that's tried first. Only if that's
// somehow refused (a locked file, an AV real-time scanner holding a handle) does this fall
// back to the riskier two-step self-rename-then-replace RunStartupRecoveryAndMaybeExit is
// there to recover from.
//
// --- Off the render thread ---
// CheckForUpdateAsync/StartDownloadAsync each spawn (or reuse, once joined) the one worker
// thread this owns; GetStage()/GetBytesDownloaded()/GetTotalBytes()/GetBytesPerSecond() are
// the only things safe to read from the render thread while a worker is active - each is a
// plain atomic the worker only ever writes and the render thread only ever reads. Manifest
// fields, and the error message, are written by the worker before its final atomic m_stage
// store and only ever read after GetStage() has reported the corresponding stage - the same
// release/acquire-via-atomic-store contract CLoginAttempt::m_szMessage already relies on.

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "core/update_manifest.h"

enum class EUpdateStage : std::uint8_t {
	UPDATE_STAGE_IDLE,
	UPDATE_STAGE_CHECKING,
	UPDATE_STAGE_UP_TO_DATE,
	UPDATE_STAGE_AVAILABLE,				// a newer version exists and this build may auto-update to it
	UPDATE_STAGE_MANUAL_UPGRADE_REQUIRED, // a newer version exists, but manifest.min_upgrade_version says this
										  // build is too old to auto-update - CUpdateOverlay points the user at
										  // a manual download instead of offering one
	UPDATE_STAGE_CHECK_FAILED,			 // network/parse error while checking - deliberately distinct from
										  // UPDATE_STAGE_ERROR below so a background check failing (no user
										  // action taken yet) doesn't surface the same way a failed in-flight
										  // download/install does
	UPDATE_STAGE_DOWNLOADING,
	UPDATE_STAGE_VERIFYING,
	UPDATE_STAGE_INSTALLING,
	UPDATE_STAGE_READY_TO_RELAUNCH, // verified and swapped into place - see ConsumeReadyToRelaunch
	UPDATE_STAGE_ERROR,			 // download/verify/install failed after the user opted in - see
									// GetErrorMessage
	UPDATE_STAGE_CANCELLED,
};

class CUpdater {
  public:
	// Joins a still-running worker first, same reasoning as CLoginAttempt's own destructor.
	~CUpdater();

	void Init();

	// Fetches the manifest and compares it against currentVersion (RIFT_VERSION_STRING - a
	// plain C string, safe to copy into the worker since it's a compile-time constant, not
	// something that can be mutated/freed from under the thread). No-op if a worker is
	// already active (see IsActive) - matches CLoginAttempt::Start's own "caller keeps
	// polling IsActive, doesn't rely on this to queue" contract.
	void CheckForUpdateAsync(const char *currentVersion);

	// Downloads, verifies, and installs the update the most recent successful check found -
	// only meaningful once GetStage() reports UPDATE_STAGE_AVAILABLE (a no-op otherwise, and
	// a no-op if a worker is already active). Reads GetManifest() internally, so that must
	// still be valid (it is, until the next CheckForUpdateAsync call) when this is called.
	void StartDownloadAsync();

	// Sets the cooperative-cancellation flag the download worker's WinHttpReadData loop
	// polls between chunks - same shape as CLoginAttempt::Cancel. A no-op past
	// UPDATE_STAGE_DOWNLOADING (verify/install are both sub-second local operations with no
	// meaningful cancellation point of their own, and CUpdateOverlay stops offering Cancel
	// once the bar leaves the download phase for exactly that reason).
	void RequestCancel();

	bool IsActive() const
	{
		return m_bActive;
	}

	EUpdateStage GetStage() const;

	// Valid once GetStage() has reported anything past UPDATE_STAGE_CHECKING for the check
	// that populated it (AVAILABLE/MANUAL_UPGRADE_REQUIRED/UP_TO_DATE) - see this file's own
	// header comment on the write-then-atomic-store contract.
	const CUpdateManifest &GetManifest() const
	{
		return m_manifest;
	}

	std::uint64_t GetBytesDownloaded() const
	{
		return m_bytesDownloaded.load(std::memory_order_relaxed);
	}
	std::uint64_t GetTotalBytes() const
	{
		return m_totalBytes.load(std::memory_order_relaxed);
	}
	double GetBytesPerSecond() const
	{
		return m_bytesPerSecond.load(std::memory_order_relaxed);
	}

	// Valid once GetStage() reports UPDATE_STAGE_ERROR or UPDATE_STAGE_CHECK_FAILED.
	const char *GetErrorMessage() const
	{
		return m_szErrorMessage;
	}

	// Call once per frame regardless of activity - joins a finished worker (instantaneous
	// once GetStage() is terminal, same as CLoginAttempt::Update), and latches
	// ConsumeReadyToRelaunch's flag the frame it first notices UPDATE_STAGE_READY_TO_RELAUNCH.
	void Update();

	// True at most once, the frame the downloaded update was verified and swapped into
	// this exe's own path - cleared on read. The owner (main.cpp) should treat this like
	// CUnlockScreen's ConsumeSetupSucceeded: save whatever state matters, spawn a fresh
	// process at GetModuleFileNameW's own path (now the new build), and cleanly exit this
	// one - see updater.cpp's ApplyDownloadedExe for why the path is guaranteed to already
	// hold the new build's bytes by the time this reports true.
	bool ConsumeReadyToRelaunch();

	// Startup-only recovery for an update interrupted between the two-step fallback's own
	// rename calls (see this file's own "Applying" section) - call exactly once, before any
	// window/renderer/asset/font init, as the very first thing main() does. A true return
	// means this process instance should exit immediately (return 0 from main without doing
	// anything else): either it just repaired and relaunched a working copy of itself and a
	// fresh process is now taking over, or there was nothing for this particular instance to
	// do and a normal launch should have happened moments ago some other way. False means
	// "nothing to recover, proceed with a completely normal startup" - the overwhelmingly
	// common case.
	static bool RunStartupRecoveryAndMaybeExit();

  private:
	void WorkerCheckForUpdate(std::string currentVersion);
	void WorkerDownloadAndInstall(CUpdateManifest manifest);

	std::atomic<EUpdateStage> m_stage{EUpdateStage::UPDATE_STAGE_IDLE};
	std::atomic<bool> m_bCancelRequested{false};
	std::thread m_worker;
	bool m_bActive = false;

	// The currently-running worker's own "I'm completely done" signal - deliberately
	// separate from m_stage itself. m_stage can legitimately hold a terminal-looking value
	// (UPDATE_STAGE_AVAILABLE) left over from a *previous* worker (the check) while a *new*
	// one (the download StartDownloadAsync just started) is still running - Update() joining
	// on "does m_stage currently look terminal" alone would then block the render thread on
	// that brand new worker's own join, defeating the entire point. Reset to false (on the
	// render thread, only while m_bActive is still false) right before a worker starts;
	// stored true (on the worker thread) as that worker's very last action, after its final
	// m_stage store.
	std::atomic<bool> m_bWorkerFinished{false};

	std::atomic<std::uint64_t> m_bytesDownloaded{0};
	std::atomic<std::uint64_t> m_totalBytes{0};
	std::atomic<double> m_bytesPerSecond{0.0};

	CUpdateManifest m_manifest{};
	char m_szErrorMessage[256]{};

	bool m_bReadyToRelaunchLatched = false; // set inside Update(), not the worker - render-thread-only, no atomic needed
};
