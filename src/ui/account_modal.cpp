#include "ui/account_modal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <Windows.h>

#include "asset_manager.h"
#include "core/animator.h"
#include "ui/draw_list.h"
#include "ui/hoverable.h"
#include "ui/settings.h"
#include "ui/text.h"
#include "window.h"

namespace {
constexpr float kOpenEaseRate = 14.0f;

// The panel is clamped to a fixed max size (not a fraction of the window) so it
// doesn't balloon on a large/1440p+ monitor. Width stayed at 810; height is shorter
// than a plain 1.5 aspect would give (480, not 540) - a flatter panel that takes up
// less vertical space. kPanelMaxSizeScale grows both in lockstep with a larger Font
// Size setting so bigger text gets more room instead of being squeezed/clipped into
// the original size.
constexpr float kPanelMaxWidthBase = 810.0f;
constexpr float kPanelMaxHeightBase = 480.0f;
// Real baked pixels matching the default Settings Font Size of 16 nominal units - the
// panel's base size was tuned at that default.
constexpr float kReferenceBodyPixelHeight = 24.0f;
constexpr float kPanelMargin = 48.0f;	// never closer than this to the window edge
constexpr float kPanelScaleMin = 0.92f; // starting scale of the "pop in" animation
constexpr float kPanelBorderThickness = 1.5f;
constexpr float kPanelRadius = 16.0f;

constexpr float kLeftColumnFraction = 0.35f;
constexpr float kSeparatorThickness = 2.0f;

constexpr float kCloseBadgeSize = 40.0f;
constexpr float kCloseBadgeMargin = 12.0f;
constexpr float kCloseBadgeIconSize = 18.0f;

constexpr float kRowPadding = 24.0f;
constexpr float kRowTopPadding = 14.0f; // above the username line
constexpr float kRowLineGap = 4.0f;		// between the username and note lines
constexpr float kRowBottomPadding = 10.0f;
constexpr float kRowGap = 8.0f;
constexpr float kRowButtonGap = 10.0f;

constexpr float kLoginButtonWidth = 108.0f;
constexpr float kLoginButtonMargin = 16.0f;

// The "visible in N games" chip's own icon + count number - see VisibilityChipRect and
// DrawEditAccount, the one place both its size and its drawing are computed, so they
// can't disagree about how wide the chip needs to be to fit its own content.
constexpr float kVisibilityChipIconSize = 20.0f;
constexpr float kVisibilityChipIconInset = 12.0f;
constexpr float kVisibilityChipIconGap = 8.0f;
constexpr float kVisibilityChipRightPadding = 16.0f;

float PanelMaxSizeScale(const CFontManager &fonts)
{
	return std::max(1.0f, fonts.GetBody().GetPixelHeight() / kReferenceBodyPixelHeight);
}

// How many games mask marks visible - shared by VisibilityChipRect (sizing) and
// DrawEditAccount (the actual number drawn), so the two can never disagree. mask == 0
// means "no explicit choice made yet", which GetEffectiveVisibleMask treats as "this
// account's own banner only", so this reads that the same way: count 1.
std::uint32_t VisibleGameCountFromMask(std::uint16_t mask)
{
	if (mask == 0) {
		return 1;
	}
	std::uint32_t count = 0;
	for (std::uint32_t b = 0; b < kCarouselMaxBanners; b += 1) {
		if ((mask & (1u << b)) != 0) {
			count += 1;
		}
	}
	return count;
}

// "1 game" / "N games" - shared by VisibilityChipRect (sizing) and DrawEditAccount (drawing).
std::uint64_t FormatVisibleGameCountText(std::uint16_t mask, char *pOut, std::size_t outCapacity)
{
	const std::uint32_t count = VisibleGameCountFromMask(mask);
	const int written = std::snprintf(pOut, outCapacity, "%u %s", count, count == 1 ? "game" : "games");
	return written > 0 ? static_cast<std::uint64_t>(written) : 0;
}

float RowHeightFor(const CFontManager &fonts)
{
	return kRowTopPadding + fonts.GetBody().GetLineHeight() + kRowLineGap + fonts.GetSecondary().GetLineHeight() +
		   kRowBottomPadding;
}

// Body, not secondary: the section title ("Accounts", "Edit Account", "Add Account")
// is a real title treatment, not a barely-there caption, so the height it needs to
// fit derives from the bigger face too.
float HeaderHeightFor(const CFontManager &fonts)
{
	return fonts.GetBody().GetLineHeight() + 20.0f;
}

float FooterHeightFor(const CFontManager &fonts)
{
	return std::max(56.0f, fonts.GetBody().GetLineHeight() + 20.0f);
}

// Login/Save/Cancel/Delete's shared height - one size for every primary footer action.
float ActionButtonHeightFor(const CFontManager &fonts)
{
	return std::max(36.0f, fonts.GetBody().GetLineHeight() + 14.0f);
}

// Body, not secondary: the field holds a value the user typed (a username, note, or
// password), not a dim label.
float EditFieldInputHeightFor(const CFontManager &fonts)
{
	return std::max(34.0f, fonts.GetBody().GetLineHeight() + 12.0f);
}

// The label sits close to its own box (kEditFieldLabelGap); the larger gap
// (kEditFieldGroupGap) comes after the box, separating it from the *next* field's
// label.
constexpr float kEditFieldLabelGap = 8.0f;
constexpr float kEditFieldGroupGap = 26.0f;

float EditFieldBlockHeightFor(const CFontManager &fonts)
{
	return fonts.GetSecondary().GetLineHeight() + kEditFieldLabelGap + EditFieldInputHeightFor(fonts) +
		   kEditFieldGroupGap;
}

// Row action buttons (edit/remove) - derived from real font metrics like every other
// control here, rather than a flat pixel constant that reads undersized at a larger
// Font Size.
float RowButtonSizeFor(const CFontManager &fonts)
{
	return std::max(28.0f, fonts.GetSecondary().GetLineHeight() + 8.0f);
}

constexpr Color kColorTextBright{232, 232, 236, 255};
constexpr Color kColorTextDim{150, 150, 156, 255};
constexpr Color kColorTextFaint{130, 130, 136, 255};
constexpr Color kColorSuccess{80, 200, 120, 255};
constexpr Color kColorError{220, 90, 80, 255};

// A soft shadow beneath the panel - layered semi-transparent dark rounded rects,
// offset down slightly, so the panel reads as lifted off the dimmed backdrop instead
// of looking flat-pasted onto it. Same layering trick CCarousel's card glow uses.
void DrawPanelShadow(CDrawList &drawList, Rect panel, float amount)
{
	constexpr int kLayers = 4;
	constexpr float kMaxExpand = 20.0f;
	constexpr float kYOffset = 10.0f;
	for (int i = kLayers; i >= 1; i -= 1) {
		const float t = static_cast<float>(i) / static_cast<float>(kLayers);
		const float expand = kMaxExpand * t;
		const auto shadowAlpha = static_cast<std::uint8_t>(18.0f * t * amount);
		drawList.AddRectRoundedFilled(panel.X - expand, panel.Y - expand + kYOffset, panel.W + expand * 2.0f,
									  panel.H + expand * 2.0f, CDrawList::UniformRadii(kPanelRadius + expand * 0.4f),
									  Color{0, 0, 0, shadowAlpha});
	}
}

// A plain textured icon filling `rect` exactly - a no-op if the texture failed to
// load, so a missing asset degrades to "nothing drawn" rather than a garbage sample.
void DrawCenteredTexture(CDrawList &drawList, Rect rect, const CTexture *pTexture, Color tint)
{
	if (pTexture == nullptr) {
		return;
	}
	drawList.AddRectRoundedTextured(rect.X, rect.Y, rect.W, rect.H, kCornerRadiiNone, pTexture, tint);
}

// The row remove button's glyph - no embedded icon exists for this, so it's
// hand-drawn like the grid/eye glyphs below.
void DrawXGlyph(CDrawList &drawList, Rect rect, Color color)
{
	const float cx = rect.X + rect.W * 0.5f;
	const float cy = rect.Y + rect.H * 0.5f;
	const float r = std::min(rect.W, rect.H) * 0.24f;
	const float thickness = std::max(2.0f, rect.H * 0.09f);
	drawList.AddLine(cx - r, cy - r, cx + r, cy + r, thickness, color);
	drawList.AddLine(cx - r, cy + r, cx + r, cy - r, thickness, color);
}

// The password field's show/hide button - EyeVisible/EyeHiddenIcon (asset_manager.h),
// not the hand-drawn ring+pupil glyph this used to be.
void DrawEyeGlyph(CDrawList &drawList, CAssetManager &assets, Rect rect, bool revealed, Color color)
{
	drawList.AddRectRoundedTextured(rect.X, rect.Y, rect.W, rect.H, kCornerRadiiNone,
									revealed ? assets.GetIconEyeVisible() : assets.GetIconEyeHidden(), color);
}

// --- circular login progress indicator ---
constexpr float kIndicatorOuterRadius = 40.0f;
constexpr float kIndicatorRingThickness = 8.0f;
constexpr float kIndicatorInnerRadius = kIndicatorOuterRadius - kIndicatorRingThickness;
constexpr float kIndicatorGlowMargin = 22.0f;			// how far the shader's halo can bloom outward
constexpr float kIndicatorSweepDeg = 112.0f;			// the comet-tail arc's angular length
constexpr float kIndicatorRotationDegPerSec = 260.0f; // how fast the in-progress sweep spins
constexpr float kIndicatorFullSweepDeg = 360.0f;		// CDrawList::AddCircularProgress's "solid ring" sentinel

// Reflects what CLoginAttempt's worker (core/login_attempt.cpp) actually does at each stage
// now that it drives a real CRiotClient instead of a fake timed sequence - WAITING_FOR_PROCESS
// covers both launching the Riot Client and waiting for its window to exist (the two are
// merged from the UI's perspective; the worker doesn't distinguish them either), CONNECTING
// is bringing that window forward and giving it focus, AUTHENTICATING is the form actually
// being submitted. SUCCESS/ERROR are only ever shown as a fallback - see DrawLoginProgress,
// which prefers CLoginAttempt::GetTerminalMessage's real (and, for an error, Riot's own)
// message whenever one is available.
CStringView LoginStageMessage(ELoginStage stage)
{
	switch (stage) {
		case ELoginStage::LOGIN_STAGE_IDLE:
			return StringViewFromCString("");
		case ELoginStage::LOGIN_STAGE_WAITING_FOR_PROCESS:
			return StringViewFromCString("Launching Riot Client...");
		case ELoginStage::LOGIN_STAGE_CONNECTING:
			return StringViewFromCString("Waiting for Riot Client...");
		case ELoginStage::LOGIN_STAGE_AUTHENTICATING:
			return StringViewFromCString("Logging in...");
		case ELoginStage::LOGIN_STAGE_LAUNCHING:
			return StringViewFromCString("Launching game...");
		case ELoginStage::LOGIN_STAGE_SUCCESS:
			return StringViewFromCString("Logged in!");
		case ELoginStage::LOGIN_STAGE_ERROR:
			return StringViewFromCString("Something went wrong.");
		case ELoginStage::LOGIN_STAGE_CANCELLED:
			return StringViewFromCString("Cancelled.");
	}
	return StringViewFromCString("");
}

// Greedy word-wrap into at most kMaxLoginMessageLines lines, each no wider than maxWidth
// (TextWidth-measured) - needed once the login progress message can be a real backend error
// straight from the Riot Client (see CLoginAttempt::GetTerminalMessage) rather than one of
// the short, fixed strings above; "Your login credentials don't match an account in our
// system." alone is comfortably wider than this panel's column at any normal font size. A
// single word wider than maxWidth on its own still gets its own line rather than being cut
// mid-word - not worth a harder wrap for text this short-lived. Returns how many of
// outLines were actually filled in.
constexpr std::uint32_t kMaxLoginMessageLines = 3;

std::uint32_t WrapLoginMessage(const CFont &font, CStringView text, float maxWidth,
							   CStringView outLines[kMaxLoginMessageLines])
{
	std::uint32_t lineCount = 0;
	std::uint64_t lineStart = 0;
	std::uint64_t i = 0;

	while (i < text.Length && lineCount < kMaxLoginMessageLines) {
		while (i < text.Length && text.pData[i] == ' ') {
			i += 1;
		}
		const std::uint64_t wordStart = i;
		while (i < text.Length && text.pData[i] != ' ') {
			i += 1;
		}
		const std::uint64_t wordEnd = i;
		if (wordStart == wordEnd) {
			break;
		}

		const bool lineHasContent = wordStart > lineStart;
		const CStringView candidate{text.pData + lineStart, wordEnd - lineStart};
		if (lineHasContent && TextWidth(font, candidate) > maxWidth) {
			// This word doesn't fit on the current line - commit everything before it
			// (trimming the trailing space) and start a new line here instead.
			outLines[lineCount] = CStringView{text.pData + lineStart, wordStart - 1 - lineStart};
			lineCount += 1;
			lineStart = wordStart;
		}
	}

	if (lineCount < kMaxLoginMessageLines && lineStart < text.Length) {
		outLines[lineCount] = CStringView{text.pData + lineStart, text.Length - lineStart};
		lineCount += 1;
	}
	return lineCount;
}
} // namespace

