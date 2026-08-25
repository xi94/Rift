#include "core/login_attempt.h"

#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include <Windows.h>

#include "core/debug_log.h"
#include "core/thread_util.h"
#include "core/ui_automation.h"

namespace {
// Every diagnostic line out of this file shares one category tag - see core/debug_log.h.
constexpr const char *kLogCategory = "login";

const char *StageName(ELoginStage stage)
{
	switch (stage) {
		case ELoginStage::LOGIN_STAGE_IDLE:
			return "IDLE";
		case ELoginStage::LOGIN_STAGE_WAITING_FOR_PROCESS:
			return "WAITING_FOR_PROCESS";
		case ELoginStage::LOGIN_STAGE_CONNECTING:
			return "CONNECTING";
		case ELoginStage::LOGIN_STAGE_AUTHENTICATING:
			return "AUTHENTICATING";
		case ELoginStage::LOGIN_STAGE_LAUNCHING:
			return "LAUNCHING";
		case ELoginStage::LOGIN_STAGE_SUCCESS:
			return "SUCCESS";
		case ELoginStage::LOGIN_STAGE_ERROR:
			return "ERROR";
		case ELoginStage::LOGIN_STAGE_CANCELLED:
			return "CANCELLED";
	}
	return "?";
}

// Waiting for the freshly launched Riot Client's own window is deliberately unbounded (see
// CRiotClient::kWaitForeverMs), unlike every other step here. A cold Electron start (the Riot
// Client's own multi-process launch, confirmed slow-ish against a real install) is the single
// biggest source of latency in this whole flow, and how slow it actually is depends on the
// machine, its disk, and whether the client decides to patch itself first - so any number
// picked here is really a guess at someone else's hardware, and getting it wrong means telling
// a user their login failed while the client they asked for is still visibly starting up in
// front of them. Everything past this point is a step that either lands quickly or is genuinely
// broken, so those keep their timeouts; this one is bounded by the user's own Cancel instead,
// which is live for the whole wait (the modal's Login button reads "Cancel" while an attempt is
// in flight - see ui/account_modal.cpp) and interrupts it within a poll interval.
constexpr std::uint32_t kFormTimeoutMs = 10000;
constexpr std::uint32_t kResultTimeoutMs = 6000;

// What BringToForeground/SetKeyboardFocus get - named rather than repeated inline at their call
// sites below.
constexpr std::uint32_t kForegroundTimeoutMs = 5000;
constexpr std::uint32_t kFocusTimeoutMs = 5000;

// How long to wait for the "Play" button once a login has already succeeded (no error
// tooltip shown) - see CRiotClient::WaitForPlayButtonAndClick's own comment. Generous but not
// load-bearing: the attempt is still reported as a success even if this times out, since the
// login itself already went through by the time this runs - it's just missing the manual
// press Riot's own auto-launch used to handle.
constexpr std::uint32_t kPlayButtonTimeoutMs = 8000;

// Riot's own wording for a real backend error (see CRiotClient::WaitForLoginError's own
// comment) isn't shown verbatim - two exact messages have been confirmed via Inspect against
// a real install: "Your login credentials don't match an account in our system." for wrong
// credentials, and "Sorry, we're having trouble signing you in right now. Please try again
// later." for a backend-side problem (surfaced by account-testing heavy enough to trip
// Riot's own rate limiting) - the second one isn't any more informative than this project's
// own fallback, and there's no guarantee its exact wording stays stable across Riot Client
// versions/locales anyway. The one case worth calling out specifically is wrong credentials -
// see LooksLikeInvalidCredentials - since that's actionable (try a different password) in a
// way "the server had a problem" isn't.
constexpr const char *kInvalidCredentialsMessage = "Invalid username or password.";
constexpr const char *kServerErrorMessage = "Something went wrong - Riot's servers might be overloaded. Try again in a moment.";
constexpr const char *kGameInProgressMessage = "A game is already running - close it before switching accounts.";
constexpr const char *kNoRiotClientMessage = "Couldn't find the Riot Client - is it installed?";
constexpr const char *kLaunchFailedMessage = "Couldn't launch the Riot Client.";
constexpr const char *kWindowTimeoutMessage = "The Riot Client didn't respond in time.";
constexpr const char *kFormTimeoutMessage = "Couldn't find the Riot Client's login form.";
constexpr const char *kUnresponsiveClientMessage = "The Riot Client stopped responding - try again.";
// Distinct from kServerErrorMessage, which this path used to borrow: a CUiAutomation that
// won't start is a problem on this machine (Windows' own accessibility stack), and telling
// someone Riot's servers are overloaded sends them off looking in entirely the wrong place.
constexpr const char *kAutomationFailedMessage = "Couldn't start Windows UI Automation - try again.";

// How long a cancelled-but-unacknowledged worker gets before it's abandoned - see Cancel(). A
// healthy one notices within a 100ms poll interval.
constexpr auto kCancelGracePeriod = std::chrono::milliseconds(5000);

// Every stage change in this file goes through here rather than storing Stage directly, so
// the log is guaranteed to show the same sequence the UI actually sees - a stage that gets
// stored on a path nobody thought to log is exactly the gap that makes a rare hang hard to
// read afterwards. The memory ordering is release for every stage, not just the terminal one:
// the intermediate stores were relaxed only because nothing but the progress UI reads them,
// and paying for release on a handful of stores per attempt buys the simpler invariant.
void StoreStage(SLoginAttemptState &state, ELoginStage stage)
{
	DebugLog::Write(kLogCategory, "stage -> %s%s", StageName(stage),
					state.szMessage[0] != '\0' ? " (with a message)" : "");
	state.Stage.store(stage, std::memory_order_release);
}

void SetMessage(char *pBuffer, std::uint32_t bufferSize, const char *pMessage)
{
	std::strncpy(pBuffer, pMessage, bufferSize - 1);
	pBuffer[bufferSize - 1] = '\0';
}

// True if wide is the Riot Client's own "these credentials don't match an account" message -
// confirmed via Inspect against a real install ("Your login credentials don't match an
// account in our system."). A plain substring check on "credentials" is enough: among every
// message WaitForLoginError has actually been observed to surface (this one, and Riot's own
// generic backend-error text), it's the only one that contains that word at all.
bool LooksLikeInvalidCredentials(const std::wstring &wide)
{
	return wide.find(L"credentials") != std::wstring::npos;
}

bool IsCancelled(const SLoginAttemptState &state)
{
	return state.bCancelRequested.load(std::memory_order_relaxed);
}

enum class ESubmitResult {
	FORM_NOT_FOUND, // SubmitLogin itself never found the username/password fields
	ERROR_SHOWN,	 // the Riot Client's own inline error tooltip appeared - outErrorMessage is its text
	NO_ERROR_SHOWN,  // nothing appeared within the timeout - the "assume success" inference
};

// Submits the login form and waits for a result - shared by the initial attempt and the one
// retry WorkerMain does for a non-credentials error (see its own comment on why that retry
// exists and why it's bounded to exactly once).
//
// pIgnorePreviousMessage is only set for that retry call - see CRiotClient::WaitForLoginError's
// own comment for why a resubmit needs it: without it, the retry can read the FIRST attempt's
// still-on-screen error tooltip as if it were the second attempt's own result, before the Riot
// Client has actually replaced it - a real, observed bug, not a hypothetical: a genuine invalid-
// credentials response on the retry was getting silently reported as the first attempt's
// generic server error instead.
ESubmitResult SubmitAndWaitForResult(SLoginAttemptState &state, CUiAutomation &uiAutomation,
									 std::wstring &outErrorMessage,
									 const std::wstring *pIgnorePreviousMessage = nullptr)
{
	outErrorMessage.clear();
	if (!state.RiotClient.SubmitLogin(uiAutomation, StringViewFromCString(state.szUsername),
									  StringViewFromCString(state.szPassword), kFormTimeoutMs,
									  &state.bCancelRequested)) {
		return ESubmitResult::FORM_NOT_FOUND;
	}
	if (state.RiotClient.WaitForLoginError(uiAutomation, outErrorMessage, kResultTimeoutMs, &state.bCancelRequested,
										   pIgnorePreviousMessage)) {
		return ESubmitResult::ERROR_SHOWN;
	}
	return ESubmitResult::NO_ERROR_SHOWN;
}

// Stores LOGIN_STAGE_CANCELLED if the worker's own cancellation flag is set, otherwise
// pFailureMessage as an ERROR - shared by every step below that just came back from a
// CRiotClient poll returning false, since that return means either "the user cancelled" or
// "this step genuinely failed/timed out" and the worker can't tell which without re-checking
// the same flag itself (see core/riot_client.h's own file comment on why CRiotClient's own
// methods don't return a tri-state for this).
void StoreCancelledOrError(SLoginAttemptState &state, const char *pFailureMessage)
{
	if (IsCancelled(state)) {
		StoreStage(state, ELoginStage::LOGIN_STAGE_CANCELLED);
		return;
	}
	SetMessage(state.szMessage, SLoginAttemptState::kMaxMessageLength, pFailureMessage);
	StoreStage(state, ELoginStage::LOGIN_STAGE_ERROR);
}

// The background thread body Start() launches: drives pRiotClient through a real login via
// CUiAutomation, storing pStage at each step so the render thread's progress UI can follow
// along (see account_modal.cpp's DrawLoginProgress). A fresh CUiAutomation, not one shared
// across attempts - see ui_automation.h's own file comment for why that's required, not just
// tidy, given this runs on a brand new OS thread every call.
void RunLoginAttempt(SLoginAttemptState &state)
{
	state.szMessage[0] = '\0';
	StoreStage(state, ELoginStage::LOGIN_STAGE_WAITING_FOR_PROCESS);

	// Refuses to touch anything while an actual match is in progress - see
	// CRiotClient::IsGameInProgress's own comment. Checked before the kill-and-relaunch
	// below, not after, since there's nothing to undo if this bails out here.
	if (CRiotClient::IsGameInProgress()) {
		SetMessage(state.szMessage, SLoginAttemptState::kMaxMessageLength, kGameInProgressMessage);
		StoreStage(state, ELoginStage::LOGIN_STAGE_ERROR);
		return;
	}
	if (IsCancelled(state)) {
		StoreStage(state, ELoginStage::LOGIN_STAGE_CANCELLED);
		return;
	}

	// Always kill, then always launch fresh - never "reuse if already open". An already-
	// running Riot Client can be sitting logged in on its own homepage/library rather than
	// the login screen, which has no username/password fields to find at all - see
	// core/riot_client.h's own file comment; this was a real, confirmed failure mode, not a
	// hypothetical.
	CRiotClient::KillAllClientProcesses();

	if (!state.RiotClient.ResolveExecutablePath()) {
		SetMessage(state.szMessage, SLoginAttemptState::kMaxMessageLength, kNoRiotClientMessage);
		StoreStage(state, ELoginStage::LOGIN_STAGE_ERROR);
		return;
	}
	const CStringView launchProduct = CRiotClient::LaunchProductForBannerTitle(StringViewFromCString(state.szGameTitle));
	if (!state.RiotClient.Launch(launchProduct)) {
		SetMessage(state.szMessage, SLoginAttemptState::kMaxMessageLength, kLaunchFailedMessage);
		StoreStage(state, ELoginStage::LOGIN_STAGE_ERROR);
		return;
	}

	// Not just "a window exists" - CRiotClient::WaitForWindow now also holds out for its owning
	// thread to actually be pumping messages, which is what the whole rest of this function
	// depends on and what a cold Electron start takes its time getting to. No deadline on this
	// one wait (see kWaitForeverMs above): false here therefore only ever means the user
	// cancelled, which StoreCancelledOrError already lands as CANCELLED - kWindowTimeoutMessage
	// stays as the honest fallback for the case that can no longer happen on its own.
	if (!state.RiotClient.WaitForWindow(CRiotClient::kWaitForeverMs, &state.bCancelRequested)) {
		StoreCancelledOrError(state, kWindowTimeoutMessage);
		return;
	}
	if (IsCancelled(state)) {
		StoreStage(state, ELoginStage::LOGIN_STAGE_CANCELLED);
		return;
	}

	StoreStage(state, ELoginStage::LOGIN_STAGE_CONNECTING);
	state.RiotClient.BringToForeground(kForegroundTimeoutMs, &state.bCancelRequested);
	state.RiotClient.SetKeyboardFocus(kFocusTimeoutMs, &state.bCancelRequested);
	if (IsCancelled(state)) {
		StoreStage(state, ELoginStage::LOGIN_STAGE_CANCELLED);
		return;
	}

	CUiAutomation uiAutomation;
	if (!uiAutomation.Init()) {
		DebugLog::Write(kLogCategory, "CUiAutomation::Init failed - see the uia lines just above for the HRESULT");
		SetMessage(state.szMessage, SLoginAttemptState::kMaxMessageLength, kAutomationFailedMessage);
		StoreStage(state, ELoginStage::LOGIN_STAGE_ERROR);
		return;
	}

	StoreStage(state, ELoginStage::LOGIN_STAGE_AUTHENTICATING);
	std::wstring errorMessage;
	ESubmitResult result = SubmitAndWaitForResult(state, uiAutomation, errorMessage);
	if (result == ESubmitResult::FORM_NOT_FOUND) {
		StoreCancelledOrError(state, kFormTimeoutMessage);
		return;
	}

	// A generic "we're having trouble signing you in" error - never wrong credentials,
	// which resubmitting the exact same password obviously can't fix - has been confirmed
	// to reliably clear up on one immediate retry against the exact same, still-open
	// client: no kill/relaunch, just bring it back to the foreground and resubmit the same
	// form once more. This is the one situation a retry without a fresh kill-and-relaunch
	// is actually correct - every other failure path above already went through
	// KillAllClientProcesses once for this attempt and doesn't get a second chance within
	// it (see this file's own header comment on why a timeout means "tell the user", not
	// "wait a little longer").
	if (result == ESubmitResult::ERROR_SHOWN && !LooksLikeInvalidCredentials(errorMessage)) {
		if (IsCancelled(state)) {
			StoreStage(state, ELoginStage::LOGIN_STAGE_CANCELLED);
			return;
		}

		StoreStage(state, ELoginStage::LOGIN_STAGE_CONNECTING);
		state.RiotClient.BringToForeground(kForegroundTimeoutMs, &state.bCancelRequested);
		state.RiotClient.SetKeyboardFocus(kFocusTimeoutMs, &state.bCancelRequested);
		if (IsCancelled(state)) {
			StoreStage(state, ELoginStage::LOGIN_STAGE_CANCELLED);
			return;
		}

		StoreStage(state, ELoginStage::LOGIN_STAGE_AUTHENTICATING);
		// See SubmitAndWaitForResult's own comment on pIgnorePreviousMessage - without it this
		// resubmit could read the first attempt's own still-on-screen error right back as if it
		// were this attempt's result.
		const std::wstring previousErrorMessage = errorMessage;
		result = SubmitAndWaitForResult(state, uiAutomation, errorMessage, &previousErrorMessage);
		if (result == ESubmitResult::FORM_NOT_FOUND) {
			StoreCancelledOrError(state, kFormTimeoutMessage);
			return;
		}
	}

	if (result == ESubmitResult::ERROR_SHOWN) {
		SetMessage(state.szMessage, SLoginAttemptState::kMaxMessageLength,
				  LooksLikeInvalidCredentials(errorMessage) ? kInvalidCredentialsMessage : kServerErrorMessage);
		StoreStage(state, ELoginStage::LOGIN_STAGE_ERROR);
		return;
	}
	if (IsCancelled(state)) {
		StoreStage(state, ELoginStage::LOGIN_STAGE_CANCELLED);
		return;
	}

	// No error surfaced within the timeout - the best signal available right now that the
	// attempt went through (see CRiotClient::WaitForLoginError's own caveat: this is an
	// inference, not a confirmed success - no element for an actual success state has been
	// captured yet).
	//
	// Riot's own --launch-product/--launch-patchline auto-launch (see CRiotClient::Launch's
	// own comment) stopped actually starting the game as of a recent Riot Client update - it
	// now only navigates to the right product's page without pressing Play, so this presses it
	// manually. Not itself load-bearing for whether this attempt is reported as a success: the
	// login already succeeded by this point (no error tooltip appeared) regardless of whether
	// the Play button happens to be found/clicked - just skip it if it isn't.
	StoreStage(state, ELoginStage::LOGIN_STAGE_LAUNCHING);
	state.RiotClient.WaitForPlayButtonAndClick(uiAutomation, kPlayButtonTimeoutMs, &state.bCancelRequested);
	if (IsCancelled(state)) {
		StoreStage(state, ELoginStage::LOGIN_STAGE_CANCELLED);
		return;
	}

	StoreStage(state, ELoginStage::LOGIN_STAGE_SUCCESS);
}

// Co-owns the state block it drives (see SLoginAttemptState) so abandoning this thread mid-call
// can't leave it writing into freed memory.
void WorkerMain(std::shared_ptr<SLoginAttemptState> pState)
{
	DebugLog::Write(kLogCategory, "worker started");
	RunLoginAttempt(*pState);
	// Set here, not at each of RunLoginAttempt's own returns, so an early-out added there later
	// can't forget it.
	pState->bWorkerFinished.store(true, std::memory_order_release);

	// This runs AFTER the terminal stage store above, and the gap between the two is real: the
	// worker still has to unwind its CUiAutomation (a cross-process COM release plus a
	// CoUninitialize). A log that ends at "worker finished" with no "worker exiting" after it
	// is that teardown having hung - a distinct failure from the login itself hanging, and one
	// that used to be invisible.
	DebugLog::Write(kLogCategory, "worker finished (stage %s), unwinding",
					StageName(pState->Stage.load(std::memory_order_acquire)));
}
} // namespace

