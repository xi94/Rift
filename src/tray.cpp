#include "tray.h"

#include <shellapi.h>
#include <wchar.h>

#include "core/debug_log.h"
#include "resource.h"
#include "stb/stb_image.h"

namespace {
constexpr const char *kLogCategory = "tray";

constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT_PTR kTrayIconId = 1;
constexpr UINT_PTR kRetryTimerId = 1;
constexpr UINT kRetryIntervalMs = 1000;
constexpr std::uint32_t kMaxAddAttempts = 10;

constexpr UINT kMenuIdShow = 1001;
constexpr UINT kMenuIdExit = 1002;
constexpr UINT kMenuIdPlaceholder = 1003;
constexpr UINT kMenuIdQuickLoginBase = 2000;

constexpr int kItemHeight = 26;
constexpr int kSeparatorHeight = 7;
constexpr int kPaddingX = 12;
constexpr int kArrowWidth = 18;
constexpr int kIconGap = 8;
constexpr int kMinItemWidth = 170;

constexpr Color kColorBg{30, 30, 34, 255};
constexpr Color kColorText{220, 220, 224, 255};
constexpr Color kColorTextDim{140, 140, 148, 255};
constexpr Color kColorSeparator{60, 60, 66, 255};

const wchar_t *const kTrayWindowClassName = L"rift_tray_window_class";

int MenuIconSize()
{
	const int size = GetSystemMetrics(SM_CXSMICON);
	return size > 0 ? size : 16;
}

// Explorer broadcasts this after a restart; the icon has to be re-added or it is gone for
// the rest of the session.
UINT TaskbarCreatedMessage()
{
	static const UINT message = RegisterWindowMessageW(L"TaskbarCreated");
	return message;
}

COLORREF ToColorRef(Color color)
{
	return RGB(color.R, color.G, color.B);
}

Color BlendToward(Color from, Color to, float amount)
{
	const auto lerp = [amount](std::uint8_t a, std::uint8_t b) {
		return static_cast<std::uint8_t>(static_cast<float>(a) +
										 (static_cast<float>(b) - static_cast<float>(a)) * amount);
	};
	return Color{lerp(from.R, to.R), lerp(from.G, to.G), lerp(from.B, to.B), 255};
}

void ToWide(const char *pUtf8, wchar_t *pOut, int outCapacity)
{
	if (MultiByteToWideChar(CP_UTF8, 0, pUtf8, -1, pOut, outCapacity) <= 0) {
		pOut[0] = L'\0';
	}
}

HFONT CreateMenuFont()
{
	NONCLIENTMETRICSW metrics{};
	metrics.cbSize = sizeof(metrics);
	if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
		return nullptr;
	}
	return CreateFontIndirectW(&metrics.lfMenuFont);
}

