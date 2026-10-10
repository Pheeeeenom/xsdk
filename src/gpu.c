#include "xsdk/gpu.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

enum {
	RING_BYTES = 0x8000,
	RING_DWORDS = RING_BYTES / 4,
	// log2 of RING_BYTES in 8-byte units (libxenon:drivers/xenos/xe.c:21, 366).
	RING_SIZE_LOG2 = 12,
	// libxenon's read pointer write-back block size (xe.c:981).
	RPTR_BLOCK_SIZE_LOG2 = 6,
	PAGE_BYTES = 0x10000,
	// What D3D passes first (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_video.cc:196).
	ENGINES_FLAGS = 0x4F810000,
	CLOSE_TIMEOUT_MS = 1000,
	// A NOP with one dword after it, the smallest packet that can fill the end of the ring.
	FILLER_DWORDS = 2,
};

// Byte offsets in the write-back page.
enum {
	RPTR_OFFSET = 0x00,
	FENCE_OFFSET = 0x20,
};

enum {
	PM4_EVENT_WRITE_SHD = 0x58,
	// The cache-flush events wedge the GPU before render state is set; this one lands (docs/HARDWARE.md).
	PS_DONE_TS = 3,
	// An address's low bits pick a byte swap; 2 swaps each dword's bytes (xenia:src/xenia/gpu/xenos.h:193-197).
	SWAP_8IN32 = 2,
	// Register index (xenia:src/xenia/gpu/graphics_system.cc:208-213).
	CP_RB_WPTR = 0x01C5,
};

// GPU registers as titles see them (xenia:src/xenia/gpu/graphics_system.cc:92-95).
static volatile uint32_t *const gpu_registers = (volatile uint32_t *)0x7FC80000;

// Volatile, so every store stays 32 bits: a merged 64-bit one can fault here (docs/HARDWARE.md).
static volatile uint32_t *ring;
static volatile uint32_t *writeback;
static uint32_t writeback_physical;
static uint32_t write_index;
static uint32_t last_fence;

// VdShutdownEngines calls it (docs/HARDWARE.md); the SDK has nothing to clean up there.
static void engines_shut_down(void)
{
}

static void free_memory(void)
{
	if (writeback)
		MmFreePhysicalMemory(0, (void *)writeback);
	if (ring)
		MmFreePhysicalMemory(0, (void *)ring);
	writeback = NULL;
	ring = NULL;
}

// Dwords the title can write before it reaches what the GPU hasn't read.
static uint32_t free_dwords(void)
{
	return (writeback[RPTR_OFFSET / 4] - write_index - 1) & (RING_DWORDS - 1);
}

static void kick(void)
{
	// Drain the write-combined stores before the GPU reads them (libxenon:drivers/xenos/xe.c:97-106).
	__asm__ volatile("sync" ::: "memory");
	gpu_registers[CP_RB_WPTR] = write_index;
}

static void wait_for_room(uint32_t dword_count)
{
	if (free_dwords() >= dword_count)
		return;
	kick();
	while (free_dwords() < dword_count)
		(void)xsdk_thread_sleep(1);
}

NTSTATUS xsdk_gpu_open(void)
{
	assert(!ring);
	// Write-combined, as titles allocate the ring (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_video.cc:235-238).
	ring = MmAllocatePhysicalMemoryEx(
		0, RING_BYTES, PAGE_READWRITE | PAGE_WRITECOMBINE | MEM_LARGE_PAGES, 0, UINT32_MAX, PAGE_BYTES);
	// Uncached, so the CPU sees what the GPU writes back.
	writeback =
		MmAllocatePhysicalMemoryEx(0, PAGE_BYTES, PAGE_READWRITE | PAGE_NOCACHE | MEM_LARGE_PAGES, 0, UINT32_MAX, 0);
	if (!ring || !writeback) {
		xsdk_log_write("gpu: MmAllocatePhysicalMemoryEx failed");
		free_memory();
		return STATUS_NO_MEMORY;
	}
	writeback_physical = MmGetPhysicalAddress((const void *)writeback);
	writeback[RPTR_OFFSET / 4] = 0;
	writeback[FENCE_OFFSET / 4] = 0;
	write_index = 0;
	last_fence = 0;

	// No microcode: the kernel keeps the command processor's own (docs/HARDWARE.md).
	const uint32_t engines = VdInitializeEngines(ENGINES_FLAGS, (void *)engines_shut_down, NULL, NULL, NULL);
	if (!engines) {
		xsdk_log_write("gpu: VdInitializeEngines failed: 0x%08X", (unsigned)engines);
		free_memory();
		return STATUS_UNSUCCESSFUL;
	}
	VdInitializeRingBuffer(MmGetPhysicalAddress((const void *)ring), RING_SIZE_LOG2);
	VdEnableRingBufferRPtrWriteBack(writeback_physical + RPTR_OFFSET, RPTR_BLOCK_SIZE_LOG2);
	return STATUS_SUCCESS;
}

NTSTATUS xsdk_gpu_close(void)
{
	if (!ring)
		return STATUS_SUCCESS;
	const NTSTATUS status = xsdk_gpu_wait(xsdk_gpu_fence(), CLOSE_TIMEOUT_MS);
	if (!NT_SUCCESS(status)) {
		// VdShutdownEngines never returns while the GPU is wedged, and the GPU may still read the ring.
		xsdk_log_write("gpu: the GPU didn't finish: 0x%08X; keeping it and its memory", (unsigned)status);
		ring = NULL;
		writeback = NULL;
		return status;
	}
	VdShutdownEngines();
	free_memory();
	return STATUS_SUCCESS;
}

volatile uint32_t *xsdk_gpu_reserve(uint32_t dword_count)
{
	assert(ring && dword_count <= RING_DWORDS - FILLER_DWORDS);
	// Keep room at the end for a NOP, so the ring can always wrap.
	if (write_index + dword_count > RING_DWORDS - FILLER_DWORDS) {
		const uint32_t filler_dwords = RING_DWORDS - write_index;
		wait_for_room(filler_dwords);
		ring[write_index] = xsdk_gpu_type3(PM4_NOP, filler_dwords - 1);
		write_index = 0;
	}
	wait_for_room(dword_count);
	volatile uint32_t *const packets = &ring[write_index];
	write_index += dword_count;
	return packets;
}

uint32_t xsdk_gpu_fence(void)
{
	const uint32_t fence = last_fence + 1;
	volatile uint32_t *const packet = xsdk_gpu_reserve(4);
	packet[0] = xsdk_gpu_type3(PM4_EVENT_WRITE_SHD, 3);
	packet[1] = PS_DONE_TS;
	packet[2] = (writeback_physical + FENCE_OFFSET) | SWAP_8IN32;
	packet[3] = fence;
	last_fence = fence;
	kick();
	return fence;
}

NTSTATUS xsdk_gpu_wait(uint32_t fence, uint32_t timeout_ms)
{
	for (uint32_t waited_ms = 0;; waited_ms++) {
		if ((int32_t)(writeback[FENCE_OFFSET / 4] - fence) >= 0)
			return STATUS_SUCCESS;
		if (waited_ms == timeout_ms)
			return STATUS_TIMEOUT;
		(void)xsdk_thread_sleep(1);
	}
}
