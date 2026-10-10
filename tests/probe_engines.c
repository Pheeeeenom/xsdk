#include <stdint.h>
#include <time.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// Milestone 4 by SCOPE.md section 3.2's option 2, through the call that takes the GPU from XAM: D3D titles call
// VdInitializeEngines, which has XAM's D3D device give up the GPU (docs/HARDWARE.md). This calls it with no microcode,
// to see whether the kernel hands the GPU over and keeps the microcode already running. It leaves XAM's interrupt
// callback alone, sets the ring up with the kernel's calls, submits libxenon's scratch fence with a MEM_WRITE and then
// an EVENT_WRITE_SHD fence, and calls VdShutdownEngines before exiting. It logs after every step, since the kernel may
// fault without microcode.

enum {
	RING_BYTES = 0x8000,
	// log2 of RING_BYTES in 8-byte units (libxenon:drivers/xenos/xe.c:21, 366).
	RING_SIZE_LOG2 = 12,
	// libxenon's read pointer write-back block size (xe.c:981).
	RPTR_BLOCK_SIZE_LOG2 = 6,
	PAGE_BYTES = 0x10000,
	FENCE_TIMEOUT_MS = 1000,
	// XAM submits every vblank (docs/HARDWARE.md), so a quarter of a second shows whether it still does.
	WATCH_MS = 250,
	// What D3D passes first (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_video.cc:196).
	ENGINES_FLAGS = 0x4F810000,
	POISON = 0xDEADBEEF,
	MEM_WRITE_VALUE = 0x4D454D57,
	SCRATCH_VALUE = 0x53435230,
	EVENT_VALUE = 0x45564E54,
};

// Register byte offsets, written through the ring as libxenon's Xe_pWriteReg does (xe.c:28-38); the names are Xenia's
// (xenia:src/xenia/gpu/register_table.inc:39-65).
enum {
	SCRATCH_UMSK = 0x0770,
	SCRATCH_ADDR = 0x0774,
	SCRATCH_REG0 = 0x15E0,
	WAIT_UNTIL = 0x1720,
	// libxenon's values: write back scratch registers 0, 1, 4 and 5 (xe.c:986), and wait for idle (xe.c:1223-1226).
	SCRATCH_UMSK_VALUE = 0x20033,
	WAIT_UNTIL_IDLE = 0x20000,
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
	SCRATCH_SLOT = 0x100,
};

// Volatile, so each put is one 32-bit store: GCC merges plain ones into 64-bit stores, and one at an address that isn't
// 8-byte aligned faults in write-combined memory (docs/HARDWARE.md).
typedef struct {
	volatile uint32_t *ring;
	uint32_t write_index;
} Ring;

// GPU registers as titles see them (xenia:src/xenia/gpu/graphics_system.cc:92-95); offsets are in bytes, as libxenon's.
static volatile uint32_t *const gpu_registers = (volatile uint32_t *)0x7FC80000;

static volatile uint32_t engines_callback_calls;

static uint32_t read_register(uint32_t offset)
{
	return gpu_registers[offset / 4];
}

static void write_register(uint32_t offset, uint32_t value)
{
	gpu_registers[offset / 4] = value;
}

// Its signature isn't known, so it takes nothing and only counts.
static void count_engines_callback(void)
{
	engines_callback_calls++;
}

static void put(Ring *ring, uint32_t value)
{
	ring->ring[ring->write_index++] = value;
}

static void put_register(Ring *ring, uint32_t register_offset, uint32_t value)
{
	put(ring, register_offset >> 2);
	put(ring, value);
}

static void put_type3(Ring *ring, uint32_t opcode, uint32_t payload_dwords)
{
	put(ring, 3u << 30 | (payload_dwords - 1) << 16 | opcode << 8);
}

// The ring is write-combined: drain the CPU's writes before the command processor may read them (libxenon:
// xe.c:97-106 syncs before the write pointer).
static void kick(const Ring *ring)
{
	__asm__ volatile("sync" ::: "memory");
	write_register(0x0714, ring->write_index);
}

// D3D's GPU hang report names 0x1740 RBBM_STATUS and 0x0700-0x0714 CP_RB_BASE to CP_RB_WPTR (docs/HARDWARE.md).
static void report_state(const char *when)
{
	xsdk_log_write("engines: %s: 0x07D8 %08X (bit 28: halted), RBBM_STATUS %08X (bit 31: busy)", when,
		(unsigned)read_register(0x07D8), (unsigned)read_register(0x1740));
	xsdk_log_write("engines: %s: base %08X, control %08X, read pointer write-back %08X, read %08X, write %08X", when,
		(unsigned)read_register(0x0700), (unsigned)read_register(0x0704), (unsigned)read_register(0x070C),
		(unsigned)read_register(0x0710), (unsigned)read_register(0x0714));
}

static void report_devices(const char *when)
{
	xsdk_log_write("engines: %s: VdGlobalDevice holds %08X, VdGlobalXamDevice holds %08X", when,
		(unsigned)*VdGlobalDevice, (unsigned)*VdGlobalXamDevice);
}

static uint32_t elapsed_ms(const struct timespec *start)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)((now.tv_sec - start->tv_sec) * 1000 + (now.tv_nsec - start->tv_nsec) / 1000000);
}

