#include "core/debug_log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <share.h> // _SH_DENYWR, for the shared open in Init()
#include <string>

#include <Windows.h>
#include <shlobj.h>

#include "core/crash_handler.h"
#include "core/version.h"

namespace {

// How long a CScope has to stay open before its destructor bothers reporting it. Every poll
// loop in this project ticks at 100ms, so anything under this is the normal case and saying
// so every time would bury the abnormal one.
constexpr std::uint64_t kSlowCallMs = 250;

// How long a still-open CScope goes unreported before the watchdog first calls it out, and
// the ceiling its (doubling) reporting interval grows to afterwards. Escalating rather than
// fixed so a genuinely abandoned call (see core/thread_util.h - its breadcrumb never closes)
// settles into an occasional reminder instead of a line every scan forever.
constexpr std::uint64_t kFirstReportMs = 2000;
constexpr std::uint64_t kMaxReportIntervalMs = 60000;

// How long any one call has to be stuck before this writes the one-shot diagnostic minidump.
// Comfortably past every real timeout in the login flow (the longest is CRiotClient's own
// 10s form wait), so this only ever fires for something that genuinely isn't coming back.
constexpr std::uint64_t kHangDumpMs = 20000;

// How long the render thread can go without a MarkUiThreadAlive() before the watchdog calls
// the UI frozen. Two seconds is many missed frames - far past any legitimate hitch (a shader
// compile, a swapchain resize), and the point of this is to be unambiguous.
constexpr std::uint64_t kUiStallMs = 2000;

constexpr std::uint64_t kWatchdogScanIntervalMs = 500;

// Fixed, never grown: a breadcrumb belonging to an abandoned thread is never released (see
// debug_log.h's own header comment on why that's deliberate), so this is really "how many
// permanently wedged calls can be tracked before tracking stops being useful anyway". Far
// more than a healthy run ever uses at once.
constexpr std::int32_t kMaxBreadcrumbs = 64;

struct SBreadcrumb {
	bool bInUse = false;
	DWORD ThreadId = 0;
	std::uint64_t StartMs = 0;
	std::uint64_t NextReportMs = 0;     // an absolute tick, not a duration
	std::uint64_t ReportIntervalMs = 0; // doubles each time it fires - see kMaxReportIntervalMs
	const char *pCategory = nullptr;
	char szLabel[160]{};
};

std::atomic<bool> g_bEnabled{false};
std::atomic<bool> g_bInitialized{false};

std::uint64_t g_startTicks = 0;

// Guards g_pFile and the stdio/OutputDebugString writes below, so two threads' lines never
// interleave mid-line. Never held across anything that can block.
SRWLOCK g_writeLock = SRWLOCK_INIT;
FILE *g_pFile = nullptr;
std::string g_filePath;

// Guards g_breadcrumbs. Strictly inner to g_writeLock in the sense that nothing holding this
// ever takes that one - the watchdog copies out what it wants to report, drops this, and only
// then writes.
SRWLOCK g_breadcrumbLock = SRWLOCK_INIT;
SBreadcrumb g_breadcrumbs[kMaxBreadcrumbs];

std::atomic<std::uint64_t> g_lastUiAliveMs{0};
std::atomic<bool> g_bUiStallReported{false};
std::atomic<bool> g_bHangDumpWritten{false};

HANDLE g_hWatchdogStop = nullptr;
HANDLE g_hWatchdogThread = nullptr;

std::uint64_t NowMs()
{
	return GetTickCount64();
}

// %LOCALAPPDATA%\Rift\logs - the same "this app's own local state" root crash_handler.cpp,
// storage.cpp and master_key.cpp already use, rather than writing next to the .exe (which may
// sit in Program Files, where a normal user can't write at all).
std::wstring LogDirectory()
{
	PWSTR pLocalAppData = nullptr;
	std::wstring dir;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &pLocalAppData))) {
		dir = pLocalAppData;
		dir += L"\\Rift\\logs";
	}
	if (pLocalAppData != nullptr) {
		CoTaskMemFree(pLocalAppData);
	}
	if (!dir.empty()) {
		SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
	}
	return dir;
}

