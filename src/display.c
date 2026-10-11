#include "xsdk/display.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "gpu_private.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// Display registers by index: libxenon's names (libxenon:drivers/xenos/xenos.h:11-54), else Xenia's style.
enum {
	UNKNOWN_0A10 = 0x0A10,
	UNKNOWN_0A11 = 0x0A11,
	D1CRTC_DOUBLE_BUFFER_CONTROL = 0x183B,
	D1GRPH_PRIMARY_SURFACE_ADDRESS = 0x1844,
	D1GRPH_PITCH = 0x1848,
	D1GRPH_X_END = 0x184D,
	D1GRPH_Y_END = 0x184E,
	DC_LUT_RW_MODE = 0x1921,
	DC_LUT_RW_INDEX = 0x1922,
	DC_LUT_WRITE_EN_MASK = 0x1927,
	DC_LUT_AUTOFILL = 0x1928,
	DC_LUTA_CONTROL = 0x1930,
	// The line being scanned, in viewport lines (xenia:src/xenia/gpu/graphics_system.cc:189-192; docs/HARDWARE.md).
	R500_D1MODE_V_COUNTER = 0x194C,
	AVIVO_D1MODE_VIEWPORT_SIZE = 0x1961,
	AVIVO_D1SCL_SCALER_ENABLE = 0x1964,
	AVIVO_D1SCL_SCALER_TAP_CONTROL = 0x1965,
	// Filter control, scale factor and start phase, horizontal then vertical (docs/HARDWARE.md).
	UNKNOWN_196C = 0x196C,
	UNKNOWN_196D = 0x196D,
	UNKNOWN_196E = 0x196E,
	UNKNOWN_1970 = 0x1970,
	UNKNOWN_1971 = 0x1971,
	UNKNOWN_1972 = 0x1972,
	AVIVO_D1SCL_UPDATE = 0x1973,
	// The viewport's width over 4, less one (libxenon:drivers/xenos/xenos.c:390, 397).
	UNKNOWN_197A = 0x197A,
};

enum {
	APPLY_TIMEOUT_MS = 100,
	// Reads of 0x60EC a flip from the vblank callback allows before going on.
	FLIP_POLLS = 1000,
	AUTOFILL_TIMEOUT_MS = 100,
	AUTOFILL_DONE = 2,
	LUT_ALL_CHANNELS = 7,
	WIDTH_720P = 1280,
	HEIGHT_720P = 720,
	SCALE_ONE_LOG2 = 24,
	// 2 taps each way through the built-in filter, the only setting that scaled whole (docs/HARDWARE.md).
	TWO_TAPS = 0x101,
	TAPS = 2,
	BUILT_IN_FILTER = 0x100,
	// Holds the scaler's registers until it's released (xf86-video-radeonhd:src/rhd_regs.h:412-413).
	SCALER_UPDATE_LOCK = 0x10000,
	// Three frames at 60 Hz.
	SCALER_TAKE_TIMEOUT_US = 50000,
	MICROSECONDS_PER_SECOND = 1000000,
};

typedef struct {
	uint32_t address;
	uint32_t pitch;
	uint32_t x_end;
	uint32_t y_end;
} Surface;

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
	SCALER_REGISTERS,
};

// The registers the scaling probes wrote and put back, which gave the display back to XAM (docs/HARDWARE.md).
static const uint32_t scaler_registers[SCALER_REGISTERS] = { AVIVO_D1SCL_SCALER_ENABLE, AVIVO_D1SCL_SCALER_TAP_CONTROL,
	AVIVO_D1MODE_VIEWPORT_SIZE, UNKNOWN_197A, UNKNOWN_196C, UNKNOWN_196D, UNKNOWN_196E, UNKNOWN_1970, UNKNOWN_1971,
	UNKNOWN_1972 };

static bool is_open;
static uint32_t mode_height;
static Surface old_surface;
// Set while the title's front buffer is 1280x720 on the SDK's scaling rather than the kernel's.
static bool scaling;
static bool scaler_applied;
static uint32_t old_scaler[SCALER_REGISTERS];
static uint32_t new_scaler[SCALER_REGISTERS];

