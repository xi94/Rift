#include "tray.h"

#include <shellapi.h>
#include <wchar.h>

namespace {
constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT_PTR kTrayIconId = 1;
constexpr UINT kMenuIdOpen = 1001;
constexpr UINT kMenuIdExit = 1002;
constexpr UINT kMenuIdQuickLoginBase = 2000;
constexpr UINT kMenuIdCopyPasswordBase = kMenuIdQuickLoginBase + kTrayMaxAccountItems;

const wchar_t *const kTrayWindowClassName = L"rift_tray_window_class";

// Valid only for the duration of ShowContextMenu's TrackPopupMenu call below - a
// Quick Login/Copy Password click delivers WM_COMMAND synchronously from inside that
// call, and this is how HandleMessage maps the command id back to an account without
// exposing transient state through CTray's own public surface.
const TrayAccountItem *g_pMenuItems = nullptr;
std::uint32_t g_nMenuItemCount = 0;

// UTF-8 -> UTF-16 for the Win32 menu APIs (AppendMenuW etc.), which only take wide
// strings - every label this file builds ultimately funnels through here.
void ToWide(const char *pUtf8, wchar_t *pOut, int outCapacity)
{
	const int written = MultiByteToWideChar(CP_UTF8, 0, pUtf8, -1, pOut, outCapacity);
	if (written <= 0) {
		pOut[0] = L'\0';
	}
}
} // namespace

void CTray::ShowContextMenu()
{
	POINT cursor;
	GetCursorPos(&cursor);

	TrayAccountItem items[kTrayMaxAccountItems];
	std::uint32_t itemCount = 0;
	if (m_pAccountListCallback != nullptr) {
		itemCount = m_pAccountListCallback(m_pAccountListCallbackUserData, items, kTrayMaxAccountItems);
	}

	const HMENU menu = CreatePopupMenu();
	AppendMenuW(menu, MF_STRING, kMenuIdOpen, L"Open Rift");
	AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

	// Grouped by banner as the items arrive - the account-list callback walks banners in
	// order and emits each one's accounts contiguously, so "BannerIndex changed from the
	// previous item" is enough to know a new game submenu is needed. Submenus created
	// here (gameMenu, accountMenu) are owned by `menu` once attached via MF_POPUP and
	// destroyed recursively by the DestroyMenu(menu) below - no separate cleanup loop
	// needed.
	HMENU gameMenu = nullptr;
	std::int32_t currentBannerIndex = -2; // never a real banner index; forces the first item to open a submenu
	for (std::uint32_t i = 0; i < itemCount; i += 1) {
		const TrayAccountItem &item = items[i];

		if (item.BannerIndex != currentBannerIndex) {
			gameMenu = CreatePopupMenu();
			wchar_t title[64];
			ToWide(item.BannerTitle, title, 64);
			AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(gameMenu), title);
			currentBannerIndex = item.BannerIndex;
		}

		wchar_t username[64];
		ToWide(item.Username, username, 64);

		const HMENU accountMenu = CreatePopupMenu();
		AppendMenuW(accountMenu, MF_STRING, kMenuIdQuickLoginBase + i, L"Login");
		AppendMenuW(accountMenu, MF_STRING, kMenuIdCopyPasswordBase + i, L"Copy Password");
		AppendMenuW(gameMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(accountMenu), username);
	}

	if (itemCount > 0) {
		AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
	}
	AppendMenuW(menu, MF_STRING, kMenuIdExit, L"Exit");

	g_pMenuItems = items;
	g_nMenuItemCount = itemCount;

	// Required so the menu dismisses correctly if the user clicks away from it.
	SetForegroundWindow(m_hWnd);
	TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, m_hWnd, nullptr);
	PostMessageW(m_hWnd, WM_NULL, 0, 0);

	g_pMenuItems = nullptr;
	g_nMenuItemCount = 0;

	DestroyMenu(menu);
}

