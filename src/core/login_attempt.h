#pragma once

// One login attempt's cross-thread state. No mutex: a single-producer (the worker
// thread) / single-consumer (the render thread) enum needs nothing more than an atomic -
// the worker's final store (a terminal stage) is the one synchronization point the render
// thread's load pairs with. szMessage is written by the worker before that same store
// (see WorkerMain in the .cpp) and only ever read after GetStage() has reported a terminal
// stage, so the release/acquire pairing on Stage is what makes reading it afterward safe
// without its own separate synchronization.
//
// Drives core/riot_client.h's CRiotClient to actually launch the Riot Client and log into
// it - not a simulation. A fresh CUiAutomation is constructed on the worker thread itself
// (see ui_automation.h's own file comment on why one can't be shared across threads).
//
// Cancel() is a real interrupt, not a UI-only "stop showing this": it sets m_bCancelRequested,
// which the worker's own CRiotClient calls (each taking it as an optional parameter) check on
// every poll iteration, so a Cancel click actually stops whichever wait is currently in
// flight within about one poll interval, rather than the worker continuing to launch/kill
// processes and type credentials invisibly in the background after the UI has already moved
// on. LOGIN_STAGE_CANCELLED (not IDLE, and not ERROR) is what the worker lands on when it
// notices - a real terminal stage (Update() joins it like any other), but deliberately
// distinct from ERROR so main.cpp's login-flash post-process effect (which only reacts to
// SUCCESS/ERROR) doesn't flash red for a deliberate cancellation.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>

#include "core/riot_client.h"
#include "core/string_view.h"

enum class ELoginStage : std::uint8_t {
	LOGIN_STAGE_IDLE,
	LOGIN_STAGE_WAITING_FOR_PROCESS,
	LOGIN_STAGE_CONNECTING,
	LOGIN_STAGE_AUTHENTICATING,
	LOGIN_STAGE_LAUNCHING,
	LOGIN_STAGE_SUCCESS,
	LOGIN_STAGE_ERROR,
	LOGIN_STAGE_CANCELLED,
};

// Everything one attempt owns, heap-allocated and co-owned (std::shared_ptr) by CLoginAttempt
// and its worker - deliberately not plain members of CLoginAttempt the worker points into.
//
// That co-ownership is what lets CLoginAttempt give up on a worker it can neither join nor
// interrupt (a UI Automation call into an unresponsive client blocks forever - see
// ui_automation.h; see Update()'s watchdog). An abandoned worker keeps writing into the block it
// already co-owns and frees it whenever it unwinds, while the next attempt gets a brand new one -
// so a zombie can never overwrite a live attempt's stage/message, nor share its CRiotClient.
struct SLoginAttemptState {
	static constexpr std::uint32_t kMaxMessageLength = 160;

	std::atomic<ELoginStage> Stage{ELoginStage::LOGIN_STAGE_IDLE};
	std::atomic<bool> bCancelRequested{false};

	// The worker's very last act, after its terminal Stage store - what CLoginAttempt::Update
	// joins on, since a terminal Stage still leaves it unwinding its CUiAutomation.
	std::atomic<bool> bWorkerFinished{false};

	// Sized to match CAccount's own fields (core/account.h) - copied in CLoginAttempt::Start,
	// read only by the worker thread afterward.
	char szUsername[64]{};
	char szPassword[128]{};
	char szGameTitle[32]{};

	// See this file's own comment on when this is safe to read.
	char szMessage[kMaxMessageLength]{};

	// Per-attempt, never reused across them - see this struct's own comment.
	CRiotClient RiotClient;
};

class CLoginAttempt {
  public:
	// If a worker is still running (see IsActive) when this object is destroyed, joins it
	// first rather than letting std::thread's own destructor run against a still-joinable
	// thread - that terminates the whole process (a real, observed crash: closing the app, or
	// this object otherwise being destroyed, while a Cancel hadn't yet actually been noticed
	// by the worker). Requests cancellation first so this doesn't sit here for however long
	// the worker would otherwise still be doing real work.
	~CLoginAttempt();

	// Zeroes to the at-rest state (Idle, no worker) - safe to call before the first real
	// attempt ever starts.
	void Init();

