#include <stdint.h>

#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// The display took a title's surface address but stayed black, while XAM shows its own surface by also setting 0x60EC
// (docs/HARDWARE.md, "How XAM switches the display"). This applies the surface the way XAM did: first only the address,
// on a red 640x480 buffer, then XAM's whole register set, on a blue 852x480 buffer, then the old values, each followed
// by 0x60EC = 1, which XAM's change showed until the next frame.

enum {
	PAGE_BYTES = 0x10000,
	HOLD_MS = 4000,
	APPLY_TIMEOUT_MS = 100,
	TILE_PIXELS = 32,
	// XAM's 852x480 surface (docs/HARDWARE.md).
	WIDE_WIDTH = 852,
	WIDE_PITCH = 864,
	WIDE_HEIGHT = 480,
	WIDE_SCALE = 0x00AA6660,
	WIDE_SCALE_PHASE = 0x0005D600,
	// libxenon's blue << 24 | green << 16 | red << 8 (libxenon:drivers/console/console.c:67-70).
	RED = 0x0000FF00,
	BLUE = 0xFF000000,
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

int main(void)
{
	xsdk_log_write("display probe");
	report("at start");
	const Surface original = read_surface();
	const uint32_t height = read_register(D1GRPH_Y_END);

	const NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("display: xsdk_gpu_open returned 0x%08X", (unsigned)status);
		return 1;
	}
	volatile uint32_t *const red = allocate(original.pitch, height, RED);
	volatile uint32_t *const blue = allocate(WIDE_PITCH, WIDE_HEIGHT, BLUE);
	if (!original.pitch || !height || !red || !blue) {
		xsdk_log_write("display: no buffers for a %ux%u mode", (unsigned)original.pitch, (unsigned)height);
		return 1;
	}

	Surface only_address = original;
	only_address.surface = MmGetPhysicalAddress((const void *)red);
	write_surface(&only_address);
	apply();
	report("red, address only");
	xsdk_log_write("display: holding red for %u ms", (unsigned)HOLD_MS);
	(void)xsdk_thread_sleep(HOLD_MS);

	const Surface wide = {
		.surface = MmGetPhysicalAddress((const void *)blue),
		.pitch = WIDE_PITCH,
		.x_end = WIDE_PITCH,
		.viewport_size = WIDE_WIDTH << 16 | WIDE_HEIGHT,
		.scale = WIDE_SCALE,
		.scale_phase = WIDE_SCALE_PHASE,
		.line_width = WIDE_WIDTH / 4 - 1,
	};
	write_surface(&wide);
	apply();
	report("blue, XAM's registers");
	xsdk_log_write("display: holding blue for %u ms", (unsigned)HOLD_MS);
	(void)xsdk_thread_sleep(HOLD_MS);

	write_surface(&original);
	apply();
	report("after restoring");
	return 0;
}
