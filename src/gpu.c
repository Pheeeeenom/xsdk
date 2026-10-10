#include "xsdk/gpu.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "gpu_private.h"
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
	SET_UP_TIMEOUT_MS = 1000,
	// A NOP with one dword after it, the smallest packet that can fill the end of the ring.
	FILLER_DWORDS = 2,
	RECTANGLE_FLOATS = 6,
	// Resolve sizes kept at once, so a frame resolving at a few sizes doesn't wait for the GPU.
	RECTANGLE_SLOTS = 8,
	RECTANGLE_STRIDE = 0x20,
};

// Byte offsets in the write-back page.
enum {
	RPTR_OFFSET = 0x00,
	FENCE_OFFSET = 0x20,
	RECTANGLE_OFFSET = 0x40,
};

enum {
	// The cache-flush events wedge the GPU before render state is set; this one lands (docs/HARDWARE.md).
	PS_DONE_TS = 3,
	// An address's low bits pick a byte swap; 2 swaps each dword's bytes (xenia:src/xenia/gpu/xenos.h:193-197).
	SWAP_8IN32 = 2,
};

// Volatile, so every store stays 32 bits: a merged 64-bit one can fault here (docs/HARDWARE.md).
static volatile uint32_t *ring;
static volatile uint32_t *writeback;
static uint32_t writeback_physical;
static uint32_t write_index;
static uint32_t last_fence;

typedef struct {
	uint32_t width;
	uint32_t height;
	// The fence after which no queued resolve reads the corners.
	uint32_t fence;
} Rectangle;

static Rectangle rectangles[RECTANGLE_SLOTS];

// VdShutdownEngines calls it (docs/HARDWARE.md); the SDK has nothing to clean up there.
static void engines_shut_down(void)
{
}

static volatile uint32_t *allocate(uint32_t size_bytes, uint32_t protect)
{
	const uint32_t rounded = (size_bytes + PAGE_BYTES - 1) & ~(PAGE_BYTES - 1);
	volatile uint32_t *const memory =
		MmAllocatePhysicalMemoryEx(0, rounded, PAGE_READWRITE | protect | MEM_LARGE_PAGES, 0, UINT32_MAX, PAGE_BYTES);
	if (!memory)
		xsdk_log_write("gpu: MmAllocatePhysicalMemoryEx(%u bytes) failed", (unsigned)rounded);
	return memory;
}

