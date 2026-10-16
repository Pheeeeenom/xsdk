#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"

// newlib's malloc grows its heap through sbrk; this one reserves address space once and commits it as it grows.
enum {
	HEAP_COMMIT_BYTES = 64 * 1024,
	// MmQueryStatistics counts 4 KB pages (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc:496).
	PAGE_BYTES = 4096,
};

void *sbrk(ptrdiff_t increment);

static uintptr_t heap_base;
static uintptr_t heap_end;
static uintptr_t heap_break;
static uintptr_t heap_committed;

// The heap may grow into all the memory the title has free at its first growth (docs/HARDWARE.md).
static NTSTATUS free_bytes(uint32_t *size_bytes)
{
	MM_QUERY_STATISTICS_RESULT statistics = { .size = sizeof(statistics) };
	NTSTATUS status = MmQueryStatistics(&statistics);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("sbrk: MmQueryStatistics failed: 0x%08X", (unsigned)status);
		return status;
	}
	const uint32_t available_bytes = statistics.title.available_pages * PAGE_BYTES;
	*size_bytes = (available_bytes + HEAP_COMMIT_BYTES - 1) & ~(uint32_t)(HEAP_COMMIT_BYTES - 1);
	return status;
}

static NTSTATUS reserve_heap(void)
{
	uint32_t size_bytes;
	NTSTATUS status = free_bytes(&size_bytes);
	if (!NT_SUCCESS(status))
		return status;
	void *base = NULL;
	status = NtAllocateVirtualMemory(&base, &size_bytes, MEM_RESERVE, PAGE_READWRITE, 0);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("sbrk: NtAllocateVirtualMemory(reserve %u MB) failed: 0x%08X", (unsigned)(size_bytes >> 20),
			(unsigned)status);
		return status;
	}
	xsdk_log_write("sbrk: the heap may grow to %u MB from 0x%08X, the title's free memory",
		(unsigned)(size_bytes >> 20), (unsigned)(uintptr_t)base);
	heap_base = (uintptr_t)base;
	heap_end = heap_base + size_bytes;
	heap_break = heap_base;
	heap_committed = heap_base;
	return status;
}

static NTSTATUS commit_heap(uintptr_t end)
{
	void *base = (void *)heap_committed;
	uint32_t size_bytes = (end - heap_committed + HEAP_COMMIT_BYTES - 1) & ~(uint32_t)(HEAP_COMMIT_BYTES - 1);
	NTSTATUS status = NtAllocateVirtualMemory(&base, &size_bytes, MEM_COMMIT, PAGE_READWRITE, 0);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("sbrk: NtAllocateVirtualMemory(commit) failed: 0x%08X", (unsigned)status);
		return status;
	}
	heap_committed += size_bytes;
	return status;
}

void *sbrk(ptrdiff_t increment)
{
	if (!heap_base && !NT_SUCCESS(reserve_heap())) {
		errno = ENOMEM;
		return (void *)-1;
	}
	if (increment < 0 && (uintptr_t)-increment > heap_break - heap_base) {
		errno = EINVAL;
		return (void *)-1;
	}
	if (increment > 0 && (uintptr_t)increment > heap_end - heap_break) {
		errno = ENOMEM;
		return (void *)-1;
	}
	uintptr_t new_break = heap_break + increment;
	if (new_break > heap_committed && !NT_SUCCESS(commit_heap(new_break))) {
		errno = ENOMEM;
		return (void *)-1;
	}
	void *old_break = (void *)heap_break;
	heap_break = new_break;
	return old_break;
}
