#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "probe_gpu.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// Takes the GPU with VdInitializeEngines and no microcode, submits fences, and gives it back (docs/HARDWARE.md).

enum {
	RING_BYTES = 0x8000,
	// log2 of RING_BYTES in 8-byte units (libxenon:drivers/xenos/xe.c:21, 366).
	RING_SIZE_LOG2 = 12,
	// libxenon's read pointer write-back block size (xe.c:981).
	RPTR_BLOCK_SIZE_LOG2 = 6,
	PAGE_BYTES = 0x10000,
	FENCE_TIMEOUT_MS = 1000,
	FENCES = 3,
	// XAM submits every vblank (docs/HARDWARE.md), so a quarter of a second shows whether it still does.
	WATCH_MS = 250,
	// What D3D passes first (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_video.cc:196).
	ENGINES_FLAGS = 0x4F810000,
	POISON = 0xDEADBEEF,
	MEM_WRITE_VALUE = 0x4D454D57,
	SCRATCH_VALUE = 0x53435230,
	EVENT_VALUE = 0x45564E54,
};

// Registers are written through the ring as libxenon does (xe.c:28-38).
enum {
	// libxenon's values: write back scratch registers 0, 1, 4 and 5 (xe.c:986), and wait for idle (xe.c:1223-1226).
	SCRATCH_UMSK_VALUE = 0x20033,
	WAIT_UNTIL_IDLE = 0x20000,
};

// Type-3 packets (xenia:src/xenia/gpu/xenos.h:1572-1622, 1657-1663).
enum {
	PM4_MEM_WRITE = 0x3D,
	PM4_EVENT_WRITE_SHD = 0x58,
	// A packet address's low bits pick a byte swap; 2 swaps the bytes of each dword (xenos.h:193-197).
	SWAP_8IN32 = 2,
};

// Byte offsets of the write-back page's slots.
enum {
	RPTR_SLOT = 0x00,
	MEM_WRITE_SLOT = 0x10,
	EVENT_SLOT = 0x20,
	SCRATCH_SLOT = 0x100,
};

// Events for VGT_EVENT_INITIATOR (xenia:src/xenia/gpu/xenos.h:1541-1566).
enum {
	VS_DONE_TS = 2,
	PS_DONE_TS = 3,
	CACHE_FLUSH_TS = 4,
};

// Shader-done events first, since a cache-flush event wedged the GPU (docs/HARDWARE.md); stops at the first to fail.
static const uint32_t fence_events[] = { VS_DONE_TS, PS_DONE_TS, CACHE_FLUSH_TS };

// Volatile, so every put is one 32-bit store: a merged 64-bit one can fault here (docs/HARDWARE.md).
typedef struct {
	volatile uint32_t *ring;
	uint32_t write_index;
} Ring;

static volatile uint32_t engines_callback_calls;

// Its signature isn't known, so it takes nothing and only counts.
static void count_engines_callback(void)
{
	engines_callback_calls++;
}

static void put(Ring *ring, uint32_t value)
{
	ring->ring[ring->write_index++] = value;
}

static void put_register(Ring *ring, uint32_t index, uint32_t value)
{
	put(ring, index);
	put(ring, value);
}

static void put_type3(Ring *ring, uint32_t opcode, uint32_t payload_dwords)
{
	put(ring, xsdk_gpu_type3(opcode, payload_dwords));
}

// Drain the write-combined stores before the GPU reads them (libxenon:drivers/xenos/xe.c:97-106).
static void kick(const Ring *ring)
{
	__asm__ volatile("sync" ::: "memory");
	probe_gpu_registers[CP_RB_WPTR] = ring->write_index;
}

static void report_devices(const char *when)
{
	xsdk_log_write("engines: %s: VdGlobalDevice holds %08X, VdGlobalXamDevice holds %08X", when,
		(unsigned)*VdGlobalDevice, (unsigned)*VdGlobalXamDevice);
}

// Returns how often the write pointer moved: XAM's device moves it every vblank while it runs.
static uint32_t count_write_pointer_moves(void)
{
	struct timespec start;
	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	uint32_t last = probe_gpu_registers[CP_RB_WPTR];
	uint32_t moves = 0;
	while (probe_elapsed_ms(&start) < WATCH_MS) {
		const uint32_t write = probe_gpu_registers[CP_RB_WPTR];
		moves += write != last;
		last = write;
		(void)xsdk_thread_sleep(1);
	}
	return moves;
}

