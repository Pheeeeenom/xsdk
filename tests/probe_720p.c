#include <stdbool.h>
#include <stdint.h>

#include "probe_gpu.h"
#include "tiled_index.h"
#include "xsdk/display.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// Logs the kernel's scaler registers, then scales a 1280x720 image to the output without and then with 0x65BC set.

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	PIXELS = WIDTH * HEIGHT,
	PHASE_MS = 10000,
	DOUBLE_BUFFER_POLLS = 100000,
	MAX_COMMANDS = 512,
	UNKNOWN_DWORDS_LOGGED = 32,
	COMMANDS_LOGGED = 64,
	CORNER = 32,
	BLOCK = 128,
	GREY = 0xFF404040,
	WHITE = 0xFFFFFFFF,
	BLACK = 0xFF000000,
	RED = 0xFFFF0000,
	GREEN = 0xFF00FF00,
	BLUE = 0xFF0000FF,
	YELLOW = 0xFFFFFF00,
	// The kernel's filter type and taps here (docs/HARDWARE.md, "The console outputs 1920 pixels wide").
	FILTER_TYPE = 1,
	TAP_CONTROL = 0x505,
	SCALE_ONE_LOG2 = 24,
	TYPE2_NOP = 0x80000000,
};

// Display registers by index: libxenon's names (libxenon:drivers/xenos/xenos.h:11-60), else Xenia's style.
enum {
	AVIVO_D1SCL_SCALER_TAP_CONTROL = 0x1965,
	UNKNOWN_196D = 0x196D,
	UNKNOWN_196E = 0x196E,
	UNKNOWN_196F = 0x196F,
	UNKNOWN_1971 = 0x1971,
	UNKNOWN_1972 = 0x1972,
	AVIVO_D1SCL_UPDATE = 0x1973,
	UNKNOWN_197A = 0x197A,
};

// The kernel's scaler packets' registers, less 0x6044-0x6050 and 0x6578, read back untrusted (docs/HARDWARE.md).
static const uint32_t kernel_set[] = { 0x1964, 0x1965, 0x194A, 0x1960, 0x1961, 0x1848, 0x1849, 0x184A, 0x184B, 0x184C,
	0x184D, 0x184E, 0x197A, 0x1968, 0x196C, 0x196D, 0x196E, 0x196F, 0x1970, 0x1971, 0x1972, 0x1977 };

enum {
	SCALER_ENABLE,
	SCALER_TAP_CONTROL,
	VIEWPORT_SIZE,
	VIEWPORT_WIDTH_QUARTER,
	HORIZONTAL_SCALE,
	HORIZONTAL_PHASE,
	REGISTER_196F,
	VERTICAL_SCALE,
	VERTICAL_PHASE,
	WRITTEN,
};

// Only scaler registers the hand-set scaling wrote before, which gave the display back, and 0x65BC beside them.
static const uint32_t written[WRITTEN] = { AVIVO_D1SCL_SCALER_ENABLE, AVIVO_D1SCL_SCALER_TAP_CONTROL,
	AVIVO_D1MODE_VIEWPORT_SIZE, UNKNOWN_197A, UNKNOWN_196D, UNKNOWN_196E, UNKNOWN_196F, UNKNOWN_1971, UNKNOWN_1972 };

