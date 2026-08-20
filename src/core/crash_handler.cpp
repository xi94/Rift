#include "core/crash_handler.h"

#include <atomic>
#include <cstdlib>
#include <cwchar>
#include <exception>
#include <string>

#include <Windows.h>
#include <CommCtrl.h>
#include <DbgHelp.h>
#include <combaseapi.h>
#include <shellapi.h>
#include <shlobj.h>

namespace {

// Guards against a crash happening WHILE this is already handling one (e.g. writing the dump
// itself faults, or two threads crash at once) - the second one just falls through to
// whatever's left of the OS's own default handling instead of recursing or racing the first.
std::atomic<bool> g_bHandlingCrash{false};

// %LOCALAPPDATA%\Rift\crashes - created on demand; this project's storage.cpp/master_key.cpp
// already treat %LOCALAPPDATA% as the one place this app keeps its own local state, so crash
// dumps land next to that rather than introducing a new convention.
std::wstring CrashDumpDirectory()
{
	PWSTR pLocalAppData = nullptr;
	std::wstring dir;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &pLocalAppData))) {
		dir = pLocalAppData;
		dir += L"\\Rift\\crashes";
	}
	if (pLocalAppData != nullptr) {
		CoTaskMemFree(pLocalAppData);
	}
	if (!dir.empty()) {
		SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
	}
	return dir;
}

std::wstring FormatTimestamp()
{
	SYSTEMTIME st;
	GetLocalTime(&st);
	wchar_t buffer[32];
	swprintf_s(buffer, L"%04u%02u%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
	return buffer;
}

// The faulting instruction's address, expressed relative to this process's own main module
// base - i.e. what a disassembler or WinDbg would call "Rift.exe+0x...". The one piece of
// information from a raw crash that's actually useful to someone reading disassembly by hand
// without loading the full dump first (see this file's own header comment).
std::wstring ModuleRelativeOffset(void *pAddress)
{
	const HMODULE hModule = GetModuleHandleW(nullptr);
	wchar_t modulePath[MAX_PATH]{};
	GetModuleFileNameW(hModule, modulePath, ARRAYSIZE(modulePath));

	const wchar_t *pBaseName = wcsrchr(modulePath, L'\\');
	pBaseName = pBaseName != nullptr ? pBaseName + 1 : modulePath;

	const auto base = reinterpret_cast<std::uintptr_t>(hModule);
	const auto address = reinterpret_cast<std::uintptr_t>(pAddress);

	wchar_t buffer[MAX_PATH + 32];
	if (address >= base) {
		swprintf_s(buffer, L"%s+0x%llX", pBaseName, static_cast<unsigned long long>(address - base));
	} else {
		swprintf_s(buffer, L"%s (address outside module: 0x%p)", pBaseName, pAddress);
	}
	return buffer;
}

// pExceptionPointers may be null (the std::terminate/purecall/invalid-parameter paths below
// have no real SEH exception to point at) - MiniDumpWriteDump treats a null
// MINIDUMP_EXCEPTION_INFORMATION* as "just capture the current state of every thread", which
// is exactly the right fallback there. dumpDirectory is passed in rather than resolved here
// again - the caller already needs it separately for the crash dialog's "Open Crash Folder"
// button, and it's still valid even when this itself fails to produce a dump (nothing to open
// then, but the folder path is still correct).
std::wstring WriteMiniDump(EXCEPTION_POINTERS *pExceptionPointers, const std::wstring &dumpDirectory)
{
	if (dumpDirectory.empty()) {
		return L"";
	}

	const std::wstring path = dumpDirectory + L"\\Rift_crash_" + FormatTimestamp() + L".dmp";
	const HANDLE hFile =
		CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile == INVALID_HANDLE_VALUE) {
		return L"";
	}

	MINIDUMP_EXCEPTION_INFORMATION exceptionInfo{};
	exceptionInfo.ThreadId = GetCurrentThreadId();
	exceptionInfo.ExceptionPointers = pExceptionPointers;
	exceptionInfo.ClientPointers = FALSE;

	// Full memory, not just stack summaries - this handler's own audience (see this file's
	// own header comment) wants to be able to load the dump in a real debugger and inspect
	// memory/registers by hand, not just get a curated call stack.
	const auto dumpType = static_cast<MINIDUMP_TYPE>(MiniDumpWithFullMemory | MiniDumpWithHandleData |
													  MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);

	const BOOL written = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, dumpType,
										   pExceptionPointers != nullptr ? &exceptionInfo : nullptr, nullptr, nullptr);
	CloseHandle(hFile);
	return written != FALSE ? path : L"";
}

