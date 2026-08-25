#pragma once

// Thin, general-purpose wrapper around Win32 UI Automation (the same accessibility API a
// screen reader uses) - lets this process inspect and drive another process's UI the way a
// human clicking through it would: find its window, find a control inside that window by
// AutomationId/Name/ControlType, read or set a value, invoke a button, focus a control, and
// synthesize real keystrokes. Nothing here knows anything about the Riot Client specifically -
// see core/riot_client.h for the one class that does, built entirely on top of this one.
//
// UI Automation's client API is COM-based and expects an initialized apartment on whichever
// thread calls it - Init() calls CoInitializeEx(COINIT_MULTITHREADED) itself so a caller
// doesn't need its own COM setup. MTA, not STA: this class only ever polls synchronously
// (FindFirst, GetCurrentName, ...) and never subscribes to UI Automation's own event
// callbacks, which is the one thing that would actually need STA's message-loop-driven
// dispatch - MTA is both simpler here and the only one that lets a caller like
// CLoginAttempt's worker thread (a fresh OS thread per login attempt) actually work at all,
// since CoInitializeEx must be called on every thread that touches the interface, not just
// once on whichever thread happened to create it. Even so: every method on a given
// CUiAutomation instance (and every CUiElement it hands back) must still be used from the
// thread Init() was called on for that instance, never shared across threads - construct a
// fresh instance per thread rather than trying to reuse one.
//
// Every wait/find here is a poll, not an instant lookup - a freshly-launched process's window,
// and the controls inside it, often don't exist yet for the first several hundred milliseconds,
// so "not found yet" and "never going to exist" are different outcomes a single lookup can't
// tell apart. See kPollIntervalMs in ui_automation.cpp for the polling interval every Wait*
// method shares.

#include <cstdint>
#include <string>

#include <UIAutomation.h>
#include <Windows.h>
#include <wrl/client.h>

// One found UI Automation element, thinly wrapped so most call sites never touch the raw COM
// interface directly. Copyable (ComPtr handles the refcounting) - default-constructed instances
// are the "not found" state every CUiAutomation lookup returns instead of null/an error code.
class CUiElement {
  public:
	CUiElement() = default;
	explicit CUiElement(Microsoft::WRL::ComPtr<IUIAutomationElement> pElement);

	bool IsValid() const
	{
		return m_pElement != nullptr;
	}

	// Escape hatch for anything this wrapper doesn't cover yet - every real IUIAutomationElement
	// method is still reachable through here.
	const Microsoft::WRL::ComPtr<IUIAutomationElement> &Get() const
	{
		return m_pElement;
	}

	// A true text-entry write via IUIAutomationValuePattern - not every control supports it
	// (returns false when it doesn't); CUiAutomation::SendKeystrokes after SetFocus is the
	// fallback for one that doesn't.
	bool SetValue(const wchar_t *pText) const;

	// IUIAutomationInvokePattern::Invoke - for a button that should be clicked programmatically
	// rather than via a synthesized mouse click.
	bool Invoke() const;

	// This element's own IUIAutomationElement::SetFocus - the element-specific counterpart to
	// CUiAutomation::SendKeystrokes, which always types into whatever currently has keyboard
	// focus, not any particular element. Call this immediately before SendKeystrokes.
	bool SetFocus() const;

	// IUIAutomationElement::get_CurrentHasKeyboardFocus - whether this element genuinely has
	// OS-level keyboard focus right now, not just whether SetFocus was last called on it.
	// SetFocus can return before that's actually true yet (the underlying focus change some
	// providers post rather than apply synchronously) - a caller about to synthesize a
	// keystroke via CUiAutomation::SendKey/SendKeystrokes right after SetFocus should poll
	// this first rather than assume SetFocus already landed, or the keystroke can go nowhere
	// meaningful. Returns false (not just "no") if the underlying property read itself fails.
	bool HasKeyboardFocus() const;

