#include <stdint.h>

#include "probe_gpu.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// Shows CPU-filled colours through the registers XAM sets for the Guide, before and after filling the LUT.

enum {
	PAGE_BYTES = 0x10000,
	HOLD_MS = 4000,
	COLOUR_MS = 3000,
	PATTERN_HOLD_MS = 5000,
	APPLY_TIMEOUT_MS = 100,
	AUTOFILL_TIMEOUT_MS = 100,
	STRIPE_PIXELS = 64,
	TILE_PIXELS = 32,
	WHITE = 0xFFFFFFFF,
};

typedef struct {
	const char *name;
	uint32_t pixel;
} Colour;

// 0xAARRGGBB stored little-endian, as the display reads it (libxenon:drivers/console/console.c:67-70).
static const Colour colours[] = {
	{ "red", 0x0000FFFF },
	{ "green", 0x00FF00FF },
	{ "blue", 0xFF0000FF },
};

static void report(const char *when)
{
	volatile const uint32_t *const registers = probe_gpu_registers;
	xsdk_log_write(
		"display: %s: 2840 %08X, 2844 %08X, surface %08X, pitch %08X, x end %08X, y end %08X, LUT autofill "
		"%08X, 60EC %08X",
		when, (unsigned)registers[UNKNOWN_0A10], (unsigned)registers[UNKNOWN_0A11],
		(unsigned)registers[D1GRPH_PRIMARY_SURFACE_ADDRESS], (unsigned)registers[D1GRPH_PITCH],
		(unsigned)registers[D1GRPH_X_END], (unsigned)registers[D1GRPH_Y_END], (unsigned)registers[DC_LUT_AUTOFILL],
		(unsigned)registers[D1CRTC_DOUBLE_BUFFER_CONTROL]);
}

// Sets 0x60EC after the registers, as XAM's change showed, and waits for the display to clear it.
static void apply(void)
{
	volatile uint32_t *const registers = probe_gpu_registers;
	registers[D1CRTC_DOUBLE_BUFFER_CONTROL] = 1;
	for (uint32_t waited_ms = 0; registers[D1CRTC_DOUBLE_BUFFER_CONTROL] && waited_ms < APPLY_TIMEOUT_MS; waited_ms++)
		(void)xsdk_thread_sleep(1);
	registers[D1CRTC_DOUBLE_BUFFER_CONTROL] = 0;
}

// XAM writes the address and pitch to both 0x2840 and D1GRPH (docs/HARDWARE.md).
static void set_surface(uint32_t physical_address, uint32_t pitch)
{
	volatile uint32_t *const registers = probe_gpu_registers;
	registers[UNKNOWN_0A10] = physical_address;
	registers[UNKNOWN_0A11] = pitch;
	registers[D1GRPH_PRIMARY_SURFACE_ADDRESS] = physical_address;
	registers[D1GRPH_PITCH] = pitch;
	registers[D1GRPH_X_END] = pitch;
	apply();
}

// libxenon's LUT set-up: autofill writes a linear ramp (libxenon:drivers/xenos/xenos.c:419-424).
static void fill_lut(void)
{
	volatile uint32_t *const registers = probe_gpu_registers;
	registers[DC_LUTA_CONTROL] = 0;
	registers[DC_LUT_RW_INDEX] = 0;
	registers[DC_LUT_RW_MODE] = 0;
	registers[DC_LUT_WRITE_EN_MASK] = 7;
	registers[DC_LUT_AUTOFILL] = 1;
	uint32_t waited_ms = 0;
	while (!(registers[DC_LUT_AUTOFILL] & 2) && waited_ms < AUTOFILL_TIMEOUT_MS) {
		(void)xsdk_thread_sleep(1);
		waited_ms++;
	}
	xsdk_log_write("display: LUT autofill %s after %u ms", registers[DC_LUT_AUTOFILL] & 2 ? "done" : "not done",
		(unsigned)waited_ms);
}

// tiled_index comes from libxenon:drivers/console/console.c:60-61, which carries this notice:
//
// Copyright 2009  Georg Lukas <georg@op-co.de>
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
// THE POSSIBILITY OF SUCH DAMAGE.

// A pixel's index in 32x32 tiles.
static uint32_t tiled_index(uint32_t x, uint32_t y, uint32_t width)
{
	return ((y >> 5) * 32 * width + ((x >> 5) << 10) + (x & 3) + ((y & 1) << 2) + (((x & 31) >> 2) << 3) +
			   (((y & 31) >> 1) << 6)) ^
		((y & 8) << 2);
}

static void fill(volatile uint32_t *pixels, uint32_t pixel_count, uint32_t pixel)
{
	for (uint32_t i = 0; i < pixel_count; i++)
		pixels[i] = pixel;
	__asm__ volatile("sync" ::: "memory");
}

// Vertical red and blue stripes with a white diagonal from the top-left corner.
static void draw_pattern(volatile uint32_t *pixels, uint32_t width, uint32_t height)
{
	for (uint32_t y = 0; y < height; y++) {
		for (uint32_t x = 0; x < width; x++) {
			uint32_t pixel = (x / STRIPE_PIXELS) % 2 ? colours[2].pixel : colours[0].pixel;
			if (x == y || x == y + 1)
				pixel = WHITE;
			pixels[tiled_index(x, y, width)] = pixel;
		}
	}
	__asm__ volatile("sync" ::: "memory");
}

static void hold(const char *what, uint32_t ms)
{
	xsdk_log_write("display: showing %s for %u ms", what, (unsigned)ms);
	(void)xsdk_thread_sleep(ms);
}

int main(void)
{
	xsdk_log_write("display probe");
	report("at start");
	const uint32_t old_surface = probe_gpu_registers[D1GRPH_PRIMARY_SURFACE_ADDRESS];
	const uint32_t width = probe_gpu_registers[D1GRPH_PITCH];
	const uint32_t height = probe_gpu_registers[D1GRPH_Y_END];
	const uint32_t pixel_count = width * ((height + TILE_PIXELS - 1) & ~(TILE_PIXELS - 1));
	const uint32_t bytes = (pixel_count * 4 + PAGE_BYTES - 1) & ~(PAGE_BYTES - 1);
	volatile uint32_t *const pixels = MmAllocatePhysicalMemoryEx(
		0, bytes, PAGE_READWRITE | PAGE_WRITECOMBINE | MEM_LARGE_PAGES, 0, UINT32_MAX, PAGE_BYTES);
	if (!width || !height || !pixels) {
		xsdk_log_write("display: no buffer for a %ux%u mode", (unsigned)width, (unsigned)height);
		return 1;
	}
	const uint32_t physical = MmGetPhysicalAddress((const void *)pixels);
	fill(pixels, pixel_count, WHITE);

	const NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("display: xsdk_gpu_open returned 0x%08X", (unsigned)status);
		return 1;
	}
	set_surface(physical, width);
	report("white with 0x2840");
	hold("white with 0x2840", HOLD_MS);

	fill_lut();
	report("after filling the LUT");
	hold("white after filling the LUT", HOLD_MS);

	for (uint32_t i = 0; i < sizeof colours / sizeof colours[0]; i++) {
		fill(pixels, pixel_count, colours[i].pixel);
		hold(colours[i].name, COLOUR_MS);
	}
	draw_pattern(pixels, width, height);
	hold("red and blue stripes with a white diagonal", PATTERN_HOLD_MS);

	set_surface(old_surface, width);
	report("after restoring");
	return 0;
}
