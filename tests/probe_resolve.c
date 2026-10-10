#include <stdint.h>

#include "xsdk/display.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// Resolves pure red through the SDK and shows it, so the screen tells which order a resolve stores red and blue in.

enum {
	WIDTH = 640,
	HEIGHT = 480,
	PIXELS = WIDTH * HEIGHT,
	FENCE_TIMEOUT_MS = 1000,
	HOLD_MS = 10000,
	RED_ARGB = 0xFFFF0000,
	// The display's colours as words in memory, bytes B, G, R, A (docs/HARDWARE.md).
	MAGENTA = 0xFF00FFFF,
	RED = 0x0000FFFF,
	BLUE = 0xFF0000FF,
};

static const char *colour_name(uint32_t pixel)
{
	switch (pixel) {
	case RED:
		return "red";
	case BLUE:
		return "blue";
	case MAGENTA:
		return "magenta, so nothing was written";
	default:
		return "neither red nor blue";
	}
}

int main(void)
{
	xsdk_log_write("resolve probe");
	const NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("resolve: xsdk_gpu_open returned 0x%08X", (unsigned)status);
		return 1;
	}
	uint32_t width = 0;
	uint32_t height = 0;
	const NTSTATUS display_status = xsdk_display_open(&width, &height);
	if (!NT_SUCCESS(display_status) || width != WIDTH || height != HEIGHT) {
		xsdk_log_write(
			"resolve: no 640x480 display: 0x%08X, %ux%u", (unsigned)display_status, (unsigned)width, (unsigned)height);
		return 1;
	}
	volatile uint32_t *const pixels = xsdk_gpu_alloc(PIXELS * 4);
	if (!pixels)
		return 1;
	for (uint32_t i = 0; i < PIXELS; i++)
		pixels[i] = MAGENTA;
	__asm__ volatile("sync" ::: "memory");

	// The first resolve clears eDRAM to red; the second copies that red out.
	const uint32_t address = xsdk_gpu_address(pixels);
	xsdk_gpu_resolve(address, WIDTH, HEIGHT, RED_ARGB);
	xsdk_gpu_resolve(address, WIDTH, HEIGHT, RED_ARGB);
	const NTSTATUS waited = xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
	uint32_t matching = 0;
	for (uint32_t i = 0; i < PIXELS; i++)
		matching += pixels[i] == pixels[0];
	xsdk_log_write("resolve: fence 0x%08X; a red clear stored %08X, which the display shows as %s", (unsigned)waited,
		(unsigned)pixels[0], colour_name(pixels[0]));
	xsdk_log_write("resolve: %u of %u pixels hold it", (unsigned)matching, (unsigned)PIXELS);

	const NTSTATUS shown = xsdk_display_show(address, WIDTH);
	xsdk_log_write("resolve: showing it for %u ms: 0x%08X", (unsigned)HOLD_MS, (unsigned)shown);
	(void)xsdk_thread_sleep(HOLD_MS);
	xsdk_display_close();
	xsdk_gpu_free(pixels);
	return 0;
}
