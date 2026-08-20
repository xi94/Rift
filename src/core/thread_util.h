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
