#include <stdint.h>
#include <time.h>

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

// GPU registers as titles see them (xenia:src/xenia/gpu/graphics_system.cc:92-95); offsets are in bytes, as libxenon's.
static volatile uint32_t *const gpu_registers = (volatile uint32_t *)0x7FC80000;

// Written only by the callback.
static volatile uint32_t interrupt_counts[SOURCE_OTHER + 1];

static uint32_t read_register(uint32_t offset)
{
	return gpu_registers[offset / 4];
}

// Runs in the kernel's interrupt handling, so it only counts.
static void count_interrupt(uint32_t source, void *context)
{
	(void)context;
	interrupt_counts[source < SOURCE_OTHER ? source : SOURCE_OTHER]++;
}

static uint32_t elapsed_ms(const struct timespec *start)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)((now.tv_sec - start->tv_sec) * 1000 + (now.tv_nsec - start->tv_nsec) / 1000000);
}

// 0x0714 is the ring's write pointer (libxenon:drivers/xenos/xe.c:104).
static Watch watch_write_pointer(void)
{
	struct timespec start;
	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	uint32_t last = read_register(0x0714);
	Watch watch = { .moves = 0, .first_write = last, .last_write = last };
	while (elapsed_ms(&start) < WATCH_MS) {
		const uint32_t write = read_register(0x0714);
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

// What each register means comes from how libxenon uses it (drivers/xenos/xe.c:104, 210-221, 258, 305-330).
static void report_state(const char *when)
{
	xsdk_log_write("interrupt: %s: 0x07D8 %08X, 0x1740 %08X (bit 31: busy), ring base %08X, read %08X, write %08X",
		when, (unsigned)read_register(0x07D8), (unsigned)read_register(0x1740), (unsigned)read_register(0x0700),
		(unsigned)read_register(0x0710), (unsigned)read_register(0x0714));
}

int main(void)
{
	xsdk_log_write("interrupt probe");
	report_state("at start");
	watch_and_report("with XAM's callback");

	VdSetGraphicsInterruptCallback(count_interrupt, 0);
	watch_and_report("with the title's callback");

	VdSetGraphicsInterruptCallback(0, 0);
	watch_and_report("with no callback");
	report_state("at end");
	return 0;
}