CAccountModal::CAccountModal(CFontManager &fonts, CCarousel &carousel, CWindow &window, const CSettings &settings,
							 CAssetManager &assets)
	: m_fonts(fonts)
	, m_carousel(carousel)
	, m_window(window)
	, m_settings(settings)
	, m_assets(assets)
	, m_gameSelect(fonts)
{
	m_editUsername.Init(StringViewFromCString(""));
	m_editNote.Init(StringViewFromCString(""));
	m_editPassword.Init(StringViewFromCString(""));
	m_login.Init();
}

// --- Layout ---

Rect CAccountModal::PanelRect() const
{
	const float sizeScale = PanelMaxSizeScale(m_fonts);
	const float panelMaxWidth = kPanelMaxWidthBase * sizeScale;
	const float panelMaxHeight = kPanelMaxHeightBase * sizeScale;

	const auto windowW = static_cast<float>(m_window.GetWidth());
	const auto windowH = static_cast<float>(m_window.GetHeight());
	const float availableW = std::max(0.0f, windowW - kPanelMargin * 2.0f);
	const float availableH = std::max(0.0f, windowH - kPanelMargin * 2.0f);

	float w = std::min(panelMaxWidth, availableW);
	float h = std::min(panelMaxHeight, availableH);

	// Whichever dimension is the binding constraint wins; shrink the other to match the
	// target aspect so clamping never distorts the panel.
	const float targetAspect = panelMaxWidth / panelMaxHeight;
	if (w / h > targetAspect) {
		w = h * targetAspect;
	} else {
		h = w / targetAspect;
	}

	const float openScale = kPanelScaleMin + (1.0f - kPanelScaleMin) * m_flOpenAmount;
	w *= openScale;
	h *= openScale;

	return Rect{(windowW - w) * 0.5f, (windowH - h) * 0.5f, w, h};
}

CAccountModal::Layout CAccountModal::ComputeLayout() const
{
	Layout layout{};
	layout.Panel = PanelRect();
	layout.Inner = Rect{layout.Panel.X + kPanelBorderThickness, layout.Panel.Y + kPanelBorderThickness,
						layout.Panel.W - kPanelBorderThickness * 2.0f, layout.Panel.H - kPanelBorderThickness * 2.0f};

	const float footerHeight = FooterHeightFor(m_fonts);
	layout.Content = Rect{layout.Inner.X, layout.Inner.Y, layout.Inner.W, layout.Inner.H - footerHeight};
	layout.Footer = Rect{layout.Inner.X, layout.Inner.Y + layout.Inner.H - footerHeight, layout.Inner.W, footerHeight};

	layout.Left = Rect{layout.Content.X, layout.Content.Y, layout.Content.W * kLeftColumnFraction, layout.Content.H};
	const float rightX = layout.Content.X + layout.Content.W * kLeftColumnFraction + kSeparatorThickness;
	layout.Right = Rect{rightX, layout.Content.Y, layout.Content.X + layout.Content.W - rightX, layout.Content.H};

	return layout;
}

Rect CAccountModal::AccountsScrollRegionRect(Rect right) const
{
	const float headerHeight = HeaderHeightFor(m_fonts);
	return Rect{right.X, right.Y + headerHeight, right.W, right.H - headerHeight};
}

float CAccountModal::AccountsContentHeight(std::uint32_t accountCount) const
{
	if (accountCount == 0) {
		return 0.0f;
	}
	const float rowHeight = RowHeightFor(m_fonts);
	return static_cast<float>(accountCount) * rowHeight + static_cast<float>(accountCount - 1) * kRowGap;
}

constexpr float kScrollbarTrackMargin = 4.0f;

Rect CAccountModal::AccountsScrollbarTrackRect(Rect scrollRegion) const
{
	return Rect{scrollRegion.X + scrollRegion.W - kScrollbarWidth - kScrollbarTrackMargin, scrollRegion.Y,
				kScrollbarWidth, scrollRegion.H};
}

Rect CAccountModal::CloseBadgeRect(Rect left) const
{
	return Rect{left.X + kCloseBadgeMargin, left.Y + kCloseBadgeMargin, kCloseBadgeSize, kCloseBadgeSize};
}

Rect CAccountModal::LoginButtonRect(Rect footer) const
{
	const float height = ActionButtonHeightFor(m_fonts);
	return Rect{footer.X + footer.W - kLoginButtonMargin - kLoginButtonWidth, footer.Y + (footer.H - height) * 0.5f,
				kLoginButtonWidth, height};
}

Rect CAccountModal::RowRect(Rect right, std::uint32_t index) const
{
	const float headerHeight = HeaderHeightFor(m_fonts);
	const float rowHeight = RowHeightFor(m_fonts);
	return Rect{right.X + kRowPadding,
				right.Y + headerHeight + static_cast<float>(index) * (rowHeight + kRowGap) -
					m_accountsScroll.m_flScrollOffset,
				right.W - kRowPadding * 2.0f, rowHeight};
}

Rect CAccountModal::RowRemoveButtonRect(Rect row) const
{
	const float size = RowButtonSizeFor(m_fonts);
	return Rect{row.X + row.W - size, row.Y + (row.H - size) * 0.5f, size, size};
}

Rect CAccountModal::RowEditButtonRect(Rect row) const
{
	const Rect remove = RowRemoveButtonRect(row);
	return Rect{remove.X - kRowButtonGap - remove.W, remove.Y, remove.W, remove.H};
}

// A circular icon-only badge now, not a wide "+ Add Account" pill - same
// RowButtonSizeFor sizing and hover treatment as a row's own Edit/Remove buttons (see
// DrawAddAccountButton), just anchored in the header instead of a row.
Rect CAccountModal::AddAccountButtonRect(Rect right) const
{
	const float headerHeight = HeaderHeightFor(m_fonts);
	const float size = RowButtonSizeFor(m_fonts);
	return Rect{right.X + right.W - kRowPadding - size, right.Y + (headerHeight - size) * 0.5f, size, size};
}

// The 4 stacked blocks (Username/Note/Password/visibility chip) settle into the upper
// portion of the available space (weighted well above dead-center) rather than a large
// dead gap before the footer, or being perfectly centered the way most dialogs don't lay
// out a short form.
float CAccountModal::EditFormContentOffsetY(Rect right) const
{
	const float available = right.H - HeaderHeightFor(m_fonts);
	const float contentHeight = EditFieldBlockHeightFor(m_fonts) * 4.0f;
	return std::max(0.0f, (available - contentHeight) * 0.22f);
}

Rect CAccountModal::EditFieldBlockRect(Rect right, std::uint32_t index, float yOffset) const
{
	const float headerHeight = HeaderHeightFor(m_fonts);
	const float blockHeight = EditFieldBlockHeightFor(m_fonts);
	return Rect{right.X + kRowPadding, right.Y + headerHeight + yOffset + static_cast<float>(index) * blockHeight,
				right.W - kRowPadding * 2.0f, blockHeight};
}

// Anchored right below the label rather than the bottom of the block - bottom-anchoring
// was a real bug (all the block's slack piled up above the field instead of splitting
// into a small label gap and a larger trailing gap before the next group).
Rect CAccountModal::EditFieldInputRect(Rect block) const
{
	const float labelHeight = m_fonts.GetSecondary().GetLineHeight();
	return Rect{block.X, block.Y + labelHeight + kEditFieldLabelGap, block.W, EditFieldInputHeightFor(m_fonts)};
}

