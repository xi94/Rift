#include "core/riot_client.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <thread>
#include <vector>

#include <TlHelp32.h>

#include <nlohmann/json.hpp>

#include "core/thread_util.h"

namespace {
constexpr const char *kInstallsJsonPath = "C:\\ProgramData\\Riot Games\\RiotClientInstalls.json";
constexpr const char *kExecutablePathKey = "rc_default";

// The Riot Client's own main window title, confirmed via Inspect against a real install -
// see riot_client.h's own file comment for why matching on this, not a process id, is this
// class's primary window lookup.
constexpr const wchar_t *kClientWindowTitle = L"Riot Client";

// The login form's two Edit-control field names, confirmed via Inspect against a real
// install's login page.
constexpr const wchar_t *kUsernameFieldName = L"USERNAME";
constexpr const wchar_t *kPasswordFieldName = L"PASSWORD";

// The Riot Client's own inline login-failure indicator: a ToolTip-type element named exactly
// this, confirmed via Inspect - see WaitForLoginError's own comment. This is only the generic
// heading, shown identically for every failure reason - it does NOT tell WaitForLoginError
// which one happened; the reason text is a separate element, matched independently below.
constexpr const wchar_t *kLoginErrorToolTipName = L"Login error";

// The two failure-reason texts confirmed via Inspect against a real install (see examples/
// invalid_password_element.png and examples/we're_having_troubles_signing_you_in.png) - each is
// its own Text-control element's exact Name, found separately from the "Login error" tooltip
// above rather than as a reliable descendant of it (that nesting isn't consistent - see
// WaitForLoginError's own comment). Riot may have other failure reasons this project hasn't
// seen yet; anything other than these two exact strings falls back to the generic tooltip text.
constexpr const wchar_t *kInvalidCredentialsReasonText = L"Your login credentials don't match an account in our system.";
constexpr const wchar_t *kTroubleSigningInReasonText =
	L"Sorry, we're having trouble signing you in right now. Please try again later.";

// The Riot Client's own "start the game" button once logged in, confirmed via Inspect: a
// Button-type element named exactly "Play" - see WaitForPlayButtonAndClick's own comment.
constexpr const wchar_t *kPlayButtonName = L"Play";

// Every Riot Client/League process this project knows how to relaunch cleanly - "some for
// now" per where this list came from; deliberately NOT the games themselves, see
// kGameProcessNames below and this file's own header comment.
constexpr const wchar_t *const kClientProcessNames[]{
	L"Riot Client.exe", L"RiotClientServices.exe", L"RiotClientUx.exe", L"RiotClientUxRender.exe",
	L"LeagueClient.exe", L"LeagueClientUx.exe", L"LoR.exe",
};

// The actual running-game processes a kill-and-relaunch must never touch - see
// CRiotClient::IsGameInProgress. "League of Legends.exe" (the real in-match client, distinct
// from LeagueClient.exe's lobby/launcher, which IS safe to kill) is this file's own addition
// to the same "don't interrupt an active match" list VALORANT-Win64-Shipping.exe came with -
// worth double-checking against a real in-game League process if that ever needs confirming.
constexpr const wchar_t *const kGameProcessNames[]{
	L"VALORANT-Win64-Shipping.exe",
	L"League of Legends.exe",
};

// Ordinal (byte-exact, locale-independent) case-insensitive comparison - process image names
// are plain ASCII in every case this project cares about, and ordinal comparison sidesteps
// any locale-specific casing surprise a culture-aware compare could introduce for something
// that's really just an exact-match check.
bool ProcessNameEquals(const wchar_t *pExeFileName, const wchar_t *pTarget)
{
	return CompareStringOrdinal(pExeFileName, -1, pTarget, -1, TRUE) == CSTR_EQUAL;
}

bool AnyProcessNameMatches(const wchar_t *const *pNames, std::size_t nameCount)
{
	const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) {
		return false;
	}

	bool found = false;
	PROCESSENTRY32W entry{.dwSize = sizeof(entry)};
	if (Process32FirstW(snapshot, &entry)) {
		do {
			for (std::size_t i = 0; i < nameCount && !found; i += 1) {
				found = ProcessNameEquals(entry.szExeFile, pNames[i]);
			}
		} while (!found && Process32NextW(snapshot, &entry));
	}
	CloseHandle(snapshot);
	return found;
}

