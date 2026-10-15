#include <stdbool.h>
#include <stdint.h>

#include "probe_gpu.h"
#include "tests.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"

enum {
	FENCE_TIMEOUT_MS = 1000,
	// Copies a register into memory (xenia:src/xenia/gpu/xenos.h:1588, command_processor.cc:1069-1090).
	PM4_REG_TO_MEM = 0x3E,
	SWAP_8IN32 = 2,
	// The performance counters' control and a counter (xenia:src/xenia/gpu/register_table.inc:45-52).
	CP_PERFMON_CNTL = 0x01F5,
	RBBM_PERFCOUNTER1_SELECT = 0x0396,
	RBBM_PERFCOUNTER1_LOW = 0x0399,
	// It counts every clock once they're on (Linux:drivers/gpu/drm/msm/registers/adreno/a2xx.xml:719-720, 1022-1025).
	RBBM1_COUNT = 0,
	PERF_STATE_RESET = 0,
	PERF_STATE_ENABLE = 1,
	// The GPU's 500 MHz, which the counter keeps against the CPU's clock (docs/HARDWARE.md).
	GPU_CLOCKS_PER_US = 500,
	NANOSECONDS_PER_MICROSECOND = 1000,
	// WAIT_UNTIL's wait_3d_idleclean (xenia:src/xenia/gpu/registers.h:110).
	WAIT_3D_IDLE_CLEAN = 1 << 17,
	RESOLVE_WIDTH = 1280,
	RESOLVE_HEIGHT = 720,
	CLEAR_ARGB = 0xFF202020,
	WAIT_ROUNDS = 8,
	WAIT_LATE_MAX_US = 100,
	// As long as a 1 ms sleep takes on the console (docs/HARDWARE.md), so a wait outlasting it gains nothing.
	WAIT_SPIN_US = 2000,
	// Each fence gives the ring a segment of 14 dwords, so the 8192-dword ring wraps a few times.
	WRAP_ROUNDS = 4000,
	// 25600 dwords with no fence, which the ring runs in segments: the first after a fence 1024 dwords, each next twice
	// the last, up to 8192 (src/gpu.c).
	LONG_WRITE_NOPS = 400,
	NOP_DWORDS = 64,
	SEGMENT_FIRST_DWORDS = 1024,
	SEGMENT_MAX_DWORDS = 8192,
	// 5 MB, so the 4 MB stream wraps, with a fence every 64 KB.
	STREAM_WRAP_NOPS = 20480,
	NOPS_A_FENCE = 256,
};

static void put_nop(uint32_t payload_dwords)
{
	volatile uint32_t *const packet = xsdk_gpu_reserve(1 + payload_dwords);
	packet[0] = xsdk_gpu_type3(PM4_NOP, payload_dwords);
	for (uint32_t i = 1; i <= payload_dwords; i++)
		packet[i] = 0;
}

// The segments a write of this many dwords right after a fence fills, so the ring runs them before the next fence.
static uint32_t whole_segments(uint32_t dwords)
{
	uint32_t segments = 0;
	uint32_t size = SEGMENT_FIRST_DWORDS;
	for (uint32_t end = size; end <= dwords; end += size) {
		segments++;
		size = size * 2 < SEGMENT_MAX_DWORDS ? size * 2 : SEGMENT_MAX_DWORDS;
	}
	return segments;
}

// A NOP of 4 dwords, then a fence that kicks once; then a long write with no fence, which still runs.
static void test_stats(void)
{
	XsdkGpuStats before;
	XsdkGpuStats after;
	xsdk_gpu_stats(&before);
	put_nop(3);
	xsdk_gpu_stats(&after);
	const bool counted_nop = after.dwords - before.dwords == 4 && after.kicks == before.kicks;
	(void)xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
	xsdk_gpu_stats(&before);
	check("gpu: stats count the dwords written and the fence's kick", counted_nop && before.kicks == after.kicks + 1);

	for (uint32_t i = 0; i < LONG_WRITE_NOPS; i++)
		put_nop(NOP_DWORDS - 1);
	xsdk_gpu_stats(&after);
	const uint32_t kicks = after.kicks - before.kicks;
	const bool ran = NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
	xsdk_log_write(
		"gpu: %u dwords with no fence: %u segments", (unsigned)(after.dwords - before.dwords), (unsigned)kicks);
	check("gpu: a long write runs in segments, before its fence",
		after.dwords - before.dwords >= LONG_WRITE_NOPS * NOP_DWORDS &&
			kicks >= whole_segments(LONG_WRITE_NOPS * NOP_DWORDS) / 2 && ran);
}

// More than the stream holds, so it wraps and reuses its blocks behind the GPU.
static void test_stream_wrap(void)
{
	XsdkGpuStats before;
	XsdkGpuStats after;
	xsdk_gpu_stats(&before);
	uint32_t fence = 0;
	for (uint32_t i = 1; i <= STREAM_WRAP_NOPS; i++) {
		put_nop(NOP_DWORDS - 1);
		if (i % NOPS_A_FENCE == 0)
			fence = xsdk_gpu_fence();
	}
	const bool ran = NT_SUCCESS(xsdk_gpu_wait(fence, FENCE_TIMEOUT_MS));
	xsdk_gpu_stats(&after);
	xsdk_log_write("gpu: %u dwords as the stream wraps: %u segments, %u waits, %u us waiting",
		(unsigned)(after.dwords - before.dwords), (unsigned)(after.kicks - before.kicks),
		(unsigned)(after.waits - before.waits), (unsigned)(after.waited_us - before.waited_us));
	check("gpu: fences as the stream wraps", ran && after.dwords - before.dwords >= STREAM_WRAP_NOPS * NOP_DWORDS);
}

