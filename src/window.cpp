#include "window.h"

#include <cassert>
#include <cmath>

#include <dwmapi.h>
#include <windowsx.h>

#include "resource.h"

namespace {
const wchar_t *const kWindowClassName = L"rift_window_class";

// physical client-area pixels -> logical pixels (see CWindow::GetDpiScale).
float ToLogical(float physical, float dpiScale)
{
	return physical / dpiScale;
}

// A maximized window's rect extends past the visible monitor (by design, so the resize
// border lands off-screen); insetting the client rect by the standard frame size keeps
// content within the work area instead of covering the taskbar - Windows does this
// automatically as part of the frame WM_NCCALCSIZE below removes. Per-DPI (not
// GetSystemMetrics' single system-wide answer) so this stays correct on a non-100%-
// scaled monitor.
void ApplyMaximizedInset(HWND hWnd, RECT &rect, UINT dpi)
{
	if (!IsZoomed(hWnd)) {
		return;
	}

	const LONG borderX = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
	const LONG borderY = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
	rect.left += borderX;
	rect.top += borderY;
	rect.right -= borderX;
	rect.bottom -= borderY;
}

// Resize-border hit testing: WM_NCCALCSIZE below removes the standard frame entirely
// (client rect = window rect), so the OS no longer knows where the resize grips are -
// this reimplements that part of DefWindowProc's job so edge/corner dragging still
// works.
LRESULT HitTestResizeEdges(HWND hWnd, POINT cursorScreen, UINT dpi)
{
	if (IsZoomed(hWnd)) {
		return HTNOWHERE; // nothing to grab while maximized
	}

	RECT windowRect;
	GetWindowRect(hWnd, &windowRect);

	const LONG borderX = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
	const LONG borderY = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);

	const bool atLeft = cursorScreen.x < windowRect.left + borderX;
	const bool atRight = cursorScreen.x >= windowRect.right - borderX;
	const bool atTop = cursorScreen.y < windowRect.top + borderY;
	const bool atBottom = cursorScreen.y >= windowRect.bottom - borderY;

	if (atTop && atLeft) {
		return HTTOPLEFT;
	}
	if (atTop && atRight) {
		return HTTOPRIGHT;
	}
	if (atBottom && atLeft) {
		return HTBOTTOMLEFT;
	}
	if (atBottom && atRight) {
		return HTBOTTOMRIGHT;
	}
	if (atLeft) {
		return HTLEFT;
	}
	if (atRight) {
		return HTRIGHT;
	}
	if (atTop) {
		return HTTOP;
	}
	if (atBottom) {
		return HTBOTTOM;
	}
	return HTNOWHERE;
}

// The one LoadCursorW call site for every ECursorKind - see SetCursorKind's own comment
// for why the actual ::SetCursor call happens both there and in WM_SETCURSOR below rather
// than just once.
HCURSOR CursorForKind(ECursorKind kind)
{
	switch (kind) {
		case ECursorKind::CURSOR_HAND:
			return LoadCursorW(nullptr, IDC_HAND);
		case ECursorKind::CURSOR_IBEAM:
			return LoadCursorW(nullptr, IDC_IBEAM);
		case ECursorKind::CURSOR_DRAG:
			return LoadCursorW(nullptr, IDC_ARROW);
		case ECursorKind::CURSOR_ARROW:
		default:
			return LoadCursorW(nullptr, IDC_ARROW);
	}
}
} // namespace

CWindow::~CWindow()
{
	if (m_hWnd != nullptr) {
		DestroyWindow(m_hWnd);
		m_hWnd = nullptr;
	}
}

