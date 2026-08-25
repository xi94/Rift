#include "core/ui_automation.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>
#include <vector>

#include <TlHelp32.h>

#include "core/debug_log.h"

namespace {
using Microsoft::WRL::ComPtr;

// Every diagnostic line out of this file shares one category tag - see core/debug_log.h.
constexpr const char *kLogCategory = "uia";

// Shared by every Wait* method below - how often a poll re-checks after a miss. Short enough
// that a control appearing "as soon as it can" doesn't feel like an extra stall, long enough
// not to burn a whole CPU core spinning on IUIAutomation calls, which are not cheap.
constexpr std::uint32_t kPollIntervalMs = 100;

// A window's owning process launched by CreateProcessW is very often not the process that ends
// up owning the app's real, visible window - a bootstrapper that launches the real app as a
// child and exits, or (the Riot Client's own case, confirmed against a real install) a
// multi-process Electron/Chromium app where the launched process is a lightweight parent and
// only one of several child/grandchild processes it spawns (the "browser" process; the rest -
// gpu-process, renderer, crashpad-handler, ... - own no window at all) is the one with a window.
// Searching every descendant of rootProcessId, not just rootProcessId itself, is what makes
// FindTopLevelWindow actually work against apps shaped like that instead of only the simplest
// single-process case. A fresh snapshot every call, not cached, since a process tree can change
// (a crashed/relaunched subprocess) for as long as a caller keeps polling.
std::vector<DWORD> CollectDescendantProcessIds(DWORD rootProcessId)
{
	std::vector<DWORD> result{rootProcessId};

	const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) {
		return result;
	}

	std::vector<std::pair<DWORD, DWORD>> parentChildPairs; // (parentProcessId, processId)
	PROCESSENTRY32W entry{.dwSize = sizeof(entry)};
	if (Process32FirstW(snapshot, &entry)) {
		do {
			parentChildPairs.emplace_back(entry.th32ParentProcessID, entry.th32ProcessID);
		} while (Process32NextW(snapshot, &entry));
	}
	CloseHandle(snapshot);

	// Repeated passes over the same fixed snapshot until one adds nothing new - simple BFS
	// outward from the root; the process tree from one snapshot is finite, so this always
	// terminates.
	bool addedAny = true;
	while (addedAny) {
		addedAny = false;
		for (const auto &[parentProcessId, processId] : parentChildPairs) {
			const bool parentInSet = std::find(result.begin(), result.end(), parentProcessId) != result.end();
			const bool alreadyInSet = std::find(result.begin(), result.end(), processId) != result.end();
			if (parentInSet && !alreadyInSet) {
				result.push_back(processId);
				addedAny = true;
			}
		}
	}
	return result;
}

// EnumWindows callback state for CUiAutomation::FindTopLevelWindow.
struct FindWindowState {
	const std::vector<DWORD> *pCandidateProcessIds;
	HWND Result;
};

BOOL CALLBACK FindTopLevelWindowProc(HWND hWnd, LPARAM lParam)
{
	auto *pState = reinterpret_cast<FindWindowState *>(lParam);

	DWORD windowProcessId = 0;
	GetWindowThreadProcessId(hWnd, &windowProcessId);
	if (std::find(pState->pCandidateProcessIds->begin(), pState->pCandidateProcessIds->end(), windowProcessId) ==
		pState->pCandidateProcessIds->end()) {
		return TRUE; // keep enumerating
	}

	// Top-level and actually shown - GetWindow(hWnd, GW_OWNER) rules out an owned dialog/
	// tooltip (those aren't "the" window a caller means), IsWindowVisible rules out a hidden
	// tool/message-only window some frameworks create before their real UI is ready.
	if (GetWindow(hWnd, GW_OWNER) != nullptr || !IsWindowVisible(hWnd)) {
		return TRUE;
	}

	// Implausibly small to be a real UI window - some Chromium-derived helper processes
	// create a tiny/zero-size window for internal purposes even though they're not the app's
	// real window either.
	RECT rect;
	if (GetWindowRect(hWnd, &rect) && (rect.right - rect.left) < 50) {
		return TRUE;
	}

	pState->Result = hWnd;
	return FALSE; // found it, stop enumerating
}