CLoginAttempt::~CLoginAttempt()
{
	if (m_pState != nullptr) {
		m_pState->bCancelRequested.store(true, std::memory_order_relaxed);
	}
	// Bounded, not a bare join(): the worker may be stuck forever inside a single UI
	// Automation COM call against an unresponsive Riot Client, a real and unavoidable
	// risk this class's own cancellation flag can't interrupt mid-call (see
	// ui_automation.h's own file comment) - a plain join() here used to hang this
	// process's entire shutdown whenever that happened, with no exception ever thrown
	// for the crash handler to catch. See core/thread_util.h's own file comment. Detaching is
	// safe precisely because a worker co-owns its state block rather than pointing into this
	// object - see SLoginAttemptState's own comment.
	if (m_worker.joinable()) {
		DebugLog::Write(kLogCategory, "shutting down with a worker still running - cancelling and joining");
	}
	const DebugLog::CScope scope(kLogCategory, "shutdown join of the login worker");
	JoinWithTimeoutOrDetach(m_worker, std::chrono::milliseconds(3000));
}

void CLoginAttempt::Init()
{
	// Deliberately doesn't touch m_worker: dropping this object's reference to the current
	// state block is the whole reset, and the block itself stays alive under the worker's own
	// reference for as long as it needs it.
	m_pState.reset();
	m_bActive = false;
}