// Occupies the 4th stacked block position, right after Username/Note/Password. A pill
// (icon + its own visible-game count, plain text - see DrawEditAccount), sized to fit
// exactly that content rather than a fixed width: a floating count badge here used to
// clip into the very popup this chip opens (anchored right at its own edge, chip itself
// below), a real reported bug that giving the number its own place in the chip's normal
// layout avoids entirely.
Rect CAccountModal::VisibilityChipRect(Rect right, float yOffset) const
{
	// Same input-row band as EditFieldInputRect - reads as a real field, label and all
	// (see DrawEditAccount), not a stray icon in the block's empty label row.
	const Rect block = EditFieldBlockRect(right, 3, yOffset);
	const float labelHeight = m_fonts.GetSecondary().GetLineHeight();
	const float y = block.Y + labelHeight + kEditFieldLabelGap;
	const float height = EditFieldInputHeightFor(m_fonts);

	char countBuffer[16];
	const std::uint64_t countLen = FormatVisibleGameCountText(m_gameSelect.GetMask(), countBuffer, sizeof(countBuffer));
	const float countTextW = TextWidth(m_fonts.GetSecondary(), CStringView{countBuffer, countLen});

	const float width = kVisibilityChipIconInset + kVisibilityChipIconSize + kVisibilityChipIconGap + countTextW +
						kVisibilityChipRightPadding;
	return Rect{block.X, y, width, height};
}

// Save reuses LoginButtonRect's exact geometry so the primary action always lands in the
// same spot the Login button would; Cancel sits to its left.
Rect CAccountModal::EditCancelButtonRect(Rect save) const
{
	return Rect{save.X - kLoginButtonMargin - kLoginButtonWidth, save.Y, kLoginButtonWidth, save.H};
}

// Bottom-left of the footer - Delete, only shown when editing an existing row.
Rect CAccountModal::EditDeleteButtonRect(Rect footer) const
{
	const float height = ActionButtonHeightFor(m_fonts);
	return Rect{footer.X + kLoginButtonMargin, footer.Y + (footer.H - height) * 0.5f, kLoginButtonWidth, height};
}

constexpr float kPasswordRevealButtonSize = 24.0f;

Rect CAccountModal::PasswordRevealButtonRect(Rect passwordField) const
{
	return Rect{passwordField.X + passwordField.W - kPasswordRevealButtonSize - 6.0f,
				passwordField.Y + (passwordField.H - kPasswordRevealButtonSize) * 0.5f, kPasswordRevealButtonSize,
				kPasswordRevealButtonSize};
}

bool CAccountModal::ResolveVisibleAccount(std::uint32_t bannerIndex, std::uint32_t queryIndex,
										  VisibleAccountRef &out) const
{
	VisibleAccountRef refs[kCarouselMaxVisibleAccounts];
	const std::uint32_t count = m_carousel.GetVisibleAccounts(bannerIndex, refs);
	if (queryIndex >= count) {
		return false;
	}
	out = refs[queryIndex];
	return true;
}

void CAccountModal::StartLoginFor(std::uint32_t bannerIndex, std::uint32_t queryIndex)
{
	VisibleAccountRef ref{};
	if (!ResolveVisibleAccount(bannerIndex, queryIndex, ref)) {
		return;
	}
	const CAccount &account = m_carousel.GetBanner(ref.BannerIndex).Accounts[ref.AccountIndex];

	// The launch-product target comes from bannerIndex (the game view the user was actually
	// looking at when they clicked Login), not ref.BannerIndex (wherever this account's own
	// record happens to be stored) - a single Riot account is shared across every Riot game,
	// and this app's own cross-visibility feature can show the same account under a banner
	// it wasn't created under, so "which banner owns the CAccount data" and "which game the
	// user meant to launch" are genuinely different questions. Confirmed as a real bug, not
	// just theoretical: a cross-visible account launched the wrong game's client until this
	// used bannerIndex here.
	const CBanner &viewedBanner = m_carousel.GetBanner(bannerIndex);
	m_login.Start(account.GetUsername(), StringViewFromCString(account.m_szPassword), viewedBanner.Title);
}

// --- Public API ---

void CAccountModal::Open(std::int32_t bannerIndex)
{
	m_bIsOpen = true;
	m_nBannerIndex = bannerIndex;
	m_mode = EAccountModalMode::ACCOUNT_MODAL_MODE_ACCOUNT_LIST;
	m_accountsScroll =
		CScrollable{}; // fresh scroll position per open - a different banner has a different account count

	// No account is pre-selected on open - the user picks a row explicitly (or none at
	// all, and Login just stays disabled - see DrawFooter).
	m_nSelectedAccountIndex = -1;
}

void CAccountModal::Close()
{
	m_bIsOpen = false;
}

void CAccountModal::OpenForQuickLogin(std::int32_t bannerIndex, std::int32_t accountIndex)
{
	Open(bannerIndex);

	// A previous attempt on this same CLoginAttempt might not have actually finished yet (see
	// CLoginAttempt::Start's own comment) - fall back to the plain account list Open() already
	// set up rather than showing a login progress view for an attempt that silently didn't
	// start.
	if (m_login.IsActive()) {
		return;
	}

	m_nSelectedAccountIndex = accountIndex;
	m_mode = EAccountModalMode::ACCOUNT_MODAL_MODE_LOGIN_PROGRESS;
	m_flLoginElapsedSeconds = 0.0f;
	StartLoginFor(static_cast<std::uint32_t>(bannerIndex), static_cast<std::uint32_t>(accountIndex));
}

void CAccountModal::StartAddAccount()
{
	m_mode = EAccountModalMode::ACCOUNT_MODAL_MODE_EDIT_ACCOUNT;
	m_nEditAccountIndex = -1;
	m_editUsername.SetValue(StringViewFromCString(""));
	m_editNote.SetValue(StringViewFromCString(""));
	m_editPassword.SetValue(StringViewFromCString(""));
	m_editUsername.m_bFocused = true;
	m_editNote.m_bFocused = false;
	m_editPassword.m_bFocused = false;
	m_bEditPasswordRevealed = false;
	// Explicitly the currently-open banner's own bit, not a raw 0 "implicit" mask - a raw
	// 0 would leave the chip claiming "Visible in 1 game" (GetEffectiveVisibleMask's
	// display convention) while the popup itself showed no row checked at all. Being
	// explicit from the start also means this is already the "sole remaining checked row"
	// CGameSelectPopup protects - a brand new account can't accidentally end up visible
	// nowhere.
	const std::uint16_t initialMask = m_nBannerIndex >= 0 ? static_cast<std::uint16_t>(1u << m_nBannerIndex) : 0;
	// Seeds the popup's working mask without actually showing it - Open+immediate-Close,
	// matching CGameSelectPopup's "mask persists across close/reopen within the same edit
	// session" contract. bounds is never actually read (Close() runs before any Draw/
	// hit-test could), so a plain full-window rect is fine here.
	m_gameSelect.Open(
		initialMask, &m_carousel.GetBanner(0), m_carousel.GetBannerCount(), Rect{},
		Rect{0.0f, 0.0f, static_cast<float>(m_window.GetWidth()), static_cast<float>(m_window.GetHeight())});
	m_gameSelect.Close();
}

void CAccountModal::StartEditAccount(std::uint32_t queryIndex)
{
	m_mode = EAccountModalMode::ACCOUNT_MODAL_MODE_EDIT_ACCOUNT;
	m_nEditAccountIndex = static_cast<std::int32_t>(queryIndex);
	std::uint16_t mask = 0;

	if (m_nBannerIndex >= 0) {
		VisibleAccountRef ref{};
		if (ResolveVisibleAccount(static_cast<std::uint32_t>(m_nBannerIndex), queryIndex, ref)) {
			const CAccount &account = m_carousel.GetBanner(ref.BannerIndex).Accounts[ref.AccountIndex];
			m_editUsername.SetValue(account.GetUsername());
			m_editNote.SetValue(account.GetNote());
			m_editPassword.SetValue(StringViewFromCString(account.m_szPassword));
			mask = account.GetEffectiveVisibleMask(ref.BannerIndex);
		}
	}

	m_editUsername.m_bFocused = true;
	m_editNote.m_bFocused = false;
	m_editPassword.m_bFocused = false;
	m_bEditPasswordRevealed = false;
	m_gameSelect.Close();

	// The popup's Open snapshots its own working mask; re-open+close immediately just to
	// seed it without actually showing it, matching CGameSelectPopup's "mask persists
	// across close/reopen within the same edit session" contract. bounds is never
	// actually read here, same reasoning as StartAddAccount's own seed-only Open call.
	m_gameSelect.Open(
		mask, &m_carousel.GetBanner(0), m_carousel.GetBannerCount(), Rect{},
		Rect{0.0f, 0.0f, static_cast<float>(m_window.GetWidth()), static_cast<float>(m_window.GetHeight())});
	m_gameSelect.Close();
}

void CAccountModal::RemoveAccountRow(std::uint32_t queryIndex)
{
	if (m_nBannerIndex < 0) {
		return;
	}

	VisibleAccountRef ref{};
	if (!ResolveVisibleAccount(static_cast<std::uint32_t>(m_nBannerIndex), queryIndex, ref)) {
		return;
	}

	m_carousel.RemoveAccount(ref.BannerIndex, ref.AccountIndex);
	if (m_nSelectedAccountIndex == static_cast<std::int32_t>(queryIndex)) {
		m_nSelectedAccountIndex = -1;
	} else if (m_nSelectedAccountIndex > static_cast<std::int32_t>(queryIndex)) {
		m_nSelectedAccountIndex -= 1;
	}
}

PendingHit CAccountModal::ConsumePendingRightClickRow()
{
	const PendingHit pending = m_pendingRightClickRow;
	m_pendingRightClickRow = PendingHit{};
	return pending;
}

void CAccountModal::Update(float deltaSeconds)
{
	const float target = m_bIsOpen ? 1.0f : 0.0f;
	m_flOpenAmount = CAnimator::EaseToward(m_flOpenAmount, target, kOpenEaseRate, deltaSeconds);
	if (!m_bIsOpen && m_flOpenAmount < 0.002f) {
		m_flOpenAmount = 0.0f;
		m_nBannerIndex = -1; // fully closed: forget which banner, next open sets it fresh
	}

	if (m_mode == EAccountModalMode::ACCOUNT_MODAL_MODE_ACCOUNT_LIST) {
		m_accountsScroll.Update(deltaSeconds);
	}
	if (m_mode == EAccountModalMode::ACCOUNT_MODAL_MODE_LOGIN_PROGRESS) {
		m_flLoginElapsedSeconds += deltaSeconds;
	}
	if (m_mode == EAccountModalMode::ACCOUNT_MODAL_MODE_EDIT_ACCOUNT) {
		m_editUsername.Update(deltaSeconds);
		m_editNote.Update(deltaSeconds);
		m_editPassword.Update(deltaSeconds);
	}

	// m_gameSelect is a plain value member, not a real CWidgetStack entry - nothing calls
	// SetMouseGated on it automatically the way the stack does for this modal itself, so
	// its hover checks would silently never fire (CWidget::m_flMouseX/Y default off-screen)
	// without this explicit forward. This modal's own m_flMouseX/Y are already correctly
	// gated by whoever owns *this* widget, so simply passing them through is correct even
	// when this modal itself is gated away (its own m_flMouseX/Y would already be -1,-1).
	m_gameSelect.SetMouseGated(false, m_flMouseX, m_flMouseY);
	// Same "not a real CWidgetStack member" situation as SetMouseGated just above -
	// nothing advances its open/close fold animation unless this modal does it explicitly.
	m_gameSelect.Update(deltaSeconds);

	// Always, regardless of mode/visibility - CLoginAttempt::Update's join is always safe
	// and effectively instantaneous once the worker reaches a terminal stage.
	m_login.Update();
}

