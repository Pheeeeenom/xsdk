#include <stdint.h>
#include <time.h>

#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// Nothing a title gives the display shows (docs/HARDWARE.md), while XAM's Guide does. Part one leaves the GPU with XAM
// and logs every change in the display's register blocks while the user opens and closes the Guide. Part two
// snapshots the same blocks around VdInitializeEngines and VdShutdownEngines and logs what they change. It only reads
// display registers.

enum {
	WATCH_MS = 15000,
	SAMPLE_MS = 20,
	SETTLE_MS = 100,
	MAX_CHANGES_KEPT = 512,
	// A counter changes every sample, so only each register's first few changes are kept.
	CHANGES_KEPT_PER_REGISTER = 8,
	REGISTER_COUNT = 384,
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

// Byte offsets around the display registers libxenon uses: 0x2840, the CRTC and D1GRPH, the LUT, D1MODE and the
// scaler, and 0x79xx, where it talks to the video encoder (libxenon:drivers/xenos/xenos.h:11-54, xenos.c:40-160, 384).
static const Range ranges[] = {
	{ 0x2800, 64 },
	{ 0x6000, 64 },
	{ 0x6100, 64 },
	{ 0x6400, 64 },
	{ 0x6500, 64 },
	{ 0x7900, 64 },
};

// GPU registers as titles see them (xenia:src/xenia/gpu/graphics_system.cc:92-95); libxenon's base is the same block.
static volatile uint32_t *const gpu_registers = (volatile uint32_t *)0x7FC80000;

static uint32_t values[REGISTER_COUNT];
static uint32_t change_counts[REGISTER_COUNT];
static uint32_t before[REGISTER_COUNT];
static Change changes[MAX_CHANGES_KEPT];

static uint32_t read_register(uint32_t offset)
{
	return gpu_registers[offset / 4];
}

static uint32_t offset_of(uint32_t index)
{
	for (uint32_t r = 0; r < sizeof ranges / sizeof ranges[0]; r++) {
		if (index < ranges[r].count)
			return ranges[r].first + index * 4;
		index -= ranges[r].count;
	}
	return 0;
}

static uint32_t elapsed_ms(const struct timespec *start)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)((now.tv_sec - start->tv_sec) * 1000 + (now.tv_nsec - start->tv_nsec) / 1000000);
}

static void snapshot(uint32_t *into)
{
	for (uint32_t i = 0; i < REGISTER_COUNT; i++)
		into[i] = read_register(offset_of(i));
}

static void dump(const char *when, const uint32_t *snapshot_values)
{
	for (uint32_t i = 0; i < REGISTER_COUNT; i += 8) {
		const uint32_t *v = &snapshot_values[i];
		xsdk_log_write("guide: %s: %04X: %08X %08X %08X %08X %08X %08X %08X %08X", when, (unsigned)offset_of(i),
			(unsigned)v[0], (unsigned)v[1], (unsigned)v[2], (unsigned)v[3], (unsigned)v[4], (unsigned)v[5],
			(unsigned)v[6], (unsigned)v[7]);
	}
}

// Returns how many changes have been kept.
static uint32_t sample(uint32_t at_ms, uint32_t kept)
{
	for (uint32_t i = 0; i < REGISTER_COUNT; i++) {
		const uint32_t offset = offset_of(i);
		const uint32_t value = read_register(offset);
		if (value == values[i])
			continue;
		if (kept < MAX_CHANGES_KEPT && change_counts[i] < CHANGES_KEPT_PER_REGISTER)
			changes[kept++] = (Change){ at_ms, offset, values[i], value };
		change_counts[i]++;
		values[i] = value;
	}
	return kept;
}

static void watch_guide(void)
{
	xsdk_log_write("guide: part one: open the Guide in the next few seconds, then close it");
	snapshot(values);
	dump("at start", values);

	struct timespec start;
	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	uint32_t kept = 0;
	for (uint32_t now_ms = 0; now_ms < WATCH_MS; now_ms = elapsed_ms(&start)) {
		kept = sample(now_ms, kept);
		(void)xsdk_thread_sleep(SAMPLE_MS);
	}

	for (uint32_t i = 0; i < REGISTER_COUNT; i++) {
		if (change_counts[i])
			xsdk_log_write("guide: %04X changed %u times", (unsigned)offset_of(i), (unsigned)change_counts[i]);
	}
	for (uint32_t i = 0; i < kept; i++) {
		xsdk_log_write("guide: at %u ms: %04X %08X -> %08X", (unsigned)changes[i].at_ms, (unsigned)changes[i].offset,
			(unsigned)changes[i].old_value, (unsigned)changes[i].new_value);
	}
}

// Logs registers that changed, leaving out the ones part one saw changing all the time.
static void log_differences(const char *what, const uint32_t *old_values, const uint32_t *new_values)
{
	uint32_t differences = 0;
	for (uint32_t i = 0; i < REGISTER_COUNT; i++) {
		if (old_values[i] == new_values[i] || change_counts[i] > CHANGES_KEPT_PER_REGISTER)
			continue;
		differences++;
		xsdk_log_write("guide: %s: %04X %08X -> %08X", what, (unsigned)offset_of(i), (unsigned)old_values[i],
			(unsigned)new_values[i]);
	}
	xsdk_log_write("guide: %s: %u registers changed", what, (unsigned)differences);
}

int main(void)
{
	xsdk_log_write("guide probe");
	watch_guide();

	xsdk_log_write("guide: part two: taking the GPU and giving it back");
	snapshot(before);
	const NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("guide: xsdk_gpu_open returned 0x%08X", (unsigned)status);
		return 1;
	}
	(void)xsdk_thread_sleep(SETTLE_MS);
	snapshot(values);
	log_differences("VdInitializeEngines", before, values);

	const NTSTATUS closed = xsdk_gpu_close();
	(void)xsdk_thread_sleep(SETTLE_MS);
	snapshot(before);
	xsdk_log_write("guide: xsdk_gpu_close returned 0x%08X", (unsigned)closed);
	log_differences("VdShutdownEngines", values, before);
	return 0;
}
