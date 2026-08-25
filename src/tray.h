#pragma once

// A tray icon backed by a hidden message-only window, sharing CWindow's thread and message
// pump - main.cpp just calls TakeEvent after pumping to see what happened.
//
// The context menu is owner-drawn to match the app's own palette, and is laid out as:
// every game, each with its icon and a submenu of the accounts visible under it, then a
// separator, then Show Application / Exit Application. It is rebuilt from the live carousel
// every time it opens, so account edits show up immediately with nothing to invalidate.

#include <cstdint>

#include <Windows.h>

#include "core/types.h"

enum class ETrayEventType : std::uint8_t {
	TRAY_EVENT_NONE,
	TRAY_EVENT_SHOW_WINDOW,
	TRAY_EVENT_EXIT_REQUESTED,
	TRAY_EVENT_QUICK_LOGIN, // GetPendingBannerIndex/GetPendingAccountIndex identify the account
};

constexpr std::uint32_t kTrayMaxGames = 16;
constexpr std::uint32_t kTrayMaxAccountItems = 256;

struct TrayGameItem {
	char Title[64];
	std::int32_t BannerIndex;
	std::uint32_t FirstAccount; // index into TrayMenuModel::Accounts
	std::uint32_t AccountCount;
};

// BannerIndex is the game this row is listed under, and QueryIndex is its position within
// that banner's own CCarousel::GetVisibleAccounts result - not the owning banner and raw
// account index. That pairing is what CAccountModal::OpenForQuickLogin expects, and it is
// what makes a cross-visible account log into the game the user actually picked it under.
struct TrayAccountItem {
	// What the row says: the account's note when it has one, otherwise its username (see
	// main.cpp's BuildTrayMenu). The note is the name a person actually gave the account -
	// "main", "smurf", "eu west" - so it identifies the row better than a login ever does,
	// and falling back means a row is never blank. Sized for the longer of the two.
	char Label[64];
	std::int32_t BannerIndex;
	std::int32_t QueryIndex;
};

struct TrayMenuModel {
	TrayGameItem Games[kTrayMaxGames];
	std::uint32_t GameCount;
	TrayAccountItem Accounts[kTrayMaxAccountItems];
	std::uint32_t AccountCount;
};

// Fills the model for one menu open. Called synchronously from inside the context-menu
// handler, because TrackPopupMenu blocks the frame loop's thread for as long as the menu is
// open and nothing polled once per frame could answer in time. Read-only.
using TrayMenuCallback = void (*)(void *pUserData, TrayMenuModel &outModel);

// One owner-drawn row. Lives in CTray for the duration of one TrackPopupMenu call; the
// pointer is what AppendMenuW carries as the item's data.
struct TrayMenuEntry {
	wchar_t szLabel[96];
	HBITMAP hIcon;
	bool bIndent;
	bool bSeparator;
	bool bSubmenu;
	bool bDisabled;
};

constexpr std::uint32_t kTrayMaxMenuEntries = kTrayMaxAccountItems + kTrayMaxGames + 8;

class CTray {
  public:
	CTray() = default;
	~CTray();
	CTray(const CTray &) = delete;
	CTray &operator=(const CTray &) = delete;

	// Creates the message-only window and adds the icon. Returns false only if the window
	// itself couldn't be created - a failed Shell_NotifyIcon is retried on a timer and again
	// whenever Explorer restarts, so it is not treated as fatal here.
	bool Create(const wchar_t *pTooltip);

	void SetMenuCallback(TrayMenuCallback callback, void *pUserData);

	// Decodes an embedded PNG into the menu's icon column for one banner. Safe to skip; a
	// game without one just draws no icon.
	void SetGameIcon(std::int32_t bannerIndex, const std::uint8_t *pPngBytes, std::uint64_t length);

	// Drives the menu's hover highlight. Call whenever the accent may have changed.
	void SetAccentColor(Color accent);

	bool IsIconVisible() const
	{
		return m_bIconAdded;
	}

	ETrayEventType TakeEvent();

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

	bool AddIcon();
	void RemoveIcon();
	void ShowContextMenu();
	TrayMenuEntry *PushEntry(const wchar_t *pLabel, HBITMAP hIcon, bool bIndent, bool bSubmenu, bool bSeparator,
							 bool bDisabled);
	void RebuildBrushes();

	void OnMeasureItem(MEASUREITEMSTRUCT *pMeasure) const;
	void OnDrawItem(const DRAWITEMSTRUCT *pDraw) const;

	HWND m_hWnd = nullptr;
	HICON m_hIcon = nullptr;
	bool m_bIconAdded = false;
	std::uint32_t m_addAttempts = 0;
	wchar_t m_szTooltip[128]{};

	HFONT m_hMenuFont = nullptr;
	HBRUSH m_hBackBrush = nullptr;
	HBRUSH m_hHoverBrush = nullptr;
	Color m_accent{108, 90, 220, 255};
	HBITMAP m_gameIcons[kTrayMaxGames]{};

	ETrayEventType m_pendingEvent = ETrayEventType::TRAY_EVENT_NONE;
	std::int32_t m_nPendingBannerIndex = -1;
	std::int32_t m_nPendingAccountIndex = -1;

	TrayMenuModel m_model{};
	TrayMenuEntry m_entries[kTrayMaxMenuEntries]{};
	std::uint32_t m_entryCount = 0;

	TrayMenuCallback m_pMenuCallback = nullptr;
	void *m_pMenuCallbackUserData = nullptr;
};
