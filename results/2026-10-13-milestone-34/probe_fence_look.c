#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "probe_gpu.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// Times a 1280x720 resolve by the GPU's clock while the CPU waits on its fence in different ways: asleep, looking at
// the fence without a break, and looking every few microseconds, to see what the looks cost the GPU.

enum {
	FENCE_TIMEOUT_MS = 1000,
	PM4_REG_TO_MEM = 0x3E,
	SWAP_8IN32 = 2,
	CP_PERFMON_CNTL = 0x01F5,
	RBBM_PERFCOUNTER1_SELECT = 0x0396,
	RBBM_PERFCOUNTER1_LOW = 0x0399,
	WAIT_3D_IDLE_CLEAN = 1 << 17,
	GPU_CLOCKS_PER_US = 500,
	WIDTH = 1280,
	HEIGHT = 720,
	ROUNDS = 16,
	ASLEEP = UINT32_MAX,
};

// Microseconds between looks; 0 looks without a break, ASLEEP sleeps 5 ms first.
static const struct {
	const char *name;
	uint32_t interval_us;
} ways[] = {
	{ "asleep 5 ms", ASLEEP },
	{ "looking without a break", 0 },
	{ "looking every 1 us", 1 },
	{ "looking every 2 us", 2 },
	{ "looking every 5 us", 5 },
	{ "looking every 10 us", 10 },
	{ "looking every 20 us", 20 },
	{ "looking every 50 us", 50 },
	{ "asleep 5 ms again", ASLEEP },
	{ "looking without a break again", 0 },
};

static uint64_t now_ns(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

static void put_register(uint32_t index, uint32_t value)
{
	volatile uint32_t *const packet = xsdk_gpu_reserve(2);
	packet[0] = index;
	packet[1] = value;
}

static void put_clock_copy(uint32_t address)
{
	put_register(WAIT_UNTIL, WAIT_3D_IDLE_CLEAN);
	volatile uint32_t *const packet = xsdk_gpu_reserve(3);
	packet[0] = xsdk_gpu_type3(PM4_REG_TO_MEM, 2);
	packet[1] = RBBM_PERFCOUNTER1_LOW;
	packet[2] = address | SWAP_8IN32;
}

static bool wait(uint32_t fence, uint32_t interval_us)
{
	if (interval_us == ASLEEP) {
		(void)xsdk_thread_sleep(5);
		return xsdk_gpu_fence_done(fence);
	}
	const uint64_t start = now_ns();
	while (!xsdk_gpu_fence_done(fence)) {
		const uint64_t next = now_ns() + (uint64_t)interval_us * 1000u;
		while (now_ns() < next) {
		}
		if (now_ns() - start > (uint64_t)FENCE_TIMEOUT_MS * 1000000u)
			return false;
	}
	return true;
}

static void time_waits(const char *name, uint32_t interval_us, volatile uint32_t *target, volatile uint32_t *clocks)
{
	uint32_t job_min = UINT32_MAX;
	uint32_t job_max = 0;
	uint64_t job_sum = 0;
	uint32_t late_max = 0;
	uint64_t late_sum = 0;
	bool landed = true;
	for (uint32_t i = 0; i < ROUNDS && landed; i++) {
		clocks[0] = 0;
		clocks[1] = 0;
		__asm__ volatile("sync" ::: "memory");
		put_clock_copy(xsdk_gpu_address(clocks));
		xsdk_gpu_resolve(xsdk_gpu_address(target), WIDTH, HEIGHT, 0xFF202020);
		put_clock_copy(xsdk_gpu_address(clocks) + 4);
		landed = wait(xsdk_gpu_fence(), interval_us);
		const uint32_t returned = probe_gpu_registers[RBBM_PERFCOUNTER1_LOW];
		const uint32_t job = clocks[1] - clocks[0];
		const uint32_t late = returned - clocks[1];
		job_min = job < job_min ? job : job_min;
		job_max = job > job_max ? job : job_max;
		job_sum += job;
		late_max = late > late_max ? late : late_max;
		late_sum += late;
	}
	xsdk_log_write("fence look: %s: resolve %u to %u clocks, mean %u; returned at most %u clocks after the fence, "
				   "mean %u; %s",
		name, (unsigned)job_min, (unsigned)job_max,
		(unsigned)(job_sum / ROUNDS), (unsigned)late_max, (unsigned)(late_sum / ROUNDS),
		landed ? "every fence landed" : "a fence didn't land");
}

int main(void)
{
	xsdk_log_write("fence look probe, %u GPU clocks a microsecond", (unsigned)GPU_CLOCKS_PER_US);
	const NTSTATUS status = xsdk_gpu_open();
	volatile uint32_t *const target = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(WIDTH, HEIGHT));
	volatile uint32_t *const clocks = xsdk_gpu_alloc(8);
	if (!NT_SUCCESS(status) || !target || !clocks) {
		xsdk_log_write("fence look: set-up failed 0x%08X", (unsigned)status);
		return 1;
	}
	put_register(CP_PERFMON_CNTL, 0);
	put_register(RBBM_PERFCOUNTER1_SELECT, 0);
	put_register(CP_PERFMON_CNTL, 1);
	(void)xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
	for (uint32_t i = 0; i < sizeof ways / sizeof ways[0]; i++)
		time_waits(ways[i].name, ways[i].interval_us, target, clocks);
	for (uint32_t i = 0; i < 4; i++) {
		const uint64_t start = now_ns();
		const NTSTATUS waited = xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
		xsdk_log_write("fence look: xsdk_gpu_wait on a fence alone took %u ns, 0x%08X", (unsigned)(now_ns() - start),
			(unsigned)waited);
	}
	put_register(RBBM_PERFCOUNTER1_SELECT, 0);
	put_register(CP_PERFMON_CNTL, 0);
	xsdk_log_write("fence look: xsdk_gpu_close 0x%08X", (unsigned)xsdk_gpu_close());
	return 0;
}
