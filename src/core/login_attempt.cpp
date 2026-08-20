#include "core/login_attempt.h"

#include <chrono>
#include <cstring>
#include <string>

#include <Windows.h>

#include "core/thread_util.h"
#include "core/ui_automation.h"

namespace {
// Generous - a cold Electron start (the Riot Client's own multi-process launch, confirmed
// slow-ish against a real install) is the single biggest source of latency in this whole
// flow, and a timeout here means "tell the user it failed", not "wait a little longer" - see
// core/riot_client.h's own file comment on why a fresh worker thread doesn't get another
// chance to retry within the same attempt.
constexpr std::uint32_t kLaunchTimeoutMs = 20000;
constexpr std::uint32_t kFormTimeoutMs = 10000;
constexpr std::uint32_t kResultTimeoutMs = 6000;

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

// Everything the worker thread needs, bundled into one value std::thread copies into its own
// storage - every pointer here points into the owning CLoginAttempt's own member data, valid
// for as long as that object is (it outlives every worker it starts).
struct WorkerArgs {
	std::atomic<ELoginStage> *pStage;
	const std::atomic<bool> *pCancelRequested;
	CRiotClient *pRiotClient;
	const char *pUsername;
	const char *pPassword;
	const char *pGameTitle;
	char *pMessage;
	std::uint32_t MessageBufferSize;
};

bool IsCancelled(const WorkerArgs &args)
{
	return args.pCancelRequested->load(std::memory_order_relaxed);
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
ESubmitResult SubmitAndWaitForResult(const WorkerArgs &args, CUiAutomation &uiAutomation, std::wstring &outErrorMessage,
									 const std::wstring *pIgnorePreviousMessage = nullptr)
{
	outErrorMessage.clear();
	if (!args.pRiotClient->SubmitLogin(uiAutomation, StringViewFromCString(args.pUsername),
									   StringViewFromCString(args.pPassword), kFormTimeoutMs, args.pCancelRequested)) {
		return ESubmitResult::FORM_NOT_FOUND;
	}
	if (args.pRiotClient->WaitForLoginError(uiAutomation, outErrorMessage, kResultTimeoutMs, args.pCancelRequested,
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
void StoreCancelledOrError(const WorkerArgs &args, const char *pFailureMessage)
{
	if (IsCancelled(args)) {
		args.pStage->store(ELoginStage::LOGIN_STAGE_CANCELLED, std::memory_order_release);
		return;
	}
	SetMessage(args.pMessage, args.MessageBufferSize, pFailureMessage);
	args.pStage->store(ELoginStage::LOGIN_STAGE_ERROR, std::memory_order_release);
}

// The background thread body Start() launches: drives pRiotClient through a real login via
// CUiAutomation, storing pStage at each step so the render thread's progress UI can follow
// along (see account_modal.cpp's DrawLoginProgress). A fresh CUiAutomation, not one shared
// across attempts - see ui_automation.h's own file comment for why that's required, not just
// tidy, given this runs on a brand new OS thread every call.
void WorkerMain(WorkerArgs args)
{
	args.pMessage[0] = '\0';
	args.pStage->store(ELoginStage::LOGIN_STAGE_WAITING_FOR_PROCESS, std::memory_order_relaxed);

	// Refuses to touch anything while an actual match is in progress - see
	// CRiotClient::IsGameInProgress's own comment. Checked before the kill-and-relaunch
	// below, not after, since there's nothing to undo if this bails out here.
	if (CRiotClient::IsGameInProgress()) {
		SetMessage(args.pMessage, args.MessageBufferSize, kGameInProgressMessage);
		args.pStage->store(ELoginStage::LOGIN_STAGE_ERROR, std::memory_order_release);
		return;
	}
	if (IsCancelled(args)) {
		args.pStage->store(ELoginStage::LOGIN_STAGE_CANCELLED, std::memory_order_release);
		return;
	}

	// Always kill, then always launch fresh - never "reuse if already open". An already-
	// running Riot Client can be sitting logged in on its own homepage/library rather than
	// the login screen, which has no username/password fields to find at all - see
	// core/riot_client.h's own file comment; this was a real, confirmed failure mode, not a
	// hypothetical.
	CRiotClient::KillAllClientProcesses();

	if (!args.pRiotClient->ResolveExecutablePath()) {
		SetMessage(args.pMessage, args.MessageBufferSize, kNoRiotClientMessage);
		args.pStage->store(ELoginStage::LOGIN_STAGE_ERROR, std::memory_order_release);
		return;
	}
	const CStringView launchProduct = CRiotClient::LaunchProductForBannerTitle(StringViewFromCString(args.pGameTitle));
	if (!args.pRiotClient->Launch(launchProduct)) {
		SetMessage(args.pMessage, args.MessageBufferSize, kLaunchFailedMessage);
		args.pStage->store(ELoginStage::LOGIN_STAGE_ERROR, std::memory_order_release);
		return;
	}

	if (!args.pRiotClient->WaitForWindow(kLaunchTimeoutMs, args.pCancelRequested)) {
		StoreCancelledOrError(args, kWindowTimeoutMessage);
		return;
	}
	if (IsCancelled(args)) {
		args.pStage->store(ELoginStage::LOGIN_STAGE_CANCELLED, std::memory_order_release);
		return;
	}

	args.pStage->store(ELoginStage::LOGIN_STAGE_CONNECTING, std::memory_order_relaxed);
	args.pRiotClient->BringToForeground(5000, args.pCancelRequested);
	args.pRiotClient->SetKeyboardFocus(5000, args.pCancelRequested);
	if (IsCancelled(args)) {
		args.pStage->store(ELoginStage::LOGIN_STAGE_CANCELLED, std::memory_order_release);
		return;
	}

	CUiAutomation uiAutomation;
	if (!uiAutomation.Init()) {
		SetMessage(args.pMessage, args.MessageBufferSize, kServerErrorMessage);
		args.pStage->store(ELoginStage::LOGIN_STAGE_ERROR, std::memory_order_release);
		return;
	}

	args.pStage->store(ELoginStage::LOGIN_STAGE_AUTHENTICATING, std::memory_order_relaxed);
	std::wstring errorMessage;
	ESubmitResult result = SubmitAndWaitForResult(args, uiAutomation, errorMessage);
	if (result == ESubmitResult::FORM_NOT_FOUND) {
		StoreCancelledOrError(args, kFormTimeoutMessage);
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
		if (IsCancelled(args)) {
			args.pStage->store(ELoginStage::LOGIN_STAGE_CANCELLED, std::memory_order_release);
			return;
		}

		args.pStage->store(ELoginStage::LOGIN_STAGE_CONNECTING, std::memory_order_relaxed);
		args.pRiotClient->BringToForeground(5000, args.pCancelRequested);
		args.pRiotClient->SetKeyboardFocus(5000, args.pCancelRequested);
		if (IsCancelled(args)) {
			args.pStage->store(ELoginStage::LOGIN_STAGE_CANCELLED, std::memory_order_release);
			return;
		}

		args.pStage->store(ELoginStage::LOGIN_STAGE_AUTHENTICATING, std::memory_order_relaxed);
		// See SubmitAndWaitForResult's own comment on pIgnorePreviousMessage - without it this
		// resubmit could read the first attempt's own still-on-screen error right back as if it
		// were this attempt's result.
		const std::wstring previousErrorMessage = errorMessage;
		result = SubmitAndWaitForResult(args, uiAutomation, errorMessage, &previousErrorMessage);
		if (result == ESubmitResult::FORM_NOT_FOUND) {
			StoreCancelledOrError(args, kFormTimeoutMessage);
			return;
		}
	}

	if (result == ESubmitResult::ERROR_SHOWN) {
		SetMessage(args.pMessage, args.MessageBufferSize,
				  LooksLikeInvalidCredentials(errorMessage) ? kInvalidCredentialsMessage : kServerErrorMessage);
		args.pStage->store(ELoginStage::LOGIN_STAGE_ERROR, std::memory_order_release);
		return;
	}
	if (IsCancelled(args)) {
		args.pStage->store(ELoginStage::LOGIN_STAGE_CANCELLED, std::memory_order_release);
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
	args.pStage->store(ELoginStage::LOGIN_STAGE_LAUNCHING, std::memory_order_relaxed);
	args.pRiotClient->WaitForPlayButtonAndClick(uiAutomation, kPlayButtonTimeoutMs, args.pCancelRequested);
	if (IsCancelled(args)) {
		args.pStage->store(ELoginStage::LOGIN_STAGE_CANCELLED, std::memory_order_release);
		return;
	}

	args.pStage->store(ELoginStage::LOGIN_STAGE_SUCCESS, std::memory_order_release);
}
} // namespace

CLoginAttempt::~CLoginAttempt()
{
	if (m_worker.joinable()) {
		m_bCancelRequested.store(true, std::memory_order_relaxed);
		// Bounded, not a bare join(): the worker may be stuck forever inside a single UI
		// Automation COM call against an unresponsive Riot Client, a real and unavoidable
		// risk this class's own cancellation flag can't interrupt mid-call (see
		// ui_automation.h's own file comment) - a plain join() here used to hang this
		// process's entire shutdown whenever that happened, with no exception ever thrown
		// for the crash handler to catch. See core/thread_util.h's own file comment.
		JoinWithTimeoutOrDetach(m_worker, std::chrono::milliseconds(3000));
	}
}

void CLoginAttempt::Init()
{
	m_stage.store(ELoginStage::LOGIN_STAGE_IDLE, std::memory_order_relaxed);
	m_bCancelRequested.store(false, std::memory_order_relaxed);
	m_bActive = false;
	m_szUsername[0] = '\0';
	m_szPassword[0] = '\0';
	m_szGameTitle[0] = '\0';
	m_szMessage[0] = '\0';
}

bool CLoginAttempt::IsTerminalStage(ELoginStage stage)
{
	return stage == ELoginStage::LOGIN_STAGE_SUCCESS || stage == ELoginStage::LOGIN_STAGE_ERROR ||
		   stage == ELoginStage::LOGIN_STAGE_CANCELLED;
}

void CLoginAttempt::Start(CStringView username, CStringView password, CStringView gameTitle)
{
	if (m_bActive) {
		// See this method's own header comment - a previous attempt that hasn't reached a
		// terminal stage yet might currently be stuck inside a single slow UI Automation call
		// with no cancellation point of its own; joining it here would block the calling
		// (render) thread for however long that call takes, a real observed freeze. Refuse
		// instead of blocking - the caller is expected to gate on IsActive() so this is only
		// ever reached once the previous attempt is actually done.
		if (!IsTerminalStage(GetStage())) {
			return;
		}
		if (m_worker.joinable()) {
			m_worker.join(); // already terminal - effectively instantaneous
		}
		m_bActive = false;
	}

	// Copied into this object's own buffers, not held as the caller's CStringViews - see
	// this class's own file comment.
	StringViewCopyToFixed(m_szUsername, sizeof(m_szUsername), username);
	StringViewCopyToFixed(m_szPassword, sizeof(m_szPassword), password);
	StringViewCopyToFixed(m_szGameTitle, sizeof(m_szGameTitle), gameTitle);
	m_szMessage[0] = '\0';
	m_bCancelRequested.store(false, std::memory_order_relaxed);

	m_stage.store(ELoginStage::LOGIN_STAGE_WAITING_FOR_PROCESS, std::memory_order_relaxed);
	const WorkerArgs args{
		.pStage = &m_stage,
		.pCancelRequested = &m_bCancelRequested,
		.pRiotClient = &m_riotClient,
		.pUsername = m_szUsername,
		.pPassword = m_szPassword,
		.pGameTitle = m_szGameTitle,
		.pMessage = m_szMessage,
		.MessageBufferSize = kMaxMessageLength,
	};
	m_worker = std::thread(WorkerMain, args);
	m_bActive = true;
}

void CLoginAttempt::Cancel()
{
	if (!m_bActive || IsTerminalStage(GetStage())) {
		return;
	}
	m_bCancelRequested.store(true, std::memory_order_relaxed);
}

ELoginStage CLoginAttempt::GetStage() const
{
	return m_stage.load(std::memory_order_acquire);
}

CStringView CLoginAttempt::GetTerminalMessage() const
{
	return StringViewFromCString(m_szMessage);
}

void CLoginAttempt::Update()
{
	if (!m_bActive) {
		return;
	}
	if (!IsTerminalStage(GetStage())) {
		return;
	}

	if (m_worker.joinable()) {
		m_worker.join();
	}
	m_bActive = false;
}
