#include "ui/widget_stack.h"

#include <algorithm>

namespace {
// Walks entries in visual top-to-bottom order (topmost entries, most-recently-pushed
// first; then normal entries, most-recently-pushed first) calling visitor(CWidget&)
// for each. Stops as soon as visitor returns true, returning true itself - the shape
// every Dispatch* method needs ("first consumer wins"). Update needs the same order
// but never wants to stop early, so it just always returns false from its visitor.
template <typename VisitorFunction>
bool IterateTopDown(std::vector<CWidgetStack::Entry> &entries, VisitorFunction &&visitor)
{
	for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
		if (it->m_bAlwaysTopmost && visitor(*it->m_pWidget)) {
			return true;
		}
	}
	for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
		if (!it->m_bAlwaysTopmost && visitor(*it->m_pWidget)) {
			return true;
		}
	}
	return false;
}
} // namespace

CWidget *CWidgetStack::Push(std::unique_ptr<CWidget> widget, bool alwaysTopmost)
{
	CWidget *pRaw = widget.get();
	m_aWidgets.push_back(Entry{std::move(widget), alwaysTopmost});
	return pRaw;
}

void CWidgetStack::Remove(CWidget *pWidget)
{
	std::erase_if(m_aWidgets, [pWidget](const Entry &entry) { return entry.m_pWidget.get() == pWidget; });
}

void CWidgetStack::SetRealMousePosition(float x, float y)
{
	m_flRealMouseX = x;
	m_flRealMouseY = y;
}

void CWidgetStack::Update(float deltaSeconds)
{
	bool somethingAboveIsBlocking = false;

	IterateTopDown(m_aWidgets, [&](CWidget &widget) {
		widget.SetMouseGated(somethingAboveIsBlocking, m_flRealMouseX, m_flRealMouseY);
		widget.Update(deltaSeconds);
		somethingAboveIsBlocking |= widget.IsBlocking();
		return false;
	});
} 

void CWidgetStack::Draw(CDrawList &drawList)
{
	for (const Entry &entry : m_aWidgets) {
		if (!entry.m_bAlwaysTopmost && entry.m_pWidget->m_bVisible) {
			entry.m_pWidget->Draw(drawList);
		}
	}

	for (const Entry &entry : m_aWidgets) {
		if (entry.m_bAlwaysTopmost && entry.m_pWidget->m_bVisible) {
			entry.m_pWidget->Draw(drawList);
		}
	}
}

bool CWidgetStack::DispatchPointerDown(float x, float y)
{
	return IterateTopDown(m_aWidgets, [&](CWidget &widget) { return widget.m_bVisible && widget.OnPointerDown(x, y); });
}

bool CWidgetStack::DispatchPointerMove(float x, float y)
{
	return IterateTopDown(m_aWidgets, [&](CWidget &widget) { return widget.m_bVisible && widget.OnPointerMove(x, y); });
}

bool CWidgetStack::DispatchPointerUp(float x, float y)
{
	return IterateTopDown(m_aWidgets, [&](CWidget &widget) { return widget.m_bVisible && widget.OnPointerUp(x, y); });
}

bool CWidgetStack::DispatchRightPointerUp(float x, float y)
{
	return IterateTopDown(m_aWidgets,
						  [&](CWidget &widget) { return widget.m_bVisible && widget.OnRightPointerUp(x, y); });
}

bool CWidgetStack::DispatchScroll(float x, float y, float wheelDelta)
{
	return IterateTopDown(m_aWidgets,
						  [&](CWidget &widget) { return widget.m_bVisible && widget.OnScroll(x, y, wheelDelta); });
}

bool CWidgetStack::DispatchKeyDown(std::uint32_t keyCode)
{
	return IterateTopDown(m_aWidgets, [&](CWidget &widget) { return widget.m_bVisible && widget.OnKeyDown(keyCode); });
}

bool CWidgetStack::DispatchChar(std::uint32_t character)
{
	return IterateTopDown(m_aWidgets, [&](CWidget &widget) { return widget.m_bVisible && widget.OnChar(character); });
}

ECursorKind CWidgetStack::GetDesiredCursor() const
{
	auto CheckEntries = [](const std::vector<Entry> &entries, bool wantTopmost) -> ECursorKind {
		for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
			if (it->m_bAlwaysTopmost != wantTopmost || !it->m_pWidget->m_bVisible) {
				continue;
			}
			const ECursorKind cursor = it->m_pWidget->GetDesiredCursor();
			if (cursor != ECursorKind::CURSOR_ARROW) {
				return cursor;
			}
		}
		return ECursorKind::CURSOR_ARROW;
	};

	const ECursorKind topmostCursor = CheckEntries(m_aWidgets, true);
	return topmostCursor != ECursorKind::CURSOR_ARROW ? topmostCursor : CheckEntries(m_aWidgets, false);
}