bool CAccountModal::OnPointerDown(float x, float y)
{
	if (!IsBlocking() || m_mode != EAccountModalMode::ACCOUNT_MODAL_MODE_ACCOUNT_LIST) {
		return IsBlocking();
	}
	if (m_nBannerIndex < 0 || static_cast<std::uint32_t>(m_nBannerIndex) >= m_carousel.GetBannerCount()) {
		return IsBlocking();
	}

	VisibleAccountRef refs[kCarouselMaxVisibleAccounts];
	const std::uint32_t visibleCount = m_carousel.GetVisibleAccounts(static_cast<std::uint32_t>(m_nBannerIndex), refs);

	const Layout layout = ComputeLayout();
	const Rect scrollRegion = AccountsScrollRegionRect(layout.Right);
	const Rect track = AccountsScrollbarTrackRect(scrollRegion);
	const float contentHeight = AccountsContentHeight(visibleCount);
	m_accountsScroll.OnPointerDown(x, y, track, contentHeight, scrollRegion.H);
	return true;
}

bool CAccountModal::OnPointerMove(float x, float y)
{
	if (!m_accountsScroll.IsDragging()) {
		return IsBlocking();
	}
	if (m_nBannerIndex < 0 || static_cast<std::uint32_t>(m_nBannerIndex) >= m_carousel.GetBannerCount()) {
		return IsBlocking();
	}

	VisibleAccountRef refs[kCarouselMaxVisibleAccounts];
	const std::uint32_t visibleCount = m_carousel.GetVisibleAccounts(static_cast<std::uint32_t>(m_nBannerIndex), refs);

	const Layout layout = ComputeLayout();
	const Rect scrollRegion = AccountsScrollRegionRect(layout.Right);
	const Rect track = AccountsScrollbarTrackRect(scrollRegion);
	const float contentHeight = AccountsContentHeight(visibleCount);
	m_accountsScroll.OnPointerMove(y, track, contentHeight, scrollRegion.H);
	(void)x;
	return true;
}

bool CAccountModal::OnPointerUp(float x, float y)
{
	if (m_accountsScroll.IsDragging()) {
		m_accountsScroll.OnPointerUp();
		return true;
	}

	if (!IsBlocking()) {
		return false;
	}

	const Layout layout = ComputeLayout();

	if (RectContainsPoint(CloseBadgeRect(layout.Left), x, y)) {
		Close();
		return true;
	}

	if (!RectContainsPoint(layout.Panel, x, y)) {
		Close();
		return true;
	}

	const bool haveBanner =
		m_nBannerIndex >= 0 && static_cast<std::uint32_t>(m_nBannerIndex) < m_carousel.GetBannerCount();
	const auto bannerIndex = haveBanner ? static_cast<std::uint32_t>(m_nBannerIndex) : 0u;

	if (m_mode == EAccountModalMode::ACCOUNT_MODAL_MODE_ACCOUNT_LIST && haveBanner) {
		if (RectContainsPoint(AddAccountButtonRect(layout.Right), x, y)) {
			StartAddAccount();
			return true;
		}

		VisibleAccountRef refs[kCarouselMaxVisibleAccounts];
		const std::uint32_t visibleCount = m_carousel.GetVisibleAccounts(bannerIndex, refs);

		const Rect scrollRegion = AccountsScrollRegionRect(layout.Right);
		// Hit-testing is CPU-side and doesn't know about the GPU clip rect the draw side
		// applies - gate on the *click position* explicitly, so a click landing in the
		// header can never be misattributed to a row that's scrolled (and now genuinely
		// invisible, not just visually covered) behind it.
		const bool clickInScrollRegion = RectContainsPoint(scrollRegion, x, y);
		bool clickedARow = false;
		for (std::uint32_t i = 0; clickInScrollRegion && i < visibleCount; i += 1) {
			const Rect row = RowRect(layout.Right, i);
			if (row.Y + row.H <= scrollRegion.Y || row.Y >= scrollRegion.Y + scrollRegion.H) {
				continue; // scrolled out of view
			}

			if (RectContainsPoint(RowEditButtonRect(row), x, y)) {
				StartEditAccount(i);
				return true;
			}
			if (RectContainsPoint(RowRemoveButtonRect(row), x, y)) {
				RemoveAccountRow(i);
				return true;
			}
			if (RectContainsPoint(row, x, y)) {
				m_nSelectedAccountIndex = static_cast<std::int32_t>(i);
				clickedARow = true;
				break;
			}
		}
		// Clicking anywhere else in the list (the gaps between rows, or empty space below
		// the last one) deselects - "click off to unselect."
		if (clickInScrollRegion && !clickedARow) {
			m_nSelectedAccountIndex = -1;
		}

		// !m_login.IsActive(): a previous attempt (most often one just Cancelled) might not
		// have actually finished yet - see DrawFooter's own comment on why the Login button
		// mirrors this same condition rather than just m_nSelectedAccountIndex.
		if (m_nSelectedAccountIndex >= 0 && !m_login.IsActive() &&
			RectContainsPoint(LoginButtonRect(layout.Footer), x, y)) {
			m_mode = EAccountModalMode::ACCOUNT_MODAL_MODE_LOGIN_PROGRESS;
			m_flLoginElapsedSeconds = 0.0f;
			StartLoginFor(bannerIndex, static_cast<std::uint32_t>(m_nSelectedAccountIndex));
		}
	} else if (m_mode == EAccountModalMode::ACCOUNT_MODAL_MODE_LOGIN_PROGRESS) {
		// Cancel/Back share the Login button's rect. CLoginAttempt::Cancel is a real
		// interrupt (see its own comment) - the background worker checks it within about one
		// poll interval, rather than continuing to launch/kill processes and type credentials
		// invisibly after the UI has already moved on. Safe to call even once the attempt has
		// already reached a terminal stage (a no-op then, "Back" rather than a real cancel).
		if (RectContainsPoint(LoginButtonRect(layout.Footer), x, y)) {
			m_login.Cancel();
			m_mode = EAccountModalMode::ACCOUNT_MODAL_MODE_ACCOUNT_LIST;
		}
	} else if (m_mode == EAccountModalMode::ACCOUNT_MODAL_MODE_EDIT_ACCOUNT) {
		const float formOffsetY = EditFormContentOffsetY(layout.Right);
		const Rect chip = VisibilityChipRect(layout.Right, formOffsetY);

		// The popup takes priority over everything else in the form while open - a click
		// either toggles one of its rows or (missing the popup entirely) closes it, same
		// click-anywhere-else-closes pattern CColorPicker's popup uses.
		if (m_gameSelect.IsBlocking()) {
			if (!m_gameSelect.OnPointerDown(x, y)) {
				m_gameSelect.Close();
			}
			return true;
		}

		if (RectContainsPoint(chip, x, y)) {
			// Clamped to this modal's own right (form) column, not layout.Panel - a first
			// attempt at this fix used Panel and still let the popup spill past the
			// separator above the footer (Delete/Cancel/Save), since Panel's own height
			// includes the footer strip while Content/Right stop above it. layout.Right is
			// exactly the column this popup visually belongs to and already excludes the
			// footer both vertically and horizontally, so this is the correct bound, not
			// just a smaller one.
			m_gameSelect.Open(m_gameSelect.GetMask(), &m_carousel.GetBanner(0), m_carousel.GetBannerCount(), chip,
							  layout.Right);
			return true;
		}

		const Rect usernameField = EditFieldInputRect(EditFieldBlockRect(layout.Right, 0, formOffsetY));
		const Rect noteField = EditFieldInputRect(EditFieldBlockRect(layout.Right, 1, formOffsetY));
		const Rect passwordField = EditFieldInputRect(EditFieldBlockRect(layout.Right, 2, formOffsetY));

		if (RectContainsPoint(PasswordRevealButtonRect(passwordField), x, y)) {
			m_bEditPasswordRevealed = !m_bEditPasswordRevealed;
			return true;
		}

		m_editUsername.m_bFocused = RectContainsPoint(usernameField, x, y);
		m_editNote.m_bFocused = RectContainsPoint(noteField, x, y);
		m_editPassword.m_bFocused = RectContainsPoint(passwordField, x, y);
		// Click-to-position within a field isn't implemented - end is a reasonable
		// default.
		if (m_editUsername.m_bFocused) {
			m_editUsername.SetValue(m_editUsername.GetValue());
		}
		if (m_editNote.m_bFocused) {
			m_editNote.SetValue(m_editNote.GetValue());
		}
		if (m_editPassword.m_bFocused) {
			m_editPassword.SetValue(m_editPassword.GetValue());
		}

		const Rect save = LoginButtonRect(layout.Footer);
		if (RectContainsPoint(EditCancelButtonRect(save), x, y)) {
			m_mode = EAccountModalMode::ACCOUNT_MODAL_MODE_ACCOUNT_LIST;
		} else if (m_editUsername.GetValue().Length > 0 && RectContainsPoint(save, x, y)) {
			if (haveBanner) {
				if (m_nEditAccountIndex < 0) {
					m_carousel.AddAccount(bannerIndex, m_editUsername.GetValue(), m_editNote.GetValue(),
										  m_editPassword.GetValue());
					CBanner &banner = m_carousel.GetBanner(bannerIndex);
					const std::uint32_t savedIndex = banner.AccountCount - 1;
					// AddAccount goes through CAccount::Init, which always resets
					// m_uVisibleBannerMask to 0 - write the form's working mask back in
					// afterward rather than threading it through (every other caller
					// wants the plain default).
					banner.Accounts[savedIndex].m_uVisibleBannerMask = m_gameSelect.GetMask();
					// A brand new account is always the last of bannerIndex's own
					// accounts, which is also always exactly where the query places it
					// (own-banner accounts occupy the query's first slots, in order), so
					// this stays a valid query index for the freshly-added row.
					m_nSelectedAccountIndex = static_cast<std::int32_t>(savedIndex);
				} else {
					VisibleAccountRef ref{};
					if (ResolveVisibleAccount(bannerIndex, static_cast<std::uint32_t>(m_nEditAccountIndex), ref)) {
						m_carousel.UpdateAccount(ref.BannerIndex, ref.AccountIndex, m_editUsername.GetValue(),
												 m_editNote.GetValue(), m_editPassword.GetValue());
						m_carousel.GetBanner(ref.BannerIndex).Accounts[ref.AccountIndex].m_uVisibleBannerMask =
							m_gameSelect.GetMask();
					}
				}
			}
			m_mode = EAccountModalMode::ACCOUNT_MODAL_MODE_ACCOUNT_LIST;
		} else if (m_nEditAccountIndex >= 0 && RectContainsPoint(EditDeleteButtonRect(layout.Footer), x, y)) {
			if (haveBanner) {
				VisibleAccountRef ref{};
				if (ResolveVisibleAccount(bannerIndex, static_cast<std::uint32_t>(m_nEditAccountIndex), ref)) {
					m_carousel.RemoveAccount(ref.BannerIndex, ref.AccountIndex);
				}
				m_nSelectedAccountIndex = -1;
			}
			m_mode = EAccountModalMode::ACCOUNT_MODAL_MODE_ACCOUNT_LIST;
		}
	}

	return true;
}

