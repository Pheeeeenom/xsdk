#include <stdbool.h>
#include <stdint.h>

#include "tests.h"
#include "xsdk/display.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

enum {
	WIDTH = 640,
	HEIGHT = 480,
	PIXELS = WIDTH * HEIGHT,
	BUFFERS = 2,
	FRAMES = 240,
	FRAME_MS = 16,
	FENCE_TIMEOUT_MS = 1000,
	MAX_MISSES_LOGGED = 4,
	FADE_STEP = 4,
};

// Fades between magenta and green; red and blue stay equal, so either byte order the copy uses checks the same.
static uint32_t colour_at(uint32_t frame)
{
	const uint32_t phase = frame * FADE_STEP % 512;
	const uint32_t level = phase < 256 ? phase : 511 - phase;
	return 0xFF000000 | level << 16 | (255 - level) << 8 | level;
}

// A resolve stores the colour's bytes B, G, R, A, as the display reads them (docs/HARDWARE.md).
static uint32_t as_stored(uint32_t argb)
{
	return __builtin_bswap32(argb);
}

// The first, middle and last pixel; reading all of write-combined memory every frame would take too long.
static bool holds(volatile const uint32_t *pixels, uint32_t value)
{
	return pixels[0] == value && pixels[PIXELS / 2] == value && pixels[PIXELS - 1] == value;
}

// Each frame resolves into the buffer not on screen, clearing eDRAM to the next colour, then shows it.
static void present_frames(volatile uint32_t *const *buffers, bool has_display)
{
	uint32_t misses = 0;
	bool fences_landed = true;
	bool shown = true;
	for (uint32_t frame = 0; frame < FRAMES && fences_landed; frame++) {
		volatile uint32_t *const buffer = buffers[frame % BUFFERS];
		const uint32_t address = xsdk_gpu_address(buffer);
		xsdk_gpu_resolve(address, WIDTH, HEIGHT, colour_at(frame));
		fences_landed = NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
		// The first resolve copies whatever eDRAM held; each later one, the colour the one before cleared to.
		const uint32_t expected = as_stored(colour_at(frame - 1));
		if (frame > 0 && fences_landed && !holds(buffer, expected)) {
			if (misses < MAX_MISSES_LOGGED)
				xsdk_log_write(
					"present: frame %u held %08X, not %08X", (unsigned)frame, (unsigned)buffer[0], (unsigned)expected);
			misses++;
		}
		if (has_display)
			shown = shown && NT_SUCCESS(xsdk_display_show(address, WIDTH));
		(void)xsdk_thread_sleep(FRAME_MS);
	}
	check("present: every resolve's fence landed", fences_landed);
	check("present: every frame held the colour cleared before it", fences_landed && misses == 0);
	if (has_display)
		check("present: every frame shown", shown);
}

void test_present(void)
{
	const NTSTATUS status = xsdk_gpu_open();
	check("present: GPU open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return;
	uint32_t width = 0;
	uint32_t height = 0;
	const NTSTATUS display_status = xsdk_display_open(&width, &height);
	const bool has_display = NT_SUCCESS(display_status) && width == WIDTH && height == HEIGHT;
	check("present: display open in a 640x480 mode", has_display);
	volatile uint32_t *const buffers[BUFFERS] = { xsdk_gpu_alloc(PIXELS * 4), xsdk_gpu_alloc(PIXELS * 4) };
	check("present: front buffers", buffers[0] && buffers[1]);
	// Without a display, as in Xenia, the frames still resolve and are checked.
	if (buffers[0] && buffers[1])
		present_frames(buffers, has_display);
	xsdk_display_close();
	for (uint32_t i = 0; i < BUFFERS; i++)
		xsdk_gpu_free(buffers[i]);
	check("present: GPU close", NT_SUCCESS(xsdk_gpu_close()));
}