// Decodes an embedded PNG and box-downscales it into a premultiplied 32bpp DIB that
// AlphaBlend can draw straight onto the menu. Box-filtered rather than letting AlphaBlend
// stretch: these icons are a few hundred pixels square and a nearest-neighbour drop to 16px
// looks obviously broken next to the rest of the app.
HBITMAP DecodePngToPremultipliedDib(const std::uint8_t *pBytes, std::uint64_t length, int targetSize)
{
	int width = 0;
	int height = 0;
	int channels = 0;
	unsigned char *pPixels = stbi_load_from_memory(pBytes, static_cast<int>(length), &width, &height, &channels, 4);
	if (pPixels == nullptr || width <= 0 || height <= 0) {
		return nullptr;
	}

	BITMAPINFO info{};
	info.bmiHeader.biSize = sizeof(info.bmiHeader);
	info.bmiHeader.biWidth = targetSize;
	info.bmiHeader.biHeight = -targetSize; // top-down
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	info.bmiHeader.biCompression = BI_RGB;

	void *pDibBits = nullptr;
	const HBITMAP hBitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pDibBits, nullptr, 0);
	if (hBitmap == nullptr || pDibBits == nullptr) {
		stbi_image_free(pPixels);
		return nullptr;
	}

	auto *pOut = static_cast<std::uint8_t *>(pDibBits);
	for (int y = 0; y < targetSize; y += 1) {
		const int srcY0 = y * height / targetSize;
		const int srcY1 = (y + 1) * height / targetSize > srcY0 ? (y + 1) * height / targetSize : srcY0 + 1;
		for (int x = 0; x < targetSize; x += 1) {
			const int srcX0 = x * width / targetSize;
			const int srcX1 = (x + 1) * width / targetSize > srcX0 ? (x + 1) * width / targetSize : srcX0 + 1;

			std::uint32_t r = 0;
			std::uint32_t g = 0;
			std::uint32_t b = 0;
			std::uint32_t a = 0;
			std::uint32_t samples = 0;
			for (int sy = srcY0; sy < srcY1 && sy < height; sy += 1) {
				for (int sx = srcX0; sx < srcX1 && sx < width; sx += 1) {
					const std::uint8_t *pTexel = pPixels + (static_cast<std::size_t>(sy) * width + sx) * 4;
					r += pTexel[0];
					g += pTexel[1];
					b += pTexel[2];
					a += pTexel[3];
					samples += 1;
				}
			}
			if (samples == 0) {
				samples = 1;
			}

			const std::uint32_t alpha = a / samples;
			std::uint8_t *pDest = pOut + (static_cast<std::size_t>(y) * targetSize + x) * 4;
			pDest[0] = static_cast<std::uint8_t>((b / samples) * alpha / 255); // BGRA, premultiplied
			pDest[1] = static_cast<std::uint8_t>((g / samples) * alpha / 255);
			pDest[2] = static_cast<std::uint8_t>((r / samples) * alpha / 255);
			pDest[3] = static_cast<std::uint8_t>(alpha);
		}
	}

	stbi_image_free(pPixels);
	return hBitmap;
}
} // namespace

void CTray::RebuildBrushes()
{
	if (m_hBackBrush != nullptr) {
		DeleteObject(m_hBackBrush);
	}
	if (m_hHoverBrush != nullptr) {
		DeleteObject(m_hHoverBrush);
	}
	m_hBackBrush = CreateSolidBrush(ToColorRef(kColorBg));
	m_hHoverBrush = CreateSolidBrush(ToColorRef(BlendToward(kColorBg, m_accent, 0.42f)));
}

void CTray::SetAccentColor(Color accent)
{
	if (accent.R == m_accent.R && accent.G == m_accent.G && accent.B == m_accent.B) {
		return;
	}
	m_accent = accent;
	RebuildBrushes();
}

void CTray::SetGameIcon(std::int32_t bannerIndex, const std::uint8_t *pPngBytes, std::uint64_t length)
{
	if (bannerIndex < 0 || static_cast<std::uint32_t>(bannerIndex) >= kTrayMaxGames || pPngBytes == nullptr) {
		return;
	}
	if (m_gameIcons[bannerIndex] != nullptr) {
		DeleteObject(m_gameIcons[bannerIndex]);
	}
	m_gameIcons[bannerIndex] = DecodePngToPremultipliedDib(pPngBytes, length, MenuIconSize());
}

TrayMenuEntry *CTray::PushEntry(const wchar_t *pLabel, HBITMAP hIcon, bool bIndent, bool bSubmenu, bool bSeparator,
								bool bDisabled)
{
	if (m_entryCount >= kTrayMaxMenuEntries) {
		return nullptr;
	}
	TrayMenuEntry &entry = m_entries[m_entryCount];
	m_entryCount += 1;

	entry.szLabel[0] = L'\0';
	if (pLabel != nullptr) {
		wcsncpy_s(entry.szLabel, pLabel, _TRUNCATE);
	}
	entry.hIcon = hIcon;
	entry.bIndent = bIndent;
	entry.bSubmenu = bSubmenu;
	entry.bSeparator = bSeparator;
	entry.bDisabled = bDisabled;
	return &entry;
}