// How long KillProcessesByName gives the processes it terminated to actually be gone before it
// stops waiting and lets the caller carry on regardless. Generous: a Riot Client tree normally
// unwinds in well under this, and the cost of being wrong in the other direction (see that
// function's own comment) is an attempt that hangs until the user cancels it.
constexpr auto kProcessExitTimeout = std::chrono::milliseconds(3000);

// Waits (bounded, against one shared deadline) for every handle in processes to signal, then
// closes them all - the handles are consumed either way, whether the wait succeeded or timed out.
void WaitForProcessesToExit(std::vector<HANDLE> &processes, std::chrono::milliseconds timeout)
{
	const auto deadline = std::chrono::steady_clock::now() + timeout;

	// WaitForMultipleObjects tops out at MAXIMUM_WAIT_OBJECTS handles at a time, and a Riot
	// Client tree mid-teardown can genuinely exceed that (its Electron shell spawns a renderer,
	// a gpu-process and a crashpad-handler of its own, and League's lobby client does the same),
	// so this waits in batches - all against the one deadline computed above, rather than
	// handing each batch its own full timeout.
	for (std::size_t offset = 0; offset < processes.size(); offset += MAXIMUM_WAIT_OBJECTS) {
		const DWORD count =
			static_cast<DWORD>(std::min<std::size_t>(MAXIMUM_WAIT_OBJECTS, processes.size() - offset));
		const auto now = std::chrono::steady_clock::now();
		const DWORD remainingMs =
			now >= deadline
				? 0
				: static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
		WaitForMultipleObjects(count, processes.data() + offset, TRUE, remainingMs);
	}

	for (const HANDLE process : processes) {
		CloseHandle(process);
	}
	processes.clear();
}

// Terminates every running process whose image name matches one of pNames, then waits (bounded -
// see kProcessExitTimeout) for them to actually be gone.
//
// That wait is load-bearing, not tidiness: TerminateProcess only *requests* termination and
// returns immediately, so without it this function comes back while the old client is still
// unwinding - and CRiotClient::Launch runs straight into a Riot Client that is still holding its
// own single-instance lock. The new instance then hands off to the dying one and exits, no new
// window ever appears, and the caller's WaitForWindow (unbounded by design - see
// core/login_attempt.cpp) waits for something that is never coming until the user cancels it.
// Windows also outlive their process only until it finishes tearing down, so waiting here is
// equally what keeps FindClientWindow from latching onto the old client's about-to-vanish HWND.
void KillProcessesByName(const wchar_t *const *pNames, std::size_t nameCount)
{
	const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) {
		return;
	}

	std::vector<HANDLE> terminated;
	PROCESSENTRY32W entry{.dwSize = sizeof(entry)};
	if (Process32FirstW(snapshot, &entry)) {
		do {
			bool matches = false;
			for (std::size_t i = 0; i < nameCount && !matches; i += 1) {
				matches = ProcessNameEquals(entry.szExeFile, pNames[i]);
			}
			if (!matches) {
				continue;
			}
			// SYNCHRONIZE alongside PROCESS_TERMINATE purely so the handle can be waited on
			// below. Best-effort either way - see this file's own header comment on
			// KillAllClientProcesses.
			const HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, entry.th32ProcessID);
			if (process == nullptr) {
				continue;
			}
			if (TerminateProcess(process, 0)) {
				terminated.push_back(process); // WaitForProcessesToExit closes it
			} else {
				CloseHandle(process);
			}
		} while (Process32NextW(snapshot, &entry));
	}
	CloseHandle(snapshot);

	WaitForProcessesToExit(terminated, kProcessExitTimeout);
}

// UTF-8 (this project's own string convention - see core/string_view.h) -> UTF-16, the one the
// Win32 *W APIs this class calls into actually need. Returns an empty string on a conversion
// failure or empty input, same "just doesn't have a path" outcome ResolveExecutablePath already
// treats as failure.
std::wstring Utf8ToWide(CStringView text)
{
	if (text.Length == 0) {
		return std::wstring{};
	}

	const int length = MultiByteToWideChar(CP_UTF8, 0, text.pData, static_cast<int>(text.Length), nullptr, 0);
	if (length <= 0) {
		return std::wstring{};
	}

	std::wstring wide(static_cast<std::size_t>(length), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text.pData, static_cast<int>(text.Length), wide.data(), length);
	return wide;
}

// True once, and only once, pCancelRequested is both non-null and observed set - the shared
// early-out check every poll loop below and in login_attempt.cpp's WorkerMain both use.
bool IsCancelled(const std::atomic<bool> *pCancelRequested)
{
	return pCancelRequested != nullptr && pCancelRequested->load(std::memory_order_relaxed);
}

