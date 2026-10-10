#include <stdbool.h>
#include <stdint.h>

#include "probe_gpu.h"
#include "tiled_index.h"
#include "xsdk/display.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// Scales a 1280x720 image with the scaler on at 1:1, then to the output with fewer and fewer taps, each its own tint.

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	PIXELS = WIDTH * HEIGHT,
	VARIANT_MS = 6000,
	DOUBLE_BUFFER_POLLS = 100000,
	CORNER = 32,
	BLOCK = 128,
	WHITE = 0xFFFFFFFF,
	BLACK = 0xFF000000,
	RED = 0xFFFF0000,
	GREEN = 0xFF00FF00,
	BLUE = 0xFF0000FF,
	YELLOW = 0xFFFFFF00,
	SCALE_ONE_LOG2 = 24,
};

// Display registers by index: libxenon's names (libxenon:drivers/xenos/xenos.h:11-60), else Xenia's style.
enum {
	AVIVO_D1SCL_SCALER_TAP_CONTROL = 0x1965,
	UNKNOWN_196D = 0x196D,
	UNKNOWN_196E = 0x196E,
	UNKNOWN_1971 = 0x1971,
	UNKNOWN_1972 = 0x1972,
	AVIVO_D1SCL_UPDATE = 0x1973,
	UNKNOWN_197A = 0x197A,
};

enum {
	SCALER_ENABLE,
	SCALER_TAP_CONTROL,
	VIEWPORT_SIZE,
	VIEWPORT_WIDTH_QUARTER,
	HORIZONTAL_SCALE,
	HORIZONTAL_PHASE,
	VERTICAL_SCALE,
	VERTICAL_PHASE,
	WRITTEN,
};

// Only the scaler registers the hand-set scaling wrote, which gave the display back (docs/HARDWARE.md).
static const uint32_t written[WRITTEN] = { AVIVO_D1SCL_SCALER_ENABLE, AVIVO_D1SCL_SCALER_TAP_CONTROL,
	AVIVO_D1MODE_VIEWPORT_SIZE, UNKNOWN_197A, UNKNOWN_196D, UNKNOWN_196E, UNKNOWN_1971, UNKNOWN_1972 };

typedef struct {
	const char *name;
	// Each variant's background, so a capture shows which one is on screen.
	uint32_t background_argb;
	bool to_output;
	// Horizontal taps in bits 8-11, vertical in bits 0-3, as the phases that fit read them (docs/HARDWARE.md).
	uint32_t tap_control;
} Variant;

static const Variant variants[] = {
	{ "grey: scaler on at 1:1, taps 0x505", 0xFF404040, false, 0x505 },
	{ "blue: scaled to the output, taps 0x505 as before", 0xFF202060, true, 0x505 },
	{ "green: scaled, 4 vertical taps", 0xFF206020, true, 0x504 },
	{ "red: scaled, 3 vertical taps", 0xFF602020, true, 0x503 },
	{ "olive: scaled, 2 vertical taps", 0xFF606020, true, 0x502 },
	{ "teal: scaled, 2 horizontal taps", 0xFF206060, true, 0x205 },
	{ "purple: scaled, 2 taps each way", 0xFF602060, true, 0x202 },
};

// The display reads a word's bytes as B, G, R, A (docs/HARDWARE.md, "A title's buffer on screen").
static void put(volatile uint32_t *pixels, uint32_t x, uint32_t y, uint32_t argb)
{
	pixels[tiled_index(x, y, WIDTH)] = __builtin_bswap32(argb);
}

static void fill(volatile uint32_t *pixels, uint32_t x0, uint32_t y0, uint32_t size, uint32_t argb)
{
	for (uint32_t y = y0; y < y0 + size; y++)
		for (uint32_t x = x0; x < x0 + size; x++)
			put(pixels, x, y, argb);
}

// A one-pixel white border, a square in each corner, and blocks of one-pixel stripes and checks in the middle.
static void draw_image(volatile uint32_t *pixels, uint32_t background_argb)
{
	for (uint32_t y = 0; y < HEIGHT; y++)
		for (uint32_t x = 0; x < WIDTH; x++)
			put(pixels, x, y, x == 0 || y == 0 || x == WIDTH - 1 || y == HEIGHT - 1 ? WHITE : background_argb);
	fill(pixels, 1, 1, CORNER, RED);
	fill(pixels, WIDTH - 1 - CORNER, 1, CORNER, GREEN);
	fill(pixels, 1, HEIGHT - 1 - CORNER, CORNER, BLUE);
	fill(pixels, WIDTH - 1 - CORNER, HEIGHT - 1 - CORNER, CORNER, YELLOW);
	const uint32_t top = HEIGHT / 2 - BLOCK / 2;
	for (uint32_t y = 0; y < BLOCK; y++) {
		for (uint32_t x = 0; x < BLOCK; x++) {
			put(pixels, WIDTH / 2 - BLOCK * 3 / 2 + x, top + y, x & 1 ? WHITE : BLACK);
			put(pixels, WIDTH / 2 - BLOCK / 2 + x, top + y, y & 1 ? WHITE : BLACK);
			put(pixels, WIDTH / 2 + BLOCK / 2 + x, top + y, (x ^ y) & 1 ? WHITE : BLACK);
		}
	}
	__asm__ volatile("sync" ::: "memory");
}