bool CWindow::Create(const wchar_t *pTitle, std::uint32_t width, std::uint32_t height)
{
	// Per-Monitor-V2: the window is told its true DPI on every monitor (including via
	// WM_DPICHANGED when dragged between two of different scale) and must scale itself -
	// the alternative (no awareness declared) has Windows bitmap-stretch our already-
	// rendered frame to fill the physical monitor, which is exactly the blur a crisp,
	// hand-drawn UI shouldn't have. Must be called before the first window/DC is created
	// for the whole process; this app only ever creates the one window, so doing it here
	// is equivalent to doing it at the top of main(). (Also declared in app.manifest,
	// which is the authoritative source Windows actually uses for an unmanifested-vs-not
	// compatibility heuristic - this runtime call is a documented fallback on top of that,
	// kept for the same reason the original f4 project kept its own.)
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	// Best guess before the window exists to ask GetDpiForWindow - corrected below,
	// immediately after creation, once we know which monitor it actually landed on.
	const float initialDpiScale = static_cast<float>(GetDpiForSystem()) / 96.0f;

	m_nWidth = width;
	m_nHeight = height;
	m_nPhysicalWidth = static_cast<std::uint32_t>(std::lround(static_cast<float>(width) * initialDpiScale));
	m_nPhysicalHeight = static_cast<std::uint32_t>(std::lround(static_cast<float>(height) * initialDpiScale));
	m_flDpiScale = initialDpiScale;
	m_bShouldClose = false;

	const HINSTANCE hInstance = GetModuleHandleW(nullptr);

	WNDCLASSEXW windowClass{
		.cbSize = sizeof(WNDCLASSEXW),
		.style = CS_HREDRAW | CS_VREDRAW,
		.lpfnWndProc = WindowProc,
		.cbClsExtra = 0,
		.cbWndExtra = 0,
		.hInstance = hInstance,
		// Rift's own app icon (app.rc), not the generic IDI_APPLICATION stand-in - shows
		// up in the taskbar, Alt+Tab, and this window's own Alt+Space system menu.
		// LoadImageW (not the simpler LoadIconW, which only ever returns the large-icon
		// size) with each context's own system metric picks the best-matching individual
		// resolution out of the .ico's several embedded sizes, so the small taskbar icon
		// is a real small bitmap, not the large one just squished down.
		.hIcon = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
											   GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0)),
		.hCursor = LoadCursorW(nullptr, IDC_ARROW),
		.hbrBackground = nullptr,
		.lpszMenuName = nullptr,
		.lpszClassName = kWindowClassName,
		.hIconSm = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
												 GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0)),
	};
	RegisterClassExW(&windowClass);

	// Centered on the primary monitor's work area (SPI_GETWORKAREA - the primary
	// monitor's bounds minus the taskbar, always in physical pixels/screen coordinates,
	// matching m_nPhysicalWidth/Height below), not CW_USEDEFAULT's OS-chosen cascade
	// position - a fresh install/first run should open somewhere predictable and centered
	// rather than wherever Windows' own placement heuristic happens to land it.
	RECT workArea{};
	SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
	const int windowX = workArea.left + ((workArea.right - workArea.left) - static_cast<int>(m_nPhysicalWidth)) / 2;
	const int windowY = workArea.top + ((workArea.bottom - workArea.top) - static_cast<int>(m_nPhysicalHeight)) / 2;

	// No AdjustWindowRect: WM_NCCALCSIZE (below, via HandleMessage) makes the client rect
	// equal the window rect (outside the maximized case), so the requested size already is
	// the client size. Sized in physical pixels (m_nPhysicalWidth/Height) so the window's
	// true on-screen size matches the requested logical size at initialDpiScale; corrected
	// below if the window lands on a monitor initialDpiScale guessed wrong. `this` rides
	// along as lpCreateParams so WindowProc's WM_NCCREATE handler can stash it in
	// GWLP_USERDATA before any other message arrives.
	m_hWnd = CreateWindowExW(0, kWindowClassName, pTitle, WS_OVERLAPPEDWINDOW, windowX, windowY,
							 static_cast<int>(m_nPhysicalWidth), static_cast<int>(m_nPhysicalHeight), nullptr, nullptr,
							 hInstance, this);
	if (m_hWnd == nullptr) {
		return false;
	}

	// Now that the window exists (and CreateWindowExW has placed it on a real monitor),
	// ask for its actual DPI rather than trust the pre-placement guess above - correct the
	// physical size in place if they differ, keeping the logical size exactly what the
	// caller asked for either way.
	const UINT actualDpi = GetDpiForWindow(m_hWnd);
	const float actualDpiScale = static_cast<float>(actualDpi) / 96.0f;
	if (actualDpiScale != m_flDpiScale) {
		m_flDpiScale = actualDpiScale;
		const auto correctedWidth = static_cast<int>(std::lround(static_cast<float>(width) * actualDpiScale));
		const auto correctedHeight = static_cast<int>(std::lround(static_cast<float>(height) * actualDpiScale));
		SetWindowPos(m_hWnd, nullptr, 0, 0, correctedWidth, correctedHeight,
					 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	}
	// WM_SIZE (from either CreateWindowExW or the corrective SetWindowPos above) has
	// already filled in m_nPhysicalWidth/Height and m_nWidth/Height by now.

	// The one real DWM dependency: keeps the native drop shadow / rounded corners now that
	// WM_NCCALCSIZE has zeroed out the standard frame. Aero Snap, the minimize animation,
	// and Alt+Tab thumbnails need no Dwm* call at all - those come from WS_OVERLAPPEDWINDOW
	// plus DWM composition, which Windows 10/11 always has on.
	const MARGINS margins{.cxLeftWidth = 0, .cxRightWidth = 0, .cyTopHeight = 1, .cyBottomHeight = 0};
	DwmExtendFrameIntoClientArea(m_hWnd, &margins);

	// Deliberately left hidden - see Show()'s own comment for why the caller, not Create(),
	// decides when the window actually appears.
	return true;
}

