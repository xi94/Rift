#pragma once

// A process-wide "last resort" crash handler - installs a top-level SEH exception filter
// (SetUnhandledExceptionFilter) plus a std::terminate handler (std::set_terminate) and a few
// smaller CRT hooks (_set_purecall_handler, _set_invalid_parameter_handler) that funnel into
// the same terminate path, so that ANY otherwise-fatal condition on ANY thread in this process
// - a genuine access violation, a stack overflow, an uncaught C++ exception, a pure virtual
// call, a std::thread destroyed while still joinable (a real, previously observed crash in
// this exact codebase - see login_attempt.h's own file comment on why CLoginAttempt's
// destructor now joins it first) - gets caught here instead of silently vanishing or showing
// Windows' own generic "stopped working" dialog with no useful information in it.
//
// On a catch, this: writes a .dmp minidump (via DbgHelp's MiniDumpWriteDump, with
// MiniDumpWithFullMemory so the whole process's memory is in it, not just stack summaries) to
// %LOCALAPPDATA%\Rift\crashes - loadable directly in WinDbg or Visual Studio's own minidump
// debugger to get a real symbolicated call stack (given this build's own .pdb), or, for
// reading raw disassembly/registers by hand, everything needed to do that too - then shows a
// blocking native TaskDialog (comctl32 v6 - see app.manifest's own dependency block) reporting
// the crashing module+offset (e.g. "Rift.exe+0x1A2B3C" - directly usable against a disassembly
// of the built .exe, same address format a debugger's own disassembly view shows) and the dump
// file's full path, with an "Open Crash Folder" button alongside Close. No IDCANCEL button and
// no TDF_ALLOW_DIALOG_CANCELLATION, so the title bar's close button/Alt-F4/Escape are all
// simply ignored - Close is the only way out, before the process actually terminates. The
// dialog is deliberately synchronous/blocking and un-skippable, not a toast or a log line -
// the whole point is that this can't just be missed. The reason text reports what was actually
// detected (an OS exception's own standard name, or "Unhandled exception" for the
// std::terminate/pure-virtual-call/invalid-parameter paths that have no real exception to name)
// rather than guessing at why - std::terminate in particular has several real causes, not just
// the one or two most common ones.
//
// Call once, as early as possible in main() - before window/device creation, before spawning
// any worker thread - so literally everything after this point is covered. Every mechanism
// installed here is process-wide, not per-thread, so this one call covers every thread this
// process ever creates (including CLoginAttempt's own worker threads), not just the one that
// calls it.
void InstallCrashHandler();
