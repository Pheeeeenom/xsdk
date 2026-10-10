#include <stdbool.h>
#include <stdint.h>

#include "probe_gpu.h"
#include "tiled_index.h"
#include "xsdk/display.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// Scales a 1280x720 image with fewer and fewer vertical taps, then with the 2-tap built-in filter, each its own tint.

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
	// SCL_H/V_2TAP_HARDCODE_COEF_EN (linux:drivers/gpu/drm/amd/include/asic_reg/dce/dce_8_0_sh_mask.h).
	TWO_TAP_BUILT_IN = 0x100,
};

// Display registers by index: libxenon's names (libxenon:drivers/xenos/xenos.h:11-60), else Xenia's style.
enum {
	UNKNOWN_1948 = 0x1948,
	UNKNOWN_1949 = 0x1949,
	UNKNOWN_1952 = 0x1952,
	UNKNOWN_1954 = 0x1954,
	AVIVO_D1SCL_SCALER_TAP_CONTROL = 0x1965,
	UNKNOWN_1968 = 0x1968,
	UNKNOWN_1969 = 0x1969,
	UNKNOWN_196C = 0x196C,
	UNKNOWN_196D = 0x196D,
	UNKNOWN_196E = 0x196E,
	UNKNOWN_196F = 0x196F,
	UNKNOWN_1970 = 0x1970,
	UNKNOWN_1971 = 0x1971,
	UNKNOWN_1972 = 0x1972,
	AVIVO_D1SCL_UPDATE = 0x1973,
	UNKNOWN_1974 = 0x1974,
	UNKNOWN_1977 = 0x1977,
	UNKNOWN_1978 = 0x1978,
	UNKNOWN_1979 = 0x1979,
	UNKNOWN_197A = 0x197A,
	UNKNOWN_1B2F = 0x1B2F,
	UNKNOWN_1C46 = 0x1C46,
};

enum {
	SCALER_ENABLE,
	SCALER_TAP_CONTROL,
	VIEWPORT_SIZE,
	VIEWPORT_WIDTH_QUARTER,
	HORIZONTAL_FILTER,
	HORIZONTAL_SCALE,
	HORIZONTAL_PHASE,
	VERTICAL_FILTER,
	VERTICAL_SCALE,
	VERTICAL_PHASE,
	WRITTEN,
};

// The hand-set scaling's registers, which gave the display back, and the filter controls (docs/HARDWARE.md).
static const uint32_t written[WRITTEN] = { AVIVO_D1SCL_SCALER_ENABLE, AVIVO_D1SCL_SCALER_TAP_CONTROL,
	AVIVO_D1MODE_VIEWPORT_SIZE, UNKNOWN_197A, UNKNOWN_196C, UNKNOWN_196D, UNKNOWN_196E, UNKNOWN_1970, UNKNOWN_1971,
	UNKNOWN_1972 };

// Read only: line buffer, underflow and filter registers (xenon-emu:XenosRegisters.h:906-952), with 0x1948 and 0x1969.
static const uint32_t logged[] = { UNKNOWN_1948, UNKNOWN_1949, UNKNOWN_1952, UNKNOWN_1954, UNKNOWN_1968, UNKNOWN_1969,
	UNKNOWN_196C, UNKNOWN_196F, UNKNOWN_1970, AVIVO_D1SCL_UPDATE, UNKNOWN_1974, UNKNOWN_1977, UNKNOWN_1978,
	UNKNOWN_1979, UNKNOWN_197A, UNKNOWN_1B2F, UNKNOWN_1C46 };

// The status registers among them, read again after each variant.
static const uint32_t statuses[] = { UNKNOWN_1949, AVIVO_D1SCL_UPDATE, UNKNOWN_1974, UNKNOWN_1978 };

typedef struct {
	const char *name;
	// Each variant's background, so a capture shows which one is on screen.
	uint32_t background_argb;
	bool to_output;
	// Horizontal taps less one in bits 8-11, vertical in bits 0-2 (dce_8_0_sh_mask.h, dce_transform.c).
	uint32_t tap_control;
	bool built_in;
} Variant;

