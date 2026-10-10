#include "xsdk/display.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

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
	AVIVO_D1MODE_VIEWPORT_SIZE = 0x1961,
};

enum {
	APPLY_TIMEOUT_MS = 100,
	AUTOFILL_TIMEOUT_MS = 100,
	AUTOFILL_DONE = 2,
	LUT_ALL_CHANNELS = 7,
};

typedef struct {
	uint32_t address;
	uint32_t pitch;
	uint32_t x_end;
	uint32_t y_end;
} Surface;

static bool is_open;
static uint32_t mode_height;
static Surface old_surface;

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
			return STATUS_TIMEOUT;
		(void)xsdk_thread_sleep(1);
	}
	return STATUS_SUCCESS;
}

NTSTATUS xsdk_display_open(uint32_t *width, uint32_t *height)
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
	*width = viewport >> 16;
	*height = viewport & 0xFFFF;
	mode_height = *height;
	is_open = true;
	return STATUS_SUCCESS;
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
			return STATUS_TIMEOUT;
		}
		(void)xsdk_thread_sleep(1);
	}
	gpu_registers[D1CRTC_DOUBLE_BUFFER_CONTROL] = 0;
	return STATUS_SUCCESS;
}

NTSTATUS xsdk_display_show(uint32_t address, uint32_t pitch_pixels)
{
	assert(is_open);
	const Surface surface = { .address = address, .pitch = pitch_pixels, .x_end = pitch_pixels, .y_end = mode_height };
	return apply(&surface);
}

void xsdk_display_close(void)
{
	if (!is_open)
		return;
	(void)apply(&old_surface);
	is_open = false;
}
