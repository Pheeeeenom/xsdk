#include <stdint.h>
#include <string.h>

#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// The display controller takes a title's surface address, but the screen stays black (docs/HARDWARE.md). D3D calls
// VdSetDisplayMode, which may be what switches the kernel's 640x480 placeholder off. So this calls it, then swaps with
// VdSwap, then sets the surface directly as libxenon does, logging the display registers after each step, including
// raw dumps of the CRTC and LUT blocks next to the registers libxenon names. Then it shows red, green and blue in
// libxenon's pixel layout, and stripes and a diagonal drawn with libxenon's tile layout.

enum {
	PAGE_BYTES = 0x10000,
	SWAP_DWORDS = 64,
	FENCE_TIMEOUT_MS = 1000,
	SETTLE_MS = 100,
	HOLD_MS = 3000,
	PATTERN_HOLD_MS = 5000,
	STRIPE_PIXELS = 64,
	TILE_PIXELS = 32,
	PATTERN = 0xA5A5A5A5,
	SYSTEM_BUFFER_DWORDS = 64,
	SYSTEM_WRITEBACK_OFFSET = 0x40,
	// RGB, 8-bit front buffer, and the bit Xenia says is always set.
	DISPLAY_MODE_FLAGS = 0x40000000,
};

// Texture fetch constant fields (xenia:src/xenia/gpu/xenos.h:97-102, 193-198, 457, 1021-1028, 1102-1107, 1184-1285).
enum {
	FETCH_TYPE_TEXTURE = 2,
	FORMAT_8_8_8_8 = 6,
	ENDIAN_8IN32 = 2,
	SWIZZLE_XYZW = 0 | 1 << 3 | 2 << 6 | 3 << 9,
	DIMENSION_2D = 1,
};

