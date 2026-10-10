#include <stdbool.h>
#include <stdint.h>

#include "probe_gpu.h"
#include "tiled_index.h"
#include "xsdk/display.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// Shows a 1280x720 image scaled to the console's output, with start phases from a formula, then zeroed, then restores.

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	PIXELS = WIDTH * HEIGHT,
	SCALED_MS = 10000,
	ZERO_PHASE_MS = 8000,
	DOUBLE_BUFFER_POLLS = 100000,
	CORNER = 32,
	BLOCK = 128,
	GREY = 0xFF404040,
	WHITE = 0xFFFFFFFF,
	BLACK = 0xFF000000,
	RED = 0xFFFF0000,
	GREEN = 0xFF00FF00,
	BLUE = 0xFF0000FF,
	YELLOW = 0xFFFFFF00,
	// The kernel's tap control at a 1920x1080 output (docs/HARDWARE.md, "The console outputs 1920 pixels wide").
	TAP_CONTROL = 0x505,
	SCALE_ONE_LOG2 = 24,
};

// Display registers by index: libxenon's names (libxenon:drivers/xenos/xenos.h:11-60), else Xenia's style.
enum {
	D1CRTC_H_TOTAL = 0x1800,
	D1CRTC_H_BLANK_START_END = 0x1801,
	AVIVO_D1MODE_VIEWPORT_START = 0x1960,
	AVIVO_D1SCL_SCALER_TAP_CONTROL = 0x1965,
	UNKNOWN_1968 = 0x1968,
	UNKNOWN_196C = 0x196C,
	UNKNOWN_196D = 0x196D,
	UNKNOWN_196E = 0x196E,
	UNKNOWN_1970 = 0x1970,
	UNKNOWN_1971 = 0x1971,
	UNKNOWN_1972 = 0x1972,
	AVIVO_D1SCL_UPDATE = 0x1973,
	UNKNOWN_1977 = 0x1977,
	UNKNOWN_197A = 0x197A,
};

enum {
	SCALER_ENABLE,
	SCALER_TAP_CONTROL,
	VIEWPORT_SIZE,
	VIEWPORT_WIDTH_QUARTER,
	VIEWPORT_START,
	REGISTER_1968,
	HORIZONTAL_SCALE,
	VERTICAL_SCALE,
	REGISTER_196C,
	REGISTER_1970,
	HORIZONTAL_PHASE,
	VERTICAL_PHASE,
	REGISTER_1977,
	SCALER_REGISTERS,
};

// The scaler's registers in libxenon's order under AVIVO_D1SCL_UPDATE (libxenon:drivers/xenos/xenos.c:381-417).
static const struct {
	const char *name;
	uint32_t index;
} scaler[SCALER_REGISTERS] = {
	{ "AVIVO_D1SCL_SCALER_ENABLE", AVIVO_D1SCL_SCALER_ENABLE },
	{ "AVIVO_D1SCL_SCALER_TAP_CONTROL", AVIVO_D1SCL_SCALER_TAP_CONTROL },
	{ "AVIVO_D1MODE_VIEWPORT_SIZE", AVIVO_D1MODE_VIEWPORT_SIZE },
	{ "UNKNOWN_197A", UNKNOWN_197A },
	{ "AVIVO_D1MODE_VIEWPORT_START", AVIVO_D1MODE_VIEWPORT_START },
	{ "UNKNOWN_1968", UNKNOWN_1968 },
	{ "UNKNOWN_196D", UNKNOWN_196D },
	{ "UNKNOWN_1971", UNKNOWN_1971 },
	{ "UNKNOWN_196C", UNKNOWN_196C },
	{ "UNKNOWN_1970", UNKNOWN_1970 },
	{ "UNKNOWN_196E", UNKNOWN_196E },
	{ "UNKNOWN_1972", UNKNOWN_1972 },
	{ "UNKNOWN_1977", UNKNOWN_1977 },
};

static void log_scaler(const char *when)
{
	for (uint32_t i = 0; i < SCALER_REGISTERS; i++)
		xsdk_log_write("720p: %s: %s %08X", when, scaler[i].name, (unsigned)probe_gpu_registers[scaler[i].index]);
}