// Source over output in 8.24, and (taps + 2) / 2 + scale / 2 in 16.16 (docs/HARDWARE.md, "Scaling 1280x720 by hand").
static uint32_t scale(uint32_t source, uint32_t output)
{
	return (uint32_t)(((uint64_t)source << SCALE_ONE_LOG2) / output);
}

static uint32_t phase(uint32_t taps, uint32_t scale_8_24)
{
	return ((taps + 2) << 15) + (scale_8_24 >> 9);
}

static void write_scaler(const uint32_t *values)
{
	volatile uint32_t *const r = probe_gpu_registers;
	r[AVIVO_D1SCL_UPDATE] = 1;
	for (uint32_t i = 0; i < WRITTEN; i++)
		r[written[i]] = values[i];
	r[AVIVO_D1SCL_UPDATE] = 0;
}

// xsdk_display_show's writes for a WIDTH by HEIGHT surface, then 0x60EC.
static void show(uint32_t address)
{
	volatile uint32_t *const r = probe_gpu_registers;
	r[UNKNOWN_0A10] = address;
	r[UNKNOWN_0A11] = WIDTH;
	r[D1GRPH_PRIMARY_SURFACE_ADDRESS] = address;
	r[D1GRPH_PITCH] = WIDTH;
	r[D1GRPH_X_END] = WIDTH;
	r[D1GRPH_Y_END] = HEIGHT;
	r[D1CRTC_DOUBLE_BUFFER_CONTROL] = 1;
	for (uint32_t polls = 0; r[D1CRTC_DOUBLE_BUFFER_CONTROL] && polls < DOUBLE_BUFFER_POLLS; polls++)
		;
	r[D1CRTC_DOUBLE_BUFFER_CONTROL] = 0;
}

static void run_variant(const Variant *variant, const VIDEO_MODE *mode, volatile uint32_t *pixels)
{
	const uint32_t output_width = variant->to_output ? mode->display_width : WIDTH;
	const uint32_t output_height = variant->to_output ? mode->display_height : HEIGHT;
	uint32_t values[WRITTEN];
	values[SCALER_ENABLE] = 1;
	values[SCALER_TAP_CONTROL] = variant->tap_control;
	values[VIEWPORT_SIZE] = WIDTH << 16 | HEIGHT;
	values[VIEWPORT_WIDTH_QUARTER] = WIDTH / 4 - 1;
	values[HORIZONTAL_SCALE] = scale(WIDTH, output_width);
	values[VERTICAL_SCALE] = scale(HEIGHT, output_height);
	values[HORIZONTAL_PHASE] = phase(variant->tap_control >> 8 & 0xF, values[HORIZONTAL_SCALE]);
	values[VERTICAL_PHASE] = phase(variant->tap_control & 0xF, values[VERTICAL_SCALE]);
	draw_image(pixels, variant->background_argb);
	write_scaler(values);
	xsdk_log_write("720p: %s: scale %08X by %08X, phases %08X and %08X, read back %08X %08X %08X %08X; %u ms",
		variant->name, (unsigned)values[HORIZONTAL_SCALE], (unsigned)values[VERTICAL_SCALE],
		(unsigned)values[HORIZONTAL_PHASE], (unsigned)values[VERTICAL_PHASE],
		(unsigned)probe_gpu_registers[UNKNOWN_196D], (unsigned)probe_gpu_registers[UNKNOWN_1971],
		(unsigned)probe_gpu_registers[UNKNOWN_196E], (unsigned)probe_gpu_registers[UNKNOWN_1972], (unsigned)VARIANT_MS);
	(void)xsdk_thread_sleep(VARIANT_MS);
}

int main(void)
{
	xsdk_log_write("720p taps probe");
	const NTSTATUS status = xsdk_gpu_open();
	uint32_t width = 0;
	uint32_t height = 0;
	const NTSTATUS display_status = NT_SUCCESS(status) ? xsdk_display_open(&width, &height) : status;
	volatile uint32_t *const pixels = xsdk_gpu_alloc(PIXELS * 4);
	VIDEO_MODE mode;
	VdQueryVideoMode(&mode);
	xsdk_log_write("720p: GPU 0x%08X, display 0x%08X %ux%u, output %ux%u", (unsigned)status, (unsigned)display_status,
		(unsigned)width, (unsigned)height, (unsigned)mode.display_width, (unsigned)mode.display_height);
	if (!NT_SUCCESS(display_status) || !pixels || !mode.display_width || !mode.display_height)
		return 1;
	uint32_t start[WRITTEN];
	for (uint32_t i = 0; i < WRITTEN; i++)
		start[i] = probe_gpu_registers[written[i]];
	draw_image(pixels, variants[0].background_argb);
	show(xsdk_gpu_address(pixels));
	for (uint32_t i = 0; i < sizeof variants / sizeof variants[0]; i++)
		run_variant(&variants[i], &mode, pixels);
	write_scaler(start);
	xsdk_display_close();
	xsdk_gpu_free(pixels);
	xsdk_log_write("720p: the scaler put back; the GPU goes back at exit");
	return 0;
}