// libxenon's autofill of a linear ramp; the screen stays black without it (libxenon:drivers/xenos/xenos.c:419-424).
static NTSTATUS fill_lut(void)
{
	gpu_registers[DC_LUTA_CONTROL] = 0;
	gpu_registers[DC_LUT_RW_INDEX] = 0;
	gpu_registers[DC_LUT_RW_MODE] = 0;
	gpu_registers[DC_LUT_WRITE_EN_MASK] = LUT_ALL_CHANNELS;
	gpu_registers[DC_LUT_AUTOFILL] = 1;
	for (uint32_t waited_ms = 0; !(gpu_registers[DC_LUT_AUTOFILL] & AUTOFILL_DONE); waited_ms++) {
		if (waited_ms == AUTOFILL_TIMEOUT_MS)
			return STATUS_IO_TIMEOUT;
		(void)xsdk_thread_sleep(1);
	}
	return STATUS_SUCCESS;
}

// Source over output in 8.24 (docs/HARDWARE.md, "Scaling 1280x720 by hand").
static uint32_t scale(uint32_t source, uint32_t output)
{
	return (uint32_t)(((uint64_t)source << SCALE_ONE_LOG2) / output);
}

// (scale + taps + 1) / 2 in 16.16 (docs/HARDWARE.md, "What AMD's code says about the scaler").
static uint32_t phase(uint32_t scale_8_24)
{
	return ((TAPS + 1) << 15) + (scale_8_24 >> 9);
}

// Plans 1280x720 scaled to the output, or shown 1:1 with the scaler off as libxenon does (xenos.c:381-417).
static bool plan_scaler(void)
{
	VIDEO_MODE video;
	VdQueryVideoMode(&video);
	if (video.is_interlaced || !video.display_width || !video.display_height) {
		xsdk_log_write("display: the output is %ux%u, interlaced %u, which 1280x720 isn't tried on; the kernel's mode",
			(unsigned)video.display_width, (unsigned)video.display_height, (unsigned)video.is_interlaced);
		return false;
	}
	for (uint32_t i = 0; i < SCALER_REGISTERS; i++)
		old_scaler[i] = gpu_registers[scaler_registers[i]];
	const uint32_t horizontal = scale(WIDTH_720P, video.display_width);
	const uint32_t vertical = scale(HEIGHT_720P, video.display_height);
	const bool scaled = horizontal != 1u << SCALE_ONE_LOG2 || vertical != 1u << SCALE_ONE_LOG2;
	new_scaler[SCALER_ENABLE] = scaled;
	new_scaler[SCALER_TAP_CONTROL] = TWO_TAPS;
	new_scaler[VIEWPORT_SIZE] = WIDTH_720P << 16 | HEIGHT_720P;
	new_scaler[VIEWPORT_WIDTH_QUARTER] = WIDTH_720P / 4 - 1;
	new_scaler[HORIZONTAL_FILTER] = old_scaler[HORIZONTAL_FILTER] | BUILT_IN_FILTER;
	new_scaler[HORIZONTAL_SCALE] = horizontal;
	new_scaler[HORIZONTAL_PHASE] = phase(horizontal);
	new_scaler[VERTICAL_FILTER] = old_scaler[VERTICAL_FILTER] | BUILT_IN_FILTER;
	new_scaler[VERTICAL_SCALE] = vertical;
	new_scaler[VERTICAL_PHASE] = phase(vertical);
	xsdk_log_write("display: 1280x720 %s to the %ux%u output", scaled ? "scaled" : "shown 1:1",
		(unsigned)video.display_width, (unsigned)video.display_height);
	return true;
}

NTSTATUS xsdk_display_open(XsdkDisplayMode mode, uint32_t *width, uint32_t *height)
{
	assert(!is_open);
	// The size the display scales up, width high (xenia:src/xenia/gpu/graphics_system.cc:191-194; docs/HARDWARE.md).
	const uint32_t viewport = gpu_registers[AVIVO_D1MODE_VIEWPORT_SIZE];
	if (!viewport) {
		xsdk_log_write("display: no display controller");
		return STATUS_NOT_SUPPORTED;
	}
	const NTSTATUS status = fill_lut();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("display: the LUT autofill didn't finish: 0x%08X", (unsigned)status);
		return status;
	}
	old_surface = (Surface){
		.address = gpu_registers[D1GRPH_PRIMARY_SURFACE_ADDRESS],
		.pitch = gpu_registers[D1GRPH_PITCH],
		.x_end = gpu_registers[D1GRPH_X_END],
		.y_end = gpu_registers[D1GRPH_Y_END],
	};
	scaling = mode == XSDK_DISPLAY_720P && plan_scaler();
	scaler_applied = false;
	*width = scaling ? WIDTH_720P : viewport >> 16;
	*height = scaling ? HEIGHT_720P : viewport & 0xFFFF;
	mode_height = *height;
	is_open = true;
	return STATUS_SUCCESS;
}