std::string WideToUtf8(const std::wstring &wide)
{
	if (wide.empty()) {
		return std::string{};
	}
	const int length =
		WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
	if (length <= 0) {
		return std::string{};
	}
	std::string utf8(static_cast<std::size_t>(length), '\0');
	WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), utf8.data(), length, nullptr, nullptr);
	return utf8;
}

// One already-formatted message out to all three sinks. The file is the one that matters (it
// survives the force-kill that ends a hang); OutputDebugStringA is for watching live in a
// debugger or DebugView, and stdout only actually goes anywhere in a Debug build, which is the
// console-subsystem one - see the root CMakeLists.txt.
void WriteLine(const char *pCategory, const char *pMessage)
{
	SYSTEMTIME localTime;
	GetLocalTime(&localTime);

	const std::uint64_t elapsedMs = NowMs() - g_startTicks;

	char line[1400];
	_snprintf_s(line, _TRUNCATE, "%02u:%02u:%02u.%03u  +%4llu.%03llus  t%-5lu  %-8s  %s\n", localTime.wHour,
				localTime.wMinute, localTime.wSecond, localTime.wMilliseconds,
				static_cast<unsigned long long>(elapsedMs / 1000), static_cast<unsigned long long>(elapsedMs % 1000),
				GetCurrentThreadId(), pCategory != nullptr ? pCategory : "-", pMessage);

	AcquireSRWLockExclusive(&g_writeLock);
	if (g_pFile != nullptr) {
		std::fputs(line, g_pFile);
		// Per line, deliberately - see debug_log.h's own header comment.
		std::fflush(g_pFile);
	}
	OutputDebugStringA(line);
	std::fputs(line, stdout);
	std::fflush(stdout);
	ReleaseSRWLockExclusive(&g_writeLock);
}

// printf-style straight to WriteLine - for this file's own internal reporting, which has no
// reason to go through the public Write()'s enabled check a second time.
void WriteFormatted(const char *pCategory, const char *pFormat, ...)
{
	char message[1024];
	va_list args;
	va_start(args, pFormat);
	_vsnprintf_s(message, sizeof(message), _TRUNCATE, pFormat, args);
	va_end(args);
	WriteLine(pCategory, message);
}

// One report the watchdog decided to emit, copied out of the table so the logging itself
// happens with g_breadcrumbLock already released.
struct SPendingReport {
	const char *pCategory;
	DWORD ThreadId;
	std::uint64_t AgeMs;
	char szLabel[160];
};

void ScanBreadcrumbs()
{
	const std::uint64_t now = NowMs();

	SPendingReport reports[kMaxBreadcrumbs];
	std::int32_t reportCount = 0;
	bool bAnyPastHangThreshold = false;

	AcquireSRWLockExclusive(&g_breadcrumbLock);
	for (std::int32_t i = 0; i < kMaxBreadcrumbs; i += 1) {
		SBreadcrumb &crumb = g_breadcrumbs[i];
		if (!crumb.bInUse || now < crumb.NextReportMs) {
			continue;
		}

		const std::uint64_t ageMs = now - crumb.StartMs;
		if (ageMs >= kHangDumpMs) {
			bAnyPastHangThreshold = true;
		}

		SPendingReport &report = reports[reportCount];
		report.pCategory = crumb.pCategory;
		report.ThreadId = crumb.ThreadId;
		report.AgeMs = ageMs;
		std::memcpy(report.szLabel, crumb.szLabel, sizeof(report.szLabel));
		reportCount += 1;

		const std::uint64_t nextInterval = crumb.ReportIntervalMs * 2;
		crumb.ReportIntervalMs = nextInterval < kMaxReportIntervalMs ? nextInterval : kMaxReportIntervalMs;
		crumb.NextReportMs = now + crumb.ReportIntervalMs;
	}
	ReleaseSRWLockExclusive(&g_breadcrumbLock);

	for (std::int32_t i = 0; i < reportCount; i += 1) {
		const SPendingReport &report = reports[i];
		WriteFormatted("watchdog", "STILL RUNNING after %llums on thread t%lu  [%s] %s",
					   static_cast<unsigned long long>(report.AgeMs), report.ThreadId,
					   report.pCategory != nullptr ? report.pCategory : "-", report.szLabel);
	}

	if (!bAnyPastHangThreshold) {
		return;
	}

	bool expected = false;
	if (!g_bHangDumpWritten.compare_exchange_strong(expected, true)) {
		return; // one dump per run is the point - see kHangDumpMs
	}
	WriteLine("watchdog", "past the hang threshold - writing a diagnostic minidump of every thread");
	const std::string dumpPath = WideToUtf8(WriteDiagnosticDump(L"hang"));
	if (dumpPath.empty()) {
		WriteLine("watchdog", "diagnostic minidump FAILED to write");
	} else {
		WriteFormatted("watchdog", "diagnostic minidump written: %s", dumpPath.c_str());
	}
}

