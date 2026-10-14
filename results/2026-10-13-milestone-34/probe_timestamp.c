#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "probe_gpu.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"

// Looks for a clock the GPU writes into memory and the CPU reads, then times fences after short jobs.

enum {
	FENCE_TIMEOUT_MS = 1000,
	PM4_REG_TO_MEM = 0x3E,
	PM4_EVENT_WRITE_SHD = 0x58,
	PS_DONE_TS = 3,
	SWAP_8IN32 = 2,
	// xenia:src/xenia/gpu/register_table.inc:42-52.
	CP_PERFCOUNTER0_SELECT = 0x01E6,
	CP_PERFCOUNTER0_LOW = 0x01E7,
	CP_PERFCOUNTER0_HI = 0x01E8,
	CP_PERFMON_CNTL = 0x01F5,
	RBBM_PERFCOUNTER0_SELECT = 0x0395,
	RBBM_PERFCOUNTER1_SELECT = 0x0396,
	RBBM_PERFCOUNTER0_LOW = 0x0397,
	RBBM_PERFCOUNTER0_HI = 0x0398,
	RBBM_PERFCOUNTER1_LOW = 0x0399,
	RBBM_PERFCOUNTER1_HI = 0x039A,
	R500_D1MODE_V_COUNTER = 0x194C,
	WIDTH = 1280,
	HEIGHT = 720,
	REPEATS = 5,
	WORDS = 16,
};

static const struct {
	const char *name;
	uint32_t index;
} counters[] = {
	{ "CP_PERFCOUNTER0_LOW", CP_PERFCOUNTER0_LOW },
	{ "CP_PERFCOUNTER0_HI", CP_PERFCOUNTER0_HI },
	{ "RBBM_PERFCOUNTER0_LOW", RBBM_PERFCOUNTER0_LOW },
	{ "RBBM_PERFCOUNTER0_HI", RBBM_PERFCOUNTER0_HI },
	{ "RBBM_PERFCOUNTER1_LOW", RBBM_PERFCOUNTER1_LOW },
	{ "RBBM_PERFCOUNTER1_HI", RBBM_PERFCOUNTER1_HI },
	{ "R500_D1MODE_V_COUNTER", R500_D1MODE_V_COUNTER },
};

enum { COUNTERS = sizeof counters / sizeof counters[0] };

static volatile uint32_t *const registers = probe_gpu_registers;

