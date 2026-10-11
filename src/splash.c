#include "xsdk/splash.h"

#include <stdbool.h>
#include <stdint.h>

#include "gpu_private.h"
#include "splash_image.h"
#include "xsdk/display.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

enum {
	BUFFERS = 2,
	FADE_MS = 500,
	// Longer than a 60 Hz frame, so the buffer shown before the last one is off screen by the time it's rewritten.
	FRAME_MS = 20,
	BLACK_RGB = 0x000000,
	OPAQUE = 0xFF000000,
	PIXEL_BYTES_LOG2 = 2,
};

// A front buffer stores each pixel's bytes B, G, R, A, as resolves write them (docs/HARDWARE.md).
static uint32_t as_stored(uint32_t rgb, uint32_t level)
{
	const uint32_t red = (rgb >> 16 & 0xFF) * level / 255;
	const uint32_t green = (rgb >> 8 & 0xFF) * level / 255;
	const uint32_t blue = (rgb & 0xFF) * level / 255;
	return __builtin_bswap32(OPAQUE | red << 16 | green << 8 | blue);
}

static void fill_black(volatile uint32_t *pixels)
{
	const uint32_t black = as_stored(BLACK_RGB, 0);
	for (uint32_t i = 0; i < xsdk_gpu_resolve_bytes(SPLASH_WIDTH, SPLASH_HEIGHT) / 4; i++)
		pixels[i] = black;
}

// The logo's coloured pixels at a brightness from 0 to 255; its black ones are already black.
static void draw_logo(volatile uint32_t *pixels, uint32_t level)
{
	uint32_t pixel = 0;
	for (uint32_t i = 0; i < SPLASH_RUNS; i++) {
		const uint32_t rgb = splash_palette[splash_runs[i] >> 16];
		const uint32_t count = splash_runs[i] & 0xFFFF;
		if (rgb == BLACK_RGB) {
			pixel += count;
			continue;
		}
		const uint32_t stored = as_stored(rgb, level);
		for (uint32_t end = pixel + count; pixel < end; pixel++) {
			const uint32_t x = pixel % SPLASH_WIDTH;
			const uint32_t y = pixel / SPLASH_WIDTH;
			pixels[xsdk_tiled_2d(x, y, SPLASH_WIDTH, PIXEL_BYTES_LOG2) >> PIXEL_BYTES_LOG2] = stored;
		}
	}
	__asm__ volatile("sync" ::: "memory");
}

// Fades in by drawing each step into the buffer not on screen, then holds the last.
static bool fade_in(volatile uint32_t *const *buffers, uint32_t duration_ms)
{
	uint32_t shown_ms = 0;
	for (uint32_t frame = 0; shown_ms < duration_ms; frame++) {
		const uint32_t elapsed = frame * FRAME_MS;
		volatile uint32_t *const buffer = buffers[frame % BUFFERS];
		if (elapsed <= FADE_MS + FRAME_MS) {
			draw_logo(buffer, elapsed >= FADE_MS ? 255 : elapsed * 255 / FADE_MS);
			if (!NT_SUCCESS(xsdk_display_show(xsdk_gpu_address(buffer), SPLASH_WIDTH)))
				return false;
		}
		(void)xsdk_thread_sleep(FRAME_MS);
		shown_ms += FRAME_MS;
	}
	return true;
}

NTSTATUS xsdk_splash_show(uint32_t duration_ms)
{
	NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status))
		return status;
	uint32_t width = 0;
	uint32_t height = 0;
	status = xsdk_display_open(XSDK_DISPLAY_720P, &width, &height);
	if (NT_SUCCESS(status) && (width != SPLASH_WIDTH || height != SPLASH_HEIGHT)) {
		xsdk_display_close();
		status = STATUS_NOT_SUPPORTED;
	}
	if (!NT_SUCCESS(status)) {
		(void)xsdk_gpu_close();
		return status;
	}
	const uint32_t bytes = xsdk_gpu_resolve_bytes(SPLASH_WIDTH, SPLASH_HEIGHT);
	volatile uint32_t *const buffers[BUFFERS] = { xsdk_gpu_alloc(bytes), xsdk_gpu_alloc(bytes) };
	status = buffers[0] && buffers[1] ? STATUS_SUCCESS : STATUS_NO_MEMORY;
	if (NT_SUCCESS(status)) {
		fill_black(buffers[0]);
		fill_black(buffers[1]);
		if (!fade_in(buffers, duration_ms))
			status = STATUS_UNSUCCESSFUL;
	}
	xsdk_display_close();
	for (uint32_t i = 0; i < BUFFERS; i++)
		xsdk_gpu_free(buffers[i]);
	const NTSTATUS closed = xsdk_gpu_close();
	if (!NT_SUCCESS(status))
		xsdk_log_write("splash: failed: 0x%08X", (unsigned)status);
	return NT_SUCCESS(status) ? closed : status;
}
