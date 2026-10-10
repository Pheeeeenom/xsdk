#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "probe_gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// Watches the ring registers at title start; it never reads microcode RAM, which hangs the GPU (docs/HARDWARE.md).

enum {
	WATCH_MS = 5000,
	MAX_CHANGES_KEPT = 64,
	// libxenon checks its bits 8-11 before taking the GPU (libxenon:drivers/xenos/xe.c:363).
	UNKNOWN_039B = 0x039B,
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

static RingChange changes[MAX_CHANGES_KEPT];

static void report_state(const char *when)
{
	probe_report_cp("cp", when);
	xsdk_log_write("cp: %s: 0x0E6C %08X", when, (unsigned)probe_gpu_registers[UNKNOWN_039B]);
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
	volatile const uint32_t *const registers = probe_gpu_registers;
	return (RingState){
		.base = registers[CP_RB_BASE],
		.read = registers[CP_RB_RPTR],
		.write = registers[CP_RB_WPTR],
		.read_writeback = registers[CP_RB_RPTR_ADDR],
		.me_control = registers[UNKNOWN_01F6],
		.busy = (registers[RBBM_STATUS] & GPU_BUSY) != 0,
	};
}

static bool same_state(const RingState *a, const RingState *b)
{
	return a->base == b->base && a->read == b->read && a->write == b->write && a->read_writeback == b->read_writeback &&
		a->me_control == b->me_control && a->busy == b->busy;
}

// Logs the changes afterwards, so writing the log doesn't slow the sampling.
static void watch_ring(void)
{
	struct timespec start;
	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	RingState last = sample_ring();
	uint32_t change_count = 0;
	uint32_t samples = 0;
	uint32_t busy_samples = 0;
	for (uint32_t now_ms = 0; now_ms < WATCH_MS; now_ms = probe_elapsed_ms(&start)) {
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