// How long one cross-process lookup gets before it is written off as never coming back. The
// numbers this sits between are real, from a captured log of the hang this exists for: an
// ordinary ElementFromHandle/FindFirst against a settled Riot Client comes back in well under
// 250ms, while the one that wedged was still inside the provider 39 seconds later. Anything in
// between is arbitrary; five seconds is comfortably past the slowest legitimate first contact
// (a cold Electron client building its whole accessibility tree) and still leaves room for the
// retry that CRiotClient's own poll loops perform on the next iteration.
constexpr auto kUiaCallTimeout = std::chrono::milliseconds(5000);

// How many abandoned lookups one CUiAutomation tolerates before it declares the target wedged
// and fails every later lookup instantly (see CUiAutomation::HasWedged). Each abandoned lookup
// permanently strands one OS thread inside the provider, so this is really "how many threads is
// one login attempt allowed to leak" - and a provider that has already failed to answer twice
// in a row is not about to start. Two, times one instance per attempt, is a bounded cost;
// letting the poll loops keep spawning them for their full timeout would not be.
constexpr std::uint32_t kMaxAbandonedCalls = 2;

// The out-parameter of one bounded lookup, heap-allocated and co-owned by the caller and the
// throwaway thread - the same ownership shape SLoginAttemptState uses (see
// core/login_attempt.h), and for the same reason: an abandoned thread that finally returns,
// minutes later, must write into memory it still owns rather than into a frame that is long
// gone.
struct SBoundedLookupResult {
	ComPtr<IUIAutomationElement> Element;
};

// Runs one cross-process UI Automation lookup on a throwaway thread and waits kUiaCallTimeout
// for it. Returns the element it found (or nullptr), and sets bOutAbandoned if the thread had
// to be let go still running.
//
// The throwaway thread joins the MTA itself: apartment membership is a property of the OS
// thread, not of the interface pointer, and an MTA object used from a thread that never
// initialised COM is undefined behaviour. It is a genuinely cheap call now that
// CUiAutomation::KeepProcessMtaAlive holds the apartment open - before that, this would have
// been building and tearing down the whole MTA on every poll iteration, which is precisely the
// bug that function exists to prevent.
//
// fn is copied into the thread, so whatever it captures (the IUIAutomation, the root element)
// is captured by value and keeps its own reference alive - an abandoned thread can therefore
// never touch a released interface.
template <typename TFunc>
ComPtr<IUIAutomationElement> RunBoundedLookup(const char *pLabel, TFunc fn, bool &bOutAbandoned)
{
	auto pResult = std::make_shared<SBoundedLookupResult>();
	std::thread worker([pResult, fn]() {
		const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		fn(pResult->Element);
		if (comResult == S_OK || comResult == S_FALSE) {
			CoUninitialize();
		}
	});

	const DebugLog::CScope scope(kLogCategory, "%s", pLabel);
	const auto waitResult =
		WaitForSingleObject(worker.native_handle(), static_cast<DWORD>(kUiaCallTimeout.count()));
	if (waitResult == WAIT_OBJECT_0) {
		worker.join(); // already finished - this is instantaneous
		bOutAbandoned = false;
		return std::move(pResult->Element);
	}

	worker.detach(); // wedged inside the provider; nothing brings it back
	bOutAbandoned = true;
	DebugLog::Write(kLogCategory, "ABANDONED %s after %llums - the provider never answered", pLabel,
					static_cast<unsigned long long>(kUiaCallTimeout.count()));
	return nullptr;
}

// A VARIANT holding a copy of pText, freed automatically by VariantClear going out of scope -
// every IUIAutomation property-condition/value call below needs one of these.
struct AutoVariantString {
	VARIANT Value;

	explicit AutoVariantString(const wchar_t *pText)
	{
		VariantInit(&Value);
		Value.vt = VT_BSTR;
		Value.bstrVal = SysAllocString(pText);
	}

	~AutoVariantString()
	{
		VariantClear(&Value);
	}

	AutoVariantString(const AutoVariantString &) = delete;
	AutoVariantString &operator=(const AutoVariantString &) = delete;
};
} // namespace