static void free_memory(void)
{
	xsdk_gpu_free(writeback);
	xsdk_gpu_free(ring);
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

static uint32_t float_bits(float value)
{
	uint32_t bits;
	memcpy(&bits, &value, sizeof bits);
	return bits;
}

volatile uint32_t *xsdk_gpu_alloc(uint32_t size_bytes)
{
	return allocate(size_bytes, PAGE_WRITECOMBINE);
}

void xsdk_gpu_free(volatile uint32_t *memory)
{
	if (memory)
		MmFreePhysicalMemory(0, (void *)memory);
}

uint32_t xsdk_gpu_address(volatile const uint32_t *memory)
{
	return MmGetPhysicalAddress((const void *)memory);
}

NTSTATUS xsdk_gpu_open(void)
{
	assert(!ring);
	// Write-combined, as titles allocate the ring (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_video.cc:235-238).
	ring = allocate(RING_BYTES, PAGE_WRITECOMBINE);
	// Uncached, so the CPU sees what the GPU writes back.
	writeback = allocate(PAGE_BYTES, PAGE_NOCACHE);
	if (!ring || !writeback) {
		free_memory();
		return STATUS_NO_MEMORY;
	}
	writeback_physical = MmGetPhysicalAddress((const void *)writeback);
	writeback[RPTR_OFFSET / 4] = 0;
	writeback[FENCE_OFFSET / 4] = 0;
	write_index = 0;
	last_fence = 0;
	memset(rectangles, 0, sizeof rectangles);

	// No microcode: the kernel keeps the command processor's own (docs/HARDWARE.md).
	const uint32_t engines = VdInitializeEngines(ENGINES_FLAGS, (void *)engines_shut_down, NULL, NULL, NULL);
	if (!engines) {
		xsdk_log_write("gpu: VdInitializeEngines failed: 0x%08X", (unsigned)engines);
		free_memory();
		return STATUS_UNSUCCESSFUL;
	}
	VdInitializeRingBuffer(MmGetPhysicalAddress((const void *)ring), RING_SIZE_LOG2);
	VdEnableRingBufferRPtrWriteBack(writeback_physical + RPTR_OFFSET, RPTR_BLOCK_SIZE_LOG2);

	// Without libxenon's set-up, only every third resolve writes (docs/HARDWARE.md).
	xsdk_gpu_set_up();
	const NTSTATUS status = xsdk_gpu_wait(xsdk_gpu_fence(), SET_UP_TIMEOUT_MS);
	if (!NT_SUCCESS(status))
		xsdk_log_write("gpu: the set-up didn't finish: 0x%08X", (unsigned)status);
	return status;
}

NTSTATUS xsdk_gpu_close(void)
{
	if (!ring)
		return STATUS_SUCCESS;
	xsdk_gpu_put_back();
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

void xsdk_gpu_write(const GpuPackets *packets)
{
	volatile uint32_t *const space = xsdk_gpu_reserve(packets->count);
	for (uint32_t i = 0; i < packets->count; i++)
		space[i] = packets->dwords[i];
}

static bool fence_before(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b) < 0;
}

// The slot for this size, else the one used longest ago, after the GPU is done with it.
static uint32_t rectangle_slot(uint32_t width, uint32_t height)
{
	uint32_t oldest = 0;
	for (uint32_t i = 0; i < RECTANGLE_SLOTS; i++) {
		if (rectangles[i].width == width && rectangles[i].height == height)
			return i;
		if (fence_before(rectangles[i].fence, rectangles[oldest].fence))
			oldest = i;
	}
	Rectangle *const rectangle = &rectangles[oldest];
	if (rectangle->width) {
		if (fence_before(last_fence, rectangle->fence))
			(void)xsdk_gpu_fence();
		(void)xsdk_gpu_wait(rectangle->fence, CLOSE_TIMEOUT_MS);
	}
	// libxenon's three corners, half a pixel up and left (libxenon:drivers/xenos/xe.c:1074-1081).
	const float corners[RECTANGLE_FLOATS] = { -0.5f, -0.5f, width - 0.5f, 0, width - 0.5f, height - 0.5f };
	for (uint32_t i = 0; i < RECTANGLE_FLOATS; i++)
		writeback[(RECTANGLE_OFFSET + oldest * RECTANGLE_STRIDE) / 4 + i] = float_bits(corners[i]);
	rectangle->width = width;
	rectangle->height = height;
	return oldest;
}

uint32_t xsdk_gpu_rectangle(uint32_t width, uint32_t height)
{
	const uint32_t slot = rectangle_slot(width, height);
	// The resolve about to use it is queued before the next fence.
	rectangles[slot].fence = last_fence + 1;
	return writeback_physical + RECTANGLE_OFFSET + slot * RECTANGLE_STRIDE;
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

bool xsdk_gpu_fence_done(uint32_t fence)
{
	return (int32_t)(writeback[FENCE_OFFSET / 4] - fence) >= 0;
}

NTSTATUS xsdk_gpu_wait(uint32_t fence, uint32_t timeout_ms)
{
	for (uint32_t waited_ms = 0;; waited_ms++) {
		if (xsdk_gpu_fence_done(fence))
			return STATUS_SUCCESS;
		if (waited_ms == timeout_ms)
			return STATUS_IO_TIMEOUT;
		(void)xsdk_thread_sleep(1);
	}
}