	// Starts a background worker that drives a fresh CRiotClient through a real login: refuse if
	// a game is currently in progress (see CRiotClient::IsGameInProgress), otherwise kill every
	// Riot Client/League process this project knows how to relaunch cleanly and launch fresh
	// (see CRiotClient::KillAllClientProcesses/Launch's own comments for why always killing,
	// not reusing an already-open client, is the right default), wait for its window, submit
	// username/password, and watch for its own inline error indicator. gameTitle (a carousel
	// banner's own Title - see ui/banner.h) is resolved to a --launch-product value via
	// CRiotClient::LaunchProductForBannerTitle, so the client auto-launches into the right
	// game once logged in. A no-op (returns without starting anything) if a previous attempt
	// on this same object is still active and hasn't reached a terminal stage yet (see
	// IsActive) - a caller (see account_modal.cpp's own gating on IsActive) is expected to
	// keep the Login control disabled/inert until that's no longer true, rather than relying
	// on this to somehow queue or block for it: blocking here would mean joining a worker that
	// might currently be stuck deep inside a single slow UI Automation call with no
	// cooperative-cancellation point of its own - a real, observed freeze (the whole app stops
	// pumping messages until that call returns), not a hypothetical. Once the previous attempt
	// HAS reached a terminal stage, joining it here is effectively instantaneous, same as
	// Update()'s own join. Copies username/password into this object's own fixed buffers
	// rather than holding the caller's CStringViews across the thread boundary - the account
	// they point into isn't guaranteed to outlive the attempt (a user could delete it
	// mid-login).
	void Start(CStringView username, CStringView password, CStringView gameTitle);

	// Requests that the in-flight attempt (if any) stop as soon as it next checks - see this
	// class's own file comment. A no-op if nothing is active or the attempt has already
	// reached a terminal stage. Does not itself join the worker; Update() still does that once
	// the worker actually finishes - IsActive keeps reporting true in the meantime, since the
	// worker hasn't stopped yet even though it's been asked to.
	//
	// Also shortens Update()'s watchdog to a short grace period: a healthy worker acknowledges
	// this within a poll interval, so one that doesn't is wedged somewhere it can't be
	// interrupted from - and the user has already said they're done waiting on it. See
	// AbandonWorker.
	void Cancel();

	// True from Start() until Update() disposes of the worker - by joining a finished one, or by
	// abandoning one that blew its watchdog (see AbandonWorker). NOT the same as "still doing
	// real work" the instant Cancel() is called, since the worker only notices that
	// asynchronously. A caller driving the Login/Cancel control (see account_modal.cpp) should
	// gate starting a new attempt on this being false, rather than calling Start() again
	// immediately after Cancel() - see Start's own comment for why that matters.
	bool IsActive() const
	{
		return m_bActive;
	}

	// The atomic load every renderer/UI read of an in-flight attempt's progress goes
	// through - never read the state block's Stage directly, so this stays the one place the
	// single-producer/single-consumer contract described above is actually enforced.
	ELoginStage GetStage() const;

	// Valid once GetStage() reports a terminal stage - see this file's own comment for why
	// that ordering is what makes this safe to read. Empty on Success/Cancelled unless
	// something notable happened anyway; always non-empty on Error (falls back to a generic
	// message if the worker couldn't get a more specific one from the Riot Client itself).
	CStringView GetTerminalMessage() const;

	static bool IsTerminalStage(ELoginStage stage);

	// Call once per frame regardless of whether anything is currently showing this attempt's
	// progress - it is what both retires a finished worker and enforces the watchdog, so an
	// attempt whose progress nothing happens to be drawing still can't get stuck active forever.
	// Joins only a worker that has already signalled it finished, so the join is always
	// effectively instantaneous; a worker past its deadline is abandoned rather than waited on
	// (see AbandonWorker). Never blocks the caller waiting for the worker to do its work.
	void Update();

  private:
	// Gives up on a worker that has blown m_watchdogDeadline: it's blocked inside a call with no
	// cancellation point and no timeout of its own, so no amount of further waiting brings it
	// back. Detaches the OS thread, hands it sole ownership of the state block it's still writing
	// into, and swaps in a fresh block already carrying the terminal result - so GetStage() reads
	// terminal immediately, IsActive() goes false, and the very next Start() is free to run.
	void AbandonWorker();

	std::shared_ptr<SLoginAttemptState> m_pState;
	std::thread m_worker;
	bool m_bActive = false; // a worker thread exists and hasn't been joined/abandoned yet

	// When the current attempt stops being given the benefit of the doubt - see AbandonWorker
	// and Update(). Only meaningful while m_bActive.
	std::chrono::steady_clock::time_point m_watchdogDeadline{};
};
