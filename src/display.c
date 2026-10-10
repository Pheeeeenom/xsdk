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
};

enum {
	APPLY_TIMEOUT_MS = 100,
	AUTOFILL_TIMEOUT_MS = 100,
	AUTOFILL_DONE = 2,
	LUT_ALL_CHANNELS = 7,
};

static bool is_open;
static uint32_t old_surface;
static uint32_t old_pitch;

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
	const uint32_t pitch = gpu_registers[D1GRPH_PITCH];
	if (!pitch) {
		xsdk_log_write("display: no display controller");
		return STATUS_NOT_SUPPORTED;
	}
	const NTSTATUS status = fill_lut();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("display: the LUT autofill didn't finish: 0x%08X", (unsigned)status);
		return status;
	}
	old_surface = gpu_registers[D1GRPH_PRIMARY_SURFACE_ADDRESS];
	old_pitch = pitch;
	*width = pitch;
	*height = gpu_registers[D1GRPH_Y_END];
	is_open = true;
	return STATUS_SUCCESS;
}

// XAM writes both 0x2840 and D1GRPH, then sets 0x60EC, which the display clears (docs/HARDWARE.md).
NTSTATUS xsdk_display_show(uint32_t address, uint32_t pitch_pixels)
{
	assert(is_open);
	gpu_registers[UNKNOWN_0A10] = address;
	gpu_registers[UNKNOWN_0A11] = pitch_pixels;
	gpu_registers[D1GRPH_PRIMARY_SURFACE_ADDRESS] = address;
	gpu_registers[D1GRPH_PITCH] = pitch_pixels;
	gpu_registers[D1GRPH_X_END] = pitch_pixels;
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

void xsdk_display_close(void)
{
	if (!is_open)
		return;
	(void)xsdk_display_show(old_surface, old_pitch);
	is_open = false;
}
