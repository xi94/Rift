#pragma once

// Drives the Riot Client (RiotClientServices.exe) from the outside: process lifecycle (kill any
// already-running Riot Client/League processes, resolve the install path, launch fresh into a
// specific product, bring it to the foreground, give it keyboard focus) plus, once it's running,
// a UI Automation-driven login (core/ui_automation.h) that finds the login form and submits it -
// the same sequence of actions a person clicking through the Riot Client's own window would
// perform, just automated.
//
// This is all built on UI Automation, the same accessibility API a screen reader uses -
// Riot's own choice to expose the Riot Client's UI through it (or fail to disable it) is what
// makes any of this possible in the first place, and the same switch that lets a screen reader
// user log in is the one this class rides. Nothing here reads process memory, injects
// anything, or hooks any API surface Riot hasn't already put in front of every user's mouse and
// keyboard. RiotClientInstalls.json - a plain, unauthenticated file Riot's own installer
// writes to %ProgramData% - is the one piece of local state this depends on to find the exe.
//
// Everything below is real, not a stub - confirmed end to end against an actual install:
// resolving rc_default, launching it, finding its window, foregrounding/focusing it, filling in
// the login form, and detecting a failed attempt via the "Login error" tooltip it shows inline
// (including a real attempt reaching Riot's own backend). What's still unconfirmed is what a
// *successful* login looks like from the outside (no element for that has been captured yet -
// see WaitForLoginError's own comment); everything else here was checked against a real Riot
// Client, not guessed at.
//
// Always kill-then-launch, never reuse-if-already-open: an already-running Riot Client can be
// sitting logged in on its own homepage/library rather than the login screen, which has no
// username/password fields to find at all - confirmed as a real failure mode, not a
// hypothetical, when an already-open client silently never got a login submitted. KillAllClientProcesses
// covers the Riot Client's own process tree plus League's lobby client (LeagueClient.exe/
// LeagueClientUx.exe) and Legends of Runeterra (LoR.exe) - deliberately NOT the games
// themselves (VALORANT-Win64-Shipping.exe, League's actual in-match client): IsGameInProgress
// checks for those separately, and the whole kill-and-relaunch flow refuses to run at all while
// either is running, rather than risk yanking a live match out from under the user.
//
// A process launched via CreateProcessW is NOT the process that ends up owning the window: the
// Riot Client is a multi-process Electron app, where the launched process (RiotClientServices.exe)
// spawns "Riot Client.exe" from a "RiotClientElectron" subfolder next to it as a child, which
// itself spawns several more child processes (gpu-process, renderer, crashpad-handler, ...) that
// own no window at all - only the one "browser" process does. Rather than chase that process
// tree, every window lookup here matches by the window's own title ("Riot Client", confirmed via
// Inspect), which is simpler, faster, and exactly what's needed regardless of which process
// currently owns it - see CUiAutomation::FindWindowByName's own comment.
//
// This class deliberately doesn't own a CUiAutomation itself - see the methods below that take
// one as a parameter instead. UI Automation's COM interface is thread-affine (see
// ui_automation.h's own file comment); this class is meant to be reused across multiple login
// attempts, each running on its own fresh worker thread (see core/login_attempt.cpp), so the
// caller constructs a CUiAutomation local to that thread and passes it in rather than this
// class trying to cache one across threads it doesn't control the lifetime of.
//
// Every wait below takes an optional pCancelRequested - a caller (CLoginAttempt's worker)
// polling a shared cancellation flag can pass it through so a user's Cancel click actually
// interrupts whichever wait is currently in flight within one poll interval, rather than only
// taking effect once the wait would have timed out on its own anyway.
//
// Also deliberate: nothing here ever calls TerminateProcess on a process it didn't itself just
// decide to kill as part of a fresh kill-and-relaunch (see KillAllClientProcesses) - the whole
// point of automating the Riot Client is to leave the user logged into it once this class is
// done, not close it out from under them afterward.

#include <atomic>
#include <cstdint>
#include <string>

#include <Windows.h>

#include "core/string_view.h"
#include "core/ui_automation.h"

