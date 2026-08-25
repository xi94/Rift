#pragma once

// An always-compiled, runtime-gated diagnostic log - on by default in a Debug build, opt-in
// (set the RIFT_DEBUG_LOG environment variable to anything) in a Release one, and a
// predictable "is it on?" branch away from free when it's off. Deliberately NOT #ifdef'd out:
// the one class of bug this exists for - the login worker occasionally wedging somewhere
// inside a cross-process call it doesn't control the other end of (see core/ui_automation.h
// and core/riot_client.cpp) - is timing-dependent and doesn't necessarily reproduce in a
// Debug build, so being able to turn the same instrumentation on in a shipped build without
// rebuilding it is the whole point.
//
// Three things live here, and the third is the one that actually finds a hang:
//
//   1. Write() - an ordinary printf-style line, timestamped, thread-tagged, categorised.
//      Flushed per line, on purpose: a hung process gets force-killed from Task Manager
//      (TerminateProcess, no unwinding, no atexit, no CRT buffer flush), so anything still
//      sitting in a stdio buffer when that happens is exactly the part that would have said
//      where it hung.
//
//   2. CScope - an RAII breadcrumb around a call that could block. Costs nothing to enter
//      and logs nothing on the way in; on the way out it reports only if the call was slow
//      (see kSlowCallMs in the .cpp), so a 100ms poll loop doesn't drown the log while a
//      single 9-second FindFirst still stands out.
//
//   3. The watchdog thread Init() starts, which is what turns a freeze into a diagnosis
//      rather than a mystery. Every scan it:
//        - reports any CScope that has been open past its next escalating deadline, so a
//          call that never returns still names itself ("still running: <label> (14.0s)")
//          in the log while it's happening, instead of leaving a log that just stops;
//        - notices the render thread not having called MarkUiThreadAlive() recently, which
//          is what distinguishes "the whole app froze" from "the login worker is stuck but
//          the UI is fine" - two very different bugs that look identical from the outside;
//        - writes one process-wide diagnostic minidump (see crash_handler.h's own
//          WriteDiagnosticDump) the first time anything crosses the hang threshold, so the
//          actual blocked call stack of every thread is captured while it's still blocked,
//          not reconstructed afterwards from log lines.
//
// Init() as early as main() can manage and Shutdown() on the way out; everything else is
// safe to call from any thread at any time, including from a thread that gets abandoned
// mid-call (see core/thread_util.h) - an abandoned thread's breadcrumb deliberately stays in
// the table and keeps reporting, since "this thread never came back" is the finding.

#include <cstdint>

namespace DebugLog {

// Opens the log file (%LOCALAPPDATA%\Rift\logs, rotating the previous run's file to
// .prev.log so a crash-and-relaunch doesn't destroy the interesting one) and starts the
// watchdog. Safe to call once; a second call does nothing.
void Init();

// Stops the watchdog and closes the file. Safe with or without a prior Init().
void Shutdown();

bool IsEnabled();

// The full path of the current run's log file, UTF-8, for printing at startup. Empty when
// logging is off or the file couldn't be opened.
const char *GetFilePath();

// One line. pCategory is a short tag ("uia", "riot", "login", "app") that gets padded into
// its own column so the log stays greppable by subsystem.
void Write(const char *pCategory, const char *pFormat, ...);

// Called once per frame from the render thread's main loop - the watchdog's only way of
// telling a frozen UI apart from a busy worker. Never call it from a worker.
void MarkUiThreadAlive();

// An RAII breadcrumb around one call that could block indefinitely - see this file's own
// comment. Construct it directly above the call, not around a whole function, so the label
// names the thing that actually blocks:
//
//     const DebugLog::CScope scope("uia", "FindFirst(Name=%ls)", pName);
//
// Copying/moving one would break the slot bookkeeping the watchdog reads, so neither is
// allowed.
class CScope {
  public:
	CScope(const char *pCategory, const char *pFormat, ...);
	~CScope();

	CScope(const CScope &) = delete;
	CScope &operator=(const CScope &) = delete;

	std::uint64_t ElapsedMs() const;

  private:
	const char *m_pCategory = nullptr;
	std::int32_t m_slot = -1; // -1 when logging is off, or when every slot was already taken
	std::uint64_t m_startMs = 0;
	char m_szLabel[160]{};
};

} // namespace DebugLog
