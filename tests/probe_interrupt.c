#include <stdint.h>
#include <time.h>

#include "probe_gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// XAM's D3D device submits to its ring every vblank while a title runs (docs/HARDWARE.md, "XAM's D3D device shares the
// command processor"). This tests whether it does so from the graphics interrupt callback: it watches XAM's write
// pointer, sets a callback of its own that only counts, and watches again. It writes no GPU registers. It clears the
// callback before exiting, so the kernel doesn't call into an unloaded title; XAM's callback can't be put back.

enum {
	WATCH_MS = 2000,
	// Sources the kernel passes: 0 at vblank, 1 for the command processor (xenia:src/xenia/gpu/graphics_system.cc:272,
	// command_processor.cc:909).
	SOURCE_VBLANK = 0,
	SOURCE_COMMAND_PROCESSOR = 1,
	SOURCE_OTHER = 2,
};

typedef struct {
	uint32_t moves;
	uint32_t first_write;
	uint32_t last_write;
} Watch;

// Written only by the callback.
static volatile uint32_t interrupt_counts[SOURCE_OTHER + 1];

// Runs in the kernel's interrupt handling, so it only counts.
static void count_interrupt(uint32_t source, void *context)
{
	(void)context;
	interrupt_counts[source < SOURCE_OTHER ? source : SOURCE_OTHER]++;
}

static Watch watch_write_pointer(void)
{
	struct timespec start;
	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	uint32_t last = probe_gpu_registers[CP_RB_WPTR];
	Watch watch = { .moves = 0, .first_write = last, .last_write = last };
	while (probe_elapsed_ms(&start) < WATCH_MS) {
		const uint32_t write = probe_gpu_registers[CP_RB_WPTR];
		if (write != last) {
			watch.moves++;
			last = write;
		}
		(void)xsdk_thread_sleep(1);
	}
	watch.last_write = last;
	return watch;
}

static void watch_and_report(const char *phase)
{
	uint32_t before[SOURCE_OTHER + 1];
	for (uint32_t i = 0; i <= SOURCE_OTHER; i++)
		before[i] = interrupt_counts[i];
	const Watch watch = watch_write_pointer();
	xsdk_log_write(
		"interrupt: %s: in %u ms the write pointer moved %u times (%08X to %08X); the title's callback ran "
		"%u times for vblank, %u for the command processor, %u for other sources",
		phase, (unsigned)WATCH_MS, (unsigned)watch.moves, (unsigned)watch.first_write, (unsigned)watch.last_write,
		(unsigned)(interrupt_counts[SOURCE_VBLANK] - before[SOURCE_VBLANK]),
		(unsigned)(interrupt_counts[SOURCE_COMMAND_PROCESSOR] - before[SOURCE_COMMAND_PROCESSOR]),
		(unsigned)(interrupt_counts[SOURCE_OTHER] - before[SOURCE_OTHER]));
}

int main(void)
{
	xsdk_log_write("interrupt probe");
	probe_report_cp("interrupt", "at start");
	watch_and_report("with XAM's callback");

	VdSetGraphicsInterruptCallback(count_interrupt, 0);
	watch_and_report("with the title's callback");

	VdSetGraphicsInterruptCallback(0, 0);
	watch_and_report("with no callback");
	probe_report_cp("interrupt", "at end");
	return 0;
}