static uint32_t commands[MAX_COMMANDS];
static uint32_t unknown[MAX_COMMANDS];

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
static void draw_image(volatile uint32_t *pixels)
{
	for (uint32_t y = 0; y < HEIGHT; y++)
		for (uint32_t x = 0; x < WIDTH; x++)
			put(pixels, x, y, x == 0 || y == 0 || x == WIDTH - 1 || y == HEIGHT - 1 ? WHITE : GREY);
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

// Asks the kernel for its packets again, only to log them and the buffer it was handed; nothing is applied.
static void log_kernel_packets(const VIDEO_MODE *mode)
{
	const D3DFILTER_PARAMETERS filter = { .nyquist = 0.0f, .flicker_filter = 0.0f, .beta = 0.0f };
	for (uint32_t i = 0; i < MAX_COMMANDS; i++)
		commands[i] = TYPE2_NOP;
	const uint32_t count =
		VdInitializeScalerCommandBuffer(0, HEIGHT << 16 | WIDTH, 0, mode->display_height << 16 | mode->display_width,
			HEIGHT << 16 | WIDTH, FILTER_TYPE, &filter, FILTER_TYPE, &filter, unknown, commands, MAX_COMMANDS);
	xsdk_log_write("720p: VdInitializeScalerCommandBuffer returned %u", (unsigned)count);
	for (uint32_t i = 0; i < count && i < COMMANDS_LOGGED; i++) {
		if (commands[i] != TYPE2_NOP)
			xsdk_log_write("720p: command %u: %08X", (unsigned)i, (unsigned)commands[i]);
	}
	for (uint32_t i = 0; i < UNKNOWN_DWORDS_LOGGED; i++)
		xsdk_log_write("720p: unknown %u: %08X", (unsigned)i, (unsigned)unknown[i]);
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

static void write_scaler(const uint32_t *values, bool with_196f)
{
	volatile uint32_t *const r = probe_gpu_registers;
	r[AVIVO_D1SCL_UPDATE] = 1;
	for (uint32_t i = 0; i < WRITTEN; i++) {
		if (i != REGISTER_196F || with_196f)
			r[written[i]] = values[i];
	}
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

// What 0x65BC should take: whichever of our values matches the kernel's register that it matched, else our H scale.
static uint32_t pick_196f(const uint32_t *start, const uint32_t *values)
{
	static const uint32_t candidates[] = { HORIZONTAL_SCALE, HORIZONTAL_PHASE, VERTICAL_SCALE, VERTICAL_PHASE };
	for (uint32_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
		if (start[REGISTER_196F] == start[candidates[i]]) {
			xsdk_log_write("720p: 0x65BC started equal to %04X, so it takes our %08X", (unsigned)written[candidates[i]],
				(unsigned)values[candidates[i]]);
			return values[candidates[i]];
		}
	}
	xsdk_log_write("720p: 0x65BC matched none of them, so it takes our horizontal scale as a guess");
	return values[HORIZONTAL_SCALE];
}

int main(void)
{
	xsdk_log_write("720p 0x65BC probe");
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
	for (uint32_t i = 0; i < sizeof kernel_set / sizeof kernel_set[0]; i++)
		xsdk_log_write("720p: at start: %04X (byte %04X) %08X", (unsigned)kernel_set[i], (unsigned)(kernel_set[i] * 4),
			(unsigned)probe_gpu_registers[kernel_set[i]]);
	log_kernel_packets(&mode);
	draw_image(pixels);

	uint32_t start[WRITTEN];
	for (uint32_t i = 0; i < WRITTEN; i++)
		start[i] = probe_gpu_registers[written[i]];
	uint32_t values[WRITTEN];
	values[SCALER_ENABLE] = 1;
	values[SCALER_TAP_CONTROL] = TAP_CONTROL;
	values[VIEWPORT_SIZE] = WIDTH << 16 | HEIGHT;
	values[VIEWPORT_WIDTH_QUARTER] = WIDTH / 4 - 1;
	values[HORIZONTAL_SCALE] = scale(WIDTH, mode.display_width);
	values[VERTICAL_SCALE] = scale(HEIGHT, mode.display_height);
	values[HORIZONTAL_PHASE] = phase(TAP_CONTROL >> 8 & 0xF, values[HORIZONTAL_SCALE]);
	values[VERTICAL_PHASE] = phase(TAP_CONTROL & 0xF, values[VERTICAL_SCALE]);
	values[REGISTER_196F] = pick_196f(start, values);

	write_scaler(values, false);
	show(xsdk_gpu_address(pixels));
	xsdk_log_write("720p: phase A, scaled as before with 0x65BC left at %08X; %u ms", (unsigned)start[REGISTER_196F],
		(unsigned)PHASE_MS);
	(void)xsdk_thread_sleep(PHASE_MS);
	write_scaler(values, true);
	xsdk_log_write("720p: phase B, with 0x65BC %08X; %u ms", (unsigned)values[REGISTER_196F], (unsigned)PHASE_MS);
	(void)xsdk_thread_sleep(PHASE_MS);

	write_scaler(start, true);
	xsdk_display_close();
	xsdk_gpu_free(pixels);
	xsdk_log_write("720p: the scaler put back; the GPU goes back at exit");
	return 0;
}