class CRiotClient {
  public:
	// Passed as timeoutMs to WaitForWindow to mean "no deadline at all" - poll until the window
	// is actually there or pCancelRequested says the user gave up, however long that takes.
	// Only that one wait honours it: every other wait here is a step that either lands quickly
	// or is genuinely broken, whereas a cold Riot Client start is just slow, and how slow
	// depends on a machine this can't measure ahead of time (see core/login_attempt.cpp's own
	// comment on why the user's own Cancel is the better bound there than any number picked
	// here). Deliberately not 0 - that reads as "don't wait at all", which is what a caller
	// leaving a timeout uninitialized would accidentally get.
	static constexpr std::uint32_t kWaitForeverMs = 0xFFFFFFFFu;

	~CRiotClient();

	// True if VALORANT's or League of Legends' own *game* process (not the Riot/League
	// client shells - see this file's own header comment) is currently running - checked
	// before KillAllClientProcesses ever runs, so a kill-and-relaunch refuses to disrupt an
	// active match instead of silently risking it.
	static bool IsGameInProgress();

	// Terminates every currently-running process this project knows how to relaunch cleanly -
	// the Riot Client's own process tree, League's lobby client, and Legends of Runeterra.
	// Best-effort: a process this can't open (already exiting, insufficient rights) is simply
	// skipped, not treated as a hard failure - Launch()/WaitForWindow afterward will surface a
	// real problem if one of these somehow survives and interferes. Never touches a game
	// process itself - see IsGameInProgress, checked separately by the caller.
	static void KillAllClientProcesses();

	// Maps this project's own carousel banner titles (see main.cpp's seeded CCarousel
	// banners) to the Riot Client's own --launch-product value for Launch() - "some for now"
	// per where this mapping came from; more may need adding as other Riot titles come up.
	// An unrecognized title returns an empty CStringView, which Launch() treats as "just open
	// the client" - still fine, just without the auto-launch-into-a-specific-game convenience
	// a real per-game shortcut gives.
	static CStringView LaunchProductForBannerTitle(CStringView bannerTitle);

	// Reads C:\ProgramData\Riot Games\RiotClientInstalls.json (via nlohmann::json, see
	// third_party) and resolves RiotClientServices.exe's path from its "rc_default" key - the
	// one key every install of this file is guaranteed to have. Returns false, leaving
	// GetExecutablePath empty, if the file is missing, unreadable, malformed, or the key
	// isn't a non-empty string - i.e. a machine with no Riot Client install at all.
	bool ResolveExecutablePath();

	const std::wstring &GetExecutablePath() const
	{
		return m_executablePath;
	}

	// Launches the resolved executable (fails if ResolveExecutablePath hasn't succeeded yet).
	// launchProduct (see LaunchProductForBannerTitle), if non-empty, is passed as
	// "--launch-product=<value> --launch-patchline=live" - the same arguments this project's
	// own per-game shortcuts use (confirmed against a real one) to have the client
	// auto-launch straight into that product once logged in, rather than landing on its
	// generic library. CreateProcessW's own return is all this waits on; it does NOT wait for
	// a window to exist - that's WaitForWindow's job, called separately once the caller is
	// actually ready to drive the UI. Consider KillAllClientProcesses first (see this file's
	// own header comment for why a fresh launch, not a reuse-if-already-open check, is the
	// right default here).
	bool Launch(CStringView launchProduct);

	bool IsRunning() const;

	// Polls (see CUiAutomation::WaitForWindowByName) for the Riot Client's own window to exist.
	// Confirmed against a real install that this alone isn't a reliable "ready" signal - the
	// client shows a transient splash/loading window (also titled "Riot Client") before the
	// real login window replaces it, so a window existing right now doesn't mean it's still the
	// same window (or a window with the login form actually built out) a moment later. Nothing
	// here caches the window it finds for that reason - see this file's own comment - so this
	// method exists mainly as an honest "is anything there yet at all" check a caller can use
	// for its own progress reporting; SubmitLogin/WaitForLoginError/BringToForeground/
	// SetKeyboardFocus each re-resolve the current window fresh regardless of whether this was
	// ever called. pCancelRequested, if non-null and observed true, ends the wait early
	// (returns false) - see this file's own header comment. timeoutMs may be kWaitForeverMs,
	// in which case pCancelRequested is the only thing that ever ends this wait short of the
	// window showing up.
	bool WaitForWindow(std::uint32_t timeoutMs, const std::atomic<bool> *pCancelRequested = nullptr) const;

