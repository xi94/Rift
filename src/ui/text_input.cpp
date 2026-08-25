#include "ui/text_input.h"

#include <algorithm>
#include <cstring>

#include <Windows.h>

#include "gfx/font.h"
#include "ui/draw_list.h"
#include "ui/text.h"

namespace {
constexpr float kCaretBlinkPeriodSeconds = 1.0f;
constexpr float kCaretWidth = 1.5f;
constexpr float kTextPadding = 8.0f;

// SetClipboardData(CF_TEXT) with a GlobalAlloc'd buffer - plain-ASCII since
// CTextInput's buffer already is.
void CopyToClipboard(CStringView text)
{
	if (text.Length == 0) {
		return;
	}
	if (!OpenClipboard(nullptr)) {
		return;
	}
	EmptyClipboard();

	const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(text.Length) + 1);
	if (memory != nullptr) {
		auto *pDestination = static_cast<char *>(GlobalLock(memory));
		if (pDestination != nullptr) {
			std::memcpy(pDestination, text.pData, text.Length);
			pDestination[text.Length] = '\0';
			GlobalUnlock(memory);
			SetClipboardData(CF_TEXT, memory);
		}
	}
	CloseClipboard();
}
} // namespace

void CTextInput::Init(CStringView initialValue)
{
	m_nLength = 0;
	m_nCursor = 0;
	m_bFocused = false;
	m_flCaretBlinkSeconds = 0.0f;
	m_nSelectionAnchor = -1;
	SetValue(initialValue);
}

void CTextInput::SetValue(CStringView value)
{
	const auto length = static_cast<std::uint32_t>(std::min<std::uint64_t>(value.Length, kTextInputCapacity));
	std::memcpy(m_szBuffer, value.pData, length);
	m_nLength = length;
	m_nCursor = length;
	m_nSelectionAnchor = -1;
}

// Normalizes the (anchor, cursor) pair into an ordered [start, end) range regardless of
// which direction the selection was extended in - every selection-consuming call below
// wants "the range," not "which end is the anchor."
namespace {
void SelectionRangeOf(std::uint32_t nCursor, std::int32_t nSelectionAnchor, std::uint32_t &start, std::uint32_t &end)
{
	const auto anchor = static_cast<std::uint32_t>(nSelectionAnchor);
	start = std::min(anchor, nCursor);
	end = std::max(anchor, nCursor);
}
} // namespace

void CTextInput::DeleteSelection()
{
	if (!HasSelection()) {
		return;
	}
	std::uint32_t start = 0;
	std::uint32_t end = 0;
	SelectionRangeOf(m_nCursor, m_nSelectionAnchor, start, end);
	const std::uint32_t count = end - start;
	for (std::uint32_t i = start; i < m_nLength - count; i += 1) {
		m_szBuffer[i] = m_szBuffer[i + count];
	}
	m_nLength -= count;
	m_nCursor = start;
	m_nSelectionAnchor = -1;
}

void CTextInput::OnChar(std::uint32_t character)
{
	if (!m_bFocused) {
		return;
	}
	if (character < 0x20 || character > 0x7E) {
		return; // printable ASCII only for now
	}
	if (!HasSelection() && m_nLength >= kTextInputCapacity) {
		return;
	}

	DeleteSelection();

	for (std::uint32_t i = m_nLength; i > m_nCursor; i -= 1) {
		m_szBuffer[i] = m_szBuffer[i - 1];
	}
	m_szBuffer[m_nCursor] = static_cast<char>(character);
	m_nLength += 1;
	m_nCursor += 1;
	m_nSelectionAnchor = -1;

	m_flCaretBlinkSeconds = 0.0f; // typing keeps the caret solid, not mid-blink-out
}

