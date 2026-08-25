#pragma once

// CWindow owns the single Win32 window this app runs in: a custom-drawn title bar (see
// ui/title_bar.h) instead of the OS-drawn caption, but WS_OVERLAPPEDWINDOW is kept so
// every native window-manager behavior (Aero Snap, the minimize/restore animation,
// Alt+Tab thumbnails, taskbar behavior) keeps working - none of that comes from the OS
// drawing the caption, it comes from the window style plus DWM composition, which is
// always on in Windows 10/11 regardless of whether an app draws its own frame. The only
// genuine DWM call this needs is DwmExtendFrameIntoClientArea (preserves the native drop
// shadow / Windows 11 rounded corners once WM_NCCALCSIZE removes the standard frame) -
// everything else title-bar-related here (WM_NCCALCSIZE, WM_NCHITTEST) is plain Win32.
//
// Hit-testing (WM_NCHITTEST, so drag-to-move/resize/Aero-Snap keep working) and drawing
// (CTitleBar) both read GetTitleBarButtonRect/TitleBarHitTest, so they can never drift
// apart. Both operate in logical pixels - the WM_NCHITTEST handler converts the physical
// screen point Win32 hands it down to logical first.
//
// Per-Monitor-V2 DPI awareness runs on a logical/physical split so nothing above this
// class needs to reason about DPI: GetWidth/GetHeight (and every Rift UI layout
// constant, hit-test, and InputEvent coordinate) stay in logical pixels regardless of the
// monitor's scale factor, while GetPhysicalWidth/Height are only for the swapchain/
// renderer boundary. GetDpiScale is physical/logical, 1.0 at 100% Windows scaling.
//
// Two things below are plain C function-pointer callbacks, not virtuals on some base
// class, because Win32 itself leaves no alternative: DefWindowProc enters its own modal
// message loop for the duration of a live border drag, and WM_DPICHANGED fires
// synchronously mid-move - both block this thread until they're done, so neither can be
// polled once per frame the way every other piece of state here is. Forcing them into
// virtual dispatch on some interface wouldn't remove the underlying Win32 constraint,
// just add an indirection for no benefit - see FORK_WITH_CLASSES.md 5.

#include <cstdint>

#include <Windows.h>

#include "core/types.h"

enum class EInputEventType : std::uint8_t {
	INPUT_EVENT_MOUSE_DOWN,
	INPUT_EVENT_MOUSE_UP,
	INPUT_EVENT_MOUSE_MOVE,
	INPUT_EVENT_MOUSE_WHEEL,
	INPUT_EVENT_RIGHT_MOUSE_UP, // WM_RBUTTONUP - opens CContextMenu; right-click-down isn't tracked, nothing needs a
								// right-drag
	INPUT_EVENT_KEY_DOWN,
	INPUT_EVENT_CHAR_TYPED, // a real typed character (WM_CHAR) - distinct from KEY_DOWN's VK_* code, which is a
							// physical key, not always a printable one
};

struct InputEvent {
	EInputEventType Type;
	float X; // logical pixels - see CWindow::GetDpiScale
	float Y; // logical pixels
	float WheelDelta;
	std::uint32_t KeyCode; // KEY_DOWN: a Win32 virtual-key code (VK_*). CHAR_TYPED: a UTF-16 code unit.
};

constexpr std::uint32_t kMaxInputEventsPerFrame = 64;

constexpr float kTitleBarHeight = 40.0f;
constexpr float kTitleBarButtonWidth = 46.0f;

// TITLE_BAR_BUTTON_UPDATE's own slot is wider than every other title bar button - it isn't
// a plain glyph, it's a labeled pill ("Update Available", "Update Failed", ...) - see
// ui/title_bar.cpp's own comment for why that's worth the extra width.
constexpr float kUpdateButtonWidth = 170.0f;

// The app's own bottom chrome strip (main.cpp draws its background; CCarousel draws its
// view-mode indicator into it - see ui/carousel.cpp's DrawStatusBarContent). Declared
// here, not main.cpp, so CCarousel can derive its own on-screen rect from this same
// constant instead of main.cpp having to hand it down some other way every frame.
constexpr float kStatusBarHeight = 26.0f;

enum class ETitleBarButton : std::uint8_t {
	TITLE_BAR_BUTTON_NONE,
	TITLE_BAR_BUTTON_MENU, // top-left; opens the app menu (CSettingsMenu) - Settings lives there
	// Immediately right of Menu - opens CUpdateOverlay once CUpdater actually has something
	// to show (see ui/title_bar.cpp's own comment). Always reserved as a real, non-HTCAPTION
	// hit-test slot here, same as every other button, even though CTitleBar only ever draws
	// into or responds on it while an update is actually available/in-flight - reserving the
	// geometry unconditionally (rather than threading CUpdater's own state into CWindow,
	// which has no business knowing that class exists) keeps this a pure layout constant,
	// matching every other button's own "CWindow only knows geometry" role.
	TITLE_BAR_BUTTON_UPDATE,
	TITLE_BAR_BUTTON_MINIMIZE,
	TITLE_BAR_BUTTON_MAXIMIZE, // toggles maximize/restore
	TITLE_BAR_BUTTON_CLOSE,
};