	bool GetName(std::wstring &outName) const;
	bool GetAutomationId(std::wstring &outAutomationId) const;

  private:
	Microsoft::WRL::ComPtr<IUIAutomationElement> m_pElement;
};

class CUiAutomation {
  public:
	// Balances a successful Init() with Shutdown() - safe even if Init() was never called or
	// failed, same as Shutdown() itself.
	~CUiAutomation();

	// Call once, early in main(), before any CUiAutomation is ever constructed - keeps the
	// process's multi-threaded apartment alive for as long as the process runs (via
	// CoIncrementMTAUsage) so that the per-thread CoInitializeEx/CoUninitialize pair every
	// Init()/Shutdown() below performs stops being an apartment create/destroy cycle. See the
	// implementation's own comment for why repeatedly building and tearing the MTA down is a
	// problem worth spending a call on. Safe to call more than once (the second does nothing),
	// and safe from any thread including the render thread - it joins no apartment itself.
	static void KeepProcessMtaAlive();

	// Must succeed before any other method is called. Safe to call once per instance; not
	// safe to call from more than one thread, or to call any other method on this instance
	// from a thread other than the one Init() ran on - see this file's own comment. In
	// particular: construct a fresh CUiAutomation on whichever thread will use it (e.g. once
	// per CLoginAttempt worker thread - see core/login_attempt.cpp), rather than sharing one
	// instance across threads even sequentially - COM apartment membership (this class joins
	// the multi-threaded apartment, MTA) is a property of the OS thread, not this C++ object,
	// so a different thread reusing an already-Init()'d instance still hasn't actually joined
	// the MTA itself.
	bool Init();

	// Releases the automation interface and balances Init's CoInitializeEx. Safe to call even
	// if Init() was never called or already failed.
	void Shutdown();

	// Plain Win32 (EnumWindows + CreateToolhelp32Snapshot), not UI Automation - but every
	// method below needs an HWND to start from (ElementFromHandle), so this lives here rather
	// than being duplicated at every call site that needs "the" window for a process. Searches
	// processId AND every one of its descendants (children, grandchildren, ...), not just
	// processId itself - confirmed necessary against a real install, not a hypothetical: the
	// Riot Client is a multi-process Electron app where the process CreateProcessW hands back
	// is a lightweight parent, and only one of several child/grandchild processes it spawns
	// (its "browser" process; the rest - gpu-process, renderer, crashpad-handler, ... - own no
	// window at all) is the one with the real, visible window. Only considers top-level,
	// visible, unowned windows above a minimum size - a process's tool/message-only/zero-size
	// helper windows are never what a caller means by "its window". Returns nullptr if none
	// exists yet.
	static HWND FindTopLevelWindow(std::uint32_t processId);

	// Plain FindWindowW(nullptr, pTitle) - an exact top-level window title match, desktop-wide.
	// Simpler and faster than FindTopLevelWindow's process-tree crawl, and the one that matters
	// most in practice: confirmed via Inspect against a real Riot Client install that its main
	// window's own title is exactly "Riot Client" regardless of which process in its multi-
	// process tree currently owns it, so title matching sidesteps the whole "which process
	// actually owns the window" question FindTopLevelWindow exists to answer. Prefer this one;
	// FindTopLevelWindow remains for a caller that only has a process id to go on (a window
	// whose title isn't known ahead of time, or has changed).
	static HWND FindWindowByName(const wchar_t *pTitle);

	// Polls FindTopLevelWindow until it succeeds or timeoutMs elapses, then wraps the result via
	// ElementFromWindow - the two most common startup-latency waits (process launched but no
	// window yet; window exists but UI Automation hasn't finished building its tree for it
	// yet) collapsed into one call.
	CUiElement WaitForWindowByProcessId(std::uint32_t processId, std::uint32_t timeoutMs) const;

	// Same wait shape as WaitForWindowByProcessId, but built on FindWindowByName - see that
	// method's own comment for why this is usually the better choice.
	CUiElement WaitForWindowByName(const wchar_t *pTitle, std::uint32_t timeoutMs) const;

