#include <stdbool.h>
#include <stdint.h>

#include "probe_gpu.h"
#include "tiled_index.h"
#include "xsdk/display.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// Shows a 1280x720 test image in today's 640x480 viewport, then 1:1 with libxenon's scaler settings, then restores.

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	PIXELS = WIDTH * HEIGHT,
	SCALED_MS = 8000,
	NATIVE_MS = 12000,
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
};

// Display registers by index: libxenon's names (libxenon:drivers/xenos/xenos.h:11-60), else Xenia's style.
enum {
	D1CRTC_H_TOTAL = 0x1800,
	D1CRTC_H_BLANK_START_END = 0x1801,
	D1CRTC_V_TOTAL = 0x1808,
	AVIVO_D1CRTC_V_BLANK_START_END = 0x1809,
	UNKNOWN_0A12 = 0x0A12,
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

typedef struct {
	const char *name;
	uint32_t index;
	// libxenon's value for a 1:1 mode of WIDTH by HEIGHT (libxenon:drivers/xenos/xenos.c:381-417).
	uint32_t native;
} Register;

// The scaler's registers libxenon sets for a mode shown 1:1, written in its order under AVIVO_D1SCL_UPDATE.
static const Register scaler[] = {
	{ "AVIVO_D1SCL_SCALER_ENABLE", AVIVO_D1SCL_SCALER_ENABLE, 0 },
	{ "AVIVO_D1SCL_SCALER_TAP_CONTROL", AVIVO_D1SCL_SCALER_TAP_CONTROL, 0x905 },
	{ "AVIVO_D1MODE_VIEWPORT_SIZE", AVIVO_D1MODE_VIEWPORT_SIZE, WIDTH << 16 | HEIGHT },
	{ "UNKNOWN_197A", UNKNOWN_197A, WIDTH / 4 - 1 },
	{ "AVIVO_D1MODE_VIEWPORT_START", AVIVO_D1MODE_VIEWPORT_START, 0 },
	{ "UNKNOWN_1968", UNKNOWN_1968, 0 },
	{ "UNKNOWN_196D", UNKNOWN_196D, 0x01000000 },
	{ "UNKNOWN_1971", UNKNOWN_1971, 0x01000000 },
	{ "UNKNOWN_196C", UNKNOWN_196C, 0 },
	{ "UNKNOWN_1970", UNKNOWN_1970, 0x01000000 },
	{ "UNKNOWN_196E", UNKNOWN_196E, 0x00060000 },
	{ "UNKNOWN_1972", UNKNOWN_1972, 0x00040000 },
	{ "UNKNOWN_1977", UNKNOWN_1977, 0 },
};

static const Register timing[] = {
	{ "D1CRTC_H_TOTAL", D1CRTC_H_TOTAL, 0 },
	{ "D1CRTC_H_BLANK_START_END", D1CRTC_H_BLANK_START_END, 0 },
	{ "D1CRTC_V_TOTAL", D1CRTC_V_TOTAL, 0 },
	{ "AVIVO_D1CRTC_V_BLANK_START_END", AVIVO_D1CRTC_V_BLANK_START_END, 0 },
	{ "UNKNOWN_0A12", UNKNOWN_0A12, 0 },
};

static uint32_t saved[sizeof scaler / sizeof scaler[0]];

static void log_registers(const char *when)
{
	for (uint32_t i = 0; i < sizeof timing / sizeof timing[0]; i++)
		xsdk_log_write("720p: %s: %s %08X", when, timing[i].name, (unsigned)probe_gpu_registers[timing[i].index]);
	for (uint32_t i = 0; i < sizeof scaler / sizeof scaler[0]; i++)
		xsdk_log_write("720p: %s: %s %08X", when, scaler[i].name, (unsigned)probe_gpu_registers[scaler[i].index]);
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

static void write_scaler(const uint32_t *values)
{
	volatile uint32_t *const r = probe_gpu_registers;
	r[AVIVO_D1SCL_UPDATE] = 1;
	for (uint32_t i = 0; i < sizeof scaler / sizeof scaler[0]; i++)
		r[scaler[i].index] = values[i];
	r[AVIVO_D1SCL_UPDATE] = 0;
}

// xsdk_display_show's writes for a WIDTH by HEIGHT surface, then 0x60EC.
static uint32_t show_native(uint32_t address)
{
	volatile uint32_t *const r = probe_gpu_registers;
	r[UNKNOWN_0A10] = address;
	r[UNKNOWN_0A11] = WIDTH;
	r[D1GRPH_PRIMARY_SURFACE_ADDRESS] = address;
	r[D1GRPH_PITCH] = WIDTH;
	r[D1GRPH_X_END] = WIDTH;
	r[D1GRPH_Y_END] = HEIGHT;
	r[D1CRTC_DOUBLE_BUFFER_CONTROL] = 1;
	uint32_t polls = 0;
	while (r[D1CRTC_DOUBLE_BUFFER_CONTROL] && polls < DOUBLE_BUFFER_POLLS)
		polls++;
	r[D1CRTC_DOUBLE_BUFFER_CONTROL] = 0;
	return polls;
}

int main(void)
{
	xsdk_log_write("720p probe");
	const NTSTATUS status = xsdk_gpu_open();
	uint32_t width = 0;
	uint32_t height = 0;
	const NTSTATUS display_status = NT_SUCCESS(status) ? xsdk_display_open(&width, &height) : status;
	volatile uint32_t *const pixels = xsdk_gpu_alloc(PIXELS * 4);
	xsdk_log_write("720p: GPU 0x%08X, display 0x%08X %ux%u", (unsigned)status, (unsigned)display_status,
		(unsigned)width, (unsigned)height);
	if (!NT_SUCCESS(display_status) || !pixels)
		return 1;
	draw_image(pixels);
	const uint32_t address = xsdk_gpu_address(pixels);
	log_registers("at start");
	for (uint32_t i = 0; i < sizeof scaler / sizeof scaler[0]; i++)
		saved[i] = probe_gpu_registers[scaler[i].index];

	const NTSTATUS shown = xsdk_display_show(address, WIDTH);
	xsdk_log_write("720p: phase A, the image in the %ux%u viewport, scaled: 0x%08X; %u ms", (unsigned)width,
		(unsigned)height, (unsigned)shown, (unsigned)SCALED_MS);
	(void)xsdk_thread_sleep(SCALED_MS);

	uint32_t native[sizeof scaler / sizeof scaler[0]];
	for (uint32_t i = 0; i < sizeof scaler / sizeof scaler[0]; i++)
		native[i] = scaler[i].native;
	write_scaler(native);
	const uint32_t polls = show_native(address);
	xsdk_log_write("720p: phase B, the image 1:1 with libxenon's scaler settings; 0x60EC cleared after %u reads; %u ms",
		(unsigned)polls, (unsigned)NATIVE_MS);
	log_registers("in phase B");
	(void)xsdk_thread_sleep(NATIVE_MS);

	write_scaler(saved);
	xsdk_display_close();
	log_registers("restored");
	xsdk_gpu_free(pixels);
	xsdk_log_write("720p: done; the GPU goes back at exit");
	return 0;
}