constexpr std::uint32_t kResponsiveProbeTimeoutMs = 750;

// Whether hWnd's owning thread is actually pumping messages right now - the standard WM_NULL
// hung-app probe. AttachThreadInput below, and the SetForegroundWindow/SetFocus calls after it,
// all block on the target thread with no timeout, and AttachThreadInput is documented to hang the
// caller outright against a thread that isn't processing messages. The Riot Client's window
// exists (and matches by title) seconds before its Electron UI thread starts pumping, so probing
// first is what keeps a slow client slow instead of wedging us.
bool IsWindowResponsive(HWND hWnd, std::uint32_t timeoutMs)
{
	DWORD_PTR probeResult = 0;
	return SendMessageTimeoutW(hWnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, timeoutMs, &probeResult) != 0;
}

// Temporarily attaches the calling thread's input queue to hWnd's owning thread's, since Windows
// otherwise silently ignores SetForegroundWindow/SetFocus calls aimed at a window from a thread
// that isn't already part of the foreground thread's input queue (the SPI_FOREGROUNDLOCKTIMEOUT
// policy). RAII so every early-return still detaches. Skips attaching to an unresponsive target -
// see IsWindowResponsive: not attaching just means the client may not come to the front, which
// callers already tolerate.
//
// Only ever constructed on ActivateWindowBounded's throwaway thread, never on a caller's own -
// both AttachThreadInput calls here are unbounded synchronous trips into hWnd's owning thread,
// and the probe above is not the guard it looks like (see RunBoundedOrAbandon's own comment in
// core/thread_util.h). Note also that once TRUE succeeds the two threads share one input queue,
// so a target that wedges *after* that point hangs the matching detach below just as solidly -
// which is precisely why the thread this runs on has to be one nothing needs back.
class ScopedThreadInputAttach {
  public:
	explicit ScopedThreadInputAttach(HWND hWnd)
	{
		if (!IsWindowResponsive(hWnd, kResponsiveProbeTimeoutMs)) {
			return;
		}
		m_targetThreadId = GetWindowThreadProcessId(hWnd, nullptr);
		m_currentThreadId = GetCurrentThreadId();
		if (m_targetThreadId != 0 && m_targetThreadId != m_currentThreadId) {
			m_bAttached = AttachThreadInput(m_currentThreadId, m_targetThreadId, TRUE) != 0;
		}
	}

	~ScopedThreadInputAttach()
	{
		if (m_bAttached) {
			AttachThreadInput(m_currentThreadId, m_targetThreadId, FALSE);
		}
	}

	ScopedThreadInputAttach(const ScopedThreadInputAttach &) = delete;
	ScopedThreadInputAttach &operator=(const ScopedThreadInputAttach &) = delete;

  private:
	DWORD m_targetThreadId = 0;
	DWORD m_currentThreadId = 0;
	bool m_bAttached = false;
};

// How long one ActivateWindowNow pass gets before it's written off as wedged - see
// ActivateWindowBounded. Long enough that an ordinarily slow activation (a cold Electron client
// still bringing its renderer up) still lands normally, short enough that a genuinely stuck one
// doesn't dominate an attempt's own timeouts.
constexpr auto kActivateTimeout = std::chrono::milliseconds(3000);

// The whole "put this window in front, optionally give it keyboard focus" pass - every call in
// here is an unbounded synchronous trip through hWnd's own window procedure, so this only ever
// runs via ActivateWindowBounded, never inline on a caller's thread.
//
// SwitchToThisWindow used to sit alongside these as a belt-and-suspenders second activation. It
// is gone deliberately: it's undocumented, it duplicates what SetForegroundWindow/
// BringWindowToTop already do, and being a full activation it is one more unbounded wndproc
// round trip for no coverage the two documented calls don't already give.
void ActivateWindowNow(HWND hWnd, bool bAlsoFocus)
{
	const ScopedThreadInputAttach attach(hWnd);

	if (IsIconic(hWnd)) {
		ShowWindow(hWnd, SW_RESTORE);
	}
	SetForegroundWindow(hWnd);
	BringWindowToTop(hWnd);
	if (bAlsoFocus) {
		// Only meaningful while the attach above is still in effect - SetFocus aimed at another
		// thread's window does nothing at all otherwise.
		SetFocus(hWnd);
	}
}

