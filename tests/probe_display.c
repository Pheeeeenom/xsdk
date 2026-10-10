#include <stdint.h>

#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// The display takes a title's surface but stays black, even with XAM's registers and 0x60EC (docs/HARDWARE.md). Every
// colour so far had a zero low byte, the alpha if the display reads pixels little-endian, as libxenon's console says
// (libxenon:drivers/console/console.c:67-70). So this shows white, 0xFFFFFFFF, opaque in any byte order: first before
// taking the GPU, then after, with black between, then red with an opaque alpha if pixels are little-endian ARGB.

enum {
	PAGE_BYTES = 0x10000,
	HOLD_MS = 4000,
	APPLY_TIMEOUT_MS = 100,
	TILE_PIXELS = 32,
	GAP_MS = 1500,
	WHITE = 0xFFFFFFFF,
	// Bytes 00 00 FF FF: alpha and red set if the display reads 0xAARRGGBB little-endian.
	RED_LITTLE_ENDIAN_ARGB = 0x0000FFFF,
};

// Display controller registers, in bytes from the register base (libxenon:drivers/xenos/xenos.h:11-54).
enum {
	D1CRTC_DOUBLE_BUFFER_CONTROL = 0x60EC,
	D1GRPH_PRIMARY_SURFACE_ADDRESS = 0x6110,
	D1GRPH_PITCH = 0x6120,
	D1GRPH_X_END = 0x6134,
	D1GRPH_Y_END = 0x6138,
	D1GRPH_UPDATE = 0x6144,
	AVIVO_D1MODE_VIEWPORT_SIZE = 0x6584,
	// XAM changes these with the viewport width; libxenon writes 0x65E8 as width / 4 - 1 (xenos.c:390).
	SCALE = 0x65B4,
	SCALE_PHASE = 0x65B8,
	LINE_WIDTH = 0x65E8,
};

typedef struct {
	uint32_t surface;
	uint32_t pitch;
	uint32_t x_end;
	uint32_t viewport_size;
	uint32_t scale;
	uint32_t scale_phase;
	uint32_t line_width;
} Surface;

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

static Surface read_surface(void)
{
	return (Surface){
		.surface = read_register(D1GRPH_PRIMARY_SURFACE_ADDRESS),
		.pitch = read_register(D1GRPH_PITCH),
		.x_end = read_register(D1GRPH_X_END),
		.viewport_size = read_register(AVIVO_D1MODE_VIEWPORT_SIZE),
		.scale = read_register(SCALE),
		.scale_phase = read_register(SCALE_PHASE),
		.line_width = read_register(LINE_WIDTH),
	};
}

static void report(const char *when)
{
	const Surface s = read_surface();
	xsdk_log_write(
		"display: %s: surface %08X, pitch %08X, x end %08X, y end %08X, viewport %08X, scale %08X, phase "
		"%08X, line width %08X, update %08X, 60EC %08X",
		when, (unsigned)s.surface, (unsigned)s.pitch, (unsigned)s.x_end, (unsigned)read_register(D1GRPH_Y_END),
		(unsigned)s.viewport_size, (unsigned)s.scale, (unsigned)s.scale_phase, (unsigned)s.line_width,
		(unsigned)read_register(D1GRPH_UPDATE), (unsigned)read_register(D1CRTC_DOUBLE_BUFFER_CONTROL));
}

// Sets 0x60EC after the registers, as XAM's change showed, and waits for the display to clear it.
static void apply(void)
{
	write_register(D1CRTC_DOUBLE_BUFFER_CONTROL, 1);
	uint32_t waited_ms = 0;
	while (read_register(D1CRTC_DOUBLE_BUFFER_CONTROL) && waited_ms < APPLY_TIMEOUT_MS) {
		(void)xsdk_thread_sleep(1);
		waited_ms++;
	}
	if (read_register(D1CRTC_DOUBLE_BUFFER_CONTROL)) {
		xsdk_log_write("display: 0x60EC still 1 after %u ms; clearing it", (unsigned)waited_ms);
		write_register(D1CRTC_DOUBLE_BUFFER_CONTROL, 0);
		return;
	}
	xsdk_log_write("display: 0x60EC cleared after %u ms", (unsigned)waited_ms);
}

static void write_surface(const Surface *s)
{
	write_register(D1GRPH_PRIMARY_SURFACE_ADDRESS, s->surface);
	write_register(D1GRPH_PITCH, s->pitch);
	write_register(D1GRPH_X_END, s->x_end);
	write_register(AVIVO_D1MODE_VIEWPORT_SIZE, s->viewport_size);
	write_register(SCALE, s->scale);
	write_register(SCALE_PHASE, s->scale_phase);
	write_register(LINE_WIDTH, s->line_width);
}

static volatile uint32_t *allocate(uint32_t pitch, uint32_t height, uint32_t pixel)
{
	const uint32_t pixel_count = pitch * ((height + TILE_PIXELS - 1) & ~(TILE_PIXELS - 1));
	const uint32_t bytes = (pixel_count * 4 + PAGE_BYTES - 1) & ~(PAGE_BYTES - 1);
	volatile uint32_t *const pixels = MmAllocatePhysicalMemoryEx(
		0, bytes, PAGE_READWRITE | PAGE_WRITECOMBINE | MEM_LARGE_PAGES, 0, UINT32_MAX, PAGE_BYTES);
	if (!pixels)
		return NULL;
	for (uint32_t i = 0; i < pixel_count; i++)
		pixels[i] = pixel;
	__asm__ volatile("sync" ::: "memory");
	return pixels;
}

static void show(const Surface *original, uint32_t physical, const char *what)
{
	Surface surface = *original;
	surface.surface = physical;
	write_surface(&surface);
	apply();
	report(what);
	xsdk_log_write("display: holding %s for %u ms", what, (unsigned)HOLD_MS);
	(void)xsdk_thread_sleep(HOLD_MS);
}

static void blank(const Surface *original)
{
	write_surface(original);
	apply();
	(void)xsdk_thread_sleep(GAP_MS);
}

int main(void)
{
	xsdk_log_write("display probe");
	report("at start");
	const Surface original = read_surface();
	const uint32_t height = read_register(D1GRPH_Y_END);
	volatile uint32_t *const pixels = allocate(original.pitch, height, WHITE);
	if (!original.pitch || !height || !pixels) {
		xsdk_log_write("display: no buffer for a %ux%u mode", (unsigned)original.pitch, (unsigned)height);
		return 1;
	}
	const uint32_t physical = MmGetPhysicalAddress((const void *)pixels);
	const uint32_t pixel_count = original.pitch * ((height + TILE_PIXELS - 1) & ~(TILE_PIXELS - 1));

	show(&original, physical, "white before taking the GPU");
	blank(&original);

	const NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("display: xsdk_gpu_open returned 0x%08X", (unsigned)status);
		return 1;
	}
	show(&original, physical, "white after taking the GPU");

	for (uint32_t i = 0; i < pixel_count; i++)
		pixels[i] = RED_LITTLE_ENDIAN_ARGB;
	__asm__ volatile("sync" ::: "memory");
	show(&original, physical, "red with alpha, little-endian ARGB");

	write_surface(&original);
	apply();
	report("after restoring");
	return 0;
}