static const Variant variants[] = {
	{ "grey: 1:1, 6 taps each way", 0xFF404040, false, 0x505, false },
	{ "blue: scaled, 6 taps each way, as before", 0xFF202060, true, 0x505, false },
	{ "green: scaled, 6 horizontal and 4 vertical taps", 0xFF206020, true, 0x503, false },
	{ "red: scaled, 6 horizontal and 3 vertical taps", 0xFF602020, true, 0x502, false },
	{ "olive: scaled, 6 horizontal and 2 vertical taps", 0xFF606020, true, 0x501, false },
	{ "teal: scaled, 2 horizontal and 6 vertical taps", 0xFF206060, true, 0x105, false },
	{ "purple: scaled, the built-in 2-tap filter each way", 0xFF602060, true, 0x101, true },
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

// Source over output in 8.24 (docs/HARDWARE.md, "Scaling 1280x720 by hand").
static uint32_t scale(uint32_t source, uint32_t output)
{
	return (uint32_t)(((uint64_t)source << SCALE_ONE_LOG2) / output);
}

// (scale + taps + 1) / 2 in 16.16 (dce_transform.c, calculate_inits), the field being the taps less one.
static uint32_t phase(uint32_t tap_field, uint32_t scale_8_24)
{
	return ((tap_field + 2) << 15) + (scale_8_24 >> 9);
}

static uint32_t filter_control(uint32_t start, bool built_in)
{
	return (start & ~(uint32_t)TWO_TAP_BUILT_IN) | (built_in ? TWO_TAP_BUILT_IN : 0);
}

static void write_scaler(const uint32_t *values)
{
	volatile uint32_t *const r = probe_gpu_registers;
	r[AVIVO_D1SCL_UPDATE] = 1;
	for (uint32_t i = 0; i < WRITTEN; i++)
		r[written[i]] = values[i];
	r[AVIVO_D1SCL_UPDATE] = 0;
}

// The log names each register by index and by byte offset, as the earlier logs did.
static void log_registers(const char *when, const uint32_t *list, uint32_t count)
{
	for (uint32_t i = 0; i < count; i++)
		xsdk_log_write("720p: %s: %04X (byte %04X) %08X", when, (unsigned)list[i], (unsigned)(list[i] * 4),
			(unsigned)probe_gpu_registers[list[i]]);
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

static void run_variant(
	const Variant *variant, const VIDEO_MODE *mode, const uint32_t *start, volatile uint32_t *pixels)
{
	const uint32_t output_width = variant->to_output ? mode->display_width : WIDTH;
	const uint32_t output_height = variant->to_output ? mode->display_height : HEIGHT;
	uint32_t values[WRITTEN];
	values[SCALER_ENABLE] = 1;
	values[SCALER_TAP_CONTROL] = variant->tap_control;
	values[VIEWPORT_SIZE] = WIDTH << 16 | HEIGHT;
	values[VIEWPORT_WIDTH_QUARTER] = WIDTH / 4 - 1;
	values[HORIZONTAL_FILTER] = filter_control(start[HORIZONTAL_FILTER], variant->built_in);
	values[VERTICAL_FILTER] = filter_control(start[VERTICAL_FILTER], variant->built_in);
	values[HORIZONTAL_SCALE] = scale(WIDTH, output_width);
	values[VERTICAL_SCALE] = scale(HEIGHT, output_height);
	values[HORIZONTAL_PHASE] = phase(variant->tap_control >> 8 & 0xF, values[HORIZONTAL_SCALE]);
	values[VERTICAL_PHASE] = phase(variant->tap_control & 0x7, values[VERTICAL_SCALE]);
	draw_image(pixels, variant->background_argb);
	write_scaler(values);
	xsdk_log_write("720p: %s: taps %03X, filters %08X %08X, scale %08X by %08X, phases %08X and %08X; %u ms",
		variant->name, (unsigned)values[SCALER_TAP_CONTROL], (unsigned)values[HORIZONTAL_FILTER],
		(unsigned)values[VERTICAL_FILTER], (unsigned)values[HORIZONTAL_SCALE], (unsigned)values[VERTICAL_SCALE],
		(unsigned)values[HORIZONTAL_PHASE], (unsigned)values[VERTICAL_PHASE], (unsigned)VARIANT_MS);
	xsdk_log_write("720p: read back: taps %03X, filters %08X %08X, scale %08X by %08X, phases %08X and %08X",
		(unsigned)probe_gpu_registers[AVIVO_D1SCL_SCALER_TAP_CONTROL], (unsigned)probe_gpu_registers[UNKNOWN_196C],
		(unsigned)probe_gpu_registers[UNKNOWN_1970], (unsigned)probe_gpu_registers[UNKNOWN_196D],
		(unsigned)probe_gpu_registers[UNKNOWN_1971], (unsigned)probe_gpu_registers[UNKNOWN_196E],
		(unsigned)probe_gpu_registers[UNKNOWN_1972]);
	(void)xsdk_thread_sleep(VARIANT_MS);
	log_registers("after it", statuses, sizeof statuses / sizeof statuses[0]);
}

int main(void)
{
	xsdk_log_write("720p line buffer probe");
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
	log_registers("at start", logged, sizeof logged / sizeof logged[0]);
	uint32_t start[WRITTEN];
	for (uint32_t i = 0; i < WRITTEN; i++)
		start[i] = probe_gpu_registers[written[i]];
	draw_image(pixels, variants[0].background_argb);
	show(xsdk_gpu_address(pixels));
	for (uint32_t i = 0; i < sizeof variants / sizeof variants[0]; i++)
		run_variant(&variants[i], &mode, start, pixels);
	write_scaler(start);
	log_registers("put back", written, WRITTEN);
	xsdk_display_close();
	xsdk_gpu_free(pixels);
	xsdk_log_write("720p: the scaler put back; the GPU goes back at exit");
	return 0;
}