constexpr int kOpenFolderButtonId = 1001;
constexpr int kCloseButtonId = 1002;

// TDN_BUTTON_CLICKED for kOpenFolderButtonId opens the crash folder (lpRefData is the
// dumpDirectory string ShowCrashDialog set up) and returns S_FALSE, which tells TaskDialog NOT
// to close - see ShowCrashDialog's own comment on why nothing here is allowed to be dismissed
// except by the explicit Close button. Every other notification just falls through to
// TaskDialog's own default handling (S_OK).
HRESULT CALLBACK CrashDialogCallback(HWND hWnd, UINT msg, WPARAM wParam, LPARAM /*lParam*/, LONG_PTR lpRefData)
{
	if (msg == TDN_BUTTON_CLICKED && wParam == kOpenFolderButtonId) {
		const auto *pDumpDirectory = reinterpret_cast<const wchar_t *>(lpRefData);
		if (pDumpDirectory != nullptr && pDumpDirectory[0] != L'\0') {
			ShellExecuteW(hWnd, L"open", pDumpDirectory, nullptr, nullptr, SW_SHOWNORMAL);
		}
		return S_FALSE;
	}
	return S_OK;
}

// A native TaskDialog (comctl32 v6 - see app.manifest's own dependency block, required for
// this to render as a real themed dialog rather than silently failing) rather than a plain
// MessageBoxW: gets this an actual "Open Crash Folder" button alongside Close (MessageBoxW's
// fixed button sets can't do that), and reads like the crash dialog a professional Windows
// app would actually show, not a raw debug alert. No IDCANCEL button and no
// TDF_ALLOW_DIALOG_CANCELLATION: the title bar's own close button, Alt-F4, and Escape are all
// simply ignored, so the ONLY way out is an explicit Close click - see this file's own header
// comment on why this can't be allowed to just be skipped. hwndParent is deliberately null
// (this process's own window may be in an arbitrary/broken state by the time this runs) - the
// dialog still shows as a real top-level, foreground window on its own.
void ShowCrashDialog(const std::wstring &reason, const std::wstring &offset, const std::wstring &dumpPath,
					 const std::wstring &dumpDirectory)
{
	std::wstring content = L"An unexpected error occurred and Rift needs to close. A crash report has been saved "
						   L"locally.\n\nError: " +
						   reason + L"\nLocation: " + offset;
	content += !dumpPath.empty() ? (L"\n\nSaved to:\n" + dumpPath)
								 : std::wstring(L"\n\nThe crash report itself could not be saved.");

	const TASKDIALOG_BUTTON buttons[] = {
		{kOpenFolderButtonId, L"Open Crash Folder"},
		{kCloseButtonId, L"Close"},
	};

	TASKDIALOGCONFIG config{};
	config.cbSize = sizeof(config);
	config.hwndParent = nullptr;
	config.dwFlags = TDF_SIZE_TO_CONTENT;
	config.pszWindowTitle = L"Rift";
	config.pszMainIcon = TD_ERROR_ICON;
	config.pszMainInstruction = L"Rift has stopped working";
	config.pszContent = content.c_str();
	config.pButtons = buttons;
	config.cButtons = ARRAYSIZE(buttons);
	config.nDefaultButton = kCloseButtonId;
	config.pfCallback = CrashDialogCallback;
	config.lpCallbackData = reinterpret_cast<LONG_PTR>(dumpDirectory.c_str());

	int selectedButton = 0;
	TaskDialogIndirect(&config, &selectedButton, nullptr, nullptr);
}

const wchar_t *ExceptionCodeName(DWORD code)
{
	switch (code) {
		case EXCEPTION_ACCESS_VIOLATION:
			return L"Access violation";
		case EXCEPTION_STACK_OVERFLOW:
			return L"Stack overflow";
		case EXCEPTION_ILLEGAL_INSTRUCTION:
			return L"Illegal instruction";
		case EXCEPTION_INT_DIVIDE_BY_ZERO:
			return L"Integer divide by zero";
		case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
			return L"Array bounds exceeded";
		case EXCEPTION_DATATYPE_MISALIGNMENT:
			return L"Datatype misalignment";
		case EXCEPTION_PRIV_INSTRUCTION:
			return L"Privileged instruction";
		case EXCEPTION_IN_PAGE_ERROR:
			return L"In-page I/O error";
		case EXCEPTION_BREAKPOINT:
			return L"Breakpoint (unhandled)";
		default:
			return L"Unknown exception";
	}
}