void CTextInput::OnKey(std::uint32_t keyCode)
{
	if (!m_bFocused) {
		return;
	}

	const bool ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
	const bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

	if (ctrlDown) {
		switch (keyCode) {
			case 'A': // select-all - the more broadly-expected Windows binding; see text_input.h
				m_nSelectionAnchor = 0;
				m_nCursor = m_nLength;
				m_flCaretBlinkSeconds = 0.0f;
				return;
			case 'E': // Emacs end-of-line, explicitly requested alongside Ctrl+A above
				m_nCursor = m_nLength;
				m_nSelectionAnchor = -1;
				m_flCaretBlinkSeconds = 0.0f;
				return;
			case 'C':
				if (HasSelection()) {
					std::uint32_t start = 0;
					std::uint32_t end = 0;
					SelectionRangeOf(m_nCursor, m_nSelectionAnchor, start, end);
					CopyToClipboard(CStringView{m_szBuffer + start, end - start});
				}
				return;
			case 'X':
				if (HasSelection()) {
					std::uint32_t start = 0;
					std::uint32_t end = 0;
					SelectionRangeOf(m_nCursor, m_nSelectionAnchor, start, end);
					CopyToClipboard(CStringView{m_szBuffer + start, end - start});
					DeleteSelection();
					m_flCaretBlinkSeconds = 0.0f;
				}
				return;
			case 'V':
				if (OpenClipboard(nullptr)) {
					const HANDLE handle = GetClipboardData(CF_TEXT);
					if (handle != nullptr) {
						const auto *pData = static_cast<const char *>(GlobalLock(handle));
						if (pData != nullptr) {
							DeleteSelection();
							std::uint32_t insertCount = 0;
							const std::uint64_t textLength = std::strlen(pData);
							for (std::uint64_t i = 0; i < textLength && m_nLength + insertCount < kTextInputCapacity;
								 i += 1) {
								const auto c = static_cast<unsigned char>(pData[i]);
								if (c < 0x20 || c > 0x7E) {
									continue;
								}
								insertCount += 1;
							}
							for (std::uint32_t i = m_nLength; i > m_nCursor; i -= 1) {
								m_szBuffer[i - 1 + insertCount] = m_szBuffer[i - 1];
							}
							std::uint32_t written = 0;
							for (std::uint64_t i = 0; i < textLength && written < insertCount; i += 1) {
								const auto c = static_cast<unsigned char>(pData[i]);
								if (c < 0x20 || c > 0x7E) {
									continue;
								}
								m_szBuffer[m_nCursor + written] = static_cast<char>(c);
								written += 1;
							}
							m_nLength += insertCount;
							m_nCursor += insertCount;
							m_nSelectionAnchor = -1;
							GlobalUnlock(handle);
						}
					}
					CloseClipboard();
				}
				m_flCaretBlinkSeconds = 0.0f;
				return;
			default:
				break;
		}
	}

	switch (keyCode) {
		case VK_BACK:
			if (HasSelection()) {
				DeleteSelection();
			} else if (m_nCursor > 0) {
				for (std::uint32_t i = m_nCursor - 1; i < m_nLength - 1; i += 1) {
					m_szBuffer[i] = m_szBuffer[i + 1];
				}
				m_nLength -= 1;
				m_nCursor -= 1;
			} else {
				return;
			}
			break;
		case VK_DELETE:
			if (HasSelection()) {
				DeleteSelection();
			} else if (m_nCursor < m_nLength) {
				for (std::uint32_t i = m_nCursor; i < m_nLength - 1; i += 1) {
					m_szBuffer[i] = m_szBuffer[i + 1];
				}
				m_nLength -= 1;
			} else {
				return;
			}
			break;
		case VK_LEFT:
			if (shiftDown) {
				if (m_nSelectionAnchor < 0) {
					m_nSelectionAnchor = static_cast<std::int32_t>(m_nCursor);
				}
				if (m_nCursor > 0) {
					m_nCursor -= 1;
				}
			} else {
				if (HasSelection()) {
					std::uint32_t start = 0;
					std::uint32_t end = 0;
					SelectionRangeOf(m_nCursor, m_nSelectionAnchor, start, end);
					m_nCursor = start;
				} else if (m_nCursor > 0) {
					m_nCursor -= 1;
				}
				m_nSelectionAnchor = -1;
			}
			break;
		case VK_RIGHT:
			if (shiftDown) {
				if (m_nSelectionAnchor < 0) {
					m_nSelectionAnchor = static_cast<std::int32_t>(m_nCursor);
				}
				if (m_nCursor < m_nLength) {
					m_nCursor += 1;
				}
			} else {
				if (HasSelection()) {
					std::uint32_t start = 0;
					std::uint32_t end = 0;
					SelectionRangeOf(m_nCursor, m_nSelectionAnchor, start, end);
					m_nCursor = end;
				} else if (m_nCursor < m_nLength) {
					m_nCursor += 1;
				}
				m_nSelectionAnchor = -1;
			}
			break;
		case VK_HOME:
			if (shiftDown && m_nSelectionAnchor < 0) {
				m_nSelectionAnchor = static_cast<std::int32_t>(m_nCursor);
			}
			if (!shiftDown) {
				m_nSelectionAnchor = -1;
			}
			m_nCursor = 0;
			break;
		case VK_END:
			if (shiftDown && m_nSelectionAnchor < 0) {
				m_nSelectionAnchor = static_cast<std::int32_t>(m_nCursor);
			}
			if (!shiftDown) {
				m_nSelectionAnchor = -1;
			}
			m_nCursor = m_nLength;
			break;
		default:
			return;
	}

	m_flCaretBlinkSeconds = 0.0f;
}