void ScanUiThread()
{
	const std::uint64_t lastAlive = g_lastUiAliveMs.load(std::memory_order_relaxed);
	if (lastAlive == 0) {
		return; // the render thread hasn't reached its main loop yet - nothing to compare against
	}

	const std::uint64_t sinceMs = NowMs() - lastAlive;
	if (sinceMs >= kUiStallMs) {
		// Latched: a frozen UI would otherwise say so on every single scan for as long as it
		// stays frozen. The recovery line below is what un-latches it.
		bool expected = false;
		if (g_bUiStallReported.compare_exchange_strong(expected, true)) {
			WriteFormatted("watchdog", "UI THREAD STALLED - no frame for %llums (the whole app is frozen, not just a "
									   "worker)",
						   static_cast<unsigned long long>(sinceMs));
		}
		return;
	}

	bool expected = true;
	if (g_bUiStallReported.compare_exchange_strong(expected, false)) {
		WriteLine("watchdog", "UI thread recovered - frames are being drawn again");
	}
}

DWORD WINAPI WatchdogMain(LPVOID)
{
	for (;;) {
		if (WaitForSingleObject(g_hWatchdogStop, static_cast<DWORD>(kWatchdogScanIntervalMs)) == WAIT_OBJECT_0) {
			return 0;
		}
		ScanBreadcrumbs();
		ScanUiThread();
	}
}

} // namespace