ECursorKind CAccountModal::GetDesiredCursor() const
{
	if (!IsBlocking()) {
		return ECursorKind::CURSOR_ARROW;
	}
	if (m_accountsScroll.IsDragging()) {
		return ECursorKind::CURSOR_DRAG;
	}

	const Layout layout = ComputeLayout();

	if (RectContainsPoint(CloseBadgeRect(layout.Left), m_flMouseX, m_flMouseY)) {
		return ECursorKind::CURSOR_HAND;
	}

	const bool haveBanner =
		m_nBannerIndex >= 0 && static_cast<std::uint32_t>(m_nBannerIndex) < m_carousel.GetBannerCount();
	const auto bannerIndex = haveBanner ? static_cast<std::uint32_t>(m_nBannerIndex) : 0u;

	if (m_mode == EAccountModalMode::ACCOUNT_MODAL_MODE_ACCOUNT_LIST && haveBanner) {
		if (RectContainsPoint(AddAccountButtonRect(layout.Right), m_flMouseX, m_flMouseY)) {
			return ECursorKind::CURSOR_HAND;
		}

		VisibleAccountRef refs[kCarouselMaxVisibleAccounts];
		const std::uint32_t visibleCount = m_carousel.GetVisibleAccounts(bannerIndex, refs);
		const Rect scrollRegion = AccountsScrollRegionRect(layout.Right);
		if (RectContainsPoint(scrollRegion, m_flMouseX, m_flMouseY)) {
			for (std::uint32_t i = 0; i < visibleCount; i += 1) {
				const Rect row = RowRect(layout.Right, i);
				if (row.Y + row.H <= scrollRegion.Y || row.Y >= scrollRegion.Y + scrollRegion.H) {
					continue;
				}
				if (RectContainsPoint(row, m_flMouseX, m_flMouseY)) {
					return ECursorKind::CURSOR_HAND;
				}
			}
			if (CScrollable::IsVisible(AccountsContentHeight(visibleCount), scrollRegion.H) &&
				RectContainsPoint(AccountsScrollbarTrackRect(scrollRegion), m_flMouseX, m_flMouseY)) {
				return ECursorKind::CURSOR_HAND;
			}
		}

		if (m_nSelectedAccountIndex >= 0 && !m_login.IsActive() &&
			RectContainsPoint(LoginButtonRect(layout.Footer), m_flMouseX, m_flMouseY)) {
			return ECursorKind::CURSOR_HAND;
		}
	} else if (m_mode == EAccountModalMode::ACCOUNT_MODAL_MODE_LOGIN_PROGRESS) {
		if (RectContainsPoint(LoginButtonRect(layout.Footer), m_flMouseX, m_flMouseY)) {
			return ECursorKind::CURSOR_HAND;
		}
	} else if (m_mode == EAccountModalMode::ACCOUNT_MODAL_MODE_EDIT_ACCOUNT) {
		// Not a CWidgetStack member (see CGameSelectPopup's own comment on the same
		// relationship, and Update's own explicit SetMouseGated forward above) - consult
		// it directly while its popup is open, taking priority over the form underneath
		// exactly like OnPointerDown's own handling does.
		if (m_gameSelect.IsBlocking()) {
			return m_gameSelect.GetDesiredCursor();
		}

		const float formOffsetY = EditFormContentOffsetY(layout.Right);
		if (RectContainsPoint(VisibilityChipRect(layout.Right, formOffsetY), m_flMouseX, m_flMouseY)) {
			return ECursorKind::CURSOR_HAND;
		}

		const Rect usernameField = EditFieldInputRect(EditFieldBlockRect(layout.Right, 0, formOffsetY));
		const Rect noteField = EditFieldInputRect(EditFieldBlockRect(layout.Right, 1, formOffsetY));
		const Rect passwordField = EditFieldInputRect(EditFieldBlockRect(layout.Right, 2, formOffsetY));

		if (RectContainsPoint(PasswordRevealButtonRect(passwordField), m_flMouseX, m_flMouseY)) {
			return ECursorKind::CURSOR_HAND;
		}
		if (RectContainsPoint(usernameField, m_flMouseX, m_flMouseY) ||
			RectContainsPoint(noteField, m_flMouseX, m_flMouseY) ||
			RectContainsPoint(passwordField, m_flMouseX, m_flMouseY)) {
			return ECursorKind::CURSOR_IBEAM;
		}

		const Rect save = LoginButtonRect(layout.Footer);
		if (RectContainsPoint(EditCancelButtonRect(save), m_flMouseX, m_flMouseY)) {
			return ECursorKind::CURSOR_HAND;
		}
		if (m_editUsername.GetValue().Length > 0 && RectContainsPoint(save, m_flMouseX, m_flMouseY)) {
			return ECursorKind::CURSOR_HAND;
		}
		if (m_nEditAccountIndex >= 0 && RectContainsPoint(EditDeleteButtonRect(layout.Footer), m_flMouseX, m_flMouseY)) {
			return ECursorKind::CURSOR_HAND;
		}
	}

	return ECursorKind::CURSOR_ARROW;
}

bool CAccountModal::OnRightPointerUp(float x, float y)
{
	if (!IsBlocking()) {
		return false;
	}
	if (m_mode != EAccountModalMode::ACCOUNT_MODAL_MODE_ACCOUNT_LIST) {
		return true;
	}
	if (m_nBannerIndex < 0 || static_cast<std::uint32_t>(m_nBannerIndex) >= m_carousel.GetBannerCount()) {
		return true;
	}

	const auto bannerIndex = static_cast<std::uint32_t>(m_nBannerIndex);
	VisibleAccountRef refs[kCarouselMaxVisibleAccounts];
	const std::uint32_t visibleCount = m_carousel.GetVisibleAccounts(bannerIndex, refs);

	const Layout layout = ComputeLayout();
	const Rect scrollRegion = AccountsScrollRegionRect(layout.Right);
	m_pendingRightClickRow = PendingHitMiss();
	if (!RectContainsPoint(scrollRegion, x, y)) {
		return true;
	}

	for (std::uint32_t i = 0; i < visibleCount; i += 1) {
		const Rect row = RowRect(layout.Right, i);
		if (row.Y + row.H <= scrollRegion.Y || row.Y >= scrollRegion.Y + scrollRegion.H) {
			continue;
		}
		if (RectContainsPoint(row, x, y)) {
			m_pendingRightClickRow = PendingHitIndex(static_cast<std::int32_t>(i));
			break;
		}
	}
	return true;
}

bool CAccountModal::OnScroll(float x, float y, float wheelDelta)
{
	(void)x;
	(void)y;
	if (!IsBlocking()) {
		return false;
	}
	if (m_mode != EAccountModalMode::ACCOUNT_MODAL_MODE_ACCOUNT_LIST) {
		return true;
	}
	if (m_nBannerIndex < 0 || static_cast<std::uint32_t>(m_nBannerIndex) >= m_carousel.GetBannerCount()) {
		return true;
	}

	VisibleAccountRef refs[kCarouselMaxVisibleAccounts];
	const std::uint32_t visibleCount = m_carousel.GetVisibleAccounts(static_cast<std::uint32_t>(m_nBannerIndex), refs);

	const Layout layout = ComputeLayout();
	const Rect scrollRegion = AccountsScrollRegionRect(layout.Right);
	const float contentHeight = AccountsContentHeight(visibleCount);
	m_accountsScroll.OnScroll(wheelDelta, contentHeight, scrollRegion.H);
	return true;
}

bool CAccountModal::OnKeyDown(std::uint32_t keyCode)
{
	if (!IsBlocking()) {
		return false;
	}

	if (keyCode == VK_ESCAPE) {
		if (m_mode == EAccountModalMode::ACCOUNT_MODAL_MODE_EDIT_ACCOUNT) {
			// Backs out of the edit form to the account list rather than closing the
			// whole modal - losing in-progress edits is expected, losing the game's
			// account list underneath it is not.
			m_mode = EAccountModalMode::ACCOUNT_MODAL_MODE_ACCOUNT_LIST;
		} else if (m_mode == EAccountModalMode::ACCOUNT_MODAL_MODE_ACCOUNT_LIST) {
			// Backs out one level at a time: clears the current row selection first,
			// only closing the whole modal on a second press with nothing selected.
			if (m_nSelectedAccountIndex >= 0) {
				m_nSelectedAccountIndex = -1;
			} else {
				Close();
			}
		}
		return true;
	}

	if (m_mode != EAccountModalMode::ACCOUNT_MODAL_MODE_EDIT_ACCOUNT) {
		return true;
	}

	if (keyCode == VK_TAB) {
		if (m_editUsername.m_bFocused) {
			m_editUsername.m_bFocused = false;
			m_editNote.m_bFocused = true;
		} else if (m_editNote.m_bFocused) {
			m_editNote.m_bFocused = false;
			m_editPassword.m_bFocused = true;
		} else if (m_editPassword.m_bFocused) {
			m_editPassword.m_bFocused = false;
			m_editUsername.m_bFocused = true;
		}
		return true;
	}

	// Each OnKey is a no-op on an unfocused field, so calling all three is just "route
	// to whichever one is focused" without needing an if/else chain here.
	m_editUsername.OnKey(keyCode);
	m_editNote.OnKey(keyCode);
	m_editPassword.OnKey(keyCode);
	return true;
}

