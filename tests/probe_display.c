#include <stdint.h>

#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// The kernel doesn't flip on VdSwap (docs/HARDWARE.md), so this points the display controller at the title's own
// buffer, as libxenon does. It keeps the kernel's mode and only changes the surface address, then shows red, green and
// blue, then stripes and a diagonal drawn with libxenon's tile layout: clean stripes and a straight line mean the
// display reads that layout. It puts the old address back before exiting.

enum {
	PAGE_BYTES = 0x10000,
	HOLD_MS = 3000,
	PATTERN_HOLD_MS = 5000,
	STRIPE_PIXELS = 64,
	TILE_PIXELS = 32,
};

// Display controller registers, in bytes from the register base (libxenon:drivers/xenos/xenos.h:26-44).
enum {
	D1GRPH_ENABLE = 0x6100,
	D1GRPH_CONTROL = 0x6104,
	D1GRPH_PRIMARY_SURFACE_ADDRESS = 0x6110,
	D1GRPH_PITCH = 0x6120,
	D1GRPH_X_END = 0x6134,
	D1GRPH_Y_END = 0x6138,
	D1GRPH_UPDATE = 0x6144,
	D1GRPH_FLIP_CONTROL = 0x6148,
};

typedef struct {
	const char *name;
	uint32_t pixel;
} Colour;

// libxenon writes blue << 24 | green << 16 | red << 8 (libxenon:drivers/console/console.c:67-70).
static const Colour colours[] = {
	{ "red", 0x0000FF00 },
	{ "green", 0x00FF0000 },
	{ "blue", 0xFF000000 },
};

enum {
	WHITE = 0xFFFFFF00,
};

// GPU registers as titles see them (xenia:src/xenia/gpu/graphics_system.cc:92-95); libxenon's base is the same block.
static volatile uint32_t *const gpu_registers = (volatile uint32_t *)0x7FC80000;

static uint32_t read_register(uint32_t offset)
{
	return gpu_registers[offset / 4];
}

static void write_register(uint32_t offset, uint32_t value)
{
	gpu_registers[offset / 4] = value;
}

static void report_registers(const char *when)
{
	xsdk_log_write(
		"display: %s: enable %08X, control %08X, surface %08X, pitch %08X, end %08X x %08X, update %08X, "
		"flip control %08X",
		when, (unsigned)read_register(D1GRPH_ENABLE), (unsigned)read_register(D1GRPH_CONTROL),
		(unsigned)read_register(D1GRPH_PRIMARY_SURFACE_ADDRESS), (unsigned)read_register(D1GRPH_PITCH),
		(unsigned)read_register(D1GRPH_X_END), (unsigned)read_register(D1GRPH_Y_END),
		(unsigned)read_register(D1GRPH_UPDATE), (unsigned)read_register(D1GRPH_FLIP_CONTROL));
}

// libxenon's update sequence (libxenon:drivers/xenos/xenos.c:366-379).
static void set_surface(uint32_t physical_address)
{
	write_register(D1GRPH_UPDATE, 1);
	write_register(D1GRPH_PRIMARY_SURFACE_ADDRESS, physical_address);
	write_register(D1GRPH_UPDATE, 0);
}

// A pixel's index in 32x32 tiles (libxenon:drivers/console/console.c:60-61; BSD, Copyright 2009 Georg Lukas).
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

int main(void)
{
	xsdk_log_write("display probe");
	report_registers("at start");
	const uint32_t old_surface = read_register(D1GRPH_PRIMARY_SURFACE_ADDRESS);
	const uint32_t width = read_register(D1GRPH_PITCH);
	const uint32_t height = read_register(D1GRPH_Y_END);
	const uint32_t tiled_height = (height + TILE_PIXELS - 1) & ~(TILE_PIXELS - 1);
	const uint32_t pixel_count = width * tiled_height;
	if (!width || !height) {
		xsdk_log_write("display: no mode to keep");
		return 1;
	}

	const NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("display: xsdk_gpu_open returned 0x%08X", (unsigned)status);
		return 1;
	}
	const uint32_t bytes = (pixel_count * 4 + PAGE_BYTES - 1) & ~(PAGE_BYTES - 1);
	volatile uint32_t *const pixels = MmAllocatePhysicalMemoryEx(
		0, bytes, PAGE_READWRITE | PAGE_WRITECOMBINE | MEM_LARGE_PAGES, 0, UINT32_MAX, PAGE_BYTES);
	if (!pixels) {
		xsdk_log_write("display: MmAllocatePhysicalMemoryEx failed");
		return 1;
	}
	const uint32_t physical = MmGetPhysicalAddress((const void *)pixels);
	xsdk_log_write("display: %ux%u buffer at %p (physical %08X)", (unsigned)width, (unsigned)height, (void *)pixels,
		(unsigned)physical);

	fill(pixels, pixel_count, colours[0].pixel);
	set_surface(physical);
	report_registers("after setting the surface");
	for (uint32_t i = 0; i < sizeof colours / sizeof colours[0]; i++) {
		fill(pixels, pixel_count, colours[i].pixel);
		xsdk_log_write(
			"display: showing %s (%08X) for %u ms", colours[i].name, (unsigned)colours[i].pixel, (unsigned)HOLD_MS);
		(void)xsdk_thread_sleep(HOLD_MS);
	}
	draw_pattern(pixels, width, height);
	xsdk_log_write("display: showing %u-pixel red and blue stripes and a white diagonal for %u ms",
		(unsigned)STRIPE_PIXELS, (unsigned)PATTERN_HOLD_MS);
	(void)xsdk_thread_sleep(PATTERN_HOLD_MS);
	report_registers("before restoring");

	set_surface(old_surface);
	report_registers("after restoring");
	return 0;
}
