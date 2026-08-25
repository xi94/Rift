#pragma once

// A std::thread's own destructor calls std::terminate if it's destroyed while still
// joinable, so a worker that's genuinely stuck - blocked forever inside a call this process
// doesn't control the other end of (a UI Automation COM call into an unresponsive target
// process - see core/ui_automation.h's own file comment on why that's a real, unavoidable
// risk, not a bug in how this project uses it; or a WinHTTP call with no response) - can't
// just be abandoned outright either. It has to be joined or detached before its std::thread
// object goes out of scope.
//
// This tries a bounded join first (via the underlying Win32 handle, since std::thread has no
// join-with-timeout of its own), and falls back to detaching - accepting the OS thread as a
// rare, harmless leak rather than hanging whatever's waiting on it, since a worker this stuck
// was never going to make progress anyway - if the wait times out. Every destructor in this
// project that owns a worker thread talking to another process or the network should use this
// instead of a bare join(): CLoginAttempt::~CLoginAttempt and CUpdater::~CUpdater both used to
// call a plain join() unconditionally, which meant a single wedged UI Automation/WinHTTP call
// hung this process's entire shutdown - no exception, so the crash handler never even got a
// chance to run; a user force-killing it via Task Manager then bypasses in-process exception
// handling entirely (TerminateProcess gives a target zero chance to run anything), which is
// exactly what reads as "it froze, and no crash handler ever showed up."

#include <chrono>
#include <thread>
#include <utility>

#include <Windows.h>

inline void JoinWithTimeoutOrDetach(std::thread &thread, std::chrono::milliseconds timeout)
{
	if (!thread.joinable()) {
		return;
	}
	const DWORD waitResult = WaitForSingleObject(thread.native_handle(), static_cast<DWORD>(timeout.count()));
	if (waitResult == WAIT_OBJECT_0) {
		thread.join(); // already finished - this is instantaneous
	} else {
		thread.detach(); // still stuck - let it go rather than hang whatever's waiting on this
	}
}

// Runs fn on a throwaway thread and waits up to timeout for it to finish, reporting whether it
// did. A false return means fn is still running and its thread has been detached - so fn must
// own everything it touches (capture by value, never a reference into the caller's frame), and
// the caller must be able to genuinely carry on without it.
//
// This exists for the window-activation calls in core/riot_client.cpp - AttachThreadInput,
// ShowWindow, SetForegroundWindow, BringWindowToTop and SetFocus, all aimed at ANOTHER
// process's window. Every one of those runs the *target's* window procedure synchronously and
// none of them takes a timeout, so a target whose wndproc doesn't come back blocks the caller
// forever with no cancellation point of its own - and a cold-starting Electron app doing
// synchronous cross-process IPC inside its own WM_ACTIVATE/WM_SETFOCUS handling is exactly
// that. A WM_NULL "is it pumping?" probe cannot rule it out either: that proves the thread
// dequeues messages, not that this particular message returns promptly, so it leaves the real
// risk untouched no matter how immediately before the call it runs. Bounding the call from the
// outside, on a thread nothing else needs back, is the only thing that actually can.
template <typename TFunc>
inline bool RunBoundedOrAbandon(TFunc fn, std::chrono::milliseconds timeout)
{
	std::thread worker(std::move(fn));
	const DWORD waitResult = WaitForSingleObject(worker.native_handle(), static_cast<DWORD>(timeout.count()));
	if (waitResult == WAIT_OBJECT_0) {
		worker.join(); // already finished - this is instantaneous
		return true;
	}
	worker.detach(); // wedged in someone else's window procedure; nothing brings it back
	return false;
}