bool CLoginAttempt::IsTerminalStage(ELoginStage stage)
{
	return stage == ELoginStage::LOGIN_STAGE_SUCCESS || stage == ELoginStage::LOGIN_STAGE_ERROR ||
		   stage == ELoginStage::LOGIN_STAGE_CANCELLED;
}

void CLoginAttempt::Start(CStringView username, CStringView password, CStringView gameTitle)
{
	// See this method's own header comment - a previous attempt that hasn't reached a terminal
	// stage yet might currently be stuck inside a single slow UI Automation call with no
	// cancellation point of its own; joining it here would block the calling (render) thread for
	// however long that call takes, a real observed freeze. Refuse instead of blocking - the
	// caller is expected to gate on IsActive() so this is only ever reached once the previous
	// attempt is actually done, and Update()'s watchdog guarantees that eventually happens even
	// when the worker itself never finishes.
	if (m_bActive && !IsTerminalStage(GetStage())) {
		DebugLog::Write(kLogCategory, "Start refused - the previous attempt is still active (stage %s)",
						StageName(GetStage()));
		return;
	}
	// Terminal (or never started) - this is the instantaneous case, and the bounded form is here
	// only so a worker that stored its terminal stage but is still unwinding can't stall a click.
	JoinWithTimeoutOrDetach(m_worker, std::chrono::milliseconds(50));
	m_bActive = false;

	// A brand new block per attempt - see SLoginAttemptState. Also what makes the credentials safe
	// across the thread boundary: it outlives this call whatever happens to the account they came
	// from (a user could delete it mid-login).
	auto pState = std::make_shared<SLoginAttemptState>();
	StringViewCopyToFixed(pState->szUsername, sizeof(pState->szUsername), username);
	StringViewCopyToFixed(pState->szPassword, sizeof(pState->szPassword), password);
	StringViewCopyToFixed(pState->szGameTitle, sizeof(pState->szGameTitle), gameTitle);
	pState->Stage.store(ELoginStage::LOGIN_STAGE_WAITING_FOR_PROCESS, std::memory_order_relaxed);

	m_pState = pState;
	m_worker = std::thread(WorkerMain, std::move(pState));
	m_bActive = true;
	DebugLog::Write(kLogCategory, "attempt started for game \"%s\"", m_pState->szGameTitle);
}