// ActivateWindowNow, bounded: returns whether the pass actually completed. False means it's
// still stuck inside the client's own window procedure on a thread that has been abandoned -
// the client just doesn't come forward, which is a cosmetic loss both callers already tolerate,
// rather than the whole login worker freezing where nothing but CLoginAttempt's cancel watchdog
// can ever get it back.
bool ActivateWindowBounded(HWND hWnd, bool bAlsoFocus)
{
	return RunBoundedOrAbandon([hWnd, bAlsoFocus]() { ActivateWindowNow(hWnd, bAlsoFocus); }, kActivateTimeout);
}

// Best-effort wait for element to actually receive OS-level keyboard focus after a SetFocus()
// call on it - confirmed necessary against a real install, not a hypothetical:
// IUIAutomationElement::SetFocus can return success before the underlying focus change has
// actually taken effect (the provider posts it rather than applying it synchronously), and a
// keystroke synthesized immediately afterward - CUiAutomation::SendKey/SendKeystrokes, both
// of which type into whatever currently has real focus, not whatever SetFocus was last called
// on - can silently go nowhere if it beats that change. Proceeds anyway once timeoutMs
// elapses rather than failing outright; this is extra insurance on top of SetFocus's own
// success/failure, not a hard requirement a caller should treat as load-bearing on its own.
void WaitForKeyboardFocus(const CUiElement &element, std::uint32_t timeoutMs)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	while (!element.HasKeyboardFocus()) {
		if (std::chrono::steady_clock::now() >= deadline) {
			return;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
}

// Sets a text field's value, preferring a true IUIAutomationValuePattern write and falling
// back to SetFocus + CUiAutomation::SendKeystrokes for a control that doesn't support it - see
// CRiotClient::SubmitLogin's own comment for why both paths exist.
void SetFieldValue(const CUiAutomation &uiAutomation, const CUiElement &field, const std::wstring &value)
{
	if (field.SetValue(value.c_str())) {
		return;
	}
	field.SetFocus();
	WaitForKeyboardFocus(field, 500);
	uiAutomation.SendKeystrokes(value.c_str());
}
} // namespace

CRiotClient::~CRiotClient()
{
	// Deliberately not a kill - see this file's own header comment. Just release this
	// object's own handle to the process so it doesn't leak.
	if (m_hProcess != nullptr) {
		CloseHandle(m_hProcess);
	}
}

bool CRiotClient::IsGameInProgress()
{
	return AnyProcessNameMatches(kGameProcessNames, ARRAYSIZE(kGameProcessNames));
}

void CRiotClient::KillAllClientProcesses()
{
	KillProcessesByName(kClientProcessNames, ARRAYSIZE(kClientProcessNames));
}

CStringView CRiotClient::LaunchProductForBannerTitle(CStringView bannerTitle)
{
	if (StringViewEqual(bannerTitle, StringViewFromCString("League of Legends")) ||
		StringViewEqual(bannerTitle, StringViewFromCString("Teamfight Tactics"))) {
		return StringViewFromCString("league_of_legends");
	}
	if (StringViewEqual(bannerTitle, StringViewFromCString("Valorant"))) {
		return StringViewFromCString("valorant");
	}
	if (StringViewEqual(bannerTitle, StringViewFromCString("Legends of Runeterra"))) {
		return StringViewFromCString("bacon");
	}
	if (StringViewEqual(bannerTitle, StringViewFromCString("2XKO"))) {
		return StringViewFromCString("lion");
	}
	return StringViewFromCString("");
}

bool CRiotClient::ResolveExecutablePath()
{
	m_executablePath.clear();

	std::ifstream file(kInstallsJsonPath);
	if (!file.is_open()) {
		return false;
	}

	nlohmann::json parsed;
	try {
		file >> parsed;
	} catch (const nlohmann::json::exception &) {
		return false;
	}

	const auto it = parsed.find(kExecutablePathKey);
	if (it == parsed.end() || !it->is_string()) {
		return false;
	}

	const std::string path = it->get<std::string>();
	if (path.empty()) {
		return false;
	}

	m_executablePath = Utf8ToWide(CStringView{path.data(), path.size()});
	// The JSON stores forward slashes; CreateProcessW accepts either, but normalizing keeps
	// this consistent with every other Windows path this project builds/displays.
	for (wchar_t &c : m_executablePath) {
		if (c == L'/') {
			c = L'\\';
		}
	}
	return !m_executablePath.empty();
}