CUiElement::CUiElement(ComPtr<IUIAutomationElement> pElement) : m_pElement(std::move(pElement))
{
}

bool CUiElement::SetValue(const wchar_t *pText) const
{
	if (!IsValid()) {
		return false;
	}

	ComPtr<IUIAutomationValuePattern> pValuePattern;
	if (FAILED(m_pElement->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&pValuePattern))) ||
		pValuePattern == nullptr) {
		DebugLog::Write(kLogCategory, "SetValue: element has no ValuePattern - caller falls back to keystrokes");
		return false;
	}

	// Deliberately never logs pText - this is the call the account password goes through.
	const DebugLog::CScope scope(kLogCategory, "ValuePattern::SetValue");
	BSTR bstrText = SysAllocString(pText);
	const HRESULT hr = pValuePattern->SetValue(bstrText);
	SysFreeString(bstrText);
	if (FAILED(hr)) {
		// E_ACCESSDENIED here is the classic sign of the target running elevated while this
		// process doesn't - UIPI blocks the write, and the keystroke fallback is blocked for
		// the same reason.
		DebugLog::Write(kLogCategory, "ValuePattern::SetValue FAILED hr=0x%08lX", static_cast<unsigned long>(hr));
	}
	return SUCCEEDED(hr);
}

bool CUiElement::Invoke() const
{
	if (!IsValid()) {
		return false;
	}

	ComPtr<IUIAutomationInvokePattern> pInvokePattern;
	if (FAILED(m_pElement->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&pInvokePattern))) ||
		pInvokePattern == nullptr) {
		return false;
	}

	const DebugLog::CScope scope(kLogCategory, "InvokePattern::Invoke");
	const HRESULT hr = pInvokePattern->Invoke();
	if (FAILED(hr)) {
		DebugLog::Write(kLogCategory, "InvokePattern::Invoke FAILED hr=0x%08lX", static_cast<unsigned long>(hr));
	}
	return SUCCEEDED(hr);
}

bool CUiElement::SetFocus() const
{
	if (!IsValid()) {
		return false;
	}

	const DebugLog::CScope scope(kLogCategory, "IUIAutomationElement::SetFocus");
	const HRESULT hr = m_pElement->SetFocus();
	if (FAILED(hr)) {
		DebugLog::Write(kLogCategory, "SetFocus FAILED hr=0x%08lX", static_cast<unsigned long>(hr));
	}
	return SUCCEEDED(hr);
}

bool CUiElement::HasKeyboardFocus() const
{
	if (!IsValid()) {
		return false;
	}
	BOOL hasFocus = FALSE;
	return SUCCEEDED(m_pElement->get_CurrentHasKeyboardFocus(&hasFocus)) && hasFocus != FALSE;
}

bool CUiElement::GetName(std::wstring &outName) const
{
	if (!IsValid()) {
		return false;
	}

	BSTR bstrName = nullptr;
	if (FAILED(m_pElement->get_CurrentName(&bstrName)) || bstrName == nullptr) {
		return false;
	}
	outName.assign(bstrName, SysStringLen(bstrName));
	SysFreeString(bstrName);
	return true;
}

bool CUiElement::GetAutomationId(std::wstring &outAutomationId) const
{
	if (!IsValid()) {
		return false;
	}

	BSTR bstrId = nullptr;
	if (FAILED(m_pElement->get_CurrentAutomationId(&bstrId)) || bstrId == nullptr) {
		return false;
	}
	outAutomationId.assign(bstrId, SysStringLen(bstrId));
	SysFreeString(bstrId);
	return true;
}

CUiAutomation::~CUiAutomation()
{
	Shutdown();
}