static void put_register(uint32_t index, uint32_t value)
{
	volatile uint32_t *const packet = xsdk_gpu_reserve(2);
	packet[0] = index;
	packet[1] = value;
}

// Once the GPU is idle, the command processor copies the counter into memory.
static void put_clock_copy(uint32_t address)
{
	put_register(WAIT_UNTIL, WAIT_3D_IDLE_CLEAN);
	volatile uint32_t *const packet = xsdk_gpu_reserve(3);
	packet[0] = xsdk_gpu_type3(PM4_REG_TO_MEM, 2);
	packet[1] = RBBM_PERFCOUNTER1_LOW;
	packet[2] = address | SWAP_8IN32;
}

// A resolve between two copies of the GPU's clock, then a fence; the CPU reads the clock as the wait returns.
static bool wait_returns(volatile uint32_t *target, volatile uint32_t *clocks, uint32_t *late_clocks)
{
	clocks[0] = 0;
	clocks[1] = 0;
	__asm__ volatile("sync" ::: "memory");
	put_clock_copy(xsdk_gpu_address(clocks));
	xsdk_gpu_resolve(xsdk_gpu_address(target), RESOLVE_WIDTH, RESOLVE_HEIGHT, CLEAR_ARGB);
	put_clock_copy(xsdk_gpu_address(clocks) + 4);
	const NTSTATUS status = xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
	const uint32_t returned = probe_gpu_registers[RBBM_PERFCOUNTER1_LOW];
	const uint32_t job_clocks = clocks[1] - clocks[0];
	*late_clocks = returned - clocks[1];
	xsdk_log_write("gpu: the resolve took %u us by the GPU's clock, and the wait returned %llu ns after its fence",
		(unsigned)(job_clocks / GPU_CLOCKS_PER_US),
		(unsigned long long)*late_clocks * NANOSECONDS_PER_MICROSECOND / GPU_CLOCKS_PER_US);
	return NT_SUCCESS(status) && job_clocks > 0;
}

static void test_wait_latency(void)
{
	volatile uint32_t *const target = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(RESOLVE_WIDTH, RESOLVE_HEIGHT));
	volatile uint32_t *const clocks = xsdk_gpu_alloc(2 * sizeof(uint32_t));
	const uint32_t found_control = probe_gpu_registers[CP_PERFMON_CNTL];
	const uint32_t found_select = probe_gpu_registers[RBBM_PERFCOUNTER1_SELECT];
	xsdk_log_write("gpu: the counters' control was %08X and the counter's select %08X", (unsigned)found_control,
		(unsigned)found_select);
	put_register(CP_PERFMON_CNTL, PERF_STATE_RESET);
	put_register(RBBM_PERFCOUNTER1_SELECT, RBBM1_COUNT);
	put_register(CP_PERFMON_CNTL, PERF_STATE_ENABLE);
	bool timed = target && clocks;
	uint32_t latest_clocks = 0;
	xsdk_gpu_set_wait_spin(WAIT_SPIN_US);
	for (uint32_t i = 0; i < WAIT_ROUNDS && timed; i++) {
		uint32_t late_clocks = 0;
		timed = wait_returns(target, clocks, &late_clocks);
		latest_clocks = late_clocks > latest_clocks ? late_clocks : latest_clocks;
	}
	xsdk_gpu_set_wait_spin(0);
	put_register(RBBM_PERFCOUNTER1_SELECT, found_select);
	put_register(CP_PERFMON_CNTL, found_control);
	timed = NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS)) && timed;
	// Xenia's counter holds what was written (xenia:src/xenia/gpu/graphics_system.cc:181-205), so this fails there.
	check("gpu: a wait set to spin returns within 100 us of its fence passing, by the GPU's clock",
		timed && latest_clocks <= WAIT_LATE_MAX_US * GPU_CLOCKS_PER_US);
	xsdk_gpu_free(clocks);
	xsdk_gpu_free(target);
}

void test_gpu(void)
{
	const NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status))
		xsdk_log_write("gpu: xsdk_gpu_open returned 0x%08X", (unsigned)status);
	check("gpu: open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return;

	put_nop(1);
	check("gpu: NOP and fence", NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS)));
	test_stats();
	test_stream_wrap();
	test_wait_latency();

	// NOPs of different sizes, so the ring wraps at different offsets.
	uint32_t fence = 0;
	for (uint32_t i = 0; i < WRAP_ROUNDS; i++) {
		put_nop(1 + i % 4);
		fence = xsdk_gpu_fence();
	}
	check("gpu: fences as the ring wraps", NT_SUCCESS(xsdk_gpu_wait(fence, FENCE_TIMEOUT_MS)));
	check("gpu: close", NT_SUCCESS(xsdk_gpu_close()));
}