// Display controller registers, in bytes from the register base (libxenon:drivers/xenos/xenos.h:11-54).
enum {
	D1CRTC_BLOCK = 0x6080,
	D1GRPH_ENABLE = 0x6100,
	D1GRPH_CONTROL = 0x6104,
	D1GRPH_LUT_SEL = 0x6108,
	D1GRPH_PRIMARY_SURFACE_ADDRESS = 0x6110,
	D1GRPH_PITCH = 0x6120,
	D1GRPH_X_END = 0x6134,
	D1GRPH_Y_END = 0x6138,
	D1GRPH_UPDATE = 0x6144,
	D1GRPH_FLIP_CONTROL = 0x6148,
	DC_LUTA_CONTROL = 0x64C0,
	AVIVO_D1MODE_DATA_FORMAT = 0x6528,
	AVIVO_D1MODE_VIEWPORT_SIZE = 0x6584,
	AVIVO_D1SCL_SCALER_ENABLE = 0x6590,
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

static uint32_t system_buffer[SYSTEM_BUFFER_DWORDS];
static uint32_t system_value;

static uint32_t read_register(uint32_t offset)
{
	return gpu_registers[offset / 4];
}

static void write_register(uint32_t offset, uint32_t value)
{
	gpu_registers[offset / 4] = value;
}

static void dump_block(const char *when, const char *name, uint32_t offset)
{
	xsdk_log_write("display: %s: %s %04X: %08X %08X %08X %08X %08X %08X %08X %08X", when, name, (unsigned)offset,
		(unsigned)read_register(offset), (unsigned)read_register(offset + 4), (unsigned)read_register(offset + 8),
		(unsigned)read_register(offset + 12), (unsigned)read_register(offset + 16),
		(unsigned)read_register(offset + 20), (unsigned)read_register(offset + 24),
		(unsigned)read_register(offset + 28));
}

static void report_registers(const char *when)
{
	xsdk_log_write(
		"display: %s: enable %08X, control %08X, LUT select %08X, surface %08X, pitch %08X, end %08X x "
		"%08X, update %08X, flip control %08X",
		when, (unsigned)read_register(D1GRPH_ENABLE), (unsigned)read_register(D1GRPH_CONTROL),
		(unsigned)read_register(D1GRPH_LUT_SEL), (unsigned)read_register(D1GRPH_PRIMARY_SURFACE_ADDRESS),
		(unsigned)read_register(D1GRPH_PITCH), (unsigned)read_register(D1GRPH_X_END),
		(unsigned)read_register(D1GRPH_Y_END), (unsigned)read_register(D1GRPH_UPDATE),
		(unsigned)read_register(D1GRPH_FLIP_CONTROL));
	xsdk_log_write("display: %s: data format %08X, viewport size %08X, scaler enable %08X", when,
		(unsigned)read_register(AVIVO_D1MODE_DATA_FORMAT), (unsigned)read_register(AVIVO_D1MODE_VIEWPORT_SIZE),
		(unsigned)read_register(AVIVO_D1SCL_SCALER_ENABLE));
	dump_block(when, "CRTC", D1CRTC_BLOCK);
	dump_block(when, "CRTC", D1CRTC_BLOCK + 32);
	dump_block(when, "LUT A", DC_LUTA_CONTROL);
}

static DISPLAY_INFO report_display_information(const char *when)
{
	DISPLAY_INFO info;
	memset(&info, 0, sizeof info);
	VdGetCurrentDisplayInformation(&info);
	xsdk_log_write("display: %s: front buffer %ux%u; display %ux%u", when, (unsigned)info.front_buffer_width,
		(unsigned)info.front_buffer_height, (unsigned)info.display_width, (unsigned)info.display_height);
	return info;
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

static void swap_to(volatile uint32_t *pixels, uint32_t width, uint32_t height, volatile uint32_t *system_writeback)
{
	const uint32_t address = (uint32_t)pixels;
	// A linear k_8_8_8_8 2D texture; the pitch is in pixels >> 5 (xenia:src/xenia/gpu/xenos.h:1211-1222).
	const uint32_t fetch_constant[6] = {
		FETCH_TYPE_TEXTURE | (width >> 5) << 22,
		FORMAT_8_8_8_8 | ENDIAN_8IN32 << 6 | (address >> 12) << 12,
		(width - 1) | (height - 1) << 13,
		SWIZZLE_XYZW << 1,
		0,
		DIMENSION_2D << 9,
	};
	const uint32_t texture_format = FORMAT_8_8_8_8;
	const uint32_t color_space = 0;
	volatile uint32_t *const ring_space = xsdk_gpu_reserve(SWAP_DWORDS);
	for (uint32_t i = 0; i < SWAP_DWORDS; i++)
		ring_space[i] = PATTERN;
	VdSwap(ring_space, fetch_constant, system_writeback, system_buffer, system_value, &address, &texture_format,
		&color_space, &width, &height);
	const NTSTATUS waited = xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
	xsdk_log_write("display: swap fence 0x%08X, system writeback %08X", (unsigned)waited, (unsigned)*system_writeback);
}

int main(void)
{
	xsdk_log_write("display probe");
	report_registers("at start");
	(void)report_display_information("at start");
	const uint32_t old_surface = read_register(D1GRPH_PRIMARY_SURFACE_ADDRESS);

	const NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("display: xsdk_gpu_open returned 0x%08X", (unsigned)status);
		return 1;
	}
	xsdk_log_write("display: calling VdSetDisplayMode(0x%08X)", (unsigned)DISPLAY_MODE_FLAGS);
	const uint32_t mode_result = VdSetDisplayMode(DISPLAY_MODE_FLAGS);
	xsdk_log_write("display: VdSetDisplayMode returned %08X", (unsigned)mode_result);
	(void)xsdk_thread_sleep(SETTLE_MS);
	report_registers("after VdSetDisplayMode");
	const DISPLAY_INFO info = report_display_information("after VdSetDisplayMode");

	// The buffer matches the display registers if they give a size, else the display information.
	const uint32_t width = read_register(D1GRPH_PITCH) ? read_register(D1GRPH_PITCH) : info.front_buffer_width;
	const uint32_t height = read_register(D1GRPH_Y_END) ? read_register(D1GRPH_Y_END) : info.front_buffer_height;
	const uint32_t tiled_height = (height + TILE_PIXELS - 1) & ~(TILE_PIXELS - 1);
	const uint32_t pixel_count = width * tiled_height;
	const uint32_t bytes = (pixel_count * 4 + PAGE_BYTES - 1) & ~(PAGE_BYTES - 1);
	volatile uint32_t *const pixels = MmAllocatePhysicalMemoryEx(
		0, bytes, PAGE_READWRITE | PAGE_WRITECOMBINE | MEM_LARGE_PAGES, 0, UINT32_MAX, PAGE_BYTES);
	volatile uint32_t *const page =
		MmAllocatePhysicalMemoryEx(0, PAGE_BYTES, PAGE_READWRITE | PAGE_NOCACHE | MEM_LARGE_PAGES, 0, UINT32_MAX, 0);
	if (!width || !height || !pixels || !page) {
		xsdk_log_write("display: no %ux%u buffer", (unsigned)width, (unsigned)height);
		return 1;
	}
	const uint32_t physical = MmGetPhysicalAddress((const void *)pixels);
	xsdk_log_write("display: %ux%u buffer at %p (physical %08X)", (unsigned)width, (unsigned)height, (void *)pixels,
		(unsigned)physical);
	fill(pixels, pixel_count, colours[0].pixel);

	volatile uint32_t *const system_writeback = &page[SYSTEM_WRITEBACK_OFFSET / 4];
	VdGetSystemCommandBuffer(system_buffer, &system_value);
	VdSetSystemCommandBufferGpuIdentifierAddress(system_writeback);
	swap_to(pixels, width, height, system_writeback);
	(void)xsdk_thread_sleep(SETTLE_MS);
	report_registers("after VdSwap");
	xsdk_log_write("display: showing red after VdSwap for %u ms", (unsigned)HOLD_MS);
	(void)xsdk_thread_sleep(HOLD_MS);

	if (read_register(D1GRPH_PRIMARY_SURFACE_ADDRESS) != physical) {
		set_surface(physical);
		(void)xsdk_thread_sleep(SETTLE_MS);
		report_registers("after setting the surface");
	}
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