void CUiAutomation::KeepProcessMtaAlive()
{
	// Nothing in this process lives in the MTA permanently - every CUiAutomation is created on
	// a short-lived worker thread that joins the MTA in Init() and leaves it again in
	// Shutdown() (see this class's own file comment on why it has to be per-thread). The
	// catch is that the multi-threaded apartment itself only exists while at least one thread
	// is in it: the last CoUninitialize tears the whole apartment down, unloading COM's own
	// per-apartment state and UIAutomationCore's along with it, and the next attempt then
	// builds all of it again from scratch. Repeated login attempts therefore repeatedly create
	// and destroy the MTA, and a CoCreateInstance that starts while the previous teardown is
	// still finishing is exactly the "spamming UI Automation initialisation goes wrong"
	// failure this is here to remove.
	//
	// CoIncrementMTAUsage holds the apartment open for the rest of the process without
	// joining any thread to it (which is why it can be called from the render thread, which
	// must NOT become an MTA member), so every later CoInitializeEx/CoUninitialize pair is a
	// cheap refcount change against an apartment that already exists and never goes away. The
	// cookie is deliberately never released: "for the lifetime of the process" is the point.
	static CO_MTA_USAGE_COOKIE s_cookie = nullptr;
	if (s_cookie != nullptr) {
		return;
	}

	const HRESULT hr = CoIncrementMTAUsage(&s_cookie);
	DebugLog::Write(kLogCategory, "CoIncrementMTAUsage hr=0x%08lX (process-wide MTA %s)", static_cast<unsigned long>(hr),
					SUCCEEDED(hr) ? "held open" : "NOT held - apartment will be torn down between attempts");
}

bool CUiAutomation::Init()
{
	if (m_pAutomation != nullptr) {
		return true; // idempotent - calling Init() twice on the same instance/thread is harmless
	}
	if (m_bComInitialized) {
		// A previous Init() on this instance got its COM reference but failed to create the
		// interface. Balancing that reference is Shutdown()'s job; taking a second one here
		// would leak it, and this thread is already in the apartment either way.
		DebugLog::Write(kLogCategory, "Init retried after a failed one - reusing the existing COM reference");
	} else {
		// S_OK: this call is the one that joined this thread to the MTA. S_FALSE: this thread
		// was already in a compatible apartment. Either way this thread now holds a
		// CoInitializeEx reference Shutdown() must balance with CoUninitialize.
		// RPC_E_CHANGED_MODE means this thread already called
		// CoInitializeEx(COINIT_APARTMENTTHREADED) itself before this ran - nothing to balance
		// (and nothing this class can do about it), but UI Automation still generally works
		// against whatever apartment is already active, so this isn't treated as a hard
		// failure on its own.
		const DebugLog::CScope comScope(kLogCategory, "CoInitializeEx(COINIT_MULTITHREADED)");
		const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		m_bComInitialized = comResult == S_OK || comResult == S_FALSE;
		DebugLog::Write(kLogCategory, "CoInitializeEx hr=0x%08lX (%s)", static_cast<unsigned long>(comResult),
						comResult == S_OK	   ? "joined the MTA"
						: comResult == S_FALSE ? "already in a compatible apartment"
											   : "NOT initialised by us");
	}

	// The one call in this whole flow that has actually been suspected of hanging when login
	// attempts come in quick succession - scoped so the watchdog names it if it ever does.
	const DebugLog::CScope createScope(kLogCategory, "CoCreateInstance(CLSID_CUIAutomation)");
	const HRESULT hr =
		CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_pAutomation));
	DebugLog::Write(kLogCategory, "CoCreateInstance(CLSID_CUIAutomation) hr=0x%08lX after %llums",
					static_cast<unsigned long>(hr), static_cast<unsigned long long>(createScope.ElapsedMs()));
	return SUCCEEDED(hr);
}

void CUiAutomation::Shutdown()
{
	if (m_pAutomation == nullptr && !m_bComInitialized) {
		return;
	}

	// Both halves are scoped separately because they fail differently: releasing the last
	// reference to a cross-process UI Automation proxy is a call into the target, and
	// CoUninitialize can block draining the apartment's own RPC work - and before
	// KeepProcessMtaAlive existed, the last one out also tore the whole MTA down.
	{
		const DebugLog::CScope scope(kLogCategory, "release IUIAutomation");
		m_pAutomation.Reset();
	}
	if (m_bComInitialized) {
		const DebugLog::CScope scope(kLogCategory, "CoUninitialize");
		CoUninitialize();
		m_bComInitialized = false;
	}
}