void CWindow::Show()
{
	ShowWindow(m_hWnd, SW_SHOW);
}

void CWindow::Restore()
{
	ShowWindow(m_hWnd, SW_SHOW);
	if (IsIconic(m_hWnd)) {
		ShowWindow(m_hWnd, SW_RESTORE);
	}
	SetForegroundWindow(m_hWnd);
}

void CWindow::SetResizeCallback(ResizeCallback callback, void *pUserData)
{
	m_pResizeCallback = callback;
	m_pResizeCallbackUserData = pUserData;
}

void CWindow::SetDpiChangedCallback(DpiChangedCallback callback, void *pUserData)
{
	m_pDpiChangedCallback = callback;
	m_pDpiChangedCallbackUserData = pUserData;
}

void CWindow::PumpMessages()
{
	m_nInputEventCount = 0;

	MSG message;
	while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
		if (message.message == WM_QUIT) {
			m_bShouldClose = true;
		}
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
}

Rect CWindow::GetTitleBarButtonRect(ETitleBarButton button) const
{
	assert(button != ETitleBarButton::TITLE_BAR_BUTTON_NONE);

	if (button == ETitleBarButton::TITLE_BAR_BUTTON_MENU) {
		return Rect{0.0f, 0.0f, kTitleBarButtonWidth, kTitleBarHeight};
	}
	if (button == ETitleBarButton::TITLE_BAR_BUTTON_UPDATE) {
		return Rect{kTitleBarButtonWidth, 0.0f, kUpdateButtonWidth, kTitleBarHeight};
	}

	// Right-aligned, in order Minimize, Maximize, Close (left to right) - standard order.
	float indexFromRight = 0.0f;
	switch (button) {
		case ETitleBarButton::TITLE_BAR_BUTTON_CLOSE:
			indexFromRight = 0.0f;
			break;
		case ETitleBarButton::TITLE_BAR_BUTTON_MAXIMIZE:
			indexFromRight = 1.0f;
			break;
		case ETitleBarButton::TITLE_BAR_BUTTON_MINIMIZE:
			indexFromRight = 2.0f;
			break;
		case ETitleBarButton::TITLE_BAR_BUTTON_MENU:
		case ETitleBarButton::TITLE_BAR_BUTTON_UPDATE:
		case ETitleBarButton::TITLE_BAR_BUTTON_NONE:
			break;
	}

	const float right = static_cast<float>(m_nWidth);
	return Rect{right - (indexFromRight + 1.0f) * kTitleBarButtonWidth, 0.0f, kTitleBarButtonWidth, kTitleBarHeight};
}