void CTray::OnMeasureItem(MEASUREITEMSTRUCT *pMeasure) const
{
	const auto *pEntry = reinterpret_cast<const TrayMenuEntry *>(pMeasure->itemData);
	if (pEntry == nullptr) {
		return;
	}

	if (pEntry->bSeparator) {
		pMeasure->itemWidth = kMinItemWidth;
		pMeasure->itemHeight = kSeparatorHeight;
		return;
	}

	int textWidth = 0;
	const HDC hdc = GetDC(m_hWnd);
	if (hdc != nullptr) {
		const HGDIOBJ previousFont = SelectObject(hdc, m_hMenuFont);
		SIZE size{};
		if (GetTextExtentPoint32W(hdc, pEntry->szLabel, static_cast<int>(wcslen(pEntry->szLabel)), &size)) {
			textWidth = size.cx;
		}
		SelectObject(hdc, previousFont);
		ReleaseDC(m_hWnd, hdc);
	}

	const int indent = pEntry->bIndent ? MenuIconSize() + kIconGap : 0;
	const int width = kPaddingX * 2 + indent + textWidth + (pEntry->bSubmenu ? kArrowWidth : 0);
	pMeasure->itemWidth = static_cast<UINT>(width > kMinItemWidth ? width : kMinItemWidth);
	pMeasure->itemHeight = kItemHeight;
}

