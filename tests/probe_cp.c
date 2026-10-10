#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// Logs the command processor's state as a title finds it, which D3D devices the kernel holds, every change to the ring
// registers over a few seconds, and a checksum of the ME microcode, never the microcode. The ring probe found
// something else writing the ring's write pointer while a title runs (docs/HARDWARE.md); this watches for it without
// writing to the GPU, apart from the ME RAM read address at the end.
// Don't touch the PFP microcode RAM (0x117C, 0x1180): it doesn't read back and it hangs the GPU (docs/HARDWARE.md).

enum {
	ME_WORDS = 0x900,
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

static void write_register(uint32_t offset, uint32_t value)
{
	gpu_registers[offset / 4] = value;
}

// CRC-32 over each word's big-endian bytes, so a host can compare it with zlib.crc32.
static uint32_t crc32_word(uint32_t crc, uint32_t word)
{
	for (int shift = 24; shift >= 0; shift -= 8) {
		crc ^= (word >> shift) & 0xFF;
		for (int bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
	}
	return crc;
}

// Reads the ME microcode RAM from word 0, as libxenon's loader checks it (libxenon:drivers/xenos/xe.c:243-247).
static void report_me_ram(void)
{
	write_register(0x07E4, 0);
	uint32_t crc = 0xFFFFFFFF;
	uint32_t nonzero_words = 0;
	for (uint32_t i = 0; i < ME_WORDS; i++) {
		const uint32_t word = read_register(0x07E8);
		nonzero_words += word != 0;
		crc = crc32_word(crc, word);
	}
	xsdk_log_write(
		"cp: ME RAM: %u words, %u non-zero, crc32 %08X", (unsigned)ME_WORDS, (unsigned)nonzero_words, (unsigned)~crc);
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
	report_me_ram();
	return 0;
}
