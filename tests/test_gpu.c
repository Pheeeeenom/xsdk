#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "tests.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"

enum {
	FENCE_TIMEOUT_MS = 1000,
	// The GPU's 500 MHz, near enough for the waits' latencies (docs/HARDWARE.md).
	GPU_CLOCKS_PER_US = 500,
	NANOSECONDS_PER_MICROSECOND = 1000,
	WORD_VALUE = 0x12345678,
	WORD_AFTER_WAIT = 0x00C0FFEE,
	WORD_HELD_MS = 20,
	CLOCK_SLEEP_MS = 100,
	// 498.75 a microsecond, ten times the time base's 49.875 MHz, within 0.1% (docs/HARDWARE.md, "Queries and
	// events"), in thousandths.
	CLOCK_RATE_MIN = 498250,
	CLOCK_RATE_MAX = 499250,
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

// A resolve between two copies of the GPU's clock, then a fence; the CPU reads the clock as the wait returns.
static bool wait_returns(volatile uint32_t *target, volatile uint32_t *clocks, uint64_t *late_clocks)
{
	for (uint32_t i = 0; i < 2 * XSDK_GPU_CLOCK_WORDS; i++)
		clocks[i] = 0;
	__asm__ volatile("sync" ::: "memory");
	xsdk_gpu_write_clock(xsdk_gpu_address(clocks), true);
	xsdk_gpu_resolve(xsdk_gpu_address(target), RESOLVE_WIDTH, RESOLVE_HEIGHT, CLEAR_ARGB);
	xsdk_gpu_write_clock(xsdk_gpu_address(clocks + XSDK_GPU_CLOCK_WORDS), true);
	const NTSTATUS status = xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
	const uint64_t returned = xsdk_gpu_clock();
	const uint64_t before = xsdk_gpu_clock_value(clocks);
	const uint64_t after = xsdk_gpu_clock_value(clocks + XSDK_GPU_CLOCK_WORDS);
	const uint64_t job_clocks = after - before;
	*late_clocks = returned - after;
	xsdk_log_write("gpu: the resolve took %u us by the GPU's clock, and the wait returned %llu ns after its fence",
		(unsigned)(job_clocks / GPU_CLOCKS_PER_US),
		(unsigned long long)*late_clocks * NANOSECONDS_PER_MICROSECOND / GPU_CLOCKS_PER_US);
	return NT_SUCCESS(status) && after > before && returned >= after;
}

static void test_wait_latency(void)
{
	volatile uint32_t *const target = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(RESOLVE_WIDTH, RESOLVE_HEIGHT));
	volatile uint32_t *const clocks = xsdk_gpu_alloc(2 * XSDK_GPU_CLOCK_WORDS * sizeof(uint32_t));
	bool timed = target && clocks;
	uint64_t latest_clocks = 0;
	xsdk_gpu_set_wait_spin(WAIT_SPIN_US);
	for (uint32_t i = 0; i < WAIT_ROUNDS && timed; i++) {
		uint64_t late_clocks = 0;
		timed = wait_returns(target, clocks, &late_clocks);
		latest_clocks = late_clocks > latest_clocks ? late_clocks : latest_clocks;
	}
	xsdk_gpu_set_wait_spin(0);
	// Xenia's counter holds what was written (xenia:src/xenia/gpu/graphics_system.cc:181-205), so this fails there.
	check("gpu: a wait set to spin returns within 100 us of its fence passing, by the GPU's clock",
		timed && latest_clocks <= WAIT_LATE_MAX_US * GPU_CLOCKS_PER_US);
	xsdk_gpu_free(clocks);
	xsdk_gpu_free(target);
}

static uint64_t now_us(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
}

static void sleep_ms(uint32_t ms)
{
	const struct timespec request = { .tv_sec = 0, .tv_nsec = (long)ms * 1000000 };
	(void)nanosleep(&request, NULL);
}