ETitleBarButton CWindow::TitleBarHitTest(float clientX, float clientY) const
{
	if (clientY < 0.0f || clientY >= kTitleBarHeight) {
		return ETitleBarButton::TITLE_BAR_BUTTON_NONE;
	}

	constexpr ETitleBarButton kButtons[]{
		ETitleBarButton::TITLE_BAR_BUTTON_MENU, ETitleBarButton::TITLE_BAR_BUTTON_UPDATE,
		ETitleBarButton::TITLE_BAR_BUTTON_CLOSE, ETitleBarButton::TITLE_BAR_BUTTON_MAXIMIZE,
		ETitleBarButton::TITLE_BAR_BUTTON_MINIMIZE};
	for (ETitleBarButton button : kButtons) {
		if (button == ETitleBarButton::TITLE_BAR_BUTTON_UPDATE && !m_bUpdateButtonVisible) {
			continue;
		}
		const Rect rect = GetTitleBarButtonRect(button);
		if (clientX >= rect.X && clientX < rect.X + rect.W) {
			return button;
		}
	}
	return ETitleBarButton::TITLE_BAR_BUTTON_NONE;
}

void CWindow::SetCursorKind(ECursorKind kind)
{
	m_cursorKind = kind;
	// Applied immediately, not just stashed for the next WM_SETCURSOR: that message only
	// fires on Windows' own idea of "something changed" (mainly mouse movement), so a
	// frame where the cursor kind changes without the mouse itself moving (e.g. a button
	// appearing right under an already-stationary cursor) would otherwise show the old
	// cursor until the next twitch.
	SetCursor(CursorForKind(kind));
}

void CWindow::PushInputEvent(const InputEvent &event)
{
	// Fixed-capacity, silently dropped past kMaxInputEventsPerFrame rather than growing -
	// see FORK_WITH_CLASSES.md 6 on keeping this kind of per-frame buffer fixed-capacity
	// regardless of the class rewrite.
	if (m_nInputEventCount >= kMaxInputEventsPerFrame) {
		return;
	}
	m_aInputEvents[m_nInputEventCount] = event;
	m_nInputEventCount += 1;
}