CUiElement CUiAutomation::FinishBoundedLookup(ComPtr<IUIAutomationElement> pFound, bool bAbandoned) const
{
	if (!bAbandoned) {
		return pFound != nullptr ? CUiElement{std::move(pFound)} : CUiElement{};
	}

	m_abandonedCallCount += 1;
	if (m_abandonedCallCount >= kMaxAbandonedCalls && !m_bWedged) {
		m_bWedged = true;
		DebugLog::Write(kLogCategory,
						"GIVING UP on this target - %u lookup(s) abandoned; every later one now fails immediately",
						m_abandonedCallCount);
	}
	return CUiElement{};
}

HWND CUiAutomation::FindTopLevelWindow(std::uint32_t processId)
{
	const std::vector<DWORD> candidateProcessIds = CollectDescendantProcessIds(static_cast<DWORD>(processId));
	FindWindowState state{.pCandidateProcessIds = &candidateProcessIds, .Result = nullptr};
	EnumWindows(FindTopLevelWindowProc, reinterpret_cast<LPARAM>(&state));
	return state.Result;
}

CUiElement CUiAutomation::WaitForWindowByProcessId(std::uint32_t processId, std::uint32_t timeoutMs) const
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	for (;;) {
		const HWND hWnd = FindTopLevelWindow(processId);
		if (hWnd != nullptr) {
			CUiElement element = ElementFromWindow(hWnd);
			if (element.IsValid()) {
				return element;
			}
		}

		if (std::chrono::steady_clock::now() >= deadline) {
			return CUiElement{};
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
	}
}

HWND CUiAutomation::FindWindowByName(const wchar_t *pTitle)
{
	return FindWindowW(nullptr, pTitle);
}

CUiElement CUiAutomation::WaitForWindowByName(const wchar_t *pTitle, std::uint32_t timeoutMs) const
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	for (;;) {
		const HWND hWnd = FindWindowByName(pTitle);
		if (hWnd != nullptr) {
			CUiElement element = ElementFromWindow(hWnd);
			if (element.IsValid()) {
				return element;
			}
		}

		if (std::chrono::steady_clock::now() >= deadline) {
			return CUiElement{};
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
	}
}

CUiElement CUiAutomation::ElementFromWindow(HWND hWnd) const
{
	if (m_pAutomation == nullptr || hWnd == nullptr || m_bWedged) {
		return CUiElement{};
	}

	// The call that has to wake the target's accessibility provider up - a Chromium/Electron
	// client builds its whole accessibility tree on first contact - and the one confirmed, in a
	// captured log, to have sat inside the provider for 39 seconds while the login worker had
	// no way out of it. Bounded for exactly that reason.
	char label[64];
	_snprintf_s(label, _TRUNCATE, "ElementFromHandle(hwnd=0x%p)", hWnd);

	bool bAbandoned = false;
	auto pFound = RunBoundedLookup(
		label,
		[pAutomation = m_pAutomation, hWnd](ComPtr<IUIAutomationElement> &outElement) {
			pAutomation->ElementFromHandle(hWnd, &outElement);
		},
		bAbandoned);
	return FinishBoundedLookup(std::move(pFound), bAbandoned);
}

CUiElement CUiAutomation::FindFirstDescendantByAutomationId(const CUiElement &root, const wchar_t *pAutomationId) const
{
	if (m_pAutomation == nullptr || !root.IsValid() || m_bWedged) {
		return CUiElement{};
	}

	const AutoVariantString value(pAutomationId);
	ComPtr<IUIAutomationCondition> pCondition;
	if (FAILED(m_pAutomation->CreatePropertyCondition(UIA_AutomationIdPropertyId, value.Value, &pCondition)) ||
		pCondition == nullptr) {
		return CUiElement{};
	}

	// TreeScope_Descendants against another process's whole UI tree, walked node by node over
	// COM - the most expensive call this class makes, and one more place a wedged target could
	// strand a caller. Bounded and breadcrumbed like every other lookup here.
	char label[192];
	_snprintf_s(label, _TRUNCATE, "FindFirst(AutomationId=%ls)", pAutomationId);

	bool bAbandoned = false;
	auto pFound = RunBoundedLookup(
		label,
		[pRoot = root.Get(), pCondition](ComPtr<IUIAutomationElement> &outElement) {
			pRoot->FindFirst(TreeScope_Descendants, pCondition.Get(), &outElement);
		},
		bAbandoned);
	return FinishBoundedLookup(std::move(pFound), bAbandoned);
}