void CTray::OnDrawItem(const DRAWITEMSTRUCT *pDraw) const
{
	const auto *pEntry = reinterpret_cast<const TrayMenuEntry *>(pDraw->itemData);
	if (pEntry == nullptr) {
		return;
	}

	const RECT rect = pDraw->rcItem;
	const bool bSelected = (pDraw->itemState & ODS_SELECTED) != 0 && !pEntry->bSeparator && !pEntry->bDisabled;
	RECT fill = rect;
	FillRect(pDraw->hDC, &fill, bSelected ? m_hHoverBrush : m_hBackBrush);

	if (pEntry->bSeparator) {
		RECT line{rect.left + kPaddingX, (rect.top + rect.bottom) / 2, rect.right - kPaddingX,
				  (rect.top + rect.bottom) / 2 + 1};
		const HBRUSH hBrush = CreateSolidBrush(ToColorRef(kColorSeparator));
		FillRect(pDraw->hDC, &line, hBrush);
		DeleteObject(hBrush);
		return;
	}

	const int iconSize = MenuIconSize();
	if (pEntry->hIcon != nullptr) {
		const HDC hdcMem = CreateCompatibleDC(pDraw->hDC);
		if (hdcMem != nullptr) {
			const HGDIOBJ previousBitmap = SelectObject(hdcMem, pEntry->hIcon);
			const BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
			AlphaBlend(pDraw->hDC, rect.left + kPaddingX, (rect.top + rect.bottom - iconSize) / 2, iconSize, iconSize,
					   hdcMem, 0, 0, iconSize, iconSize, blend);
			SelectObject(hdcMem, previousBitmap);
			DeleteDC(hdcMem);
		}
	}

	SetBkMode(pDraw->hDC, TRANSPARENT);
	SetTextColor(pDraw->hDC, ToColorRef(pEntry->bDisabled ? kColorTextDim : kColorText));
	const HGDIOBJ previousFont = SelectObject(pDraw->hDC, m_hMenuFont);

	const int indent = pEntry->bIndent ? iconSize + kIconGap : 0;
	RECT textRect{rect.left + kPaddingX + indent, rect.top, rect.right - kPaddingX, rect.bottom};
	DrawTextW(pDraw->hDC, pEntry->szLabel, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

	if (pEntry->bSubmenu) {
		const int cx = rect.right - kPaddingX - 4;
		const int cy = (rect.top + rect.bottom) / 2;
		const POINT arrow[3]{{cx - 4, cy - 4}, {cx, cy}, {cx - 4, cy + 4}};
		const HBRUSH hBrush = CreateSolidBrush(ToColorRef(pEntry->bDisabled ? kColorTextDim : kColorText));
		const HGDIOBJ previousBrush = SelectObject(pDraw->hDC, hBrush);
		const HGDIOBJ previousPen = SelectObject(pDraw->hDC, GetStockObject(NULL_PEN));
		Polygon(pDraw->hDC, arrow, 3);
		SelectObject(pDraw->hDC, previousPen);
		SelectObject(pDraw->hDC, previousBrush);
		DeleteObject(hBrush);
	}

	SelectObject(pDraw->hDC, previousFont);
}

void CTray::ShowContextMenu()
{
	POINT cursor;
	GetCursorPos(&cursor);

	m_model = TrayMenuModel{};
	if (m_pMenuCallback != nullptr) {
		m_pMenuCallback(m_pMenuCallbackUserData, m_model);
	}
	m_entryCount = 0;

	const HMENU menu = CreatePopupMenu();

	const auto append = [this](HMENU target, UINT flags, UINT_PTR id, const wchar_t *pLabel, HBITMAP hIcon,
							   bool bIndent, bool bSubmenu, bool bSeparator, bool bDisabled) {
		const TrayMenuEntry *pEntry = PushEntry(pLabel, hIcon, bIndent, bSubmenu, bSeparator, bDisabled);
		if (pEntry == nullptr) {
			return;
		}
		AppendMenuW(target, flags | MF_OWNERDRAW, id, reinterpret_cast<LPCWSTR>(pEntry));
	};

	for (std::uint32_t g = 0; g < m_model.GameCount; g += 1) {
		const TrayGameItem &game = m_model.Games[g];

		const HMENU gameMenu = CreatePopupMenu();
		for (std::uint32_t a = 0; a < game.AccountCount; a += 1) {
			const std::uint32_t accountIndex = game.FirstAccount + a;
			if (accountIndex >= m_model.AccountCount) {
				break;
			}
			wchar_t label[96];
			ToWide(m_model.Accounts[accountIndex].Label, label, 96);
			append(gameMenu, MF_STRING, kMenuIdQuickLoginBase + accountIndex, label, nullptr, false, false, false,
				   false);
		}
		if (game.AccountCount == 0) {
			append(gameMenu, MF_DISABLED | MF_GRAYED, kMenuIdPlaceholder, L"No accounts", nullptr, false, false, false,
				   true);
		}

		wchar_t title[96];
		ToWide(game.Title, title, 96);
		const HBITMAP hIcon = game.BannerIndex >= 0 && static_cast<std::uint32_t>(game.BannerIndex) < kTrayMaxGames
								  ? m_gameIcons[game.BannerIndex]
								  : nullptr;
		append(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(gameMenu), title, hIcon, true, true, false, false);
	}

	if (m_model.GameCount == 0) {
		append(menu, MF_DISABLED | MF_GRAYED, kMenuIdPlaceholder, L"No games", nullptr, true, false, false, true);
	}

	append(menu, MF_DISABLED | MF_GRAYED, 0, L"", nullptr, false, false, true, true);
	append(menu, MF_STRING, kMenuIdShow, L"Show Application", nullptr, true, false, false, false);
	append(menu, MF_STRING, kMenuIdExit, L"Exit Application", nullptr, true, false, false, false);

	// MIM_APPLYTOSUBMENUS only reaches submenus that already exist, so this runs last.
	MENUINFO menuInfo{};
	menuInfo.cbSize = sizeof(menuInfo);
	menuInfo.fMask = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS;
	menuInfo.hbrBack = m_hBackBrush;
	SetMenuInfo(menu, &menuInfo);

	// Required so the menu dismisses correctly when the user clicks away from it.
	SetForegroundWindow(m_hWnd);

	// Anchored at the pointer with the default alignment, the way every other tray menu on
	// Windows behaves - the earlier version picked its own corner alignment from which half
	// of the screen the cursor was in, which put the menu nowhere near the pointer.
	//
	// TPM_WORKAREA is the one addition, and it's what keeps the menu off the taskbar: by
	// default Windows only flips a menu up when it wouldn't fit on the *monitor*, so a menu
	// that fits on screen but not above the taskbar is drawn underneath it and its last item
	// (Exit) becomes unreachable. With this, the fit decision uses the work area instead, so
	// the flip happens when it actually needs to. The flag is only honored by
	// TrackPopupMenuEx and only with a TPMPARAMS - rcExclude is a degenerate rect at the
	// cursor rather than an all-zero one, which would name a real area at the screen's
	// top-left corner for Windows to position around.
	TPMPARAMS popupParams{};
	popupParams.cbSize = sizeof(popupParams);
	popupParams.rcExclude = RECT{cursor.x, cursor.y, cursor.x, cursor.y};
	TrackPopupMenuEx(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON | TPM_WORKAREA, cursor.x, cursor.y, m_hWnd,
					 &popupParams);
	PostMessageW(m_hWnd, WM_NULL, 0, 0);

	DestroyMenu(menu);
	m_entryCount = 0;
}

LRESULT CTray::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
	if (message == TaskbarCreatedMessage()) {
		DebugLog::Write(kLogCategory, "Explorer restarted - re-adding the tray icon");
		m_bIconAdded = false;
		m_addAttempts = 0;
		AddIcon();
		return 0;
	}

	switch (message) {
		case kTrayCallbackMessage: {
			const auto mouseMessage = static_cast<UINT>(LOWORD(lParam));
			if (mouseMessage == WM_LBUTTONUP) {
				m_pendingEvent = ETrayEventType::TRAY_EVENT_SHOW_WINDOW;
			} else if (mouseMessage == WM_RBUTTONUP || mouseMessage == WM_CONTEXTMENU) {
				ShowContextMenu();
			}
			return 0;
		}

		case WM_TIMER: {
			if (wParam == kRetryTimerId) {
				AddIcon();
			}
			return 0;
		}

		case WM_MEASUREITEM: {
			auto *pMeasure = reinterpret_cast<MEASUREITEMSTRUCT *>(lParam);
			if (pMeasure != nullptr && pMeasure->CtlType == ODT_MENU) {
				OnMeasureItem(pMeasure);
				return TRUE;
			}
			return DefWindowProcW(m_hWnd, message, wParam, lParam);
		}

		case WM_DRAWITEM: {
			const auto *pDraw = reinterpret_cast<const DRAWITEMSTRUCT *>(lParam);
			if (pDraw != nullptr && pDraw->CtlType == ODT_MENU) {
				OnDrawItem(pDraw);
				return TRUE;
			}
			return DefWindowProcW(m_hWnd, message, wParam, lParam);
		}

		case WM_COMMAND: {
			const UINT commandId = LOWORD(wParam);
			if (commandId == kMenuIdShow) {
				m_pendingEvent = ETrayEventType::TRAY_EVENT_SHOW_WINDOW;
			} else if (commandId == kMenuIdExit) {
				m_pendingEvent = ETrayEventType::TRAY_EVENT_EXIT_REQUESTED;
			} else if (commandId >= kMenuIdQuickLoginBase && commandId < kMenuIdQuickLoginBase + kTrayMaxAccountItems) {
				const UINT index = commandId - kMenuIdQuickLoginBase;
				if (index < m_model.AccountCount) {
					m_pendingEvent = ETrayEventType::TRAY_EVENT_QUICK_LOGIN;
					m_nPendingBannerIndex = m_model.Accounts[index].BannerIndex;
					m_nPendingAccountIndex = m_model.Accounts[index].QueryIndex;
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
	if (message == WM_NCCREATE) {
		const auto *pCreate = reinterpret_cast<const CREATESTRUCTW *>(lParam);
		SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pCreate->lpCreateParams));
	}

	auto *pTray = reinterpret_cast<CTray *>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
	if (pTray == nullptr) {
		return DefWindowProcW(hWnd, message, wParam, lParam);
	}

	// Before dispatching, not after CreateWindowExW returns: HandleMessage passes this to
	// DefWindowProcW, and WM_NCCREATE arrives while the creation call is still in flight - a
	// null handle there makes DefWindowProcW return 0, which aborts the whole creation.
	pTray->m_hWnd = hWnd;
	return pTray->HandleMessage(message, wParam, lParam);
}

bool CTray::AddIcon()
{
	if (m_bIconAdded) {
		return true;
	}

	NOTIFYICONDATAW iconData{};
	iconData.cbSize = sizeof(NOTIFYICONDATAW);
	iconData.hWnd = m_hWnd;
	iconData.uID = kTrayIconId;
	iconData.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	iconData.uCallbackMessage = kTrayCallbackMessage;
	iconData.hIcon = m_hIcon;
	wcsncpy_s(iconData.szTip, m_szTooltip, _TRUNCATE);

	m_addAttempts += 1;
	if (Shell_NotifyIconW(NIM_ADD, &iconData) == TRUE) {
		m_bIconAdded = true;
		KillTimer(m_hWnd, kRetryTimerId);
		DebugLog::Write(kLogCategory, "tray icon added on attempt %u", m_addAttempts);
		return true;
	}

	// NIM_ADD legitimately fails while the shell is still starting up or busy, so this is a
	// retry rather than an error - it is only ever fatal once the attempts run out.
	DebugLog::Write(kLogCategory, "Shell_NotifyIcon(NIM_ADD) failed on attempt %u, err=%lu", m_addAttempts,
					GetLastError());
	if (m_addAttempts < kMaxAddAttempts) {
		SetTimer(m_hWnd, kRetryTimerId, kRetryIntervalMs, nullptr);
	} else {
		KillTimer(m_hWnd, kRetryTimerId);
		DebugLog::Write(kLogCategory, "giving up on the tray icon after %u attempts", m_addAttempts);
	}
	return false;
}

void CTray::RemoveIcon()
{
	if (!m_bIconAdded) {
		return;
	}
	NOTIFYICONDATAW iconData{};
	iconData.cbSize = sizeof(NOTIFYICONDATAW);
	iconData.hWnd = m_hWnd;
	iconData.uID = kTrayIconId;
	Shell_NotifyIconW(NIM_DELETE, &iconData);
	m_bIconAdded = false;
}

bool CTray::Create(const wchar_t *pTooltip)
{
	const HINSTANCE instance = GetModuleHandleW(nullptr);

	WNDCLASSEXW windowClass{};
	windowClass.cbSize = sizeof(WNDCLASSEXW);
	windowClass.lpfnWndProc = WindowProc;
	windowClass.hInstance = instance;
	windowClass.lpszClassName = kTrayWindowClassName;
	if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
		DebugLog::Write(kLogCategory, "RegisterClassExW failed, err=%lu", GetLastError());
		return false;
	}

	if (CreateWindowExW(0, kTrayWindowClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this) ==
		nullptr) {
		DebugLog::Write(kLogCategory, "failed to create the tray window, err=%lu", GetLastError());
		m_hWnd = nullptr;
		return false;
	}

	wcsncpy_s(m_szTooltip, pTooltip, _TRUNCATE);
	m_hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
											GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
	if (m_hIcon == nullptr) {
		m_hIcon = LoadIconW(nullptr, IDI_APPLICATION);
	}

	m_hMenuFont = CreateMenuFont();
	if (m_hMenuFont == nullptr) {
		m_hMenuFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
	}
	RebuildBrushes();

	AddIcon();
	return true;
}

CTray::~CTray()
{
	RemoveIcon();
	if (m_hWnd != nullptr) {
		KillTimer(m_hWnd, kRetryTimerId);
		DestroyWindow(m_hWnd);
	}
	if (m_hMenuFont != nullptr) {
		DeleteObject(m_hMenuFont);
	}
	if (m_hBackBrush != nullptr) {
		DeleteObject(m_hBackBrush);
	}
	if (m_hHoverBrush != nullptr) {
		DeleteObject(m_hHoverBrush);
	}
	for (HBITMAP hIcon : m_gameIcons) {
		if (hIcon != nullptr) {
			DeleteObject(hIcon);
		}
	}
}

void CTray::SetMenuCallback(TrayMenuCallback callback, void *pUserData)
{
	m_pMenuCallback = callback;
	m_pMenuCallbackUserData = pUserData;
}

ETrayEventType CTray::TakeEvent()
{
	const ETrayEventType event = m_pendingEvent;
	m_pendingEvent = ETrayEventType::TRAY_EVENT_NONE;
	return event;
}