// A word the GPU writes once a resolve is done, and a wait on a word that holds the GPU until the CPU writes it.
static void test_words(void)
{
	volatile uint32_t *const target = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(RESOLVE_WIDTH, RESOLVE_HEIGHT));
	volatile uint32_t *const words = xsdk_gpu_alloc(2 * sizeof(uint32_t));
	if (!target || !words) {
		check("gpu: the GPU writes a word after a resolve", false);
		check("gpu: the GPU waits on a word until the CPU writes it", false);
		return;
	}
	const uint32_t address = xsdk_gpu_address(words);
	words[0] = 0;
	words[1] = 0;
	__asm__ volatile("sync" ::: "memory");
	xsdk_gpu_resolve(xsdk_gpu_address(target), RESOLVE_WIDTH, RESOLVE_HEIGHT, CLEAR_ARGB);
	xsdk_gpu_write_word(address, WORD_VALUE);
	const bool written = NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS)) && words[0] == WORD_VALUE;
	check("gpu: the GPU writes a word after a resolve", written);

	xsdk_gpu_wait_word(address + 4, 1);
	xsdk_gpu_write_word(address, WORD_AFTER_WAIT);
	const uint32_t fence = xsdk_gpu_fence();
	sleep_ms(WORD_HELD_MS);
	const bool held = !xsdk_gpu_fence_done(fence) && words[0] == WORD_VALUE;
	words[1] = 1;
	__asm__ volatile("sync" ::: "memory");
	const uint64_t set = now_us();
	const bool passed = NT_SUCCESS(xsdk_gpu_wait(fence, FENCE_TIMEOUT_MS)) && words[0] == WORD_AFTER_WAIT;
	xsdk_log_write("gpu: the wait on a word held %s for %u ms, then passed %s, %llu us after the CPU wrote it",
		held ? "the GPU" : "nothing", (unsigned)WORD_HELD_MS, passed ? "yes" : "no",
		(unsigned long long)(now_us() - set));
	check("gpu: the GPU waits on a word until the CPU writes it", held && passed);
	xsdk_gpu_free(words);
	xsdk_gpu_free(target);
}

// Two copies of the clock a CPU-timed sleep apart, between two reads of it by the CPU.
static void test_clock(void)
{
	volatile uint32_t *const clocks = xsdk_gpu_alloc(2 * XSDK_GPU_CLOCK_WORDS * sizeof(uint32_t));
	if (!clocks) {
		check("gpu: the clock counts at 498.75 MHz, the GPU's copies between the CPU's reads", false);
		return;
	}
	const uint64_t first = xsdk_gpu_clock();
	const uint64_t start = now_us();
	xsdk_gpu_write_clock(xsdk_gpu_address(clocks), false);
	bool landed = NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
	sleep_ms(CLOCK_SLEEP_MS);
	const uint64_t end = now_us();
	xsdk_gpu_write_clock(xsdk_gpu_address(clocks + XSDK_GPU_CLOCK_WORDS), false);
	landed = NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS)) && landed;
	const uint64_t last = xsdk_gpu_clock();
	const uint64_t before = xsdk_gpu_clock_value(clocks);
	const uint64_t after = xsdk_gpu_clock_value(clocks + XSDK_GPU_CLOCK_WORDS);
	// In thousandths of a clock a microsecond.
	const uint64_t rate = (after - before) * 1000 / (end - start);
	xsdk_log_write(
		"gpu: the clock: %llu, then copies %llu and %llu, then %llu: %llu.%03llu a microsecond, high word %u",
		(unsigned long long)first, (unsigned long long)before, (unsigned long long)after, (unsigned long long)last,
		(unsigned long long)(rate / 1000), (unsigned long long)(rate % 1000), (unsigned)(after >> 32));
	// The copies go as the command processor reaches them, a microsecond or two after the CPU's clock is read.
	check("gpu: the clock counts at 498.75 MHz, the GPU's copies between the CPU's reads",
		landed && first <= before && before < after && after <= last && rate >= CLOCK_RATE_MIN &&
			rate <= CLOCK_RATE_MAX);
	xsdk_gpu_free(clocks);
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
	test_words();
	test_clock();

	// NOPs of different sizes, so the ring wraps at different offsets.
	uint32_t fence = 0;
	for (uint32_t i = 0; i < WRAP_ROUNDS; i++) {
		put_nop(1 + i % 4);
		fence = xsdk_gpu_fence();
	}
	check("gpu: fences as the ring wraps", NT_SUCCESS(xsdk_gpu_wait(fence, FENCE_TIMEOUT_MS)));
	check("gpu: close", NT_SUCCESS(xsdk_gpu_close()));
}