static void log_display(void)
{
	VIDEO_MODE mode;
	VdQueryVideoMode(&mode);
	xsdk_log_write("720p: VdQueryVideoMode: %ux%u, interlaced %u, widescreen %u, hi-def %u, standard %u",
		(unsigned)mode.display_width, (unsigned)mode.display_height, (unsigned)mode.is_interlaced,
		(unsigned)mode.is_widescreen, (unsigned)mode.is_hi_def, (unsigned)mode.video_standard);
	DISPLAY_INFO info;
	VdGetCurrentDisplayInformation(&info);
	xsdk_log_write("720p: VdGetCurrentDisplayInformation: front buffer %ux%u, display %ux%u, interlaced %u",
		(unsigned)info.front_buffer_width, (unsigned)info.front_buffer_height, (unsigned)info.display_width,
		(unsigned)info.display_height, (unsigned)info.display_interlaced);
	for (uint32_t i = 0; i < sizeof info.scaler_parameters / sizeof info.scaler_parameters[0]; i++)
		xsdk_log_write("720p: scaler parameter %u: %08X", (unsigned)i, (unsigned)info.scaler_parameters[i]);
	xsdk_log_write("720p: D1CRTC_H_TOTAL %08X, D1CRTC_H_BLANK_START_END %08X",
		(unsigned)probe_gpu_registers[D1CRTC_H_TOTAL], (unsigned)probe_gpu_registers[D1CRTC_H_BLANK_START_END]);
}

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

// Source over output in 8.24, as the kernel's factors read: a third for 640 to 1920 (docs/HARDWARE.md).
static uint32_t scale(uint32_t source, uint32_t output)
{
	return (uint32_t)(((uint64_t)source << SCALE_ONE_LOG2) / output);
}

// (taps + 2) / 2 + scale / 2 in 16.16, which fits the kernel's and libxenon's phases (docs/HARDWARE.md).
static uint32_t phase(uint32_t taps, uint32_t scale_8_24)
{
	return ((taps + 2) << 15) + (scale_8_24 >> 9);
}

static void write_scaler(const uint32_t *values)
{
	volatile uint32_t *const r = probe_gpu_registers;
	r[AVIVO_D1SCL_UPDATE] = 1;
	for (uint32_t i = 0; i < SCALER_REGISTERS; i++)
		r[scaler[i].index] = values[i];
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

int main(void)
{
	xsdk_log_write("720p scaling probe");
	const NTSTATUS status = xsdk_gpu_open();
	uint32_t width = 0;
	uint32_t height = 0;
	const NTSTATUS display_status = NT_SUCCESS(status) ? xsdk_display_open(&width, &height) : status;
	volatile uint32_t *const pixels = xsdk_gpu_alloc(PIXELS * 4);
	xsdk_log_write("720p: GPU 0x%08X, display 0x%08X %ux%u", (unsigned)status, (unsigned)display_status,
		(unsigned)width, (unsigned)height);
	if (!NT_SUCCESS(display_status) || !pixels)
		return 1;
	log_display();
	log_scaler("at start");
	uint32_t saved[SCALER_REGISTERS];
	for (uint32_t i = 0; i < SCALER_REGISTERS; i++)
		saved[i] = probe_gpu_registers[scaler[i].index];
	VIDEO_MODE mode;
	VdQueryVideoMode(&mode);
	if (!mode.display_width || !mode.display_height)
		return 1;
	draw_image(pixels);
	const uint32_t address = xsdk_gpu_address(pixels);

	const uint32_t horizontal = scale(WIDTH, mode.display_width);
	const uint32_t vertical = scale(HEIGHT, mode.display_height);
	uint32_t values[SCALER_REGISTERS] = { 0 };
	values[SCALER_ENABLE] = 1;
	values[SCALER_TAP_CONTROL] = TAP_CONTROL;
	values[VIEWPORT_SIZE] = WIDTH << 16 | HEIGHT;
	values[VIEWPORT_WIDTH_QUARTER] = WIDTH / 4 - 1;
	values[HORIZONTAL_SCALE] = horizontal;
	values[VERTICAL_SCALE] = vertical;
	values[REGISTER_1970] = 1 << SCALE_ONE_LOG2;
	values[HORIZONTAL_PHASE] = phase(TAP_CONTROL >> 8 & 0xF, horizontal);
	values[VERTICAL_PHASE] = phase(TAP_CONTROL & 0xF, vertical);
	write_scaler(values);
	show(address);
	xsdk_log_write("720p: phase A, 1280x720 scaled to %ux%u with the formula's phases; %u ms",
		(unsigned)mode.display_width, (unsigned)mode.display_height, (unsigned)SCALED_MS);
	log_scaler("in phase A");
	(void)xsdk_thread_sleep(SCALED_MS);

	values[HORIZONTAL_PHASE] = 0;
	values[VERTICAL_PHASE] = 0;
	write_scaler(values);
	xsdk_log_write("720p: phase B, the same with both phases 0; %u ms", (unsigned)ZERO_PHASE_MS);
	(void)xsdk_thread_sleep(ZERO_PHASE_MS);

	write_scaler(saved);
	xsdk_display_close();
	log_scaler("restored");
	xsdk_gpu_free(pixels);
	xsdk_log_write("720p: done; the GPU goes back at exit");
	return 0;
}