	// SetForegroundWindow (with the AttachThreadInput trick actually needed to make it succeed
	// when this process isn't already the foreground one - Windows silently ignores a bare
	// SetForegroundWindow call from a background process by design, the
	// SPI_FOREGROUNDLOCKTIMEOUT policy) plus SwitchToThisWindow as a second, belt-and-suspenders
	// call alongside it. Retries (see this file's own comment on why nothing here trusts a
	// single lookup) until a window is found, timeoutMs elapses, or pCancelRequested is
	// observed true.
	bool BringToForeground(std::uint32_t timeoutMs = kDefaultActionTimeoutMs,
						   const std::atomic<bool> *pCancelRequested = nullptr) const;

	// SetFocus on the window itself (distinct from a specific control's own focus - see
	// CUiElement::SetFocus for that), via the same AttachThreadInput approach
	// BringToForeground uses - typically called right after it, before any UI Automation
	// lookup, so synthesized keyboard input (CUiAutomation::SendKeystrokes/SendKey) has
	// somewhere to land. Same retry shape as BringToForeground, including pCancelRequested.
	bool SetKeyboardFocus(std::uint32_t timeoutMs = kDefaultActionTimeoutMs,
						  const std::atomic<bool> *pCancelRequested = nullptr) const;

	// Fills in and submits the login form. uiAutomation must be freshly Init()'d on the calling
	// thread - see this file's own header comment for why this class doesn't own one itself.
	// Polls (see timeoutMs) for a *current* window - re-resolved fresh on every attempt, not a
	// cached reference from an earlier WaitForWindow call, precisely because that earlier
	// window can turn out to have been the splash screen (see this file's own comment) - that
	// also has the username/password fields as descendants: Edit controls named exactly
	// "USERNAME"/"PASSWORD", confirmed via Inspect against a real install; this web-based UI
	// doesn't set AutomationId on them the way a native Win32/WPF app would, so Name is
	// genuinely the only property that identifies them. Once found, sets their values
	// (preferring IUIAutomationValuePattern::SetValue, falling back to SetFocus +
	// CUiAutomation::SendKeystrokes for a control that doesn't support it), then sends
	// VK_RETURN to submit rather than hunting for a specific "log in" button, matching how a
	// real user pressing Enter in the password field would submit the form - confirmed against
	// a real install that this actually reaches Riot's own backend. Returns false if a window
	// with both fields never appeared within timeoutMs or pCancelRequested was observed true;
	// true only means the form was submitted, not that the login itself succeeded - see
	// WaitForLoginError for the one outcome this class can currently detect.
	bool SubmitLogin(const CUiAutomation &uiAutomation, CStringView username, CStringView password,
					 std::uint32_t timeoutMs, const std::atomic<bool> *pCancelRequested = nullptr) const;

