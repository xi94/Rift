#include "core/memory_arena.h"

#include <cassert>
#include <cstdlib>

CMemoryArena::~CMemoryArena()
{
	if (m_pBase != nullptr) {
		std::free(m_pBase);
	}
}

bool CMemoryArena::Init(std::uint64_t capacity)
{
	assert(m_pBase == nullptr && "CMemoryArena::Init called twice on the same arena");

	m_pBase = static_cast<std::uint8_t *>(std::malloc(capacity));
	if (m_pBase == nullptr) {
		return false;
	}

	m_nCapacity = capacity;
	m_nOffset = 0;
	return true;
}

void CMemoryArena::Reset()
{
	m_nOffset = 0;
}

void *CMemoryArena::Alloc(std::uint64_t size, std::uint64_t alignment)
{
	const std::uint64_t alignedOffset = (m_nOffset + alignment - 1) & ~(alignment - 1);
	const std::uint64_t newOffset = alignedOffset + size;

	assert(newOffset <= m_nCapacity &&
		   "CMemoryArena exhausted - this is a sizing bug at the call site that picked its capacity");

	m_nOffset = newOffset;
	return m_pBase + alignedOffset;
}