static uint64_t now_ns(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

static void spin_us(uint32_t us)
{
	const uint64_t end = now_ns() + (uint64_t)us * 1000u;
	while (now_ns() < end) {
	}
}

static void log_rates(const char *when)
{
	uint32_t before[COUNTERS];
	for (uint32_t i = 0; i < COUNTERS; i++)
		before[i] = registers[counters[i].index];
	const uint64_t start = now_ns();
	spin_us(1000);
	uint32_t after[COUNTERS];
	for (uint32_t i = 0; i < COUNTERS; i++)
		after[i] = registers[counters[i].index];
	const uint64_t took = now_ns() - start;
	for (uint32_t i = 0; i < COUNTERS; i++)
		xsdk_log_write("timestamp: %s: %s %08X then %08X over %u ns, %u more", when, counters[i].name,
			(unsigned)before[i], (unsigned)after[i], (unsigned)took, (unsigned)(after[i] - before[i]));
}

static void put_register(uint32_t index, uint32_t value)
{
	volatile uint32_t *const packet = xsdk_gpu_reserve(2);
	packet[0] = index;
	packet[1] = value;
}

static bool spin_fence(uint32_t fence)
{
	const uint64_t start = now_ns();
	while (!xsdk_gpu_fence_done(fence))
		if (now_ns() - start > (uint64_t)FENCE_TIMEOUT_MS * 1000000u)
			return false;
	return true;
}

// The command processor copies each counter into memory between two reads of it by the CPU.
static void copy_counters(volatile uint32_t *words, uint32_t words_address)
{
	for (uint32_t i = 0; i < COUNTERS; i++) {
		words[0] = 0xDEADBEEF;
		__asm__ volatile("sync" ::: "memory");
		const uint32_t before = registers[counters[i].index];
		volatile uint32_t *const packet = xsdk_gpu_reserve(3);
		packet[0] = xsdk_gpu_type3(PM4_REG_TO_MEM, 2);
		packet[1] = counters[i].index;
		packet[2] = words_address | SWAP_8IN32;
		const bool landed = spin_fence(xsdk_gpu_fence());
		const uint32_t after = registers[counters[i].index];
		xsdk_log_write("timestamp: REG_TO_MEM %s: CPU %08X, GPU %08X, CPU %08X; fence %s", counters[i].name,
			(unsigned)before, (unsigned)words[0], (unsigned)after, landed ? "landed" : "didn't land");
	}
}

// An event write with bit 31 or 30 of its initiator, which Xenia takes to write a counter
// (xenia:src/xenia/gpu/command_processor.cc:1204-1206).
static void event_counters(volatile uint32_t *words, uint32_t words_address, uint32_t bit)
{
	for (uint32_t i = 0; i < 2; i++) {
		words[i] = 0xDEADBEEF;
		__asm__ volatile("sync" ::: "memory");
		const uint32_t rbbm = registers[RBBM_PERFCOUNTER1_LOW];
		const uint32_t cp = registers[CP_PERFCOUNTER0_LOW];
		volatile uint32_t *const packet = xsdk_gpu_reserve(4);
		packet[0] = xsdk_gpu_type3(PM4_EVENT_WRITE_SHD, 3);
		packet[1] = 1u << bit | PS_DONE_TS;
		packet[2] = (words_address + i * 4) | SWAP_8IN32;
		packet[3] = 0x12345678;
		const bool landed = spin_fence(xsdk_gpu_fence());
		xsdk_log_write("timestamp: EVENT_WRITE_SHD bit %u, write %u: %08X; RBBM1 %08X, CP %08X before; fence %s",
			(unsigned)bit, (unsigned)i, (unsigned)words[i], (unsigned)rbbm, (unsigned)cp,
			landed ? "landed" : "didn't land");
		spin_us(1000);
	}
}

static void time_fences(const char *job, uint32_t resolves, uint32_t address)
{
	for (uint32_t r = 0; r < REPEATS; r++) {
		const uint64_t start = now_ns();
		for (uint32_t i = 0; i < resolves; i++)
			xsdk_gpu_resolve(address, WIDTH, HEIGHT, 0xFF202020);
		const uint32_t fence = xsdk_gpu_fence();
		const uint64_t submitted = now_ns();
		const bool landed = spin_fence(fence);
		const uint64_t spun = now_ns();
		for (uint32_t i = 0; i < resolves; i++)
			xsdk_gpu_resolve(address, WIDTH, HEIGHT, 0xFF202020);
		const uint64_t start_wait = now_ns();
		const NTSTATUS waited = xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
		const uint64_t end_wait = now_ns();
		xsdk_log_write("timestamp: %s: submitted in %u ns, passed %u ns later by spinning (%s); xsdk_gpu_wait took %u "
					   "ns, 0x%08X",
			job, (unsigned)(submitted - start), (unsigned)(spun - submitted), landed ? "landed" : "didn't land",
			(unsigned)(end_wait - start_wait), (unsigned)waited);
	}
}

int main(void)
{
	xsdk_log_write("timestamp probe");
	const NTSTATUS status = xsdk_gpu_open();
	xsdk_log_write("timestamp: xsdk_gpu_open 0x%08X", (unsigned)status);
	if (!NT_SUCCESS(status))
		return 1;
	volatile uint32_t *const words = xsdk_gpu_alloc(WORDS * 4);
	volatile uint32_t *const target = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(WIDTH, HEIGHT));
	if (!words || !target)
		return 1;
	const uint32_t words_address = xsdk_gpu_address(words);
	log_rates("as found");
	registers[CP_PERFMON_CNTL] = 0;
	registers[CP_PERFCOUNTER0_SELECT] = 0;
	registers[RBBM_PERFCOUNTER0_SELECT] = 0;
	registers[RBBM_PERFCOUNTER1_SELECT] = 0;
	registers[CP_PERFMON_CNTL] = 1;
	log_rates("selects 0 and perfmon 1 from the CPU");
	put_register(CP_PERFMON_CNTL, 0);
	put_register(CP_PERFCOUNTER0_SELECT, 0);
	put_register(RBBM_PERFCOUNTER0_SELECT, 0);
	put_register(RBBM_PERFCOUNTER1_SELECT, 0);
	put_register(CP_PERFMON_CNTL, 1);
	xsdk_log_write("timestamp: written through the ring: fence %s", spin_fence(xsdk_gpu_fence()) ? "landed" : "didn't land");
	log_rates("selects 0 and perfmon 1 through the ring");
	copy_counters(words, words_address);
	event_counters(words, words_address, 31);
	event_counters(words, words_address, 30);
	time_fences("a fence alone", 0, xsdk_gpu_address(target));
	time_fences("one 1280x720 resolve", 1, xsdk_gpu_address(target));
	time_fences("four 1280x720 resolves", 4, xsdk_gpu_address(target));
	log_rates("at the end");
	xsdk_gpu_free(target);
	xsdk_gpu_free(words);
	xsdk_log_write("timestamp: xsdk_gpu_close 0x%08X", (unsigned)xsdk_gpu_close());
	return 0;
}