using ResizeCallback = void (*)(void *pUserData);
using DpiChangedCallback = void (*)(void *pUserData);

class CWindow {
  public:
	CWindow() = default;
	~CWindow();
	CWindow(const CWindow &) = delete;
	CWindow &operator=(const CWindow &) = delete;

	// Must succeed before any other method is called. width/height are logical pixels -
	// Create declares the process Per-Monitor-V2 DPI aware and sizes the actual Win32
	// window in physical pixels for whatever monitor it lands on, so the window always
	// *looks* the same size regardless of that monitor's scale factor; GetWidth/GetHeight
	// stay exactly what's requested here. Must not be called twice on the same instance,
	// and the instance's address must stay stable for its whole lifetime afterward (the
	// window procedure stashes `this` in GWLP_USERDATA during CreateWindowExW) - so a
	// CWindow belongs on the stack in main() or behind a stable pointer, never moved.
	//
	// The window is created hidden (no WS_VISIBLE, no ShowWindow call) - GetHandle/
	// GetWidth/GetPhysicalWidth/etc. are all already valid immediately after this returns
	// (WM_SIZE fires during CreateWindowExW regardless of visibility), so a caller can
	// finish the rest of startup - renderer/asset/font init, one real RenderFrame - against
	// a fully-formed window before Show() ever makes it visible. Skipping this staging and
	// showing the window immediately is exactly what used to paint whatever garbage the
	// backbuffer started with for a frame or two before the first real draw landed.
	bool Create(const wchar_t *pTitle, std::uint32_t width, std::uint32_t height);

	// Reveals the window Create() left hidden. Call this only once the renderer is
	// initialized and at least one real frame has been drawn and presented - see Create's
	// own comment for why.
	void Show();

	void SetResizeCallback(ResizeCallback callback, void *pUserData);
	void SetDpiChangedCallback(DpiChangedCallback callback, void *pUserData);

	// Pumps this thread's Win32 message queue (which also dispatches messages for any
	// other window on this thread, e.g. the tray icon's message-only window) and refills
	// the input-event queue GetInputEvents/GetInputEventCount expose - the previous
	// frame's events are discarded at the start of this call, so a caller must read them
	// (once) between two PumpMessages calls. Also updates ShouldClose/GetWidth/GetHeight.
	void PumpMessages();

	HWND GetHandle() const
	{
		return m_hWnd;
	}
	std::uint32_t GetWidth() const
	{
		return m_nWidth;
	}
	std::uint32_t GetHeight() const
	{
		return m_nHeight;
	}
	std::uint32_t GetPhysicalWidth() const
	{
		return m_nPhysicalWidth;
	}
	std::uint32_t GetPhysicalHeight() const
	{
		return m_nPhysicalHeight;
	}
	float GetDpiScale() const
	{
		return m_flDpiScale;
	}
	bool ShouldClose() const
	{
		return m_bShouldClose;
	}

	const InputEvent *GetInputEvents() const
	{
		return m_aInputEvents;
	}
	std::uint32_t GetInputEventCount() const
	{
		return m_nInputEventCount;
	}

	// Client-space rectangle (logical pixels) for one of the title bar buttons: Menu is
	// left-aligned at the window's top-left corner; Minimize/Maximize/Close are
	// right-aligned in that order (left to right).
	Rect GetTitleBarButtonRect(ETitleBarButton button) const;

	// Sets the OS cursor glyph shown over the client area - call once per frame (after
	// CWidgetStack::Update) with whatever CWidgetStack::GetDesiredCursor just answered.
	// Applies immediately (so a frame that changes cursor kind doesn't wait on the next
	// WM_SETCURSOR to look right) and remembers the kind so WM_SETCURSOR - which Windows
	// re-sends on essentially every mouse move, and would otherwise reset the cursor back
	// to the window class's default IDC_ARROW via DefWindowProc - can keep reapplying the
	// same answer itself between calls to this.
	void SetCursorKind(ECursorKind kind);