void CTextInput::Update(float deltaSeconds)
{
	m_flCaretBlinkSeconds += deltaSeconds;
	if (m_flCaretBlinkSeconds > kCaretBlinkPeriodSeconds) {
		m_flCaretBlinkSeconds -= kCaretBlinkPeriodSeconds;
	}
}

void CTextInput::Draw(CDrawList &drawList, const CFont &font, float x, float y, float w, float h, Color textColor,
					  Color caretColor, bool masked) const
{
	char maskBuffer[kTextInputCapacity];
	CStringView value = GetValue();
	if (masked) {
		for (std::uint32_t i = 0; i < m_nLength; i += 1) {
			maskBuffer[i] = '*';
		}
		value = CStringView{maskBuffer, m_nLength};
	}

	if (m_bFocused && HasSelection()) {
		std::uint32_t start = 0;
		std::uint32_t end = 0;
		SelectionRangeOf(m_nCursor, m_nSelectionAnchor, start, end);
		const float startX = x + kTextPadding + TextWidth(font, CStringView{value.pData, start});
		const float endX = x + kTextPadding + TextWidth(font, CStringView{value.pData, end});
		drawList.AddRectFilled(startX, y + 4.0f, endX - startX, h - 8.0f, ColorFadeAlpha(caretColor, 70));
	}

	// The value's own visual center on the box's center: baseline = center + (ascent +
	// descent) / 2, with descent negative (see font.h). The old (h + ascent) * 0.5f form
	// left out the descent term entirely, which sat every typed value in this project
	// visibly low in its box - most obvious in the account form's tall fields, where the
	// text looked like it was resting on the bottom border rather than centered.
	const float baselineY = y + h * 0.5f + (font.GetAscent() + font.GetDescent()) * 0.5f;
	DrawText(drawList, font, x + kTextPadding, baselineY, value, textColor);

	if (!m_bFocused) {
		return;
	}
	if (m_flCaretBlinkSeconds >= kCaretBlinkPeriodSeconds * 0.5f) {
		return; // half the period on, half off
	}

	const float caretX = x + kTextPadding + TextWidth(font, CStringView{value.pData, m_nCursor});
	drawList.AddRectFilled(std::min(caretX, x + w - kCaretWidth), y + 4.0f, kCaretWidth, h - 8.0f, caretColor);
}