static const char *describe(uint32_t seen, uint32_t expected)
{
	if (seen == expected)
		return "landed";
	if (seen == __builtin_bswap32(expected))
		return "landed byte-swapped";
	if (seen == POISON)
		return "not written";
	return "written with something else";
}

static uint32_t wait_for_write(volatile const uint32_t *slot)
{
	uint32_t waited_ms = 0;
	while (*slot == POISON && waited_ms < FENCE_TIMEOUT_MS) {
		(void)xsdk_thread_sleep(1);
		waited_ms++;
	}
	return waited_ms;
}

// A NOP, a MEM_WRITE and libxenon's scratch fence, each with this fence's values (xe.c:985-988, 1218-1226).
static void submit_fence(Ring *ring, volatile uint32_t *writeback, uint32_t writeback_physical, uint32_t sequence)
{
	writeback[MEM_WRITE_SLOT / 4] = POISON;
	writeback[SCRATCH_SLOT / 4] = POISON;
	__asm__ volatile("sync" ::: "memory");
	const uint32_t mem_write_value = MEM_WRITE_VALUE + sequence;
	const uint32_t scratch_value = SCRATCH_VALUE + sequence;
	xsdk_log_write("engines: fence %u: appending at dword %u", (unsigned)sequence, (unsigned)ring->write_index);
	put_type3(ring, PM4_NOP, 1);
	put(ring, 0);
	put_type3(ring, PM4_MEM_WRITE, 2);
	put(ring, (writeback_physical + MEM_WRITE_SLOT) | SWAP_8IN32);
	put(ring, mem_write_value);
	put_register(ring, SCRATCH_REG0, scratch_value);
	put_register(ring, WAIT_UNTIL, WAIT_UNTIL_IDLE);
	kick(ring);
	const uint32_t waited_ms = wait_for_write(&writeback[SCRATCH_SLOT / 4]);
	const uint32_t mem_write_seen = writeback[MEM_WRITE_SLOT / 4];
	const uint32_t scratch_seen = writeback[SCRATCH_SLOT / 4];
	xsdk_log_write(
		"engines: fence %u after %u ms: MEM_WRITE %08X (%s), scratch %08X (%s), read pointer write-back %08X",
		(unsigned)sequence, (unsigned)waited_ms, (unsigned)mem_write_seen, describe(mem_write_seen, mem_write_value),
		(unsigned)scratch_seen, describe(scratch_seen, scratch_value), (unsigned)writeback[RPTR_SLOT / 4]);
}

// Returns whether the event's value landed.
static bool submit_event_fence(Ring *ring, volatile uint32_t *writeback, uint32_t writeback_physical, uint32_t event)
{
	writeback[EVENT_SLOT / 4] = POISON;
	__asm__ volatile("sync" ::: "memory");
	const uint32_t value = EVENT_VALUE + event;
	xsdk_log_write("engines: event %u: appending at dword %u", (unsigned)event, (unsigned)ring->write_index);
	put_type3(ring, PM4_EVENT_WRITE_SHD, 3);
	put(ring, event);
	put(ring, (writeback_physical + EVENT_SLOT) | SWAP_8IN32);
	put(ring, value);
	kick(ring);
	const uint32_t waited_ms = wait_for_write(&writeback[EVENT_SLOT / 4]);
	const uint32_t seen = writeback[EVENT_SLOT / 4];
	xsdk_log_write("engines: event %u after %u ms: EVENT_WRITE_SHD %08X (%s), read pointer write-back %08X",
		(unsigned)event, (unsigned)waited_ms, (unsigned)seen, describe(seen, value),
		(unsigned)writeback[RPTR_SLOT / 4]);
	return seen == value;
}