	// True if the mouse is currently over the resize-border strip (see HitTestResizeEdges
	// in the .cpp) - kept up to date by WM_NCHITTEST itself, not recomputed from polled
	// mouse position: once the cursor crosses into that strip it's non-client, so Windows
	// stops sending WM_MOUSEMOVE (sends WM_NCMOUSEMOVE instead) and the polled position
	// this window otherwise tracks goes stale right at the one moment it matters. The main
	// loop skips its per-frame SetCursorKind call while this is true, so it doesn't fight
	// the OS's own resize cursor there.
	bool IsMouseOverResizeBorder() const
	{
		return m_bMouseOverResizeBorder;
	}

	// Which title bar button (if any) contains the given client-space (logical) point.
	ETitleBarButton TitleBarHitTest(float clientX, float clientY) const;

	// Whether TITLE_BAR_BUTTON_UPDATE's reserved slot currently participates in hit-testing
	// at all - CTitleBar calls this once per frame from its own Update (see title_bar.cpp's
	// IsUpdateButtonVisible), forwarding whatever CUpdater's current stage says. False (the
	// default, and the common case - see core/updater.h) makes that slot fall through to
	// plain HTCAPTION in WM_NCHITTEST, the same draggable empty title-bar space it would be
	// if the button didn't exist at all, rather than a permanently dead, undraggable strip
	// sitting next to the Menu button whenever there's nothing to show there.
	void SetUpdateButtonVisible(bool visible)
	{
		m_bUpdateButtonVisible = visible;
	}

	// While true, a minimize (the title bar's own button, or the OS's) hides the window
	// instead, leaving the tray icon as the only way back to it.
	void SetMinimizeToTray(bool minimizeToTray)
	{
		m_bMinimizeToTray = minimizeToTray;
	}

	bool IsHidden() const
	{
		return IsWindowVisible(m_hWnd) == FALSE;
	}

	// Undoes a minimize-to-tray hide, or an ordinary minimize.
	void Restore();

	// Asks an already-running Rift to show itself, for a second instance to call before
	// bowing out. Returns false if none answered within timeoutMs - the wait exists because
	// the running instance may still be starting up and have no window yet, and a
	// double-click during that window should still surface it rather than silently do
	// nothing. Static: the caller has no CWindow of its own and must not create one.
	static bool ActivateExistingInstance(std::uint32_t timeoutMs = 3000);

  private:
	static LRESULT CALLBACK WindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
	LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
	void PushInputEvent(const InputEvent &event);

	HWND m_hWnd = nullptr;

	std::uint32_t m_nWidth = 0;
	std::uint32_t m_nHeight = 0;
	std::uint32_t m_nPhysicalWidth = 0;
	std::uint32_t m_nPhysicalHeight = 0;
	float m_flDpiScale = 1.0f;

	bool m_bShouldClose = false;
	bool m_bUpdateButtonVisible = false; // see SetUpdateButtonVisible
	bool m_bMinimizeToTray = false;		 // see SetMinimizeToTray

	// SetCapture on WM_LBUTTONDOWN keeps WM_MOUSEMOVE/WM_LBUTTONUP routed to this window
	// even once the cursor leaves the client area mid-drag - without it, dragging the
	// mouse out of the window and releasing there sends the button-up to whatever's under
	// the cursor (or nowhere), so this window never sees MOUSE_UP and any in-progress drag
	// (CCarousel's card drag, a scrollbar thumb, ...) is left stuck "pressed" forever, the
	// same class of bug title_bar.h's OnPointerDown/Up asymmetry fixed via a different
	// mechanism. m_bMouseCaptured + m_flLastMouseX/Y back a WM_CAPTURECHANGED handler that
	// synthesizes the MOUSE_UP if capture is ever lost some other way (Alt+Tab, a system
	// dialog stealing it, WM_CANCELMODE) instead of relying solely on WM_LBUTTONUP - the
	// left button's own WM_LBUTTONUP handler clears this flag before its own ReleaseCapture
	// call, so the synthesized path only fires for a genuinely abnormal capture loss.
	bool m_bMouseCaptured = false;
	float m_flLastMouseX = 0.0f;
	float m_flLastMouseY = 0.0f;

	// See IsMouseOverResizeBorder - set from WM_NCHITTEST, the one message that still fires
	// while the cursor is over this strip.
	bool m_bMouseOverResizeBorder = false;

	ResizeCallback m_pResizeCallback = nullptr;
	void *m_pResizeCallbackUserData = nullptr;

	DpiChangedCallback m_pDpiChangedCallback = nullptr;
	void *m_pDpiChangedCallbackUserData = nullptr;

	// The kind SetCursorKind last set - WM_SETCURSOR's handler reapplies this on every
	// Windows-initiated query so it sticks between SetCursorKind calls (see that method's
	// own comment).
	ECursorKind m_cursorKind = ECursorKind::CURSOR_ARROW;

	InputEvent m_aInputEvents[kMaxInputEventsPerFrame]{};
	std::uint32_t m_nInputEventCount = 0;
};