LRESULT CTray::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message) {
		case kTrayCallbackMessage: {
			const auto mouseMessage = static_cast<UINT>(LOWORD(lParam));
			if (mouseMessage == WM_LBUTTONUP) {
				m_pendingEvent = ETrayEventType::TRAY_EVENT_SHOW_WINDOW;
			} else if (mouseMessage == WM_RBUTTONUP) {
				ShowContextMenu();
			}
			return 0;
		}

		case WM_COMMAND: {
			const UINT commandId = LOWORD(wParam);
			if (commandId == kMenuIdOpen) {
				m_pendingEvent = ETrayEventType::TRAY_EVENT_SHOW_WINDOW;
			} else if (commandId == kMenuIdExit) {
				m_pendingEvent = ETrayEventType::TRAY_EVENT_EXIT_REQUESTED;
			} else if (commandId >= kMenuIdQuickLoginBase && commandId < kMenuIdQuickLoginBase + kTrayMaxAccountItems) {
				const UINT index = commandId - kMenuIdQuickLoginBase;
				if (index < g_nMenuItemCount) {
					m_pendingEvent = ETrayEventType::TRAY_EVENT_QUICK_LOGIN;
					m_nPendingBannerIndex = g_pMenuItems[index].BannerIndex;
					m_nPendingAccountIndex = g_pMenuItems[index].AccountIndex;
				}
			} else if (commandId >= kMenuIdCopyPasswordBase &&
					   commandId < kMenuIdCopyPasswordBase + kTrayMaxAccountItems) {
				const UINT index = commandId - kMenuIdCopyPasswordBase;
				if (index < g_nMenuItemCount) {
					m_pendingEvent = ETrayEventType::TRAY_EVENT_COPY_PASSWORD;
					m_nPendingBannerIndex = g_pMenuItems[index].BannerIndex;
					m_nPendingAccountIndex = g_pMenuItems[index].AccountIndex;
				}
			}
			return 0;
		}

		default:
			return DefWindowProcW(m_hWnd, message, wParam, lParam);
	}
}

LRESULT CALLBACK CTray::WindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	CTray *pTray = nullptr;

	if (message == WM_NCCREATE) {
		const auto *pCreate = reinterpret_cast<const CREATESTRUCTW *>(lParam);
		pTray = static_cast<CTray *>(pCreate->lpCreateParams);
		SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pTray));
	} else {
		pTray = reinterpret_cast<CTray *>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
	}

	if (pTray == nullptr) {
		return DefWindowProcW(hWnd, message, wParam, lParam);
	}

	return pTray->HandleMessage(message, wParam, lParam);
}

bool CTray::Create(const wchar_t *pTooltip)
{
	const HINSTANCE instance = GetModuleHandleW(nullptr);

	WNDCLASSEXW windowClass{};
	windowClass.cbSize = sizeof(WNDCLASSEXW);
	windowClass.lpfnWndProc = WindowProc;
	windowClass.hInstance = instance;
	windowClass.lpszClassName = kTrayWindowClassName;
	RegisterClassExW(&windowClass);

	m_hWnd = CreateWindowExW(0, kTrayWindowClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
	if (m_hWnd == nullptr) {
		return false;
	}

	m_hIcon = LoadIconW(nullptr, IDI_APPLICATION);

	NOTIFYICONDATAW iconData{};
	iconData.cbSize = sizeof(NOTIFYICONDATAW);
	iconData.hWnd = m_hWnd;
	iconData.uID = kTrayIconId;
	iconData.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	iconData.uCallbackMessage = kTrayCallbackMessage;
	iconData.hIcon = m_hIcon;
	wcsncpy_s(iconData.szTip, pTooltip, _TRUNCATE);

	return Shell_NotifyIconW(NIM_ADD, &iconData) == TRUE;
}

CTray::~CTray()
{
	if (m_hWnd != nullptr) {
		NOTIFYICONDATAW iconData{};
		iconData.cbSize = sizeof(NOTIFYICONDATAW);
		iconData.hWnd = m_hWnd;
		iconData.uID = kTrayIconId;
		Shell_NotifyIconW(NIM_DELETE, &iconData);
		DestroyWindow(m_hWnd);
	}
}

void CTray::SetAccountListCallback(TrayAccountListCallback callback, void *pUserData)
{
	m_pAccountListCallback = callback;
	m_pAccountListCallbackUserData = pUserData;
}

ETrayEventType CTray::TakeEvent()
{
	const ETrayEventType event = m_pendingEvent;
	m_pendingEvent = ETrayEventType::TRAY_EVENT_NONE;
	return event;
}