bool CAccountModal::OnChar(std::uint32_t character)
{
	if (!IsBlocking()) {
		return false;
	}
	if (m_mode != EAccountModalMode::ACCOUNT_MODAL_MODE_EDIT_ACCOUNT) {
		return true;
	}

	m_editUsername.OnChar(character);
	m_editNote.OnChar(character);
	m_editPassword.OnChar(character);
	return true;
}

// --- Draw ---

void CAccountModal::DrawSectionTitle(CDrawList &drawList, Rect right, float headerHeight, CStringView title,
									 std::uint8_t alpha) const
{
	const CFont &body = m_fonts.GetBody();
	DrawText(drawList, body, right.X + kRowPadding, right.Y + (headerHeight + body.GetAscent()) * 0.5f, title,
			 ColorFadeAlpha(kColorTextBright, alpha));
	// Closes the header off from whatever's below it.
	drawList.AddRectFilled(right.X + kRowPadding, right.Y + headerHeight, right.W - kRowPadding * 2.0f, 1.0f,
						   ColorFadeAlpha(Color{48, 48, 53, 255}, alpha));
}

// A circular icon-only badge - AddIcon alone, no "+ Add Account" label - the same
// visual language as a row's own Edit/Remove buttons (DrawAccountRow below) rather than
// a wide accent-filled pill: a plain "+" spelled out as text next to an icon read as
// redundant once the icon itself already reads as "add," and the row buttons already
// establish "small circular icon badge in the corner" as this modal's own convention
// for a secondary, non-primary action.
void CAccountModal::DrawAddAccountButton(CDrawList &drawList, Rect right, std::uint8_t alpha) const
{
	constexpr float kIconSize = 24.0f; // button itself is RowButtonSizeFor (>= 28px), comfortably bigger

	const Rect button = AddAccountButtonRect(right);
	const bool hover = RectContainsPoint(button, m_flMouseX, m_flMouseY);
	if (hover) {
		CHoverable::DrawLift(drawList, button, button.W * 0.5f, m_settings.m_clrAccent, alpha);
		drawList.AddRectRoundedFilled(button.X, button.Y, button.W, button.H, CDrawList::UniformRadii(button.W * 0.5f),
									  ColorFadeAlpha(Color{56, 56, 62, 255}, alpha));
	}

	const Rect iconRect{button.X + (button.W - kIconSize) * 0.5f, button.Y + (button.H - kIconSize) * 0.5f, kIconSize,
						kIconSize};
	drawList.AddRectRoundedTextured(iconRect.X, iconRect.Y, iconRect.W, iconRect.H, kCornerRadiiNone,
									m_assets.GetIconAdd(), ColorFadeAlpha(hover ? kColorTextBright : kColorTextDim, alpha));
}

void CAccountModal::DrawAccountRow(CDrawList &drawList, Rect right, Rect row, const CAccount &account, bool isSelected,
								   std::uint8_t alpha) const
{
	// Inset, not bled past row.Y - kept the top row's highlight from getting clipped by the
	// scroll region above it.
	const Rect hoverRect{row.X - 8.0f, row.Y + 3.0f, row.W + 16.0f, row.H - 6.0f};
	const bool hoverRow = !isSelected && RectContainsPoint(hoverRect, m_flMouseX, m_flMouseY);

	if (isSelected) {
		// A plain translucent grey, not accent-tinted - the left indicator bar below is
		// the only accent-colored part of a selected row, just enough to say "this one's
		// selected" without overpowering the row's own content.
		drawList.AddRectRoundedFilled(hoverRect.X, hoverRect.Y, hoverRect.W, hoverRect.H,
									  CDrawList::UniformRadii(10.0f), ColorFadeAlpha(Color{58, 58, 62, 255}, alpha));
		drawList.AddRectRoundedFilled(right.X + 8.0f, hoverRect.Y, 3.0f, hoverRect.H, CDrawList::UniformRadii(1.5f),
									  ColorFadeAlpha(m_settings.m_clrAccent, alpha));
	} else if (hoverRow) {
		drawList.AddRectRoundedFilled(hoverRect.X, hoverRect.Y, hoverRect.W, hoverRect.H,
									  CDrawList::UniformRadii(10.0f), ColorFadeAlpha(Color{38, 38, 43, 255}, alpha));
	}

	const CFont &body = m_fonts.GetBody();
	const CFont &secondary = m_fonts.GetSecondary();
	const CStringView note = account.GetNote();

	// Center the username (plus note below it, if there is one) vertically in the row.
	float usernameBaselineY;
	float noteBaselineY = 0.0f;
	if (note.Length > 0) {
		const float blockHeight = body.GetLineHeight() + kRowLineGap + secondary.GetLineHeight();
		const float blockY = row.Y + (row.H - blockHeight) * 0.5f;
		usernameBaselineY = blockY + body.GetAscent();
		noteBaselineY = blockY + body.GetLineHeight() + kRowLineGap + secondary.GetAscent();
	} else {
		usernameBaselineY = row.Y + (row.H - body.GetLineHeight()) * 0.5f + body.GetAscent();
	}
	DrawText(drawList, body, row.X, usernameBaselineY, account.GetUsername(), ColorFadeAlpha(kColorTextBright, alpha));
	if (note.Length > 0) {
		DrawText(drawList, secondary, row.X, noteBaselineY, note, ColorFadeAlpha(kColorTextDim, alpha));
	}

	drawList.AddRectFilled(row.X, row.Y + row.H - 1.0f, row.W, 1.0f, ColorFadeAlpha(Color{48, 48, 53, 255}, alpha));

	const Rect editRect = RowEditButtonRect(row);
	const Rect removeRect = RowRemoveButtonRect(row);
	const bool hoverEdit = RectContainsPoint(editRect, m_flMouseX, m_flMouseY);
	const bool hoverRemove = RectContainsPoint(removeRect, m_flMouseX, m_flMouseY);

	// A circular hover backing, same treatment as the modal's close badge, instead of a
	// bare glyph floating in empty space. A soft lift ring on top makes the hovered one
	// read as genuinely raised, not just recolored.
	if (hoverEdit) {
		CHoverable::DrawLift(drawList, editRect, editRect.W * 0.5f, m_settings.m_clrAccent, alpha);
		drawList.AddRectRoundedFilled(editRect.X, editRect.Y, editRect.W, editRect.H,
									  CDrawList::UniformRadii(editRect.W * 0.5f),
									  ColorFadeAlpha(Color{56, 56, 62, 255}, alpha));
	}
	if (hoverRemove) {
		CHoverable::DrawLift(drawList, removeRect, removeRect.W * 0.5f, kColorError, alpha);
		drawList.AddRectRoundedFilled(removeRect.X, removeRect.Y, removeRect.W, removeRect.H,
									  CDrawList::UniformRadii(removeRect.W * 0.5f),
									  ColorFadeAlpha(Color{68, 42, 42, 255}, alpha));
	}

	// Remove still has no embedded icon - its hand-drawn X glyph stays until one exists.
	constexpr float kEditIconInset = 5.0f; // editRect is a circular hover badge - inset so the icon doesn't touch its edge
	drawList.AddRectRoundedTextured(editRect.X + kEditIconInset, editRect.Y + kEditIconInset,
									editRect.W - kEditIconInset * 2.0f, editRect.H - kEditIconInset * 2.0f,
									kCornerRadiiNone, m_assets.GetIconEdit(),
									ColorFadeAlpha(hoverEdit ? kColorTextBright : kColorTextDim, alpha));
	DrawXGlyph(drawList, removeRect, ColorFadeAlpha(hoverRemove ? kColorError : kColorTextDim, alpha));
}

void CAccountModal::DrawAccountList(CDrawList &drawList, Rect right, std::uint8_t alpha) const
{
	const float headerHeight = HeaderHeightFor(m_fonts);
	DrawSectionTitle(drawList, right, headerHeight, StringViewFromCString("Accounts"), alpha);
	DrawAddAccountButton(drawList, right, alpha);

	const auto bannerIndex = static_cast<std::uint32_t>(m_nBannerIndex);
	VisibleAccountRef refs[kCarouselMaxVisibleAccounts];
	const std::uint32_t visibleCount = m_carousel.GetVisibleAccounts(bannerIndex, refs);

	const Rect scrollRegion = AccountsScrollRegionRect(right);
	// Real GPU-side clipping - a row that's scrolled halfway behind the header genuinely
	// can't paint outside scrollRegion.
	drawList.PushClipRect(scrollRegion);
	for (std::uint32_t i = 0; i < visibleCount; i += 1) {
		const Rect row = RowRect(right, i);
		if (row.Y + row.H <= scrollRegion.Y || row.Y >= scrollRegion.Y + scrollRegion.H) {
			continue; // cheap cull, clip still applies
		}
		const bool isSelected = static_cast<std::int32_t>(i) == m_nSelectedAccountIndex;
		const VisibleAccountRef &ref = refs[i];
		DrawAccountRow(drawList, right, row, m_carousel.GetBanner(ref.BannerIndex).Accounts[ref.AccountIndex],
					   isSelected, alpha);
	}
	drawList.PopClipRect();

	const float contentHeight = AccountsContentHeight(visibleCount);
	m_accountsScroll.Draw(drawList, AccountsScrollbarTrackRect(scrollRegion), contentHeight, scrollRegion.H,
						  ColorFadeAlpha(Color{120, 120, 128, 190}, alpha), m_flMouseX, m_flMouseY);
}

