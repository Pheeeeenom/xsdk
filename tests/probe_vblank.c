#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "probe_gpu.h"
#include "xsdk/display.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// Times the interrupt callback against vblank and flips, then gives the GPU back and watches XAM's device run again.

enum {
	WIDTH = 640,
	HEIGHT = 480,
	PIXELS = WIDTH * HEIGHT,
	MAX_SAMPLES = 512,
	SAMPLES_LOGGED = 24,
	FLIPS = 10,
	FLIP_GAP_MS = 100,
	FLIP_TIMEOUT_TICKS = 5000000,
	WATCH_MS = 250,
	// Two greys a shade apart, so the flips don't strobe.
	GREY_A = 0x606060FF,
	GREY_B = 0x707070FF,
};

// Registers that change once a frame; libxenon reads 0x6534's bit 12 as vblank (libxenon:drivers/xenos/xe.c:2141).
enum {
	UNKNOWN_194C = 0x194C,
	UNKNOWN_194D = 0x194D,
	UNKNOWN_1951 = 0x1951,
	VBLANK_BIT = 0x1000,
};

typedef struct {
	uint32_t ticks;
	uint32_t source;
	uint32_t status_6530;
	uint32_t status_6534;
	uint32_t status_6544;
	uint32_t update;
	uint32_t double_buffer;
} Sample;

typedef struct {
	uint32_t written;
	uint32_t cleared;
	uint32_t update_after_write;
	bool timed_out;
} Flip;

// Written only by the callback, until the main thread clears it.
static Sample samples[MAX_SAMPLES];
static volatile uint32_t sample_count;

static uint32_t ticks_now(void)
{
	uint32_t ticks;
	__asm__ volatile("mftb %0" : "=r"(ticks));
	return ticks;
}

// Runs in the kernel's interrupt handling, so it only records.
static void record_interrupt(uint32_t source, void *context)
{
	(void)context;
	const uint32_t i = sample_count;
	if (i >= MAX_SAMPLES)
		return;
	volatile const uint32_t *const r = probe_gpu_registers;
	samples[i] = (Sample){ .ticks = ticks_now(),
		.source = source,
		.status_6530 = r[UNKNOWN_194C],
		.status_6534 = r[UNKNOWN_194D],
		.status_6544 = r[UNKNOWN_1951],
		.update = r[D1GRPH_UPDATE],
		.double_buffer = r[D1CRTC_DOUBLE_BUFFER_CONTROL] };
	sample_count = i + 1;
}

// xsdk_display_show's writes, timed: the address, then 0x60EC, polled without sleeping until the display clears it.
static Flip flip_to(uint32_t address)
{
	volatile uint32_t *const r = probe_gpu_registers;
	r[UNKNOWN_0A10] = address;
	r[D1GRPH_PRIMARY_SURFACE_ADDRESS] = address;
	r[D1CRTC_DOUBLE_BUFFER_CONTROL] = 1;
	Flip flip = { .written = ticks_now(), .update_after_write = r[D1GRPH_UPDATE], .timed_out = false };
	while (r[D1CRTC_DOUBLE_BUFFER_CONTROL]) {
		if (ticks_now() - flip.written > FLIP_TIMEOUT_TICKS) {
			flip.timed_out = true;
			break;
		}
	}
	flip.cleared = ticks_now();
	r[D1CRTC_DOUBLE_BUFFER_CONTROL] = 0;
	return flip;
}

static uint32_t to_us(uint32_t ticks, uint32_t hz)
{
	return (uint32_t)((uint64_t)ticks * 1000000 / hz);
}

static void log_samples(uint32_t count, uint32_t hz)
{
	uint32_t in_vblank = 0;
	uint32_t shortest = UINT32_MAX;
	uint32_t longest = 0;
	for (uint32_t i = 0; i < count; i++) {
		in_vblank += (samples[i].status_6534 & VBLANK_BIT) != 0;
		if (i == 0)
			continue;
		const uint32_t gap = to_us(samples[i].ticks - samples[i - 1].ticks, hz);
		shortest = gap < shortest ? gap : shortest;
		longest = gap > longest ? gap : longest;
	}
	xsdk_log_write("vblank: %u callbacks, %u with 0x6534 bit 12 set; gaps %u to %u us", (unsigned)count,
		(unsigned)in_vblank, (unsigned)shortest, (unsigned)longest);
	for (uint32_t i = 0; i < count && i < SAMPLES_LOGGED; i++) {
		const Sample *const s = &samples[i];
		xsdk_log_write("vblank: %2u +%5u us source %u: 6530 %08X 6534 %08X 6544 %08X update %08X 60EC %08X",
			(unsigned)i, (unsigned)(i ? to_us(s->ticks - samples[i - 1].ticks, hz) : 0), (unsigned)s->source,
			(unsigned)s->status_6530, (unsigned)s->status_6534, (unsigned)s->status_6544, (unsigned)s->update,
			(unsigned)s->double_buffer);
	}
}

