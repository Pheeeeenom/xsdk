#include <stdint.h>
#include <time.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// A title's display stays black even with the surface pointed at its buffer (docs/HARDWARE.md), so this records what
// XAM changes to show something: it doesn't take the GPU, and logs every change to the display controller's registers
// while the user opens and closes the Guide, which XAM's D3D device draws over the title. It only reads registers.

enum {
	WATCH_MS = 15000,
	SAMPLE_MS = 20,
	MAX_CHANGES_KEPT = 256,
	// A counter changes every sample, so only each register's first few changes are kept.
	CHANGES_KEPT_PER_REGISTER = 8,
	REGISTER_COUNT = 120,
};

typedef struct {
	uint32_t first;
	uint32_t count;
} Range;

typedef struct {
	uint32_t at_ms;
	uint32_t offset;
	uint32_t old_value;
	uint32_t new_value;
} Change;

// Byte offsets around the registers libxenon names: CRTC timing, the CRTC update lock, D1GRPH, and D1MODE and the
// scaler (libxenon:drivers/xenos/xenos.h:11-54).
static const Range ranges[] = {
	{ 0x6000, 28 },
	{ 0x60E0, 8 },
	{ 0x6100, 20 },
	{ 0x6500, 64 },
};

// GPU registers as titles see them (xenia:src/xenia/gpu/graphics_system.cc:92-95); libxenon's base is the same block.
static volatile uint32_t *const gpu_registers = (volatile uint32_t *)0x7FC80000;

static uint32_t values[REGISTER_COUNT];
static uint32_t change_counts[REGISTER_COUNT];
static Change changes[MAX_CHANGES_KEPT];

static uint32_t read_register(uint32_t offset)
{
	return gpu_registers[offset / 4];
}

static uint32_t elapsed_ms(const struct timespec *start)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)((now.tv_sec - start->tv_sec) * 1000 + (now.tv_nsec - start->tv_nsec) / 1000000);
}

static void dump(const char *when)
{
	for (uint32_t r = 0; r < sizeof ranges / sizeof ranges[0]; r++) {
		for (uint32_t i = 0; i < ranges[r].count; i += 4) {
			const uint32_t offset = ranges[r].first + i * 4;
			xsdk_log_write("guide: %s: %04X: %08X %08X %08X %08X", when, (unsigned)offset,
				(unsigned)read_register(offset), (unsigned)read_register(offset + 4),
				(unsigned)read_register(offset + 8), (unsigned)read_register(offset + 12));
		}
	}
}

// Returns how many changes have been kept.
static uint32_t sample(uint32_t at_ms, uint32_t kept)
{
	uint32_t index = 0;
	for (uint32_t r = 0; r < sizeof ranges / sizeof ranges[0]; r++) {
		for (uint32_t i = 0; i < ranges[r].count; i++, index++) {
			const uint32_t offset = ranges[r].first + i * 4;
			const uint32_t value = read_register(offset);
			if (value == values[index])
				continue;
			if (kept < MAX_CHANGES_KEPT && change_counts[index] < CHANGES_KEPT_PER_REGISTER)
				changes[kept++] = (Change){ at_ms, offset, values[index], value };
			change_counts[index]++;
			values[index] = value;
		}
	}
	return kept;
}

static void report_change_counts(void)
{
	uint32_t index = 0;
	for (uint32_t r = 0; r < sizeof ranges / sizeof ranges[0]; r++) {
		for (uint32_t i = 0; i < ranges[r].count; i++, index++) {
			if (change_counts[index])
				xsdk_log_write("guide: %04X changed %u times", (unsigned)(ranges[r].first + i * 4),
					(unsigned)change_counts[index]);
		}
	}
}

int main(void)
{
	xsdk_log_write("guide probe: open the Guide in the next few seconds, then close it");
	dump("at start");
	(void)sample(0, MAX_CHANGES_KEPT);
	for (uint32_t i = 0; i < REGISTER_COUNT; i++)
		change_counts[i] = 0;

	struct timespec start;
	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	uint32_t kept = 0;
	for (uint32_t now_ms = 0; now_ms < WATCH_MS; now_ms = elapsed_ms(&start)) {
		kept = sample(now_ms, kept);
		(void)xsdk_thread_sleep(SAMPLE_MS);
	}

	report_change_counts();
	for (uint32_t i = 0; i < kept; i++) {
		xsdk_log_write("guide: at %u ms: %04X %08X -> %08X", (unsigned)changes[i].at_ms, (unsigned)changes[i].offset,
			(unsigned)changes[i].old_value, (unsigned)changes[i].new_value);
	}
	dump("at end");
	return 0;
}