void CAccountModal::DrawLoginProgress(CDrawList &drawList, Rect right, std::uint8_t alpha) const
{
	const ELoginStage stage = m_login.GetStage();
	const bool terminal = CLoginAttempt::IsTerminalStage(stage);

	const float cx = right.X + right.W * 0.5f;
	const float cy = right.Y + right.H * 0.5f - 24.0f;

	Color stageColor = m_settings.m_clrAccent;
	if (stage == ELoginStage::LOGIN_STAGE_SUCCESS) {
		stageColor = kColorSuccess;
	} else if (stage == ELoginStage::LOGIN_STAGE_ERROR) {
		stageColor = kColorError;
	}

	// A soft breathing bloom behind the ring while a stage is in flight; terminal stages
	// hold it steady instead, so the ring reads as settled the instant it lands rather than
	// still visibly animating.
	const float glowPulse =
		terminal ? 1.0f : 0.55f + 0.45f * (0.5f + 0.5f * std::sin(m_flLoginElapsedSeconds * 2.6f));
	const float glowStrength = 0.85f * glowPulse * (static_cast<float>(alpha) / 255.0f);

	// Terminal stages draw a full solid ring; an in-flight stage draws a comet-tail arc
	// that continuously rotates. Either way this is one draw call - ps_circular_progress
	// (see the D3D11 backend) computes the track, the arc/ring, its rounded caps, and the
	// glow entirely per-pixel, so the whole indicator stays smooth at any size instead of
	// faceting like tessellated geometry would.
	const float sweepAngleDeg = terminal ? kIndicatorFullSweepDeg : kIndicatorSweepDeg;
	const float startAngleDeg =
		terminal ? 0.0f
				 : std::fmod(m_flLoginElapsedSeconds * kIndicatorRotationDegPerSec, 360.0f) - 90.0f - kIndicatorSweepDeg;

	drawList.AddCircularProgress(cx, cy, kIndicatorOuterRadius, kIndicatorInnerRadius, kIndicatorGlowMargin,
								 startAngleDeg, sweepAngleDeg, glowStrength, ColorFadeAlpha(stageColor, alpha));

	if (terminal) {
		if (stage == ELoginStage::LOGIN_STAGE_SUCCESS) {
			drawList.AddLine(cx - 13.0f, cy, cx - 3.0f, cy + 11.0f, 4.0f, ColorFadeAlpha(kColorTextBright, alpha));
			drawList.AddLine(cx - 3.0f, cy + 11.0f, cx + 15.0f, cy - 11.0f, 4.0f, ColorFadeAlpha(kColorTextBright, alpha));
		} else {
			drawList.AddLine(cx - 11.0f, cy - 11.0f, cx + 11.0f, cy + 11.0f, 4.0f, ColorFadeAlpha(kColorTextBright, alpha));
			drawList.AddLine(cx - 11.0f, cy + 11.0f, cx + 11.0f, cy - 11.0f, 4.0f, ColorFadeAlpha(kColorTextBright, alpha));
		}
	}

	// The terminal message (Riot's own error text, or whatever else CLoginAttempt's worker
	// set - see core/login_attempt.cpp) takes over from the generic stage label the moment
	// one's actually available - GetTerminalMessage stays empty until then (and on a plain
	// Success with nothing more specific to say), so LoginStageMessage's own fixed strings
	// are still what's shown for every in-flight stage, and as the Success/Error fallback.
	CStringView message = LoginStageMessage(stage);
	if (terminal) {
		const CStringView terminalMessage = m_login.GetTerminalMessage();
		if (terminalMessage.Length > 0) {
			message = terminalMessage;
		}
	}

	const CFont &body = m_fonts.GetBody();
	const float maxMessageWidth = std::max(right.W - kRowPadding * 2.0f, 40.0f);
	CStringView messageLines[kMaxLoginMessageLines];
	const std::uint32_t lineCount = WrapLoginMessage(body, message, maxMessageWidth, messageLines);

	float lineY = cy + kIndicatorOuterRadius + 40.0f;
	for (std::uint32_t i = 0; i < lineCount; i += 1) {
		const float lineWidth = TextWidth(body, messageLines[i]);
		DrawText(drawList, body, cx - lineWidth * 0.5f, lineY, messageLines[i], ColorFadeAlpha(kColorTextBright, alpha));
		lineY += body.GetLineHeight();
	}
}

void CAccountModal::DrawEditField(CDrawList &drawList, Rect block, const char *pLabel, const CTextInput &input,
								  bool masked, std::uint8_t alpha) const
{
	const CFont &secondary = m_fonts.GetSecondary();
	DrawText(drawList, secondary, block.X, block.Y + secondary.GetAscent(), StringViewFromCString(pLabel),
			 ColorFadeAlpha(kColorTextFaint, alpha));

	const Rect field = EditFieldInputRect(block);
	// Focused fields get a plain white/neutral border ring (not accent-colored - a typed
	// value's input box shouldn't be game- or accent-branded, just clearly show which
	// field is active) plus a subtly lightened fill.
	constexpr Color kUnfocusedBorder{46, 46, 52, 255};
	constexpr Color kUnfocusedFill{24, 24, 27, 255};
	constexpr Color kFocusedBorder{225, 225, 230, 255};
	constexpr Color kFocusedFill{42, 42, 46, 255};
	drawList.AddRectRoundedFilled(field.X, field.Y, field.W, field.H, CDrawList::UniformRadii(8.0f),
								  ColorFadeAlpha(input.m_bFocused ? kFocusedBorder : kUnfocusedBorder, alpha));
	drawList.AddRectRoundedFilled(field.X + 1.5f, field.Y + 1.5f, field.W - 3.0f, field.H - 3.0f,
								  CDrawList::UniformRadii(7.0f),
								  ColorFadeAlpha(input.m_bFocused ? kFocusedFill : kUnfocusedFill, alpha));
	// Body, not secondary - this is the value the user is reading/typing.
	input.Draw(drawList, m_fonts.GetBody(), field.X, field.Y, field.W, field.H, ColorFadeAlpha(kColorTextBright, alpha),
			   ColorFadeAlpha(kColorTextBright, alpha), masked);
}

void CAccountModal::DrawEditAccount(CDrawList &drawList, Rect right, std::uint8_t alpha)
{
	const float headerHeight = HeaderHeightFor(m_fonts);
	const CStringView title =
		m_nEditAccountIndex < 0 ? StringViewFromCString("Add Account") : StringViewFromCString("Edit Account");
	DrawSectionTitle(drawList, right, headerHeight, title, alpha);

	const float formOffsetY = EditFormContentOffsetY(right);
	DrawEditField(drawList, EditFieldBlockRect(right, 0, formOffsetY), "Username", m_editUsername, false, alpha);
	DrawEditField(drawList, EditFieldBlockRect(right, 1, formOffsetY), "Note", m_editNote, false, alpha);
	DrawEditField(drawList, EditFieldBlockRect(right, 2, formOffsetY), "Password", m_editPassword,
				  !m_bEditPasswordRevealed, alpha);

	const Rect passwordField = EditFieldInputRect(EditFieldBlockRect(right, 2, formOffsetY));
	const Rect reveal = PasswordRevealButtonRect(passwordField);
	const bool hoverReveal = RectContainsPoint(reveal, m_flMouseX, m_flMouseY);
	DrawEyeGlyph(drawList, m_assets, reveal, m_bEditPasswordRevealed,
				 ColorFadeAlpha(hoverReveal ? kColorTextBright : kColorTextDim, alpha));

	// A pill (icon + its own visible-game count, plain dim/bright text - no colored
	// badge, no absolute-positioned overlay) - see VisibilityChipRect for why it's sized
	// to fit that content exactly, and kVisibilityChipIconSize's own comment for why the
	// icon itself stays a fixed size regardless. No label above it (unlike Username/Note/
	// Password) - "N games" already says what this is on its own.
	const Rect chip = VisibilityChipRect(right, formOffsetY);
	const bool hoverChip = RectContainsPoint(chip, m_flMouseX, m_flMouseY);

	// Always has a real border+fill, like the other fields' own unfocused state - a
	// resting chip that's only visible on hover is exactly what made this hard to notice.
	constexpr Color kChipBorder{46, 46, 52, 255};
	constexpr Color kChipFill{24, 24, 27, 255};
	constexpr Color kChipHoverBorder{225, 225, 230, 255};
	constexpr Color kChipHoverFill{42, 42, 46, 255};
	drawList.AddRectRoundedFilled(chip.X, chip.Y, chip.W, chip.H, CDrawList::UniformRadii(8.0f),
								  ColorFadeAlpha(hoverChip ? kChipHoverBorder : kChipBorder, alpha));
	drawList.AddRectRoundedFilled(chip.X + 1.5f, chip.Y + 1.5f, chip.W - 3.0f, chip.H - 3.0f,
								  CDrawList::UniformRadii(7.0f),
								  ColorFadeAlpha(hoverChip ? kChipHoverFill : kChipFill, alpha));

	const Rect icon{chip.X + kVisibilityChipIconInset, chip.Y + (chip.H - kVisibilityChipIconSize) * 0.5f,
					kVisibilityChipIconSize, kVisibilityChipIconSize};
	drawList.AddRectRoundedTextured(icon.X, icon.Y, icon.W, icon.H, kCornerRadiiNone, m_assets.GetIconListArrow(),
									ColorFadeAlpha(hoverChip ? kColorTextBright : kColorTextDim, alpha));

	char countBuffer[16];
	const std::uint64_t countLen = FormatVisibleGameCountText(m_gameSelect.GetMask(), countBuffer, sizeof(countBuffer));
	const CStringView countText{countBuffer, countLen};
	const CFont &countFont = m_fonts.GetSecondary();
	// Baseline for the text's own visual center (not just its ascent) to land on the
	// chip's true center, matching where the icon (a plain box, centered by its own
	// bounds) sits - see settings_menu.cpp's own comment on this same ascent/descent
	// math for why GetAscent() alone isn't enough.
	const float countBaselineY = chip.Y + chip.H * 0.5f + (countFont.GetAscent() + countFont.GetDescent()) * 0.5f;
	DrawText(drawList, countFont, icon.X + icon.W + kVisibilityChipIconGap, countBaselineY, countText,
			 ColorFadeAlpha(hoverChip ? kColorTextBright : kColorTextDim, alpha));

	m_gameSelect.Draw(drawList);
}