CUiElement CUiAutomation::FindFirstDescendantByName(const CUiElement &root, const wchar_t *pName) const
{
	if (m_pAutomation == nullptr || !root.IsValid() || m_bWedged) {
		return CUiElement{};
	}

	const AutoVariantString value(pName);
	ComPtr<IUIAutomationCondition> pCondition;
	if (FAILED(m_pAutomation->CreatePropertyCondition(UIA_NamePropertyId, value.Value, &pCondition)) ||
		pCondition == nullptr) {
		return CUiElement{};
	}

	char label[192];
	_snprintf_s(label, _TRUNCATE, "FindFirst(Name=%ls)", pName);

	bool bAbandoned = false;
	auto pFound = RunBoundedLookup(
		label,
		[pRoot = root.Get(), pCondition](ComPtr<IUIAutomationElement> &outElement) {
			pRoot->FindFirst(TreeScope_Descendants, pCondition.Get(), &outElement);
		},
		bAbandoned);
	return FinishBoundedLookup(std::move(pFound), bAbandoned);
}

CUiElement CUiAutomation::FindFirstDescendantByControlType(const CUiElement &root, CONTROLTYPEID controlType) const
{
	if (m_pAutomation == nullptr || !root.IsValid() || m_bWedged) {
		return CUiElement{};
	}

	VARIANT value;
	VariantInit(&value);
	value.vt = VT_I4;
	value.lVal = controlType;

	ComPtr<IUIAutomationCondition> pCondition;
	if (FAILED(m_pAutomation->CreatePropertyCondition(UIA_ControlTypePropertyId, value, &pCondition)) ||
		pCondition == nullptr) {
		return CUiElement{};
	}

	char label[64];
	_snprintf_s(label, _TRUNCATE, "FindFirst(ControlType=%d)", static_cast<int>(controlType));

	bool bAbandoned = false;
	auto pFound = RunBoundedLookup(
		label,
		[pRoot = root.Get(), pCondition](ComPtr<IUIAutomationElement> &outElement) {
			pRoot->FindFirst(TreeScope_Descendants, pCondition.Get(), &outElement);
		},
		bAbandoned);
	return FinishBoundedLookup(std::move(pFound), bAbandoned);
}

CUiElement CUiAutomation::FindFirstDescendantByNameAndControlType(const CUiElement &root, const wchar_t *pName,
																   CONTROLTYPEID controlType) const
{
	if (m_pAutomation == nullptr || !root.IsValid() || m_bWedged) {
		return CUiElement{};
	}

	const AutoVariantString nameValue(pName);
	ComPtr<IUIAutomationCondition> pNameCondition;
	if (FAILED(m_pAutomation->CreatePropertyCondition(UIA_NamePropertyId, nameValue.Value, &pNameCondition)) ||
		pNameCondition == nullptr) {
		return CUiElement{};
	}

	VARIANT typeValue;
	VariantInit(&typeValue);
	typeValue.vt = VT_I4;
	typeValue.lVal = controlType;
	ComPtr<IUIAutomationCondition> pTypeCondition;
	if (FAILED(m_pAutomation->CreatePropertyCondition(UIA_ControlTypePropertyId, typeValue, &pTypeCondition)) ||
		pTypeCondition == nullptr) {
		return CUiElement{};
	}

	ComPtr<IUIAutomationCondition> pAndCondition;
	if (FAILED(m_pAutomation->CreateAndCondition(pNameCondition.Get(), pTypeCondition.Get(), &pAndCondition)) ||
		pAndCondition == nullptr) {
		return CUiElement{};
	}

	char label[192];
	_snprintf_s(label, _TRUNCATE, "FindFirst(Name=%ls, ControlType=%d)", pName, static_cast<int>(controlType));

	bool bAbandoned = false;
	auto pFound = RunBoundedLookup(
		label,
		[pRoot = root.Get(), pAndCondition](ComPtr<IUIAutomationElement> &outElement) {
			pRoot->FindFirst(TreeScope_Descendants, pAndCondition.Get(), &outElement);
		},
		bAbandoned);
	return FinishBoundedLookup(std::move(pFound), bAbandoned);
}