bool CRiotClient::Launch(CStringView launchProduct)
{
	if (m_executablePath.empty()) {
		return false;
	}

	STARTUPINFOW startupInfo{.cb = sizeof(startupInfo)};
	PROCESS_INFORMATION processInfo{};

	// CreateProcessW's lpCommandLine is non-const (it may rewrite the buffer in place) - a
	// mutable local, not a literal/temporary, is required either way.
	std::wstring commandLine = L"\"" + m_executablePath + L"\"";
	if (launchProduct.Length > 0) {
		// The same arguments this project's own per-game shortcuts use, confirmed against a
		// real one - "live" is the one patchline every product LaunchProductForBannerTitle
		// maps to actually ships on (matching RiotClientInstalls.json's own "rc_live" key).
		commandLine += L" --launch-product=" + Utf8ToWide(launchProduct) + L" --launch-patchline=live";
	}

	const BOOL created = CreateProcessW(m_executablePath.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0,
										nullptr, nullptr, &startupInfo, &processInfo);
	if (!created) {
		return false;
	}

	CloseHandle(processInfo.hThread);
	if (m_hProcess != nullptr) {
		CloseHandle(m_hProcess); // replacing a previous handle this instance held, not killing the process it named
	}
	m_hProcess = processInfo.hProcess;
	m_processId = processInfo.dwProcessId;
	return true;
}

bool CRiotClient::IsRunning() const
{
	if (m_hProcess == nullptr) {
		return false;
	}
	DWORD exitCode = 0;
	return GetExitCodeProcess(m_hProcess, &exitCode) != 0 && exitCode == STILL_ACTIVE;
}

HWND CRiotClient::FindClientWindow() const
{
	HWND hWnd = CUiAutomation::FindWindowByName(kClientWindowTitle);
	if (hWnd == nullptr && m_processId != 0) {
		hWnd = CUiAutomation::FindTopLevelWindow(m_processId);
	}
	return hWnd;
}

CUiElement CRiotClient::CurrentWindowElement(const CUiAutomation &uiAutomation) const
{
	const HWND hWnd = FindClientWindow();
	if (hWnd == nullptr) {
		return CUiElement{};
	}
	return uiAutomation.ElementFromWindow(hWnd);
}

