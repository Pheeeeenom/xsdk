#include "xsdk/splash.h"

#include <assert.h>
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

typedef struct {
	volatile uint32_t *buffers[BUFFERS];
	// The display is open for the logo, and whether xsdk_present_open has taken it over.
	bool up;
	bool handed_over;
} Splash;

static Splash splash;

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

// Draws each step into the buffer not on screen, ending on the logo at full brightness.
static NTSTATUS fade_in(void)
{
	for (uint32_t frame = 0;; frame++) {
		const uint32_t elapsed = frame * FRAME_MS;
		volatile uint32_t *const buffer = splash.buffers[frame % BUFFERS];
		draw_logo(buffer, elapsed >= FADE_MS ? 255 : elapsed * 255 / FADE_MS);
		const NTSTATUS status = xsdk_display_show(xsdk_gpu_address(buffer), SPLASH_WIDTH);
		if (!NT_SUCCESS(status) || elapsed >= FADE_MS)
			return status;
		(void)xsdk_thread_sleep(FRAME_MS);
	}
}

static void free_buffers(void)
{
	for (uint32_t i = 0; i < BUFFERS; i++) {
		if (splash.buffers[i])
			xsdk_gpu_free(splash.buffers[i]);
	}
	splash = (Splash){ .up = false };
}

NTSTATUS xsdk_splash_begin(void)
{
	assert(!splash.up);
	uint32_t width = 0;
	uint32_t height = 0;
	NTSTATUS status = xsdk_display_open(XSDK_DISPLAY_720P, &width, &height);
	if (NT_SUCCESS(status) && (width != SPLASH_WIDTH || height != SPLASH_HEIGHT)) {
		xsdk_display_close();
		status = STATUS_NOT_SUPPORTED;
	}
	if (!NT_SUCCESS(status))
		return status;
	const uint32_t bytes = xsdk_gpu_resolve_bytes(SPLASH_WIDTH, SPLASH_HEIGHT);
	splash = (Splash){ .buffers = { xsdk_gpu_alloc(bytes), xsdk_gpu_alloc(bytes) }, .up = true };
	status = splash.buffers[0] && splash.buffers[1] ? STATUS_SUCCESS : STATUS_NO_MEMORY;
	if (NT_SUCCESS(status)) {
		fill_black(splash.buffers[0]);
		fill_black(splash.buffers[1]);
		status = fade_in();
	}
	if (!NT_SUCCESS(status)) {
		xsdk_display_close();
		free_buffers();
		xsdk_log_write("splash: failed: 0x%08X", (unsigned)status);
	}
	return status;
}

void xsdk_splash_end(void)
{
	if (!splash.up || splash.handed_over)
		return;
	xsdk_display_close();
	free_buffers();
}

bool xsdk_splash_hand_over(uint32_t *width, uint32_t *height)
{
	if (!splash.up || splash.handed_over)
		return false;
	splash.handed_over = true;
	*width = SPLASH_WIDTH;
	*height = SPLASH_HEIGHT;
	return true;
}

void xsdk_splash_free(void)
{
	assert(splash.handed_over);
	free_buffers();
}

NTSTATUS xsdk_splash_show(uint32_t duration_ms)
{
	NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status))
		return status;
	status = xsdk_splash_begin();
	if (NT_SUCCESS(status)) {
		for (uint32_t shown_ms = FADE_MS; shown_ms < duration_ms; shown_ms += FRAME_MS)
			(void)xsdk_thread_sleep(FRAME_MS);
		xsdk_splash_end();
	}
	const NTSTATUS closed = xsdk_gpu_close();
	return NT_SUCCESS(status) ? closed : status;
}