void CAccountModal::DrawFooter(CDrawList &drawList, Rect footer, std::uint8_t alpha)
{
	const CFont &body = m_fonts.GetBody();
	const CFont &secondary = m_fonts.GetSecondary();

	drawList.AddRectFilled(footer.X, footer.Y, footer.W, 1.0f, ColorFadeAlpha(Color{48, 48, 53, 255}, alpha));

	if (m_mode == EAccountModalMode::ACCOUNT_MODAL_MODE_EDIT_ACCOUNT) {
		const CStringView helperText = m_nEditAccountIndex < 0
										   ? StringViewFromCString("Fill in the new account's details")
										   : StringViewFromCString("Edit the account's details");
		// Starts past the Delete button when it's showing (editing an existing row) so
		// the two never overlap.
		const float helperX = m_nEditAccountIndex < 0
								  ? footer.X + kRowPadding
								  : EditDeleteButtonRect(footer).X + kLoginButtonWidth + kRowPadding;
		DrawText(drawList, secondary, helperX, footer.Y + (footer.H + secondary.GetAscent()) * 0.5f, helperText,
				 ColorFadeAlpha(kColorTextFaint, alpha));

		const Rect save = LoginButtonRect(footer);
		const bool saveEnabled = m_editUsername.GetValue().Length > 0;
		const bool hoverSave = saveEnabled && RectContainsPoint(save, m_flMouseX, m_flMouseY);
		const Color saveColor = !saveEnabled
									? Color{60, 58, 70, 255}
									: (hoverSave ? ColorLighten(m_settings.m_clrAccent, 20) : m_settings.m_clrAccent);
		if (hoverSave) {
			CHoverable::DrawLift(drawList, save, 8.0f, m_settings.m_clrAccent, alpha);
		}
		drawList.AddRectRoundedFilled(save.X, save.Y, save.W, save.H, CDrawList::UniformRadii(8.0f),
									  ColorFadeAlpha(saveColor, alpha));
		DrawCenteredText(drawList, body, save.X, save.Y, save.W, save.H, StringViewFromCString("Save"),
						 ColorFadeAlpha(saveEnabled ? Color{245, 245, 248, 255} : kColorTextDim, alpha));

		const Rect cancel = EditCancelButtonRect(save);
		const bool hoverCancel = RectContainsPoint(cancel, m_flMouseX, m_flMouseY);
		if (hoverCancel) {
			CHoverable::DrawLift(drawList, cancel, 8.0f, kColorTextBright, alpha);
		}
		drawList.AddRectRoundedFilled(
			cancel.X, cancel.Y, cancel.W, cancel.H, CDrawList::UniformRadii(8.0f),
			ColorFadeAlpha(hoverCancel ? Color{60, 60, 66, 255} : Color{46, 46, 52, 255}, alpha));
		DrawCenteredText(drawList, body, cancel.X, cancel.Y, cancel.W, cancel.H, StringViewFromCString("Cancel"),
						 ColorFadeAlpha(kColorTextBright, alpha));

		if (m_nEditAccountIndex >= 0) {
			const Rect del = EditDeleteButtonRect(footer);
			const bool hoverDelete = RectContainsPoint(del, m_flMouseX, m_flMouseY);
			if (hoverDelete) {
				CHoverable::DrawLift(drawList, del, 8.0f, kColorError, alpha);
			}
			drawList.AddRectRoundedFilled(
				del.X, del.Y, del.W, del.H, CDrawList::UniformRadii(8.0f),
				ColorFadeAlpha(hoverDelete ? ColorLighten(kColorError, 20) : Color{60, 45, 45, 255}, alpha));
			DrawCenteredText(drawList, body, del.X, del.Y, del.W, del.H, StringViewFromCString("Delete"),
							 ColorFadeAlpha(kColorTextBright, alpha));
		}
	} else if (m_mode == EAccountModalMode::ACCOUNT_MODAL_MODE_LOGIN_PROGRESS) {
		const bool terminal = CLoginAttempt::IsTerminalStage(m_login.GetStage());
		DrawText(drawList, secondary, footer.X + kRowPadding, footer.Y + (footer.H + secondary.GetAscent()) * 0.5f,
				 terminal ? StringViewFromCString("") : StringViewFromCString("Logging in..."),
				 ColorFadeAlpha(kColorTextFaint, alpha));

		// Same rect the Login button occupies in ACCOUNT_LIST - this button replaces it,
		// not adds to it. Cancel (in flight) and Back (terminal) do the same thing:
		// return to the account list without closing the whole modal.
		const Rect button = LoginButtonRect(footer);
		const bool hover = RectContainsPoint(button, m_flMouseX, m_flMouseY);
		if (hover) {
			CHoverable::DrawLift(drawList, button, 8.0f, kColorTextBright, alpha);
		}
		drawList.AddRectRoundedFilled(button.X, button.Y, button.W, button.H, CDrawList::UniformRadii(8.0f),
									  ColorFadeAlpha(hover ? Color{60, 60, 66, 255} : Color{46, 46, 52, 255}, alpha));
		DrawCenteredText(drawList, body, button.X, button.Y, button.W, button.H,
						 terminal ? StringViewFromCString("Back") : StringViewFromCString("Cancel"),
						 ColorFadeAlpha(kColorTextBright, alpha));
	} else {
		DrawText(drawList, secondary, footer.X + kRowPadding, footer.Y + (footer.H + secondary.GetAscent()) * 0.5f,
				 StringViewFromCString("Select an account to log in"), ColorFadeAlpha(kColorTextFaint, alpha));

		// Also disabled while a previous attempt on this same CLoginAttempt hasn't actually
		// finished yet - e.g. right after a Cancel click, before the worker has noticed it (see
		// CLoginAttempt::Start's own comment on why starting a new one before then would either
		// silently no-op or, worse, block the whole app waiting to join a worker that might be
		// stuck deep inside a single slow UI Automation call).
		const bool loginEnabled = m_nSelectedAccountIndex >= 0 && !m_login.IsActive();
		const Rect loginButton = LoginButtonRect(footer);
		const bool hoverLogin = loginEnabled && RectContainsPoint(loginButton, m_flMouseX, m_flMouseY);
		const Color buttonColor =
			!loginEnabled ? Color{60, 58, 70, 255}
						  : (hoverLogin ? ColorLighten(m_settings.m_clrAccent, 20) : m_settings.m_clrAccent);
		if (hoverLogin) {
			CHoverable::DrawLift(drawList, loginButton, 8.0f, m_settings.m_clrAccent, alpha);
		}
		drawList.AddRectRoundedFilled(loginButton.X, loginButton.Y, loginButton.W, loginButton.H,
									  CDrawList::UniformRadii(8.0f), ColorFadeAlpha(buttonColor, alpha));
		DrawCenteredText(drawList, body, loginButton.X, loginButton.Y, loginButton.W, loginButton.H,
						 StringViewFromCString("Login"),
						 ColorFadeAlpha(loginEnabled ? Color{245, 245, 248, 255} : kColorTextDim, alpha));
	}
}

void CAccountModal::Draw(CDrawList &drawList)
{
	if (m_flOpenAmount <= 0.001f) {
		return;
	}
	if (m_nBannerIndex < 0 || static_cast<std::uint32_t>(m_nBannerIndex) >= m_carousel.GetBannerCount()) {
		return;
	}

	const auto alpha = static_cast<std::uint8_t>(255.0f * m_flOpenAmount);
	const CBanner &banner = m_carousel.GetBanner(static_cast<std::uint32_t>(m_nBannerIndex));

	const auto windowW = static_cast<float>(m_window.GetWidth());
	const auto windowH = static_cast<float>(m_window.GetHeight());
	drawList.AddRectFilled(0.0f, 0.0f, windowW, windowH,
						   Color{0, 0, 0, static_cast<std::uint8_t>(160.0f * m_flOpenAmount)});

	const Layout layout = ComputeLayout();
	const Rect panel = layout.Panel;
	DrawPanelShadow(drawList, panel, m_flOpenAmount);
	drawList.AddRectRoundedFilled(panel.X, panel.Y, panel.W, panel.H, CDrawList::UniformRadii(kPanelRadius),
								  ColorFadeAlpha(Color{74, 74, 80, 255}, alpha));

	const Rect inner = layout.Inner;
	drawList.AddRectRoundedFilled(inner.X, inner.Y, inner.W, inner.H,
								  CDrawList::UniformRadii(kPanelRadius - kPanelBorderThickness),
								  ColorFadeAlpha(Color{24, 24, 27, 255}, alpha));

	// A faint top-edge highlight - a common "catching light from above" cue for an
	// elevated card on a dark UI, more visible here than a drop shadow would be against
	// the already-dark dimmed backdrop.
	drawList.AddRectFilled(inner.X + kPanelRadius, inner.Y, inner.W - kPanelRadius * 2.0f, 1.0f,
						   ColorFadeAlpha(Color{255, 255, 255, 22}, alpha));

	const Rect left = layout.Left;
	const Rect right = layout.Right;

	// Banner image: rounded only on the top-left, matching the panel's own rounded
	// corner there - every other edge of the image butts against something internal.
	if (banner.pTexture != nullptr) {
		const CornerRadii imageRadii{kPanelRadius - kPanelBorderThickness, 0.0f, 0.0f, 0.0f};
		const UvRect uv = CDrawList::ComputeCoverUv(left.W, left.H, banner.TextureAspect, 1.0f);
		drawList.AddRectRoundedTexturedUv(left.X, left.Y, left.W, left.H, imageRadii, uv.U0, uv.V0, uv.U1, uv.V1,
										  banner.pTexture, ColorFadeAlpha(Color{255, 255, 255, 255}, alpha));
	} else {
		drawList.AddRectFilled(left.X, left.Y, left.W, left.H, ColorFadeAlpha(banner.Accent, alpha));
	}
	drawList.AddRectFilled(left.X + left.W, left.Y, kSeparatorThickness, left.H,
						   ColorFadeAlpha(Color{90, 90, 96, 255}, alpha));

	// Close: the ArrowBack icon in a circular dark badge floating over the banner's
	// top-left corner.
	const Rect badge = CloseBadgeRect(left);
	const bool hoverBadge = RectContainsPoint(badge, m_flMouseX, m_flMouseY);
	drawList.AddRectRoundedFilled(
		badge.X, badge.Y, badge.W, badge.H, CDrawList::UniformRadii(badge.W * 0.5f),
		ColorFadeAlpha(Color{20, 20, 22, static_cast<std::uint8_t>(hoverBadge ? 210 : 170)}, alpha));
	const Rect badgeIcon{badge.X + (badge.W - kCloseBadgeIconSize) * 0.5f,
						 badge.Y + (badge.H - kCloseBadgeIconSize) * 0.5f, kCloseBadgeIconSize, kCloseBadgeIconSize};
	DrawCenteredTexture(drawList, badgeIcon, m_assets.GetIconArrowBack(),
						ColorFadeAlpha(Color{255, 255, 255, 255}, alpha));

	if (m_mode == EAccountModalMode::ACCOUNT_MODAL_MODE_ACCOUNT_LIST) {
		DrawAccountList(drawList, right, alpha);
	} else if (m_mode == EAccountModalMode::ACCOUNT_MODAL_MODE_LOGIN_PROGRESS) {
		DrawLoginProgress(drawList, right, alpha);
	} else {
		DrawEditAccount(drawList, right, alpha);
	}

	DrawFooter(drawList, layout.Footer, alpha);
}