HWND CRiotClient::WaitForResponsiveClientWindow(std::uint32_t timeoutMs,
											   const std::atomic<bool> *pCancelRequested) const
{
	// No deadline computed at all in the wait-forever case - see kWaitForeverMs. Cancellation is
	// still checked every iteration exactly as it is for a bounded wait, so this stays
	// interruptible rather than becoming a hang the caller can't get out of.
	const bool bWaitForever = timeoutMs == kWaitForeverMs;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(bWaitForever ? 0 : timeoutMs);
	for (;;) {
		// Both halves every iteration - "exists but not pumping yet" is the normal state for the
		// first seconds of a cold client start, and the one callers must not act on.
		const HWND hWnd = FindClientWindow();
		if (hWnd != nullptr && IsWindowResponsive(hWnd, kResponsiveProbeTimeoutMs)) {
			return hWnd;
		}
		if (IsCancelled(pCancelRequested) || (!bWaitForever && std::chrono::steady_clock::now() >= deadline)) {
			return nullptr;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}

bool CRiotClient::WaitForWindow(std::uint32_t timeoutMs, const std::atomic<bool> *pCancelRequested) const
{
	return WaitForResponsiveClientWindow(timeoutMs, pCancelRequested) != nullptr;
}

bool CRiotClient::BringToForeground(std::uint32_t timeoutMs, const std::atomic<bool> *pCancelRequested) const
{
	const HWND hWnd = WaitForResponsiveClientWindow(timeoutMs, pCancelRequested);
	if (hWnd == nullptr) {
		return false;
	}

	return ActivateWindowBounded(hWnd, false);
}

bool CRiotClient::SetKeyboardFocus(std::uint32_t timeoutMs, const std::atomic<bool> *pCancelRequested) const
{
	const HWND hWnd = WaitForResponsiveClientWindow(timeoutMs, pCancelRequested);
	if (hWnd == nullptr) {
		return false;
	}

	return ActivateWindowBounded(hWnd, true);
}

bool CRiotClient::SubmitLogin(const CUiAutomation &uiAutomation, CStringView username, CStringView password,
							  std::uint32_t timeoutMs, const std::atomic<bool> *pCancelRequested) const
{
	// Re-resolves the current window AND both fields together, every poll iteration - not a
	// window found once up front - because an earlier-found window (e.g. from a caller's own
	// prior WaitForWindow call) can turn out to have been a transient splash screen that
	// never grows these fields at all before it's replaced by the real login window. Polling
	// the pairing as a unit is what makes this correct regardless of how many window
	// transitions happen while waiting.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	CUiElement usernameField;
	CUiElement passwordField;
	for (;;) {
		const CUiElement window = CurrentWindowElement(uiAutomation);
		if (window.IsValid()) {
			usernameField =
				uiAutomation.FindFirstDescendantByNameAndControlType(window, kUsernameFieldName, UIA_EditControlTypeId);
			passwordField =
				uiAutomation.FindFirstDescendantByNameAndControlType(window, kPasswordFieldName, UIA_EditControlTypeId);
			if (usernameField.IsValid() && passwordField.IsValid()) {
				break;
			}
		}
		if (IsCancelled(pCancelRequested) || std::chrono::steady_clock::now() >= deadline) {
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	SetFieldValue(uiAutomation, usernameField, Utf8ToWide(username));
	SetFieldValue(uiAutomation, passwordField, Utf8ToWide(password));

	// Enter, not a specific "log in" button - no button's Name/AutomationId has been
	// confirmed yet (see this file's own header comment), and this matches what a real user
	// pressing Enter in the password field would do to submit the form. WaitForKeyboardFocus
	// (see its own comment) is what makes this reliable - confirmed as a real, observed
	// failure mode, not a hypothetical: sending the key right after SetFocus with no wait
	// could silently do nothing if the focus change hadn't actually landed yet.
	passwordField.SetFocus();
	WaitForKeyboardFocus(passwordField, 500);
	uiAutomation.SendKey(VK_RETURN);
	return true;
}

bool CRiotClient::WaitForLoginError(const CUiAutomation &uiAutomation, std::wstring &outMessage,
									std::uint32_t timeoutMs, const std::atomic<bool> *pCancelRequested,
									const std::wstring *pIgnoreMessage) const
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	// See this method's own header comment on pIgnoreMessage - starts true when there's nothing
	// to ignore (a first attempt), so a match is always accepted right away in that case.
	bool sawTooltipAbsent = (pIgnoreMessage == nullptr);
	for (;;) {
		const CUiElement window = CurrentWindowElement(uiAutomation);
		bool tooltipPresent = false;
		if (window.IsValid()) {
			const bool errorShowing = uiAutomation.FindFirstDescendantByName(window, kLoginErrorToolTipName).IsValid();
			if (errorShowing) {
				tooltipPresent = true;
				// The tooltip found above only confirms SOME error is showing - it's the same
				// generic heading regardless of which one. The specific reason is a separate
				// element, searched for independently across the whole window rather than as a
				// descendant of the tooltip (that nesting isn't reliable - see this method's own
				// header comment) - whichever of the two known exact reason strings is present.
				std::wstring message;
				if (uiAutomation.FindFirstDescendantByName(window, kInvalidCredentialsReasonText).IsValid()) {
					message = kInvalidCredentialsReasonText;
				} else if (uiAutomation.FindFirstDescendantByName(window, kTroubleSigningInReasonText).IsValid()) {
					message = kTroubleSigningInReasonText;
				} else {
					message = kLoginErrorToolTipName;
				}
				if (sawTooltipAbsent || pIgnoreMessage == nullptr || message != *pIgnoreMessage) {
					outMessage = std::move(message);
					return true;
				}
			}
		}
		if (!tooltipPresent) {
			sawTooltipAbsent = true;
		}
		if (IsCancelled(pCancelRequested) || std::chrono::steady_clock::now() >= deadline) {
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}

bool CRiotClient::WaitForPlayButtonAndClick(const CUiAutomation &uiAutomation, std::uint32_t timeoutMs,
											const std::atomic<bool> *pCancelRequested) const
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	for (;;) {
		const CUiElement window = CurrentWindowElement(uiAutomation);
		if (window.IsValid()) {
			const CUiElement playButton =
				uiAutomation.FindFirstDescendantByNameAndControlType(window, kPlayButtonName, UIA_ButtonControlTypeId);
			if (playButton.IsValid()) {
				playButton.Invoke();
				return true;
			}
		}
		if (IsCancelled(pCancelRequested) || std::chrono::steady_clock::now() >= deadline) {
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}