// Returns false when the GPU ended busy, wedged by a fence.
static bool submit_fences(volatile uint32_t *ring_memory, volatile uint32_t *writeback, uint32_t writeback_physical)
{
	// Append after whatever the kernel left in the ring.
	Ring ring = { .ring = ring_memory, .write_index = probe_gpu_registers[CP_RB_WPTR] };
	put_register(&ring, SCRATCH_ADDR, writeback_physical + SCRATCH_SLOT);
	put_register(&ring, SCRATCH_UMSK, SCRATCH_UMSK_VALUE);
	for (uint32_t sequence = 0; sequence < FENCES; sequence++)
		submit_fence(&ring, writeback, writeback_physical, sequence);
	probe_report_cp("engines", "after the scratch fences");
	for (uint32_t i = 0; i < sizeof fence_events / sizeof fence_events[0]; i++) {
		if (!submit_event_fence(&ring, writeback, writeback_physical, fence_events[i]))
			break;
	}
	probe_report_cp("engines", "after the event fences");
	return !(probe_gpu_registers[RBBM_STATUS] & GPU_BUSY);
}

int main(void)
{
	xsdk_log_write("engines probe");
	probe_report_cp("engines", "at start");
	report_devices("at start");

	// Write-combined, as titles allocate the ring (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_video.cc:235-238).
	volatile uint32_t *const ring_memory = MmAllocatePhysicalMemoryEx(
		0, RING_BYTES, PAGE_READWRITE | PAGE_WRITECOMBINE | MEM_LARGE_PAGES, 0, UINT32_MAX, PAGE_BYTES);
	// Uncached, so the CPU sees what the GPU writes back.
	volatile uint32_t *const writeback =
		MmAllocatePhysicalMemoryEx(0, PAGE_BYTES, PAGE_READWRITE | PAGE_NOCACHE | MEM_LARGE_PAGES, 0, UINT32_MAX, 0);
	if (!ring_memory || !writeback) {
		xsdk_log_write("engines: MmAllocatePhysicalMemoryEx failed");
		return 1;
	}
	const uint32_t ring_physical = MmGetPhysicalAddress((const void *)ring_memory);
	const uint32_t writeback_physical = MmGetPhysicalAddress((const void *)writeback);
	xsdk_log_write("engines: ring at %p (physical %08X), write-back page at %p (physical %08X)", (void *)ring_memory,
		(unsigned)ring_physical, (void *)writeback, (unsigned)writeback_physical);
	for (uint32_t i = 0; i < RING_BYTES / 4; i++)
		ring_memory[i] = 0;
	writeback[RPTR_SLOT / 4] = POISON;
	__asm__ volatile("sync" ::: "memory");

	xsdk_log_write("engines: XAM's device moved the write pointer %u times in %u ms",
		(unsigned)count_write_pointer_moves(), (unsigned)WATCH_MS);
	xsdk_log_write("engines: calling VdInitializeEngines with no microcode");
	const uint32_t result = VdInitializeEngines(ENGINES_FLAGS, (void *)count_engines_callback, 0, 0, 0);
	xsdk_log_write("engines: VdInitializeEngines returned %08X; its callback ran %u times", (unsigned)result,
		(unsigned)engines_callback_calls);
	probe_report_cp("engines", "after VdInitializeEngines");
	report_devices("after VdInitializeEngines");
	const uint32_t moves = count_write_pointer_moves();
	xsdk_log_write("engines: the write pointer moved %u times in %u ms after VdInitializeEngines", (unsigned)moves,
		(unsigned)WATCH_MS);

	bool idle = true;
	if (moves == 0) {
		VdInitializeRingBuffer(ring_physical, RING_SIZE_LOG2);
		VdEnableRingBufferRPtrWriteBack(writeback_physical + RPTR_SLOT, RPTR_BLOCK_SIZE_LOG2);
		probe_report_cp("engines", "after the kernel's ring set-up");
		idle = submit_fences(ring_memory, writeback, writeback_physical);
	} else {
		xsdk_log_write("engines: XAM's device is still submitting; not using the ring");
	}
	if (!idle) {
		xsdk_log_write("engines: the GPU is wedged; exiting without VdShutdownEngines, which would wait on it");
		return 1;
	}

	xsdk_log_write("engines: calling VdShutdownEngines");
	VdShutdownEngines();
	probe_report_cp("engines", "after VdShutdownEngines");
	report_devices("after VdShutdownEngines");
	xsdk_log_write(
		"engines: the write pointer moved %u times in %u ms after VdShutdownEngines; the callback ran %u times",
		(unsigned)count_write_pointer_moves(), (unsigned)WATCH_MS, (unsigned)engines_callback_calls);
	return 0;
}
