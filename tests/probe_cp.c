#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// Logs the command processor's state as a title finds it, which D3D devices the kernel holds, and every change to the
// ring registers over a few seconds. It only reads registers. Don't read the microcode RAM (0x07E4 and 0x07E8 for the
// ME, 0x117C and 0x1180 for the PFP): it hangs the GPU (docs/HARDWARE.md).

enum {
	WATCH_MS = 5000,
	MAX_CHANGES_KEPT = 64,
	// RBBM_STATUS bit 31: the GPU is busy (libxenon:drivers/xenos/xe.c:258).
	GPU_BUSY = 0x80000000,
};

typedef struct {
	uint32_t base;
	uint32_t read;
	uint32_t write;
	uint32_t read_writeback;
	uint32_t me_control;
	bool busy;
} RingState;

typedef struct {
	uint32_t at_ms;
	RingState state;
} RingChange;

// GPU registers as titles see them (xenia:src/xenia/gpu/graphics_system.cc:92-95); offsets are in bytes, as libxenon's.
static volatile uint32_t *const gpu_registers = (volatile uint32_t *)0x7FC80000;

static RingChange changes[MAX_CHANGES_KEPT];

static uint32_t read_register(uint32_t offset)
{
	return gpu_registers[offset / 4];
}

// What each register means comes from how libxenon uses it (drivers/xenos/xe.c:104, 210-221, 258, 305-330, 389-395).
static void report_state(const char *when)
{
	xsdk_log_write("cp: %s: 0x07D8 %08X (bit 28: halted), 0x1740 %08X (bit 31: busy), 0x0E6C %08X", when,
		(unsigned)read_register(0x07D8), (unsigned)read_register(0x1740), (unsigned)read_register(0x0E6C));
	xsdk_log_write("cp: %s: ring: base %08X, control %08X, read pointer write-back %08X, read %08X, write %08X", when,
		(unsigned)read_register(0x0700), (unsigned)read_register(0x0704), (unsigned)read_register(0x070C),
		(unsigned)read_register(0x0710), (unsigned)read_register(0x0714));
}

// D3D's GPU hang report names the device it came from; these say whether that is XAM's.
static void report_devices(void)
{
	xsdk_log_write("cp: VdGlobalDevice at %p holds %08X, VdGlobalXamDevice at %p holds %08X", (void *)VdGlobalDevice,
		VdGlobalDevice ? (unsigned)*VdGlobalDevice : 0u, (void *)VdGlobalXamDevice,
		VdGlobalXamDevice ? (unsigned)*VdGlobalXamDevice : 0u);
}

static RingState sample_ring(void)
{
	return (RingState){
		.base = read_register(0x0700),
		.read = read_register(0x0710),
		.write = read_register(0x0714),
		.read_writeback = read_register(0x070C),
		.me_control = read_register(0x07D8),
		.busy = (read_register(0x1740) & GPU_BUSY) != 0,
	};
}

static bool same_state(const RingState *a, const RingState *b)
{
	return a->base == b->base && a->read == b->read && a->write == b->write && a->read_writeback == b->read_writeback &&
		a->me_control == b->me_control && a->busy == b->busy;
}

static uint32_t elapsed_ms(const struct timespec *start)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)((now.tv_sec - start->tv_sec) * 1000 + (now.tv_nsec - start->tv_nsec) / 1000000);
}

// Samples about once a millisecond and keeps each change, logging them afterwards so the log's writes don't slow the
// sampling.
static void watch_ring(void)
{
	struct timespec start;
	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	RingState last = sample_ring();
	uint32_t change_count = 0;
	uint32_t samples = 0;
	uint32_t busy_samples = 0;
	for (uint32_t now_ms = 0; now_ms < WATCH_MS; now_ms = elapsed_ms(&start)) {
		const RingState state = sample_ring();
		samples++;
		busy_samples += state.busy;
		if (!same_state(&state, &last)) {
			if (change_count < MAX_CHANGES_KEPT)
				changes[change_count] = (RingChange){ .at_ms = now_ms, .state = state };
			change_count++;
			last = state;
		}
		(void)xsdk_thread_sleep(1);
	}

	xsdk_log_write("cp: watched %u ms: %u samples, %u busy, %u changes", (unsigned)WATCH_MS, (unsigned)samples,
		(unsigned)busy_samples, (unsigned)change_count);
	for (uint32_t i = 0; i < change_count && i < MAX_CHANGES_KEPT; i++) {
		const RingChange *change = &changes[i];
		xsdk_log_write("cp: at %u ms: base %08X, read %08X, write %08X, read pointer write-back %08X, 0x07D8 %08X, %s",
			(unsigned)change->at_ms, (unsigned)change->state.base, (unsigned)change->state.read,
			(unsigned)change->state.write, (unsigned)change->state.read_writeback, (unsigned)change->state.me_control,
			change->state.busy ? "busy" : "idle");
	}
}

int main(void)
{
	xsdk_log_write("cp probe");
	report_state("at start");
	report_devices();
	watch_ring();
	report_state("after watching");
	return 0;
}