// Returns how often the write pointer moved: XAM's device moves it every vblank while it runs.
static uint32_t count_write_pointer_moves(void)
{
	struct timespec start;
	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	uint32_t last = read_register(0x0714);
	uint32_t moves = 0;
	while (elapsed_ms(&start) < WATCH_MS) {
		const uint32_t write = read_register(0x0714);
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

static void submit_fences(volatile uint32_t *ring_memory, volatile uint32_t *writeback, uint32_t writeback_physical)
{
	// Append after whatever the kernel left in the ring.
	Ring ring = { .ring = ring_memory, .write_index = read_register(0x0714) };
	xsdk_log_write("engines: phase A: appending at dword %u", (unsigned)ring.write_index);
	put_register(&ring, SCRATCH_ADDR, writeback_physical + SCRATCH_SLOT);
	put_register(&ring, SCRATCH_UMSK, SCRATCH_UMSK_VALUE);
	put_type3(&ring, PM4_NOP, 1);
	put(&ring, 0);
	put_type3(&ring, PM4_MEM_WRITE, 2);
	put(&ring, (writeback_physical + MEM_WRITE_SLOT) | SWAP_8IN32);
	put(&ring, MEM_WRITE_VALUE);
	put_register(&ring, SCRATCH_REG0, SCRATCH_VALUE);
	put_register(&ring, WAIT_UNTIL, WAIT_UNTIL_IDLE);
	kick(&ring);
	uint32_t waited_ms = wait_for_write(&writeback[SCRATCH_SLOT / 4]);
	const uint32_t mem_write_seen = writeback[MEM_WRITE_SLOT / 4];
	const uint32_t scratch_seen = writeback[SCRATCH_SLOT / 4];
	xsdk_log_write("engines: phase A after %u ms: MEM_WRITE %08X (%s), scratch %08X (%s), read pointer write-back %08X",
		(unsigned)waited_ms, (unsigned)mem_write_seen, describe(mem_write_seen, MEM_WRITE_VALUE),
		(unsigned)scratch_seen, describe(scratch_seen, SCRATCH_VALUE), (unsigned)writeback[RPTR_SLOT / 4]);
	report_state("after phase A");

	put_type3(&ring, PM4_EVENT_WRITE_SHD, 3);
	put(&ring, CACHE_FLUSH_AND_INV_TS_EVENT);
	put(&ring, (writeback_physical + EVENT_SLOT) | SWAP_8IN32);
	put(&ring, EVENT_VALUE);
	xsdk_log_write("engines: phase B: submitting up to dword %u", (unsigned)ring.write_index);
	kick(&ring);
	waited_ms = wait_for_write(&writeback[EVENT_SLOT / 4]);
	const uint32_t event_seen = writeback[EVENT_SLOT / 4];
	xsdk_log_write("engines: phase B after %u ms: EVENT_WRITE_SHD %08X (%s), read pointer write-back %08X",
		(unsigned)waited_ms, (unsigned)event_seen, describe(event_seen, EVENT_VALUE),
		(unsigned)writeback[RPTR_SLOT / 4]);
	report_state("after phase B");
}

int main(void)
{
	xsdk_log_write("engines probe");
	report_state("at start");
	report_devices("at start");

	// The ring is write-combined, as titles allocate it (xboxkrnl_video.cc:235-238); the write-back page is uncached,
	// so the CPU sees what the GPU writes.
	volatile uint32_t *const ring_memory = MmAllocatePhysicalMemoryEx(
		0, RING_BYTES, PAGE_READWRITE | PAGE_WRITECOMBINE | MEM_LARGE_PAGES, 0, UINT32_MAX, PAGE_BYTES);
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
	writeback[MEM_WRITE_SLOT / 4] = POISON;
	writeback[EVENT_SLOT / 4] = POISON;
	writeback[SCRATCH_SLOT / 4] = POISON;
	__asm__ volatile("sync" ::: "memory");

	xsdk_log_write("engines: XAM's device moved the write pointer %u times in %u ms",
		(unsigned)count_write_pointer_moves(), (unsigned)WATCH_MS);
	xsdk_log_write("engines: calling VdInitializeEngines with no microcode");
	const uint32_t result = VdInitializeEngines(ENGINES_FLAGS, (void *)count_engines_callback, 0, 0, 0);
	xsdk_log_write("engines: VdInitializeEngines returned %08X; its callback ran %u times", (unsigned)result,
		(unsigned)engines_callback_calls);
	report_state("after VdInitializeEngines");
	report_devices("after VdInitializeEngines");
	const uint32_t moves = count_write_pointer_moves();
	xsdk_log_write("engines: the write pointer moved %u times in %u ms after VdInitializeEngines", (unsigned)moves,
		(unsigned)WATCH_MS);

	if (moves == 0) {
		VdInitializeRingBuffer(ring_physical, RING_SIZE_LOG2);
		VdEnableRingBufferRPtrWriteBack(writeback_physical + RPTR_SLOT, RPTR_BLOCK_SIZE_LOG2);
		report_state("after the kernel's ring set-up");
		submit_fences(ring_memory, writeback, writeback_physical);
	} else {
		xsdk_log_write("engines: XAM's device is still submitting; not using the ring");
	}

	xsdk_log_write("engines: calling VdShutdownEngines");
	VdShutdownEngines();
	report_state("after VdShutdownEngines");
	report_devices("after VdShutdownEngines");
	xsdk_log_write(
		"engines: the write pointer moved %u times in %u ms after VdShutdownEngines; the callback ran %u times",
		(unsigned)count_write_pointer_moves(), (unsigned)WATCH_MS, (unsigned)engines_callback_calls);
	return 0;
}