namespace DebugLog {

void Init()
{
	bool expected = false;
	if (!g_bInitialized.compare_exchange_strong(expected, true)) {
		return;
	}

	g_startTicks = NowMs();

	// On in a Debug build without asking; in any other build only when explicitly switched on,
	// so a shipped Release doesn't quietly write a log file forever - see debug_log.h's own
	// header comment on why the code is still compiled into it either way.
	const bool bForced = GetEnvironmentVariableW(L"RIFT_DEBUG_LOG", nullptr, 0) != 0;
	if (!kIsDebugBuild && !bForced) {
		return;
	}

	const std::wstring dir = LogDirectory();
	if (!dir.empty()) {
		const std::wstring path = dir + L"\\rift-debug.log";
		// Last run's log is what a user still has after force-killing a hung process and
		// relaunching - keep exactly one, so the interesting run isn't destroyed by the
		// relaunch that goes on to report it.
		const std::wstring previousPath = dir + L"\\rift-debug.prev.log";
		MoveFileExW(path.c_str(), previousPath.c_str(), MOVEFILE_REPLACE_EXISTING);

		// _wfsopen with _SH_DENYWR, not _wfopen: the ordinary open takes an exclusive lock, and
		// the single most useful thing to do with this file is read it - tail it, open it in an
		// editor, copy it out - WHILE the run that is hanging is still hung. Denying only other
		// writers keeps that possible without letting a second instance interleave into it.
		FILE *pFile = _wfsopen(path.c_str(), L"wb", _SH_DENYWR);
		if (pFile != nullptr) {
			g_pFile = pFile;
			g_filePath = WideToUtf8(path);
		}
	}

	g_bEnabled.store(true, std::memory_order_release);

	WriteFormatted("app", "Rift %s%s - diagnostic log started", kAppVersion, kIsDebugBuild ? " [debug]" : " [release]");

	g_hWatchdogStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (g_hWatchdogStop != nullptr) {
		g_hWatchdogThread = CreateThread(nullptr, 0, WatchdogMain, nullptr, 0, nullptr);
	}
	if (g_hWatchdogThread == nullptr) {
		WriteLine("app", "watchdog thread could not be started - stuck-call reporting is off for this run");
	}
}

void Shutdown()
{
	if (!g_bEnabled.load(std::memory_order_acquire)) {
		return;
	}

	WriteLine("app", "diagnostic log stopped");

	if (g_hWatchdogStop != nullptr) {
		SetEvent(g_hWatchdogStop);
	}
	if (g_hWatchdogThread != nullptr) {
		// Bounded like every other join in this project (see core/thread_util.h): the watchdog
		// can be mid-minidump, which is slow but finite, and hanging shutdown on the one thread
		// whose entire job is diagnosing hangs would be a poor joke.
		WaitForSingleObject(g_hWatchdogThread, 5000);
		CloseHandle(g_hWatchdogThread);
		g_hWatchdogThread = nullptr;
	}
	if (g_hWatchdogStop != nullptr) {
		CloseHandle(g_hWatchdogStop);
		g_hWatchdogStop = nullptr;
	}

	g_bEnabled.store(false, std::memory_order_release);

	AcquireSRWLockExclusive(&g_writeLock);
	if (g_pFile != nullptr) {
		std::fclose(g_pFile);
		g_pFile = nullptr;
	}
	ReleaseSRWLockExclusive(&g_writeLock);
}

bool IsEnabled()
{
	return g_bEnabled.load(std::memory_order_acquire);
}

const char *GetFilePath()
{
	return g_filePath.c_str();
}

void Write(const char *pCategory, const char *pFormat, ...)
{
	if (!g_bEnabled.load(std::memory_order_acquire)) {
		return;
	}

	char message[1024];
	va_list args;
	va_start(args, pFormat);
	_vsnprintf_s(message, sizeof(message), _TRUNCATE, pFormat, args);
	va_end(args);

	WriteLine(pCategory, message);
}

void MarkUiThreadAlive()
{
	// Unconditional (not gated on g_bEnabled): NowMs() is a single counter read, and gating it
	// would mean the first scan after a mid-run enable saw a stale zero and cried stall.
	g_lastUiAliveMs.store(NowMs(), std::memory_order_relaxed);
}

CScope::CScope(const char *pCategory, const char *pFormat, ...) : m_pCategory(pCategory)
{
	if (!g_bEnabled.load(std::memory_order_acquire)) {
		return;
	}

	va_list args;
	va_start(args, pFormat);
	_vsnprintf_s(m_szLabel, sizeof(m_szLabel), _TRUNCATE, pFormat, args);
	va_end(args);

	m_startMs = NowMs();

	AcquireSRWLockExclusive(&g_breadcrumbLock);
	for (std::int32_t i = 0; i < kMaxBreadcrumbs; i += 1) {
		if (g_breadcrumbs[i].bInUse) {
			continue;
		}
		g_breadcrumbs[i].bInUse = true;
		g_breadcrumbs[i].ThreadId = GetCurrentThreadId();
		g_breadcrumbs[i].StartMs = m_startMs;
		g_breadcrumbs[i].ReportIntervalMs = kFirstReportMs;
		g_breadcrumbs[i].NextReportMs = m_startMs + kFirstReportMs;
		g_breadcrumbs[i].pCategory = pCategory;
		std::memcpy(g_breadcrumbs[i].szLabel, m_szLabel, sizeof(m_szLabel));
		m_slot = i;
		break;
	}
	ReleaseSRWLockExclusive(&g_breadcrumbLock);
}

CScope::~CScope()
{
	if (m_slot >= 0) {
		AcquireSRWLockExclusive(&g_breadcrumbLock);
		g_breadcrumbs[m_slot].bInUse = false;
		ReleaseSRWLockExclusive(&g_breadcrumbLock);
	}

	if (m_startMs == 0 || !g_bEnabled.load(std::memory_order_acquire)) {
		return;
	}

	const std::uint64_t elapsedMs = NowMs() - m_startMs;
	if (elapsedMs < kSlowCallMs) {
		return; // the ordinary case - see kSlowCallMs
	}

	WriteFormatted(m_pCategory, "slow: %s took %llums", m_szLabel, static_cast<unsigned long long>(elapsedMs));
}

std::uint64_t CScope::ElapsedMs() const
{
	return m_startMs == 0 ? 0 : NowMs() - m_startMs;
}

} // namespace DebugLog