// Where each flip landed between the callbacks before and after it.
static void log_flips(const Flip *flips, uint32_t count, uint32_t hz)
{
	for (uint32_t f = 0; f < FLIPS; f++) {
		const Flip *const flip = &flips[f];
		uint32_t before = UINT32_MAX;
		uint32_t after = UINT32_MAX;
		uint32_t before_6534 = 0;
		for (uint32_t i = 0; i < count; i++) {
			const int32_t offset = (int32_t)(samples[i].ticks - flip->cleared);
			if (offset <= 0 && (uint32_t)-offset < before) {
				before = (uint32_t)-offset;
				before_6534 = samples[i].status_6534;
			}
			if (offset > 0 && (uint32_t)offset < after)
				after = (uint32_t)offset;
		}
		xsdk_log_write(
			"vblank: flip %u: 0x60EC cleared %u us after the write%s, update %08X after the write; the "
			"callbacks around it %u us before (6534 %08X) and %u us after",
			(unsigned)f, (unsigned)to_us(flip->cleared - flip->written, hz), flip->timed_out ? " (timed out)" : "",
			(unsigned)flip->update_after_write, (unsigned)to_us(before, hz), (unsigned)before_6534,
			(unsigned)to_us(after, hz));
	}
}

static void watch_xam(void)
{
	struct timespec start;
	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	const uint32_t first = probe_gpu_registers[CP_RB_WPTR];
	uint32_t last = first;
	uint32_t moves = 0;
	while (probe_elapsed_ms(&start) < WATCH_MS) {
		const uint32_t write = probe_gpu_registers[CP_RB_WPTR];
		if (write != last) {
			moves++;
			last = write;
		}
		(void)xsdk_thread_sleep(1);
	}
	xsdk_log_write("vblank: after giving the GPU back, XAM's write pointer moved %u times in %u ms (%08X to %08X)",
		(unsigned)moves, (unsigned)WATCH_MS, (unsigned)first, (unsigned)last);
}

static volatile uint32_t *grey_buffer(uint32_t grey)
{
	volatile uint32_t *const pixels = xsdk_gpu_alloc(PIXELS * 4);
	if (!pixels)
		return NULL;
	for (uint32_t i = 0; i < PIXELS; i++)
		pixels[i] = grey;
	__asm__ volatile("sync" ::: "memory");
	return pixels;
}

int main(void)
{
	xsdk_log_write("vblank probe");
	const uint32_t hz = KeQueryPerformanceFrequency();
	const NTSTATUS status = xsdk_gpu_open();
	uint32_t width = 0;
	uint32_t height = 0;
	const NTSTATUS display_status = NT_SUCCESS(status) ? xsdk_display_open(&width, &height) : status;
	volatile uint32_t *const buffers[2] = { grey_buffer(GREY_A), grey_buffer(GREY_B) };
	xsdk_log_write("vblank: time base %u Hz; GPU 0x%08X, display 0x%08X %ux%u", (unsigned)hz, (unsigned)status,
		(unsigned)display_status, (unsigned)width, (unsigned)height);
	if (!NT_SUCCESS(display_status) || !buffers[0] || !buffers[1])
		return 1;
	(void)xsdk_display_show(xsdk_gpu_address(buffers[0]), WIDTH);

	VdSetGraphicsInterruptCallback(record_interrupt, 0);
	Flip flips[FLIPS];
	for (uint32_t f = 0; f < FLIPS; f++) {
		(void)xsdk_thread_sleep(FLIP_GAP_MS);
		flips[f] = flip_to(xsdk_gpu_address(buffers[(f + 1) % 2]));
	}
	(void)xsdk_thread_sleep(FLIP_GAP_MS);
	// So the kernel doesn't call into the title once it unloads.
	VdSetGraphicsInterruptCallback(0, 0);
	const uint32_t count = sample_count;
	log_samples(count, hz);
	log_flips(flips, count, hz);

	xsdk_display_close();
	const NTSTATUS closed = xsdk_gpu_close();
	xsdk_log_write("vblank: GPU close 0x%08X", (unsigned)closed);
	watch_xam();
	return 0;
}
