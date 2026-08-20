#pragma once

// A bump-allocating scratch arena - unlike the original f4's TigerStyle rule (every
// long-lived allocation goes through one of these), this fork uses CMemoryArena for
// exactly one thing: per-frame draw-list scratch (vertex/index buffers), the one place
// FORK_WITH_CLASSES.md's memory-strategy section (see the original f4 repo) singles out
// as still having a real, unambiguous case for pre-sized bump allocation regardless of
// which broader architecture wraps it - a frame's geometry buffer has a knowable upper
// bound, and allocating it fresh every frame would be real, avoidable churn. Everything
// else in this fork (the widget tree, one-shot persistence buffers, GPU resource
// wrappers) uses ordinary RAII/heap ownership instead - see the widget base class and
// CGpuTexture for why.

#include <cstdint>
#include <cstddef>

class CMemoryArena {
  public:
	CMemoryArena() = default;
	~CMemoryArena();

	// No copying - a CMemoryArena owns one heap reservation, and a bitwise copy would
	// leave two owners pointing at the same memory.
	CMemoryArena(const CMemoryArena &) = delete;
	CMemoryArena &operator=(const CMemoryArena &) = delete;

	// Reserves m_capacity bytes once, up front. Returns false on failure - callers treat
	// that as a fatal startup error (out of memory before the app has even drawn a
	// frame), not something to retry.
	bool Init(std::uint64_t capacity);

	// Rewinds the bump pointer without releasing the underlying reservation - "clearing"
	// a scratch arena means this, once per frame, not a free/realloc cycle.
	void Reset();

	// The one allocation primitive every typed helper below funnels through - bumps
	// m_offset forward by size (aligned to alignment), asserting rather than growing or
	// falling back to malloc if the arena is exhausted. Running out here is a sizing bug
	// to fix at the call site that picked m_capacity, not a runtime condition to handle
	// gracefully.
	void *Alloc(std::uint64_t size, std::uint64_t alignment = alignof(std::max_align_t));

	template <typename T>
	T *AllocArray(std::uint64_t count)
	{
		return static_cast<T *>(Alloc(sizeof(T) * count, alignof(T)));
	}

  private:
	std::uint8_t *m_pBase = nullptr;
	std::uint64_t m_nCapacity = 0;
	std::uint64_t m_nOffset = 0;
};