CUiElement CUiAutomation::WaitForDescendantByAutomationId(const CUiElement &root, const wchar_t *pAutomationId,
														  std::uint32_t timeoutMs) const
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	for (;;) {
		CUiElement found = FindFirstDescendantByAutomationId(root, pAutomationId);
		if (found.IsValid()) {
			return found;
		}
		if (std::chrono::steady_clock::now() >= deadline) {
			return CUiElement{};
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
	}
}

CUiElement CUiAutomation::WaitForDescendantByName(const CUiElement &root, const wchar_t *pName,
												  std::uint32_t timeoutMs) const
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	for (;;) {
		CUiElement found = FindFirstDescendantByName(root, pName);
		if (found.IsValid()) {
			return found;
		}
		if (std::chrono::steady_clock::now() >= deadline) {
			return CUiElement{};
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
	}
}

CUiElement CUiAutomation::WaitForDescendantByControlType(const CUiElement &root, CONTROLTYPEID controlType,
														 std::uint32_t timeoutMs) const
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	for (;;) {
		CUiElement found = FindFirstDescendantByControlType(root, controlType);
		if (found.IsValid()) {
			return found;
		}
		if (std::chrono::steady_clock::now() >= deadline) {
			return CUiElement{};
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
	}
}

CUiElement CUiAutomation::WaitForDescendantByNameAndControlType(const CUiElement &root, const wchar_t *pName,
																 CONTROLTYPEID controlType,
																 std::uint32_t timeoutMs) const
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	for (;;) {
		CUiElement found = FindFirstDescendantByNameAndControlType(root, pName, controlType);
		if (found.IsValid()) {
			return found;
		}
		if (std::chrono::steady_clock::now() >= deadline) {
			return CUiElement{};
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
	}
}

void CUiAutomation::SendKeystrokes(const wchar_t *pText) const
{
	// Counted rather than logged per character, and never logging the text itself - this is the
	// path an account password takes when a field doesn't support ValuePattern. A non-zero
	// rejected count means SendInput was refused outright, which in practice means UIPI blocked
	// it (the target runs elevated and this process doesn't) or the secure desktop was up -
	// either way the credentials went nowhere, and the attempt then looks like an unexplained
	// timeout several steps further down with nothing to connect it back to here.
	std::uint32_t sent = 0;
	std::uint32_t rejected = 0;
	for (const wchar_t *pChar = pText; *pChar != L'\0'; pChar += 1) {
		INPUT input[2]{};
		input[0].type = INPUT_KEYBOARD;
		input[0].ki.wScan = static_cast<WORD>(*pChar);
		input[0].ki.dwFlags = KEYEVENTF_UNICODE;
		input[1] = input[0];
		input[1].ki.dwFlags |= KEYEVENTF_KEYUP;
		if (SendInput(2, input, sizeof(INPUT)) == 2) {
			sent += 1;
		} else {
			rejected += 1;
		}
	}
	DebugLog::Write(kLogCategory, "SendKeystrokes: %u character(s) sent, %u rejected%s", sent, rejected,
					rejected != 0 ? " - SendInput was blocked (elevation/UIPI?)" : "");
}

void CUiAutomation::SendKey(WORD virtualKeyCode) const
{
	INPUT input[2]{};
	input[0].type = INPUT_KEYBOARD;
	input[0].ki.wVk = virtualKeyCode;
	input[1] = input[0];
	input[1].ki.dwFlags = KEYEVENTF_KEYUP;
	const UINT inserted = SendInput(2, input, sizeof(INPUT));
	// See SendKeystrokes above on what a refusal means. Worth its own line because this is the
	// VK_RETURN that actually submits the form: if it doesn't land, nothing does, and the
	// attempt quietly times out waiting for a result it never asked for.
	DebugLog::Write(kLogCategory, "SendKey(vk=0x%02X): %u of 2 event(s) inserted%s", virtualKeyCode, inserted,
					inserted != 2 ? " - SendInput was blocked (elevation/UIPI?)" : "");
}