// Every message is either handled synchronously right here (frame-removal, hit-testing,
// the two documented callback exceptions on resize/DPI change) or translated into a
// polled InputEvent for the frame loop to read later - see window.h's file comment for why
// input is polled everywhere except those two cases.
LRESULT CWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message) {
		// Removes the standard title bar/frame entirely (client rect = window rect) so we
		// can draw our own. Must handle both wParam cases: TRUE (lParam is
		// NCCALCSIZE_PARAMS*) covers most resizes/moves, but the very first calculation at
		// window creation fires with FALSE (lParam is a plain RECT*) - skipping that case
		// means the OS draws its standard title bar until the first resize.
		case WM_NCCALCSIZE: {
			RECT *pRect = wParam == TRUE ? &reinterpret_cast<NCCALCSIZE_PARAMS *>(lParam)->rgrc[0]
										 : reinterpret_cast<RECT *>(lParam);
			ApplyMaximizedInset(m_hWnd, *pRect, GetDpiForWindow(m_hWnd));
			return 0;
		}

		// WM_NCHITTEST's real answer, now that the standard frame is gone: resize edges
		// first, then our own hand-drawn title bar's draggable caption strip, else plain
		// client area. Title bar button clicks are deliberately NOT synthetic
		// HTMINBUTTON/HTMAXBUTTON/HTCLOSE - those areas fall through to plain HTCLIENT so
		// clicks flow through the normal InputEvent pipeline like everything else.
		case WM_NCHITTEST: {
			const POINT cursorScreen{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

			const LRESULT edgeHit = HitTestResizeEdges(m_hWnd, cursorScreen, GetDpiForWindow(m_hWnd));
			// Recorded here, not derived from polled mouse position elsewhere: once the
			// cursor is over this strip it's non-client, so WM_MOUSEMOVE stops firing and
			// this is the only message left that still tracks it every move.
			m_bMouseOverResizeBorder = edgeHit != HTNOWHERE;
			if (edgeHit != HTNOWHERE) {
				return edgeHit;
			}

			POINT clientPoint = cursorScreen;
			ScreenToClient(m_hWnd,
						   &clientPoint); // still physical pixels - convert before matching logical-space rects below

			const float logicalX = ToLogical(static_cast<float>(clientPoint.x), m_flDpiScale);
			const float logicalY = ToLogical(static_cast<float>(clientPoint.y), m_flDpiScale);

			if (logicalY >= 0.0f && logicalY < kTitleBarHeight) {
				if (TitleBarHitTest(logicalX, logicalY) == ETitleBarButton::TITLE_BAR_BUTTON_NONE) {
					return HTCAPTION;
				}
			}

			return HTCLIENT;
		}

		case WM_CLOSE:
			m_bShouldClose = true;
			return 0;

		// Windows sends this on essentially every mouse move over the window (and a few
		// other times besides) to ask "what cursor do you want here" - DefWindowProc's own
		// answer is always the window class's static hCursor (plain IDC_ARROW), which would
		// silently undo SetCursorKind's whole point the very next time the mouse so much as
		// twitches. Only intercepted for HTCLIENT: resize-edge/caption hit-tests (the
		// LOWORD(lParam) values WM_NCHITTEST above can return) fall through to
		// DefWindowProc so the OS's own resize-arrow cursors at the window edges still work.
		case WM_SETCURSOR:
			if (LOWORD(lParam) == HTCLIENT) {
				SetCursor(CursorForKind(m_cursorKind));
				return TRUE;
			}
			break;

		// WM_DPICHANGED fires when the window moves to a monitor with a different scale
		// factor (or the user changes a monitor's scale while the window is on it) - the
		// same "Win32 leaves no alternative but a synchronous callback" situation
		// DpiChangedCallback documents. wParam's HIWORD is the new DPI for both axes
		// (Windows never scales x/y independently); lParam is a RECT* Windows already
		// computed as the correct window rect for the new monitor, applied verbatim rather
		// than recomputed - the SetWindowPos below then delivers WM_SIZE, which drives the
		// actual repaint via m_pResizeCallback, same as any other resize.
		case WM_DPICHANGED: {
			m_flDpiScale = static_cast<float>(HIWORD(wParam)) / 96.0f;
			// Re-bake fonts etc. at the new scale *before* the SetWindowPos below triggers
			// a repaint, so that repaint never samples a stale-DPI atlas.
			if (m_pDpiChangedCallback != nullptr) {
				m_pDpiChangedCallback(m_pDpiChangedCallbackUserData);
			}

			const RECT *pSuggested = reinterpret_cast<RECT *>(lParam);
			SetWindowPos(m_hWnd, nullptr, pSuggested->left, pSuggested->top, pSuggested->right - pSuggested->left,
						 pSuggested->bottom - pSuggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
			return 0;
		}

		case WM_SIZE: {
			if (wParam == SIZE_MINIMIZED && m_bMinimizeToTray) {
				ShowWindow(m_hWnd, SW_HIDE);
				return 0;
			}
			m_nPhysicalWidth = LOWORD(lParam);
			m_nPhysicalHeight = HIWORD(lParam);
			m_nWidth =
				static_cast<std::uint32_t>(std::lround(ToLogical(static_cast<float>(m_nPhysicalWidth), m_flDpiScale)));
			m_nHeight =
				static_cast<std::uint32_t>(std::lround(ToLogical(static_cast<float>(m_nPhysicalHeight), m_flDpiScale)));
			// Synchronous, not polled: see ResizeCallback's comment in window.h - this is
			// what keeps the window painting while the user is actively dragging a resize
			// border, which nothing in the main loop can do here.
			if (m_pResizeCallback != nullptr) {
				m_pResizeCallback(m_pResizeCallbackUserData);
			}
			return 0;
		}

		case WM_LBUTTONDOWN: {
			const float x = ToLogical(static_cast<float>(GET_X_LPARAM(lParam)), m_flDpiScale);
			const float y = ToLogical(static_cast<float>(GET_Y_LPARAM(lParam)), m_flDpiScale);
			m_flLastMouseX = x;
			m_flLastMouseY = y;
			// See m_bMouseCaptured's comment in window.h - keeps drag input flowing here
			// even once the cursor leaves the client area.
			SetCapture(m_hWnd);
			m_bMouseCaptured = true;
			PushInputEvent(InputEvent{EInputEventType::INPUT_EVENT_MOUSE_DOWN, x, y, 0.0f, 0});
			return 0;
		}

		case WM_LBUTTONUP: {
			const float x = ToLogical(static_cast<float>(GET_X_LPARAM(lParam)), m_flDpiScale);
			const float y = ToLogical(static_cast<float>(GET_Y_LPARAM(lParam)), m_flDpiScale);
			m_flLastMouseX = x;
			m_flLastMouseY = y;
			// Cleared before ReleaseCapture so the WM_CAPTURECHANGED it synchronously
			// triggers sees a normal release, not an abnormal one, and skips synthesizing
			// a second MOUSE_UP.
			m_bMouseCaptured = false;
			ReleaseCapture();
			PushInputEvent(InputEvent{EInputEventType::INPUT_EVENT_MOUSE_UP, x, y, 0.0f, 0});
			return 0;
		}

		case WM_CAPTURECHANGED:
			// Capture was lost some other way than our own WM_LBUTTONUP's ReleaseCapture
			// (Alt+Tab mid-drag, a system dialog stealing capture, WM_CANCELMODE, ...) -
			// synthesize the MOUSE_UP that will now never arrive so nothing stays stuck
			// "pressed". See m_bMouseCaptured's comment in window.h.
			if (m_bMouseCaptured) {
				m_bMouseCaptured = false;
				PushInputEvent(
					InputEvent{EInputEventType::INPUT_EVENT_MOUSE_UP, m_flLastMouseX, m_flLastMouseY, 0.0f, 0});
			}
			return 0;

		case WM_RBUTTONUP:
			PushInputEvent(InputEvent{EInputEventType::INPUT_EVENT_RIGHT_MOUSE_UP,
									  ToLogical(static_cast<float>(GET_X_LPARAM(lParam)), m_flDpiScale),
									  ToLogical(static_cast<float>(GET_Y_LPARAM(lParam)), m_flDpiScale), 0.0f, 0});
			return 0;

		case WM_MOUSEMOVE: {
			const float x = ToLogical(static_cast<float>(GET_X_LPARAM(lParam)), m_flDpiScale);
			const float y = ToLogical(static_cast<float>(GET_Y_LPARAM(lParam)), m_flDpiScale);
			m_flLastMouseX = x;
			m_flLastMouseY = y;
			PushInputEvent(InputEvent{EInputEventType::INPUT_EVENT_MOUSE_MOVE, x, y, 0.0f, 0});
			return 0;
		}

		case WM_MOUSEWHEEL: {
			POINT cursor{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
			ScreenToClient(m_hWnd, &cursor);
			const float wheelDelta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
			PushInputEvent(InputEvent{EInputEventType::INPUT_EVENT_MOUSE_WHEEL,
									  ToLogical(static_cast<float>(cursor.x), m_flDpiScale),
									  ToLogical(static_cast<float>(cursor.y), m_flDpiScale), wheelDelta, 0});
			return 0;
		}

		case WM_KEYDOWN:
			PushInputEvent(InputEvent{EInputEventType::INPUT_EVENT_KEY_DOWN, 0.0f, 0.0f, 0.0f,
									  static_cast<std::uint32_t>(wParam)});
			return 0;

		case WM_CHAR:
			PushInputEvent(InputEvent{EInputEventType::INPUT_EVENT_CHAR_TYPED, 0.0f, 0.0f, 0.0f,
									  static_cast<std::uint32_t>(wParam)});
			return 0;

		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;

		default:
			break;
	}

	return DefWindowProcW(m_hWnd, message, wParam, lParam);
}

// Recovers the CWindow instance WM_NCCREATE stashed in GWLP_USERDATA and forwards
// everything else to its own HandleMessage - the original codebase's window_proc did this
// as a free function taking the Window* directly; here the class instance itself needs to
// learn its own HWND before HandleMessage can use it (m_hWnd isn't assigned by Create()
// until CreateWindowExW returns, which is after WM_NCCREATE/WM_NCCALCSIZE/first WM_SIZE
// have already fired *during* that call), so this sets m_hWnd from the hWnd WndProc itself
// receives on every message once the instance pointer is known - always the same value
// Create() will assign moments later, just available sooner.
LRESULT CALLBACK CWindow::WindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (message == WM_NCCREATE) {
		const auto *pCreate = reinterpret_cast<const CREATESTRUCTW *>(lParam);
		SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pCreate->lpCreateParams));
	}

	auto *pWindow = reinterpret_cast<CWindow *>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
	if (pWindow != nullptr) {
		pWindow->m_hWnd = hWnd;
		return pWindow->HandleMessage(message, wParam, lParam);
	}

	return DefWindowProcW(hWnd, message, wParam, lParam);
}
