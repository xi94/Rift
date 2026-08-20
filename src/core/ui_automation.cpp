#include "core/ui_automation.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>
#include <vector>

#include <TlHelp32.h>

namespace {
using Microsoft::WRL::ComPtr;

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
		return false;
	}

	BSTR bstrText = SysAllocString(pText);
	const HRESULT hr = pValuePattern->SetValue(bstrText);
	SysFreeString(bstrText);
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

	return SUCCEEDED(pInvokePattern->Invoke());
}

bool CUiElement::SetFocus() const
{
	return IsValid() && SUCCEEDED(m_pElement->SetFocus());
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

bool CUiAutomation::Init()
{
	if (m_pAutomation != nullptr) {
		return true; // idempotent - calling Init() twice on the same instance/thread is harmless
	}

	// S_OK: this call is the one that joined this thread to the MTA. S_FALSE: this thread was
	// already in a compatible apartment. Either way this thread now holds a CoInitializeEx
	// reference Shutdown() must balance with CoUninitialize. RPC_E_CHANGED_MODE means this
	// thread already called CoInitializeEx(COINIT_APARTMENTTHREADED) itself before this ran -
	// nothing to balance (and nothing this class can do about it), but UI Automation still
	// generally works against whatever apartment is already active, so this isn't treated as a
	// hard failure on its own.
	const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	m_bComInitialized = comResult == S_OK || comResult == S_FALSE;

	const HRESULT hr =
		CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_pAutomation));
	return SUCCEEDED(hr);
}

void CUiAutomation::Shutdown()
{
	m_pAutomation.Reset();
	if (m_bComInitialized) {
		CoUninitialize();
		m_bComInitialized = false;
	}
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
	if (m_pAutomation == nullptr || hWnd == nullptr) {
		return CUiElement{};
	}

	ComPtr<IUIAutomationElement> pElement;
	if (FAILED(m_pAutomation->ElementFromHandle(hWnd, &pElement)) || pElement == nullptr) {
		return CUiElement{};
	}

	return CUiElement{std::move(pElement)};
}

CUiElement CUiAutomation::FindFirstDescendantByAutomationId(const CUiElement &root, const wchar_t *pAutomationId) const
{
	if (m_pAutomation == nullptr || !root.IsValid()) {
		return CUiElement{};
	}

	const AutoVariantString value(pAutomationId);
	ComPtr<IUIAutomationCondition> pCondition;
	if (FAILED(m_pAutomation->CreatePropertyCondition(UIA_AutomationIdPropertyId, value.Value, &pCondition)) ||
		pCondition == nullptr) {
		return CUiElement{};
	}

	ComPtr<IUIAutomationElement> pFound;
	if (FAILED(root.Get()->FindFirst(TreeScope_Descendants, pCondition.Get(), &pFound)) || pFound == nullptr) {
		return CUiElement{};
	}
	return CUiElement{std::move(pFound)};
}

CUiElement CUiAutomation::FindFirstDescendantByName(const CUiElement &root, const wchar_t *pName) const
{
	if (m_pAutomation == nullptr || !root.IsValid()) {
		return CUiElement{};
	}

	const AutoVariantString value(pName);
	ComPtr<IUIAutomationCondition> pCondition;
	if (FAILED(m_pAutomation->CreatePropertyCondition(UIA_NamePropertyId, value.Value, &pCondition)) ||
		pCondition == nullptr) {
		return CUiElement{};
	}

	ComPtr<IUIAutomationElement> pFound;
	if (FAILED(root.Get()->FindFirst(TreeScope_Descendants, pCondition.Get(), &pFound)) || pFound == nullptr) {
		return CUiElement{};
	}

	return CUiElement{std::move(pFound)};
}

CUiElement CUiAutomation::FindFirstDescendantByControlType(const CUiElement &root, CONTROLTYPEID controlType) const
{
	if (m_pAutomation == nullptr || !root.IsValid()) {
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

	ComPtr<IUIAutomationElement> pFound;
	if (FAILED(root.Get()->FindFirst(TreeScope_Descendants, pCondition.Get(), &pFound)) || pFound == nullptr) {
		return CUiElement{};
	}
	return CUiElement{std::move(pFound)};
}

CUiElement CUiAutomation::FindFirstDescendantByNameAndControlType(const CUiElement &root, const wchar_t *pName,
																   CONTROLTYPEID controlType) const
{
	if (m_pAutomation == nullptr || !root.IsValid()) {
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

	ComPtr<IUIAutomationElement> pFound;
	if (FAILED(root.Get()->FindFirst(TreeScope_Descendants, pAndCondition.Get(), &pFound)) || pFound == nullptr) {
		return CUiElement{};
	}
	return CUiElement{std::move(pFound)};
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
	for (const wchar_t *pChar = pText; *pChar != L'\0'; pChar += 1) {
		INPUT input[2]{};
		input[0].type = INPUT_KEYBOARD;
		input[0].ki.wScan = static_cast<WORD>(*pChar);
		input[0].ki.dwFlags = KEYEVENTF_UNICODE;
		input[1] = input[0];
		input[1].ki.dwFlags |= KEYEVENTF_KEYUP;
		SendInput(2, input, sizeof(INPUT));
	}
}

void CUiAutomation::SendKey(WORD virtualKeyCode) const
{
	INPUT input[2]{};
	input[0].type = INPUT_KEYBOARD;
	input[0].ki.wVk = virtualKeyCode;
	input[1] = input[0];
	input[1].ki.dwFlags = KEYEVENTF_KEYUP;
	SendInput(2, input, sizeof(INPUT));
}
