#include "ui/update_overlay.h"

#include <cstdio>
#include <cstring>

#include "core/updater.h"
#include "core/version.h"
#include "font_manager.h"
#include "ui/draw_list.h"
#include "ui/settings.h"
#include "ui/text.h"
#include "window.h"

namespace {
constexpr float kCardWidth = 460.0f;
constexpr float kCardHeight = 300.0f; // fixed - the notes box below scrolls its own overflow
									  // rather than the card ever growing to fit it
constexpr float kCardRadius = 16.0f;
constexpr float kCardPadding = 28.0f;
constexpr float kButtonHeight = 40.0f;
constexpr float kGap = 14.0f;
constexpr float kProgressBarHeight = 10.0f;
constexpr float kCloseButtonSize = 28.0f;

constexpr Color kColorTextBright{232, 232, 236, 255};
constexpr Color kColorTextDim{150, 150, 156, 255};
constexpr Color kColorError{220, 90, 80, 255};
constexpr Color kColorTrack{40, 40, 45, 255};
constexpr Color kColorBackdrop{8, 8, 10, 200}; // semi-transparent, unlike CUnlockScreen's opaque
											   // backdrop - this can be dismissed, so whatever's
											   // behind it staying faintly visible reads as "on top
											   // of the app," not "a separate screen"

// Release notes (UPDATE_STAGE_AVAILABLE only) get a fixed-height scrollable box, not an
// error/status message - those come from this project's own code (short, known, bounded)
// rather than a manifest author's free-form text, so they're wrapped in place instead
// (kMaxMessageLines below) without needing a whole scroll region for what's realistically
// never going to be more than a line or two.
constexpr float kNotesBoxPadding = 12.0f;
constexpr float kNotesScrollbarMargin = 14.0f; // reserved on the box's right edge - text never wraps into it
// Was 40 - too tight for a long note, since WrapText silently drops lines past the cap and
// the scroll box's max offset stopped short of the note's real end.
constexpr std::uint32_t kMaxNotesLines = 256;
constexpr std::uint32_t kMaxMessageLines = 4;

Rect CardRect(float windowW, float windowH, float cardHeight)
{
	return Rect{(windowW - kCardWidth) * 0.5f, (windowH - cardHeight) * 0.5f, kCardWidth, cardHeight};
}

Rect CloseButtonRect(Rect card)
{
	return Rect{card.X + card.W - kCloseButtonSize - 14.0f, card.Y + 14.0f, kCloseButtonSize, kCloseButtonSize};
}

Rect PrimaryButtonRect(Rect card)
{
	return Rect{card.X + kCardPadding, card.Y + card.H - kCardPadding - kButtonHeight, card.W - kCardPadding * 2.0f,
				kButtonHeight};
}

// Just past where the title + subtitle lines actually end, so the box's top can't drift out
// of sync with them (e.g. at a different Settings Font Size).
float NotesBoxTopY(Rect card, const CFontManager &fonts)
{
	const CFont &body = fonts.GetBody();
	const CFont &secondary = fonts.GetSecondary();
	const float titleBaselineY = card.Y + kCardPadding + body.GetAscent();
	const float subtitleBaselineY = titleBaselineY + body.GetLineHeight() + 10.0f + secondary.GetAscent();
	return subtitleBaselineY + secondary.GetDescent() + kGap;
}

// The release-notes scroll box - from just past the title/version lines down to just above
// the primary button, so its height (and therefore how much scrolls vs. how much always
// fits) is whatever's left over, not a hand-tuned number that could drift out of sync with
// the button's own position.
Rect NotesBoxRect(Rect card, const CFontManager &fonts)
{
	const Rect button = PrimaryButtonRect(card);
	const float top = NotesBoxTopY(card, fonts);
	const float bottom = button.Y - kGap;
	return Rect{card.X + kCardPadding, top, card.W - kCardPadding * 2.0f, bottom - top};
}

Rect NotesScrollbarTrackRect(Rect box)
{
	return Rect{box.X + box.W - kScrollbarWidth, box.Y, kScrollbarWidth, box.H};
}

// Whether this stage still shows the dismiss (X) button - not while something is actually
// in flight (downloading/verifying/installing), matching CUnlockScreen's own "you can't
// back out of something already happening" instinct, just scoped to only the window where
// backing out would actually be disruptive rather than this whole widget's entire lifetime.
bool StageIsDismissable(EUpdateStage stage)
{
	return stage != EUpdateStage::UPDATE_STAGE_DOWNLOADING && stage != EUpdateStage::UPDATE_STAGE_VERIFYING &&
		   stage != EUpdateStage::UPDATE_STAGE_INSTALLING;
}

void FormatBytes(std::uint64_t bytes, char *pOut, std::size_t outCapacity)
{
	if (bytes >= 1024ull * 1024ull) {
		std::snprintf(pOut, outCapacity, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
	} else {
		std::snprintf(pOut, outCapacity, "%.0f KB", static_cast<double>(bytes) / 1024.0);
	}
}

void FormatSpeed(double bytesPerSecond, char *pOut, std::size_t outCapacity)
{
	if (bytesPerSecond >= 1024.0 * 1024.0) {
		std::snprintf(pOut, outCapacity, "%.1f MB/s", bytesPerSecond / (1024.0 * 1024.0));
	} else {
		std::snprintf(pOut, outCapacity, "%.0f KB/s", bytesPerSecond / 1024.0);
	}
}

// Splits on literal '\n' only - the hard-break half of WrapText below. Kept separate (rather
// than folding straight into WrapText) since a manifest author's own explicit line breaks
// (a hand-written bullet list) should always survive as real breaks, decoupled from the
// soft, width-driven wrapping WrapParagraph does within each one.
std::uint32_t SplitLines(CStringView text, CStringView *pOutLines, std::uint32_t maxLines)
{
	std::uint32_t count = 0;
	std::uint64_t lineStart = 0;
	for (std::uint64_t i = 0; i <= text.Length && count < maxLines; i += 1) {
		if (i == text.Length || text.pData[i] == '\n') {
			pOutLines[count] = CStringView{text.pData + lineStart, i - lineStart};
			count += 1;
			lineStart = i + 1;
		}
	}
	return count;
}

// Greedy word-wrap over a single paragraph (no embedded newlines - see WrapText, which
// splits those out first). Extends the current line word-by-word for as long as it still
// fits maxWidth; a single word wider than maxWidth on its own is taken whole rather than
// split mid-word (this project's manifest text is never expected to contain one, and a mid-
// word break would need per-glyph width lookups this helper has no reason to do).
std::uint32_t WrapParagraph(const CFont &font, CStringView paragraph, float maxWidth, CStringView *pOutLines,
							std::uint32_t maxLines)
{
	if (paragraph.Length == 0) {
		if (maxLines == 0) {
			return 0;
		}
		pOutLines[0] = paragraph;
		return 1;
	}

	std::uint32_t lineCount = 0;
	std::uint64_t lineStart = 0;

	while (lineStart < paragraph.Length && lineCount < maxLines) {
		std::uint64_t lineEnd = lineStart;
		std::uint64_t scan = lineStart;

		for (;;) {
			while (scan < paragraph.Length && paragraph.pData[scan] == ' ') {
				scan += 1;
			}
			const std::uint64_t wordStart = scan;
			while (scan < paragraph.Length && paragraph.pData[scan] != ' ') {
				scan += 1;
			}
			if (wordStart == scan) {
				break; // ran out of words (trailing spaces consumed the rest of the paragraph)
			}

			const CStringView candidate{paragraph.pData + lineStart, scan - lineStart};
			if (TextWidth(font, candidate) > maxWidth && lineEnd > lineStart) {
				scan = wordStart; // this word doesn't fit - leave it for the next line
				break;
			}
			lineEnd = scan;
		}

		if (lineEnd == lineStart) {
			// A single word alone is already wider than maxWidth - take it whole so this
			// loop always makes forward progress instead of spinning forever on it.
			std::uint64_t wordEnd = lineStart;
			while (wordEnd < paragraph.Length && paragraph.pData[wordEnd] != ' ') {
				wordEnd += 1;
			}
			lineEnd = wordEnd > lineStart ? wordEnd : lineStart + 1;
		}

		pOutLines[lineCount] = CStringView{paragraph.pData + lineStart, lineEnd - lineStart};
		lineCount += 1;

		lineStart = lineEnd;
		while (lineStart < paragraph.Length && paragraph.pData[lineStart] == ' ') {
			lineStart += 1;
		}
	}

	return lineCount;
}

// SplitLines (hard breaks) + WrapParagraph (soft, width-driven breaks) composed - the real
// word-wrap every multi-line label in this file draws through, so release notes (and error/
// status messages, which get the same treatment at a smaller line cap - see kMaxMessageLines)
// never run past the card's own edge the way a plain single DrawText call used to.
std::uint32_t WrapText(const CFont &font, CStringView text, float maxWidth, CStringView *pOutLines,
					   std::uint32_t maxLines)
{
	CStringView paragraphs[16];
	const std::uint32_t paragraphCount = SplitLines(text, paragraphs, 16);

	std::uint32_t lineCount = 0;
	for (std::uint32_t p = 0; p < paragraphCount && lineCount < maxLines; p += 1) {
		lineCount += WrapParagraph(font, paragraphs[p], maxWidth, pOutLines + lineCount, maxLines - lineCount);
	}
	return lineCount;
}

// Wraps and draws in one call for the (short, unscrolled) error/status messages - returns
// the Y just past the last line drawn, in case a caller ever needs to stack something
// beneath it.
float DrawWrappedText(CDrawList &drawList, const CFont &font, float x, float y, float maxWidth, CStringView text,
					  Color color, std::uint32_t maxLines)
{
	CStringView lines[kMaxMessageLines];
	const std::uint32_t cappedMaxLines = maxLines < kMaxMessageLines ? maxLines : kMaxMessageLines;
	const std::uint32_t lineCount = WrapText(font, text, maxWidth, lines, cappedMaxLines);
	float cursorY = y;
	for (std::uint32_t i = 0; i < lineCount; i += 1) {
		DrawText(drawList, font, x, cursorY, lines[i], color);
		cursorY += font.GetLineHeight();
	}
	return cursorY;
}
} // namespace

CUpdateOverlay::CUpdateOverlay(CFontManager &fonts, CWindow &window, CSettings &settings, CUpdater &updater)
	: m_fonts(fonts)
	, m_window(window)
	, m_settings(settings)
	, m_updater(updater)
{
}

void CUpdateOverlay::Open()
{
	m_bActive = true;
}

void CUpdateOverlay::Close()
{
	m_bActive = false;
}

void CUpdateOverlay::Update(float deltaSeconds)
{
	m_notesScroll.Update(deltaSeconds);
}

bool CUpdateOverlay::OnPointerUp(float x, float y)
{
	if (!m_bActive) {
		return false;
	}

	// A scrollbar-thumb drag ending on this same release shouldn't also register as a click
	// on whatever's underneath the pointer (the primary button, the close button) - same
	// "the drag ending IS the interaction" reasoning CAccountModal's own account-list
	// scrollbar already follows.
	if (m_notesScroll.IsDragging()) {
		m_notesScroll.OnPointerUp();
		return true;
	}

	const auto windowW = static_cast<float>(m_window.GetWidth());
	const auto windowH = static_cast<float>(m_window.GetHeight());
	const EUpdateStage stage = m_updater.GetStage();
	const Rect card = CardRect(windowW, windowH, kCardHeight);

	if (StageIsDismissable(stage) && RectContainsPoint(CloseButtonRect(card), x, y)) {
		Close();
		return true;
	}

	const Rect primaryButton = PrimaryButtonRect(card);

	if (stage == EUpdateStage::UPDATE_STAGE_AVAILABLE) {
		if (RectContainsPoint(primaryButton, x, y)) {
			m_updater.StartDownloadAsync();
		}
	} else if (stage == EUpdateStage::UPDATE_STAGE_DOWNLOADING) {
		if (RectContainsPoint(primaryButton, x, y)) {
			m_updater.RequestCancel();
		}
	} else if (stage == EUpdateStage::UPDATE_STAGE_ERROR || stage == EUpdateStage::UPDATE_STAGE_CANCELLED) {
		if (RectContainsPoint(primaryButton, x, y)) {
			m_updater.StartDownloadAsync(); // re-reads the manifest CUpdater already has - a
											// plain retry, no fresh CheckForUpdateAsync needed
		}
	} else if (stage == EUpdateStage::UPDATE_STAGE_UP_TO_DATE || stage == EUpdateStage::UPDATE_STAGE_CHECK_FAILED) {
		if (RectContainsPoint(primaryButton, x, y)) {
			m_updater.CheckForUpdateAsync(kAppVersion); // a fresh manifest fetch this time,
														// not StartDownloadAsync's plain retry -
														// there is no download to retry yet
		}
	}

	return true;
}

bool CUpdateOverlay::OnPointerDown(float x, float y)
{
	if (!m_bActive) {
		return false;
	}

	if (m_updater.GetStage() == EUpdateStage::UPDATE_STAGE_AVAILABLE) {
		const auto windowW = static_cast<float>(m_window.GetWidth());
		const auto windowH = static_cast<float>(m_window.GetHeight());
		const Rect box = NotesBoxRect(CardRect(windowW, windowH, kCardHeight), m_fonts);

		const CFont &secondary = m_fonts.GetSecondary();
		CStringView lines[kMaxNotesLines];
		const std::uint32_t lineCount = WrapText(secondary, StringViewFromCString(m_updater.GetManifest().szNotes),
												 box.W - kNotesScrollbarMargin, lines, kMaxNotesLines);
		const float contentHeight = static_cast<float>(lineCount) * secondary.GetLineHeight();

		m_notesScroll.OnPointerDown(x, y, NotesScrollbarTrackRect(box), contentHeight, box.H);
	}

	return IsBlocking();
}

bool CUpdateOverlay::OnPointerMove(float x, float y)
{
	if (!m_bActive) {
		return false;
	}

	if (m_notesScroll.IsDragging()) {
		const auto windowW = static_cast<float>(m_window.GetWidth());
		const auto windowH = static_cast<float>(m_window.GetHeight());
		const Rect box = NotesBoxRect(CardRect(windowW, windowH, kCardHeight), m_fonts);

		const CFont &secondary = m_fonts.GetSecondary();
		CStringView lines[kMaxNotesLines];
		const std::uint32_t lineCount = WrapText(secondary, StringViewFromCString(m_updater.GetManifest().szNotes),
												 box.W - kNotesScrollbarMargin, lines, kMaxNotesLines);
		const float contentHeight = static_cast<float>(lineCount) * secondary.GetLineHeight();

		m_notesScroll.OnPointerMove(y, NotesScrollbarTrackRect(box), contentHeight, box.H);
	}

	return IsBlocking();
}

bool CUpdateOverlay::OnScroll(float x, float y, float wheelDelta)
{
	if (!m_bActive) {
		return false;
	}

	if (m_updater.GetStage() == EUpdateStage::UPDATE_STAGE_AVAILABLE) {
		const auto windowW = static_cast<float>(m_window.GetWidth());
		const auto windowH = static_cast<float>(m_window.GetHeight());
		const Rect box = NotesBoxRect(CardRect(windowW, windowH, kCardHeight), m_fonts);

		if (RectContainsPoint(box, x, y)) {
			const CFont &secondary = m_fonts.GetSecondary();
			CStringView lines[kMaxNotesLines];
			const std::uint32_t lineCount = WrapText(secondary, StringViewFromCString(m_updater.GetManifest().szNotes),
													 box.W - kNotesScrollbarMargin, lines, kMaxNotesLines);
			const float contentHeight = static_cast<float>(lineCount) * secondary.GetLineHeight();

			m_notesScroll.OnScroll(wheelDelta, contentHeight, box.H);
		}
	}

	return IsBlocking();
}

ECursorKind CUpdateOverlay::GetDesiredCursor() const
{
	if (!m_bActive) {
		return ECursorKind::CURSOR_ARROW;
	}

	const auto windowW = static_cast<float>(m_window.GetWidth());
	const auto windowH = static_cast<float>(m_window.GetHeight());
	const Rect card = CardRect(windowW, windowH, kCardHeight);
	const EUpdateStage stage = m_updater.GetStage();

	if (StageIsDismissable(stage) && RectContainsPoint(CloseButtonRect(card), m_flMouseX, m_flMouseY)) {
		return ECursorKind::CURSOR_HAND;
	}

	const Rect primaryButton = PrimaryButtonRect(card);
	const bool hasPrimaryAction =
		stage == EUpdateStage::UPDATE_STAGE_AVAILABLE || stage == EUpdateStage::UPDATE_STAGE_DOWNLOADING ||
		stage == EUpdateStage::UPDATE_STAGE_ERROR || stage == EUpdateStage::UPDATE_STAGE_CANCELLED ||
		stage == EUpdateStage::UPDATE_STAGE_UP_TO_DATE || stage == EUpdateStage::UPDATE_STAGE_CHECK_FAILED;
	if (hasPrimaryAction && RectContainsPoint(primaryButton, m_flMouseX, m_flMouseY)) {
		return ECursorKind::CURSOR_HAND;
	}

	return ECursorKind::CURSOR_ARROW;
}

void CUpdateOverlay::Draw(CDrawList &drawList)
{
	if (!m_bActive) {
		return;
	}

	const auto windowW = static_cast<float>(m_window.GetWidth());
	const auto windowH = static_cast<float>(m_window.GetHeight());
	const Color accent = m_settings.m_clrAccent;
	const CFont &body = m_fonts.GetBody();
	const CFont &secondary = m_fonts.GetSecondary();

	drawList.AddRectFilled(0.0f, 0.0f, windowW, windowH, kColorBackdrop);

	const EUpdateStage stage = m_updater.GetStage();
	const Rect card = CardRect(windowW, windowH, kCardHeight);
	drawList.AddRectRoundedFilled(card.X, card.Y, card.W, card.H, CDrawList::UniformRadii(kCardRadius),
								  Color{26, 26, 30, 255});

	if (StageIsDismissable(stage)) {
		const Rect close = CloseButtonRect(card);
		const bool hovered = RectContainsPoint(close, m_flMouseX, m_flMouseY);
		if (hovered) {
			drawList.AddRectRoundedFilled(close.X, close.Y, close.W, close.H, CDrawList::UniformRadii(close.W * 0.5f),
										  Color{56, 56, 62, 255});
		}
		const float cx = close.X + close.W * 0.5f;
		const float cy = close.Y + close.H * 0.5f;
		const Color glyphColor = hovered ? kColorTextBright : kColorTextDim;
		drawList.AddLine(cx - 5.0f, cy - 5.0f, cx + 5.0f, cy + 5.0f, 1.5f, glyphColor);
		drawList.AddLine(cx - 5.0f, cy + 5.0f, cx + 5.0f, cy - 5.0f, 1.5f, glyphColor);
	}

	const float contentX = card.X + kCardPadding;
	const float contentW = card.W - kCardPadding * 2.0f;
	float cursorY = card.Y + kCardPadding + body.GetAscent();

	const auto DrawTitle = [&](CStringView text) {
		DrawText(drawList, body, contentX, cursorY, text, kColorTextBright);
		cursorY += body.GetLineHeight() + 10.0f + secondary.GetAscent();
	};

	char lineBuffer[64];

	if (stage == EUpdateStage::UPDATE_STAGE_IDLE || stage == EUpdateStage::UPDATE_STAGE_CHECKING) {
		// IDLE only shows up here for a single frame at most - opening this from
		// CSettingsMenu's Check for Updates row kicks CheckForUpdateAsync first (see
		// main.cpp), which moves the stage to CHECKING before the very next Draw - so this
		// reads as "Checking..." either way, never a blank/confusing IDLE screen.
		DrawTitle(StringViewFromCString("Checking for Updates"));
		DrawText(drawList, secondary, contentX, cursorY, StringViewFromCString("This will only take a moment."),
				 kColorTextDim);
	} else if (stage == EUpdateStage::UPDATE_STAGE_UP_TO_DATE) {
		DrawTitle(StringViewFromCString("You're Up to Date"));
		std::snprintf(lineBuffer, sizeof(lineBuffer), "Rift %s is the latest version.", kAppVersion);
		DrawText(drawList, secondary, contentX, cursorY, StringViewFromCString(lineBuffer), kColorTextDim);

		const Rect button = PrimaryButtonRect(card);
		drawList.AddRectRoundedFilled(button.X, button.Y, button.W, button.H, CDrawList::UniformRadii(8.0f),
									  Color{40, 40, 45, 255});
		DrawCenteredText(drawList, body, button.X, button.Y, button.W, button.H, StringViewFromCString("Check Again"),
						 kColorTextBright);
	} else if (stage == EUpdateStage::UPDATE_STAGE_CHECK_FAILED) {
		DrawTitle(StringViewFromCString("Couldn't Check for Updates"));
		DrawWrappedText(drawList, secondary, contentX, cursorY, contentW,
						StringViewFromCString(m_updater.GetErrorMessage()), kColorError, kMaxMessageLines);

		const Rect button = PrimaryButtonRect(card);
		drawList.AddRectRoundedBordered(button.X, button.Y, button.W, button.H, CDrawList::UniformRadii(8.0f), accent,
										ColorOutlineOn(accent), 1.0f);
		DrawCenteredText(drawList, body, button.X, button.Y, button.W, button.H, StringViewFromCString("Try Again"),
						 ColorForegroundOn(accent));
	} else if (stage == EUpdateStage::UPDATE_STAGE_AVAILABLE) {
		DrawTitle(StringViewFromCString("Update Available"));
		std::snprintf(lineBuffer, sizeof(lineBuffer), "Version %s is ready to install.",
					  m_updater.GetManifest().szVersion);
		DrawText(drawList, secondary, contentX, cursorY, StringViewFromCString(lineBuffer), kColorTextDim);

		const Rect box = NotesBoxRect(card, m_fonts);
		drawList.AddRectRoundedFilled(box.X, box.Y, box.W, box.H, CDrawList::UniformRadii(8.0f),
									  Color{22, 22, 25, 255});

		const float wrapWidth = box.W - kNotesScrollbarMargin;
		CStringView lines[kMaxNotesLines];
		const std::uint32_t lineCount = WrapText(secondary, StringViewFromCString(m_updater.GetManifest().szNotes),
												 wrapWidth, lines, kMaxNotesLines);
		const float lineHeight = secondary.GetLineHeight();
		const float contentHeight = static_cast<float>(lineCount) * lineHeight;

		drawList.PushClipRect(box);
		float notesY = box.Y + kNotesBoxPadding + secondary.GetAscent() - m_notesScroll.m_flScrollOffset;
		for (std::uint32_t i = 0; i < lineCount; i += 1) {
			if (notesY > box.Y - lineHeight && notesY < box.Y + box.H + lineHeight) { // cheap cull
				DrawText(drawList, secondary, box.X + kNotesBoxPadding, notesY, lines[i], kColorTextDim);
			}
			notesY += lineHeight;
		}
		drawList.PopClipRect();

		m_notesScroll.DrawEdgeFade(drawList, box, contentHeight, box.H, Color{22, 22, 25, 255});
		m_notesScroll.Draw(drawList, NotesScrollbarTrackRect(box), contentHeight, box.H, Color{120, 120, 128, 190},
						   m_flMouseX, m_flMouseY);

		const Rect button = PrimaryButtonRect(card);
		drawList.AddRectRoundedBordered(button.X, button.Y, button.W, button.H, CDrawList::UniformRadii(8.0f), accent,
										ColorOutlineOn(accent), 1.0f);
		DrawCenteredText(drawList, body, button.X, button.Y, button.W, button.H,
						 StringViewFromCString("Download & Install"), ColorForegroundOn(accent));
	} else if (stage == EUpdateStage::UPDATE_STAGE_MANUAL_UPGRADE_REQUIRED) {
		DrawTitle(StringViewFromCString("Manual Update Required"));
		std::snprintf(lineBuffer, sizeof(lineBuffer), "Version %s is out - please download it manually",
					  m_updater.GetManifest().szVersion);
		DrawText(drawList, secondary, contentX, cursorY, StringViewFromCString(lineBuffer), kColorTextDim);
		cursorY += secondary.GetLineHeight();
		DrawText(drawList, secondary, contentX, cursorY, StringViewFromCString("from the GitHub releases page."),
				 kColorTextDim);
	} else if (stage == EUpdateStage::UPDATE_STAGE_DOWNLOADING || stage == EUpdateStage::UPDATE_STAGE_VERIFYING ||
			   stage == EUpdateStage::UPDATE_STAGE_INSTALLING) {
		DrawTitle(StringViewFromCString("Updating Rift"));

		const std::uint64_t downloaded = m_updater.GetBytesDownloaded();
		const std::uint64_t total = m_updater.GetTotalBytes();
		const float downloadFraction = total > 0 ? static_cast<float>(downloaded) / static_cast<float>(total) : 0.0f;
		const float progress = stage == EUpdateStage::UPDATE_STAGE_DOWNLOADING ? downloadFraction : 1.0f;

		const Rect track{contentX, cursorY + 8.0f, contentW, kProgressBarHeight};
		drawList.AddRectRoundedFilled(track.X, track.Y, track.W, track.H, CDrawList::UniformRadii(track.H * 0.5f),
									  kColorTrack);
		const float fillWidth = track.W * (progress < 0.02f ? 0.02f : progress); // a sliver stays visible at 0%
																				 // so the bar itself is never
																				 // literally invisible
		drawList.AddRectRoundedFilled(track.X, track.Y, fillWidth, track.H, CDrawList::UniformRadii(track.H * 0.5f),
									  accent);
		cursorY = track.Y + track.H + 14.0f + secondary.GetAscent();

		if (stage == EUpdateStage::UPDATE_STAGE_DOWNLOADING) {
			char downloadedStr[32];
			char totalStr[32];
			char speedStr[32];
			FormatBytes(downloaded, downloadedStr, sizeof(downloadedStr));
			FormatBytes(total, totalStr, sizeof(totalStr));
			FormatSpeed(m_updater.GetBytesPerSecond(), speedStr, sizeof(speedStr));
			std::snprintf(lineBuffer, sizeof(lineBuffer), "%s / %s  -  %s", downloadedStr, totalStr, speedStr);
		} else if (stage == EUpdateStage::UPDATE_STAGE_VERIFYING) {
			std::snprintf(lineBuffer, sizeof(lineBuffer), "Verifying...");
		} else {
			std::snprintf(lineBuffer, sizeof(lineBuffer), "Installing...");
		}
		DrawText(drawList, secondary, contentX, cursorY, StringViewFromCString(lineBuffer), kColorTextDim);

		if (stage == EUpdateStage::UPDATE_STAGE_DOWNLOADING) {
			const Rect button = PrimaryButtonRect(card);
			const bool hovered = RectContainsPoint(button, m_flMouseX, m_flMouseY);
			drawList.AddRectRoundedFilled(button.X, button.Y, button.W, button.H, CDrawList::UniformRadii(8.0f),
										  hovered ? Color{56, 56, 62, 255} : Color{40, 40, 45, 255});
			DrawCenteredText(drawList, body, button.X, button.Y, button.W, button.H, StringViewFromCString("Cancel"),
							 kColorTextBright);
		}
	} else if (stage == EUpdateStage::UPDATE_STAGE_READY_TO_RELAUNCH) {
		DrawTitle(StringViewFromCString("Restarting..."));
	} else if (stage == EUpdateStage::UPDATE_STAGE_ERROR || stage == EUpdateStage::UPDATE_STAGE_CANCELLED) {
		DrawTitle(
			StringViewFromCString(stage == EUpdateStage::UPDATE_STAGE_ERROR ? "Update Failed" : "Update Cancelled"));
		if (stage == EUpdateStage::UPDATE_STAGE_ERROR) {
			DrawWrappedText(drawList, secondary, contentX, cursorY, contentW,
							StringViewFromCString(m_updater.GetErrorMessage()), kColorError, kMaxMessageLines);
		}

		const Rect button = PrimaryButtonRect(card);
		drawList.AddRectRoundedBordered(button.X, button.Y, button.W, button.H, CDrawList::UniformRadii(8.0f), accent,
										ColorOutlineOn(accent), 1.0f);
		DrawCenteredText(drawList, body, button.X, button.Y, button.W, button.H, StringViewFromCString("Try Again"),
						 ColorForegroundOn(accent));
	}
}