	// Polls for the Riot Client's own inline "Login error" tooltip (confirmed via Inspect: a
	// ToolTip-type element named exactly "Login error") appearing within timeoutMs after a
	// SubmitLogin call - the login form's own way of showing a failed attempt inline rather
	// than a separate dialog. That tooltip's own name is only a generic heading, identical for
	// every failure reason - once it's found, the specific reason (e.g. "Your login credentials
	// don't match an account in our system." vs "Sorry, we're having trouble signing you in
	// right now. Please try again later.", both confirmed via Inspect) is searched for
	// separately across the whole window, NOT as a descendant of the tooltip - that nesting
	// isn't reliable, confirmed as a real, observed failure mode: a genuine invalid-credentials
	// reason was going undetected because it wasn't actually found nested under the tooltip
	// element that matched. Like SubmitLogin, re-resolves the current window fresh on every
	// poll rather than trusting one lookup. Returns true and fills outMessage with the specific
	// reason text if recognized, or the generic tooltip heading if the tooltip is showing but
	// neither known reason string is found (an as-yet-unseen failure reason); false (outMessage
	// untouched) if no error tooltip shows up in time or pCancelRequested was observed true - a
	// caller needs its own way to tell those two apart (re-checking the same flag works). False
	// is also NOT proof of success even when it wasn't a cancellation - no element for a
	// successful login has been captured/confirmed yet, so a caller treating "no error tooltip"
	// as "logged in" is making an inference this class itself doesn't verify.
	//
	// pIgnoreMessage guards against a real, observed race on a resubmit (see login_attempt.cpp's
	// retry-without-relaunch path): the *previous* attempt's tooltip can still be sitting on
	// screen, with its old text, at the instant this starts polling again, since nothing here
	// forces the Riot Client to visibly clear it before the new response arrives - a caller
	// resubmitting the exact same form has no way to tell "the old error, still there" apart
	// from "the new response, and it happens to be another error" without help. When non-null,
	// a tooltip whose text exactly matches *pIgnoreMessage is not accepted as a fresh result
	// until either its text changes (the client updated it in place) or it's been observed gone
	// at least once first (the client cleared it before showing the next one) - whichever
	// happens first. Pass nullptr (the default) for a first attempt, where there's no previous
	// message to confuse a fresh one with.
	bool WaitForLoginError(const CUiAutomation &uiAutomation, std::wstring &outMessage, std::uint32_t timeoutMs,
						   const std::atomic<bool> *pCancelRequested = nullptr,
						   const std::wstring *pIgnoreMessage = nullptr) const;

	// Polls for a Button-type element named exactly "Play" (confirmed via Inspect, on the Riot
	// Client's own library/home page once logged in - never present on the login page itself,
	// so finding it is a real confirmation of a successful login, not an inference) appearing
	// within timeoutMs, then calls its own IUIAutomationInvokePattern::Invoke - this exists
	// because Riot's own --launch-product/--launch-patchline auto-launch (see Launch's own
	// comment) stopped actually starting the game as of a recent Riot Client update: it now
	// only navigates to the right product's page without pressing Play itself, so this class
	// has to press it manually to finish what used to happen automatically. Returns true once
	// the button is found and invoked (regardless of whether Invoke itself reports success -
	// finding the button already confirms the login), false if it never appeared within
	// timeoutMs or pCancelRequested was observed true.
	bool WaitForPlayButtonAndClick(const CUiAutomation &uiAutomation, std::uint32_t timeoutMs,
								   const std::atomic<bool> *pCancelRequested = nullptr) const;

  private:
	// The default wait BringToForeground/SetKeyboardFocus give a window that isn't there on
	// the very first lookup - short, since by the time a caller reasonably calls either of
	// these (after WaitForWindow or SubmitLogin already succeeded once) something should
	// already be there almost immediately; long enough to ride out the same splash-to-real-
	// window transition SubmitLogin's own comment describes.
	static constexpr std::uint32_t kDefaultActionTimeoutMs = 5000;

	// Shared by every method above - see this file's own header comment for why title matching
	// (CUiAutomation::FindWindowByName) is the primary lookup, with the process-tree search
	// (CUiAutomation::FindTopLevelWindow) as a fallback for the unlikely case the title ever
	// doesn't match (a future client version, a different locale).
	HWND FindClientWindow() const;

	// FindClientWindow, polled until the window exists AND its owning thread is actually pumping
	// messages - the shared front half of WaitForWindow/BringToForeground/SetKeyboardFocus.
	// nullptr on timeout or cancellation. See IsWindowResponsive in the .cpp for why the
	// responsiveness half isn't optional. timeoutMs of kWaitForeverMs polls without a deadline
	// (see that constant) - the callers that pass their own timeout are unaffected.
	HWND WaitForResponsiveClientWindow(std::uint32_t timeoutMs, const std::atomic<bool> *pCancelRequested) const;

	// FindClientWindow, wrapped as a CUiAutomation element via the caller-supplied
	// uiAutomation - CUiElement{} (invalid) if no window currently exists. Always resolved
	// fresh from the current HWND, never cached, for the same reason FindClientWindow itself
	// is never cached - see this file's own header comment.
	CUiElement CurrentWindowElement(const CUiAutomation &uiAutomation) const;

	std::wstring m_executablePath;
	HANDLE m_hProcess = nullptr;
	std::uint32_t m_processId = 0;
};
