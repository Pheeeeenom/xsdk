#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// Milestone 4 by SCOPE.md section 3.2's option 2: drive the command processor as the dashboard leaves it, without
// VdInitializeEngines. Sets up a ring, submits a NOP and two fences, and logs whether the fences land in memory.

enum {
	RING_BYTES = 0x8000,
	// log2 of RING_BYTES in 8-byte units, as VdInitializeRingBuffer counts (libxenon:drivers/xenos/xe.c:21, 366).
	RING_SIZE_LOG2 = 12,
	// What titles pass (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_video.cc:244-248).
	RPTR_BLOCK_SIZE_LOG2 = 6,
	PAGE_BYTES = 0x10000,
	FENCE_TIMEOUT_MS = 1000,
	POISON = 0xDEADBEEF,
	MEM_WRITE_VALUE = 0x4D454D57,
	EVENT_VALUE = 0x45564E54,
};

// Type-3 packets and events (xenia:src/xenia/gpu/xenos.h:1541-1622, 1657-1663).
enum {
	PM4_NOP = 0x10,
	PM4_MEM_WRITE = 0x3D,
	PM4_EVENT_WRITE_SHD = 0x58,
	CACHE_FLUSH_AND_INV_TS_EVENT = 20,
	// A packet address's low bits pick a byte swap; 2 swaps the bytes of each dword (xenos.h:193-197).
	SWAP_8IN32 = 2,
};

// Byte offsets of the write-back page's slots.
enum {
	RPTR_SLOT = 0x00,
	MEM_WRITE_SLOT = 0x10,
	EVENT_SLOT = 0x20,
};

// GPU registers as titles see them (xenia:src/xenia/gpu/graphics_system.cc:92-95); offsets are in bytes, as libxenon's.
static volatile uint32_t *const gpu_registers = (volatile uint32_t *)0x7FC80000;

static uint32_t read_register(uint32_t offset)
{
	return gpu_registers[offset / 4];
}

static void write_register(uint32_t offset, uint32_t value)
{
	gpu_registers[offset / 4] = value;
}

static uint32_t type3(uint32_t opcode, uint32_t payload_dwords)
{
	return 3u << 30 | (payload_dwords - 1) << 16 | opcode << 8;
}

static uint32_t byte_swapped(uint32_t value)
{
	return __builtin_bswap32(value);
}

// What each register means comes from how libxenon uses it (drivers/xenos/xe.c:104, 210-221, 258, 305-330, 389-395).
static void report_state(const char *when)
{
	xsdk_log_write("ring: %s: 0x07D8 %08X (bit 28: halted), 0x1740 %08X (bit 31: busy)", when,
		(unsigned)read_register(0x07D8), (unsigned)read_register(0x1740));
	xsdk_log_write("ring: %s: base %08X, control %08X, read pointer write-back %08X, read %08X, write %08X", when,
		(unsigned)read_register(0x0700), (unsigned)read_register(0x0704), (unsigned)read_register(0x070C),
		(unsigned)read_register(0x0710), (unsigned)read_register(0x0714));
}

static const char *describe(uint32_t seen, uint32_t expected)
{
	if (seen == expected)
		return "landed";
	if (seen == byte_swapped(expected))
		return "landed byte-swapped";
	if (seen == POISON)
		return "not written";
	return "written with something else";
}

int main(void)
{
	xsdk_log_write("ring probe");
	report_state("at start");

	// The ring is write-combined, as titles allocate it (xboxkrnl_video.cc:235-238); the write-back page is uncached,
	// so the CPU sees what the GPU writes.
	uint32_t *const ring = MmAllocatePhysicalMemoryEx(
		0, RING_BYTES, PAGE_READWRITE | PAGE_WRITECOMBINE | MEM_LARGE_PAGES, 0, UINT32_MAX, PAGE_BYTES);
	volatile uint32_t *const writeback =
		MmAllocatePhysicalMemoryEx(0, PAGE_BYTES, PAGE_READWRITE | PAGE_NOCACHE | MEM_LARGE_PAGES, 0, UINT32_MAX, 0);
	if (!ring || !writeback) {
		xsdk_log_write("ring: MmAllocatePhysicalMemoryEx failed");
		return 1;
	}
	const uint32_t ring_physical = MmGetPhysicalAddress(ring);
	const uint32_t writeback_physical = MmGetPhysicalAddress((const void *)writeback);
	xsdk_log_write("ring: ring at %p (physical %08X), write-back page at %p (physical %08X)", (void *)ring,
		(unsigned)ring_physical, (void *)writeback, (unsigned)writeback_physical);
	memset(ring, 0, RING_BYTES);
	writeback[RPTR_SLOT / 4] = POISON;
	writeback[MEM_WRITE_SLOT / 4] = POISON;
	writeback[EVENT_SLOT / 4] = POISON;

	xsdk_log_write("ring: VdInitializeRingBuffer");
	VdInitializeRingBuffer(ring_physical, RING_SIZE_LOG2);
	xsdk_log_write("ring: VdEnableRingBufferRPtrWriteBack");
	VdEnableRingBufferRPtrWriteBack(writeback_physical + RPTR_SLOT, RPTR_BLOCK_SIZE_LOG2);
	report_state("after set-up");

	// VdInitializeRingBuffer leaves the write pointer past packets of its own (docs/HARDWARE.md); append after them.
	const uint32_t start = read_register(0x0714);
	uint32_t kernel_nonzero = 0;
	for (uint32_t i = 0; i < start && i < RING_BYTES / 4; i++)
		kernel_nonzero += ring[i] != 0;
	xsdk_log_write("ring: the kernel left the write pointer at %u; %u of the dwords before it are non-zero",
		(unsigned)start, (unsigned)kernel_nonzero);

	uint32_t end = start;
	ring[end++] = type3(PM4_NOP, 1);
	ring[end++] = 0;
	ring[end++] = type3(PM4_MEM_WRITE, 2);
	ring[end++] = (writeback_physical + MEM_WRITE_SLOT) | SWAP_8IN32;
	ring[end++] = MEM_WRITE_VALUE;
	ring[end++] = type3(PM4_EVENT_WRITE_SHD, 3);
	ring[end++] = CACHE_FLUSH_AND_INV_TS_EVENT;
	ring[end++] = (writeback_physical + EVENT_SLOT) | SWAP_8IN32;
	ring[end++] = EVENT_VALUE;
	// Drain the write-combine buffers before the command processor may read the ring, as libxenon syncs (xe.c:97-106).
	__asm__ volatile("sync" ::: "memory");
	xsdk_log_write("ring: submitting dwords %u to %u", (unsigned)start, (unsigned)end - 1);
	write_register(0x0714, end);

	bool landed = false;
	uint32_t waited_ms = 0;
	while (!landed && waited_ms < FENCE_TIMEOUT_MS) {
		(void)xsdk_thread_sleep(1);
		waited_ms++;
		landed = writeback[EVENT_SLOT / 4] != POISON;
	}
	const uint32_t mem_write_seen = writeback[MEM_WRITE_SLOT / 4];
	const uint32_t event_seen = writeback[EVENT_SLOT / 4];
	xsdk_log_write("ring: after %u ms: MEM_WRITE %08X (%s), EVENT_WRITE_SHD %08X (%s), read pointer write-back %08X",
		(unsigned)waited_ms, (unsigned)mem_write_seen, describe(mem_write_seen, MEM_WRITE_VALUE), (unsigned)event_seen,
		describe(event_seen, EVENT_VALUE), (unsigned)writeback[RPTR_SLOT / 4]);
	report_state("at end");
	return 0;
}