	CUiElement ElementFromWindow(HWND hWnd) const;

	// One-shot (non-waiting) descendant lookups - for a caller that already knows the element
	// should exist right now. TreeScope_Descendants, not TreeScope_Children: Riot Client-style
	// UI frameworks routinely nest controls several levels deep under their apparent parent.
	CUiElement FindFirstDescendantByAutomationId(const CUiElement &root, const wchar_t *pAutomationId) const;
	CUiElement FindFirstDescendantByName(const CUiElement &root, const wchar_t *pName) const;
	CUiElement FindFirstDescendantByControlType(const CUiElement &root, CONTROLTYPEID controlType) const;

	// Name AND ControlType together (IUIAutomation::CreateAndCondition) - confirmed necessary
	// against a real Riot Client install: its login form's fields are Chromium/CEF-rendered
	// <input> elements exposed with a Name (e.g. "USERNAME") but no AutomationId at all (that's
	// a native Win32/WPF convention this web-based UI doesn't follow), so Name is the only
	// property that identifies them - pairing it with ControlType (UIA_EditControlTypeId) is
	// what keeps this from also matching some unrelated element that happens to share the same
	// visible label.
	CUiElement FindFirstDescendantByNameAndControlType(const CUiElement &root, const wchar_t *pName,
													   CONTROLTYPEID controlType) const;

	// Waiting counterparts to the four lookups above - see this file's own comment for why a
	// wait, not a single attempt, is the right default while a window's UI is still settling.
	CUiElement WaitForDescendantByAutomationId(const CUiElement &root, const wchar_t *pAutomationId,
											   std::uint32_t timeoutMs) const;
	CUiElement WaitForDescendantByName(const CUiElement &root, const wchar_t *pName, std::uint32_t timeoutMs) const;
	CUiElement WaitForDescendantByControlType(const CUiElement &root, CONTROLTYPEID controlType,
											  std::uint32_t timeoutMs) const;
	CUiElement WaitForDescendantByNameAndControlType(const CUiElement &root, const wchar_t *pName,
													 CONTROLTYPEID controlType, std::uint32_t timeoutMs) const;

	// Synthesizes real keyboard input via SendInput (KEYEVENTF_UNICODE per UTF-16 code unit,
	// including surrogate pairs - Windows reassembles those on the receiving end) - types into
	// whatever control currently has keyboard focus (see CUiElement::SetFocus), not a specific
	// element. This is the fallback for a control that doesn't support
	// IUIAutomationValuePattern::SetValue, and the only real option for "press Enter to submit."
	void SendKeystrokes(const wchar_t *pText) const;

	// A single virtual-key press+release (VK_RETURN to submit a form, VK_TAB to move focus, ...)
	// - also via SendInput, so it lands the same way a real keypress would rather than a
	// WM_KEYDOWN posted straight at a window, which some UI frameworks' own input handling
	// ignores. Confirmed working this way for CRiotClient::SubmitLogin's VK_RETURN against a
	// real install (the attempt reached Riot's own backend). If SendInput ever proves
	// unreliable in some environment (it requires the window to already have real OS-level
	// keyboard focus - see CUiElement::SetFocus/CRiotClient::SetKeyboardFocus), the documented
	// fallback is PostMessage(hWnd, WM_KEYDOWN/WM_KEYUP, virtualKeyCode, ...) aimed directly at
	// the target HWND (the Chrome_WidgetWin_1/Chrome_RenderWidgetHostHWND window, for the Riot
	// Client specifically) instead of a focus-dependent synthesized input - not implemented
	// here since it hasn't been needed yet.
	void SendKey(WORD virtualKeyCode) const;

  private:
	Microsoft::WRL::ComPtr<IUIAutomation> m_pAutomation;
	bool m_bComInitialized = false; // Init() owns a CoInitializeEx reference Shutdown() must balance
};
