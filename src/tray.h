#pragma once

// A tray icon backed by a hidden message-only window. Runs on the same thread as
// CWindow; CWindow::PumpMessages already dispatches messages for every window on this
// thread (PeekMessageW with a null hwnd filter), so CTray needs no pump of its own -
// main.cpp just calls TakeEvent after pumping to see what happened.
//
// The context menu is a quick-access account list: Open Rift, then one submenu
// per game with accounts, each holding a Login / Copy Password pair per account - not
// just Open/Exit.

#include <cstdint>

#include <Windows.h>

enum class ETrayEventType : std::uint8_t {
	TRAY_EVENT_NONE,
	TRAY_EVENT_SHOW_WINDOW,
	TRAY_EVENT_EXIT_REQUESTED,
	TRAY_EVENT_QUICK_LOGIN,	  // GetPendingBannerIndex/GetPendingAccountIndex identify the account
	TRAY_EVENT_COPY_PASSWORD, // same
};

// One row the tray menu can offer for quick login/copy. CTray only needs enough to label
// the menu (title, username) - the account's actual password is resolved by the caller
// (main.cpp, against the live CCarousel) once TRAY_EVENT_COPY_PASSWORD comes back, so it
// never has to pass through this class.
struct TrayAccountItem {
	char BannerTitle[64];
	char Username[64];
	std::int32_t BannerIndex;
	std::int32_t AccountIndex;
};

constexpr std::uint32_t kTrayMaxAccountItems = 64;

// Supplies the current account list for the tray's quick-access menu. Writes up to
// capacity items into pOutItems and returns how many were written.
//
// Invoked synchronously from inside CTray's context-menu handler (a right-click on the
// icon), the same "Win32 leaves no alternative" situation window.h documents for
// ResizeCallback: TrackPopupMenu blocks the thread the frame loop runs on for as long as
// the menu is open, so - unlike every other cross-module handoff in this project - this
// can't be polled once per frame from main.cpp's loop. It's read-only (just building menu
// labels), so it doesn't compromise the "state changes are polled, not pushed via
// callback" rule the way a callback that mutated state would.
using TrayAccountListCallback = std::uint32_t (*)(void *pUserData, TrayAccountItem *pOutItems, std::uint32_t capacity);

class CTray {
  public:
	CTray() = default;
	~CTray();
	CTray(const CTray &) = delete;
	CTray &operator=(const CTray &) = delete;

	// Creates the hidden message-only window and adds the tray icon. Must run after the
	// main CWindow exists - the message-only window rides the same thread's message pump.
	bool Create(const wchar_t *pTooltip);

	// Registers the one listener the context-menu handler calls synchronously to build
	// the quick-access submenu - see TrayAccountListCallback's own doc comment for why
	// this has to be a callback at all.
	void SetAccountListCallback(TrayAccountListCallback callback, void *pUserData);

	// Reads and clears the event produced by the last tray interaction (icon click or
	// context menu choice), or TRAY_EVENT_NONE if nothing happened.
	ETrayEventType TakeEvent();

	// Valid alongside TRAY_EVENT_QUICK_LOGIN / TRAY_EVENT_COPY_PASSWORD - read these
	// right after a TakeEvent() call that returned one of those two.
	std::int32_t GetPendingBannerIndex() const
	{
		return m_nPendingBannerIndex;
	}
	std::int32_t GetPendingAccountIndex() const
	{
		return m_nPendingAccountIndex;
	}

  private:
	static LRESULT CALLBACK WindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
	LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
	void ShowContextMenu();

	HWND m_hWnd = nullptr;
	HICON m_hIcon = nullptr;
	ETrayEventType m_pendingEvent = ETrayEventType::TRAY_EVENT_NONE;
	std::int32_t m_nPendingBannerIndex = -1;
	std::int32_t m_nPendingAccountIndex = -1;

	TrayAccountListCallback m_pAccountListCallback = nullptr;
	void *m_pAccountListCallbackUserData = nullptr;
};