LONG WINAPI UnhandledExceptionFilterProc(EXCEPTION_POINTERS *pExceptionPointers)
{
	bool expected = false;
	if (!g_bHandlingCrash.compare_exchange_strong(expected, true)) {
		return EXCEPTION_CONTINUE_SEARCH; // already crashing on another thread - don't recurse/race
	}

	const DWORD code = pExceptionPointers->ExceptionRecord->ExceptionCode;

	// A stack overflow leaves almost no stack to run the rest of this handler on - reclaim the
	// guard page first (the standard technique for this exact situation) so there's at least
	// some room to work with before touching std::wstring, calling into DbgHelp, etc.
	if (code == EXCEPTION_STACK_OVERFLOW) {
		_resetstkoflw();
	}

	const std::wstring reason = ExceptionCodeName(code);
	const std::wstring offset = ModuleRelativeOffset(pExceptionPointers->ExceptionRecord->ExceptionAddress);
	const std::wstring dumpDirectory = CrashDumpDirectory();
	const std::wstring dumpPath = WriteMiniDump(pExceptionPointers, dumpDirectory);

	ShowCrashDialog(reason, offset, dumpPath, dumpDirectory);

	return EXCEPTION_EXECUTE_HANDLER; // let Windows terminate the process now - no WER popup on top of ours
}

// Shared by every non-SEH path below (std::terminate, a pure virtual call, a CRT "invalid
// parameter") - none of these hand over a real EXCEPTION_POINTERS the way an SEH fault does, so
// this captures the current register state itself (RtlCaptureContext) and treats its own
// current instruction pointer as the "crash address", which is the closest honest equivalent:
// it's not the line that caused the underlying problem, but it IS where this process actually
// stopped being able to continue.
[[noreturn]] void HandleFatalCondition(const wchar_t *pReason)
{
	bool expected = false;
	if (!g_bHandlingCrash.compare_exchange_strong(expected, true)) {
		std::abort(); // already crashing on another thread - don't recurse/race, just die
	}

	CONTEXT context{};
	RtlCaptureContext(&context);

	EXCEPTION_RECORD exceptionRecord{};
	exceptionRecord.ExceptionCode = STATUS_FATAL_APP_EXIT; // synthetic - there's no real SEH exception here
#if defined(_M_X64)
	exceptionRecord.ExceptionAddress = reinterpret_cast<void *>(context.Rip);
#elif defined(_M_IX86)
	exceptionRecord.ExceptionAddress = reinterpret_cast<void *>(context.Eip);
#endif

	EXCEPTION_POINTERS exceptionPointers{&exceptionRecord, &context};

	const std::wstring offset = ModuleRelativeOffset(exceptionRecord.ExceptionAddress);
	const std::wstring dumpDirectory = CrashDumpDirectory();
	const std::wstring dumpPath = WriteMiniDump(&exceptionPointers, dumpDirectory);

	ShowCrashDialog(pReason, offset, dumpPath, dumpDirectory);

	std::abort();
}

// Not an SEH exception at all (an uncaught C++ exception is the usual trigger, but there are
// several others - see std::terminate's own docs; a std::thread destroyed while still
// joinable, once a real crash in this exact codebase before login_attempt.h's own destructor
// started joining it first, is only one of them), so SetUnhandledExceptionFilter alone
// wouldn't catch it. This is what does. Deliberately doesn't guess which specific cause this
// particular call was - "Unhandled exception" is what's actually known for certain.
[[noreturn]] void TerminateHandler()
{
	HandleFatalCondition(L"Unhandled exception");
}

[[noreturn]] void __cdecl PureCallHandler()
{
	HandleFatalCondition(L"Pure virtual function call");
}

void __cdecl InvalidParameterHandler(const wchar_t *, const wchar_t *, const wchar_t *, unsigned int, std::uintptr_t)
{
	HandleFatalCondition(L"CRT invalid parameter");
}

} // namespace

void InstallCrashHandler()
{
	// Our own dialog (see ShowCrashDialog) replaces both of Windows' own default crash UIs -
	// suppress those so a human sees exactly one crash dialog, with useful information in it,
	// not two (or one useless one).
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

	SetUnhandledExceptionFilter(UnhandledExceptionFilterProc);
	std::set_terminate(TerminateHandler);
	_set_purecall_handler(PureCallHandler);
	_set_invalid_parameter_handler(InvalidParameterHandler);
}