void CLoginAttempt::Cancel()
{
	if (!m_bActive || m_pState == nullptr || IsTerminalStage(GetStage())) {
		return;
	}
	DebugLog::Write(kLogCategory, "cancel requested at stage %s", StageName(GetStage()));
	m_pState->bCancelRequested.store(true, std::memory_order_relaxed);

	// A worker that can still check the flag acts on it within a poll interval; one that can't is
	// wedged in a call nothing can interrupt, and the user has already said they're done waiting.
	m_cancelDeadline = std::chrono::steady_clock::now() + kCancelGracePeriod;
}

ELoginStage CLoginAttempt::GetStage() const
{
	if (m_pState == nullptr) {
		return ELoginStage::LOGIN_STAGE_IDLE;
	}
	return m_pState->Stage.load(std::memory_order_acquire);
}

CStringView CLoginAttempt::GetTerminalMessage() const
{
	if (m_pState == nullptr) {
		return StringViewFromCString("");
	}
	return StringViewFromCString(m_pState->szMessage);
}

void CLoginAttempt::AbandonWorker()
{
	// Read before the store below erases the distinction - a cancel the worker never got to
	// acknowledge should still land as CANCELLED, so the login-flash effect doesn't flash red at
	// a user who deliberately backed out.
	const bool userCancelled = m_pState != nullptr && m_pState->bCancelRequested.load(std::memory_order_relaxed);

	if (m_pState != nullptr) {
		// It may come back to life long after this - the first thing it checks should tell it to
		// stop, not to carry on typing credentials into a client nobody is watching.
		m_pState->bCancelRequested.store(true, std::memory_order_relaxed);
	}
	if (m_worker.joinable()) {
		// The interesting line in any log where this appears: the worker did not acknowledge a
		// cancel within its grace period, so it is wedged in a call with no cancellation point
		// of its own. Whichever DebugLog breadcrumb is still open on that thread names it.
		DebugLog::Write(kLogCategory, "ABANDONING the login worker - it never acknowledged the cancel (stage %s)",
						StageName(GetStage()));
		m_worker.detach(); // it owns its own state block; nothing here is left dangling
	}

	auto pAbandoned = std::make_shared<SLoginAttemptState>();
	if (userCancelled) {
		pAbandoned->Stage.store(ELoginStage::LOGIN_STAGE_CANCELLED, std::memory_order_relaxed);
	} else {
		SetMessage(pAbandoned->szMessage, SLoginAttemptState::kMaxMessageLength, kUnresponsiveClientMessage);
		pAbandoned->Stage.store(ELoginStage::LOGIN_STAGE_ERROR, std::memory_order_relaxed);
	}
	pAbandoned->bWorkerFinished.store(true, std::memory_order_relaxed);

	m_pState = std::move(pAbandoned);
	m_bActive = false;
}

void CLoginAttempt::Update()
{
	if (!m_bActive) {
		return;
	}

	// bWorkerFinished, not merely a terminal stage: the worker stores that before unwinding its
	// own CUiAutomation (CoUninitialize plus a cross-process COM release), and joining in that
	// window would park the render thread on exactly the teardown a wedged client makes slow.
	if (m_pState != nullptr && m_pState->bWorkerFinished.load(std::memory_order_acquire)) {
		// Bounded at 50ms and on the render thread: if this ever reports slow, the UI hitched.
		const DebugLog::CScope scope(kLogCategory, "render-thread join of a finished login worker");
		JoinWithTimeoutOrDetach(m_worker, std::chrono::milliseconds(50));
		m_bActive = false;
		DebugLog::Write(kLogCategory, "attempt retired (final stage %s)", StageName(GetStage()));
		return;
	}

	// Only abandoned after a requested cancel goes unacknowledged past its grace period - never
	// purely for running long. See Cancel()/AbandonWorker.
	if (m_pState != nullptr && m_pState->bCancelRequested.load(std::memory_order_relaxed) &&
		std::chrono::steady_clock::now() >= m_cancelDeadline) {
		AbandonWorker();
	}
}