static void write_scaler(const uint32_t *values)
{
	gpu_registers[AVIVO_D1SCL_UPDATE] = SCALER_UPDATE_LOCK;
	for (uint32_t i = 0; i < SCALER_REGISTERS; i++)
		gpu_registers[scaler_registers[i]] = values[i];
	gpu_registers[AVIVO_D1SCL_UPDATE] = 0;
}

// XAM writes both 0x2840 and D1GRPH, then sets 0x60EC, which the display clears (docs/HARDWARE.md).
static NTSTATUS apply(const Surface *surface)
{
	gpu_registers[UNKNOWN_0A10] = surface->address;
	gpu_registers[UNKNOWN_0A11] = surface->pitch;
	gpu_registers[D1GRPH_PRIMARY_SURFACE_ADDRESS] = surface->address;
	gpu_registers[D1GRPH_PITCH] = surface->pitch;
	gpu_registers[D1GRPH_X_END] = surface->x_end;
	gpu_registers[D1GRPH_Y_END] = surface->y_end;
	gpu_registers[D1CRTC_DOUBLE_BUFFER_CONTROL] = 1;
	for (uint32_t waited_ms = 0; gpu_registers[D1CRTC_DOUBLE_BUFFER_CONTROL]; waited_ms++) {
		if (waited_ms == APPLY_TIMEOUT_MS) {
			gpu_registers[D1CRTC_DOUBLE_BUFFER_CONTROL] = 0;
			return STATUS_IO_TIMEOUT;
		}
		(void)xsdk_thread_sleep(1);
	}
	gpu_registers[D1CRTC_DOUBLE_BUFFER_CONTROL] = 0;
	return STATUS_SUCCESS;
}

static uint32_t elapsed_us(const struct timespec *start)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)((now.tv_sec - start->tv_sec) * MICROSECONDS_PER_SECOND + (now.tv_nsec - start->tv_nsec) / 1000);
}

// The counter runs in viewport lines, under 500 on the kernel's 480, so passing 720 shows the new size has taken.
static void wait_for_scaler(void)
{
	struct timespec start;
	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	uint32_t waited_us = 0;
	while (gpu_registers[R500_D1MODE_V_COUNTER] < mode_height && waited_us < SCALER_TAKE_TIMEOUT_US)
		waited_us = elapsed_us(&start);
	xsdk_log_write("display: the line counter reached %u after %u us", (unsigned)gpu_registers[R500_D1MODE_V_COUNTER],
		(unsigned)waited_us);
}

// The display takes it at once (docs/HARDWARE.md, "The vblank callback, flips and giving the callback back").
void xsdk_display_flip(uint32_t address)
{
	gpu_registers[UNKNOWN_0A10] = address;
	gpu_registers[D1GRPH_PRIMARY_SURFACE_ADDRESS] = address;
	gpu_registers[D1CRTC_DOUBLE_BUFFER_CONTROL] = 1;
	for (uint32_t i = 0; i < FLIP_POLLS && gpu_registers[D1CRTC_DOUBLE_BUFFER_CONTROL]; i++)
		;
	gpu_registers[D1CRTC_DOUBLE_BUFFER_CONTROL] = 0;
}

NTSTATUS xsdk_display_show(uint32_t address, uint32_t pitch_pixels)
{
	assert(is_open);
	const Surface surface = { .address = address, .pitch = pitch_pixels, .x_end = pitch_pixels, .y_end = mode_height };
	if (!scaling || scaler_applied)
		return apply(&surface);
	// Not at open, while XAM's 640x480 surface still shows: the scaler changes with the first 1280x720 surface.
	write_scaler(new_scaler);
	scaler_applied = true;
	const NTSTATUS status = apply(&surface);
	// So a vblank callback set next doesn't read the line counter on the old scale (docs/HARDWARE.md).
	wait_for_scaler();
	return status;
}

void xsdk_display_close(void)
{
	if (!is_open)
		return;
	if (scaler_applied)
		write_scaler(old_scaler);
	(void)apply(&old_surface);
	is_open = false;
}
