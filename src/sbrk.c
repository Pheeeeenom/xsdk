#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"

// newlib's malloc grows its heap through sbrk; this one reserves address space once and commits it as it grows.
enum {
	HEAP_RESERVE_BYTES = 256 * 1024 * 1024,
	HEAP_COMMIT_BYTES = 64 * 1024,
};

void *sbrk(ptrdiff_t increment);

static uintptr_t heap_base;
static uintptr_t heap_break;
static uintptr_t heap_committed;

static NTSTATUS reserve_heap(void)
{
	void *base = NULL;
	uint32_t size_bytes = HEAP_RESERVE_BYTES;
	NTSTATUS status = NtAllocateVirtualMemory(&base, &size_bytes, MEM_RESERVE, PAGE_READWRITE, 0);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("sbrk: NtAllocateVirtualMemory(reserve) failed: 0x%08X", (unsigned)status);
		return status;
	}
	heap_base = (uintptr_t)base;
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
	if (increment > 0 && (uintptr_t)increment > heap_base + HEAP_RESERVE_BYTES - heap_break) {
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
