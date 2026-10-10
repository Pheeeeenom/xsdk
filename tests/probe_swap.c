#include <stdint.h>
#include <string.h>

#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// Milestone 5's first question: does VdSwap show a front buffer the title made? This fills one with a colour from the
// CPU, so no eDRAM is involved and tiling can't matter, and swaps to it, then rewrites the colour every few seconds: if
// the screen follows, the swap worked, and the order of the colours shows the channel order. The kernel's VdSwap only
// writes a fence into the ring (docs/HARDWARE.md), so this registers the fence's address first, as D3D seems to, and
// logs the display controller's registers to see whether the kernel points it at the front buffer.

enum {
	PAGE_BYTES = 0x10000,
	SWAP_DWORDS = 64,
	FENCE_TIMEOUT_MS = 1000,
	COLOUR_MS = 3000,
	PATTERN = 0xA5A5A5A5,
	SYSTEM_BUFFER_DWORDS = 64,
	// The writeback slot VdSwap is given, in the probe's uncached page.
	SYSTEM_WRITEBACK_OFFSET = 0x40,
};

// Texture fetch constant fields (xenia:src/xenia/gpu/xenos.h:97-102, 193-198, 457, 1021-1028, 1102-1107, 1184-1285).
enum {
	FETCH_TYPE_TEXTURE = 2,
	FORMAT_8_8_8_8 = 6,
	ENDIAN_8IN32 = 2,
	SWIZZLE_XYZW = 0 | 1 << 3 | 2 << 6 | 3 << 9,
	DIMENSION_2D = 1,
};

typedef struct {
	const char *name;
	uint32_t value;
} Colour;

// As the CPU writes them, 0xAARRGGBB.
static const Colour colours[] = {
	{ "red", 0xFFFF0000 },
	{ "green", 0xFF00FF00 },
	{ "blue", 0xFF0000FF },
	{ "white", 0xFFFFFFFF },
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
	AVIVO_D1MODE_VIEWPORT_SIZE = 0x6584,
	AVIVO_D1SCL_SCALER_ENABLE = 0x6590,
};

// GPU registers as titles see them (xenia:src/xenia/gpu/graphics_system.cc:92-95); libxenon's base is the same block.
static volatile uint32_t *const gpu_registers = (volatile uint32_t *)0x7FC80000;

static uint32_t system_buffer[SYSTEM_BUFFER_DWORDS];
static uint32_t system_value;

static uint32_t read_register(uint32_t offset)
{
	return gpu_registers[offset / 4];
}

static void report_display(const char *when)
{
	xsdk_log_write("swap: %s: D1GRPH enable %08X, control %08X, surface %08X, pitch %08X, end %08X x %08X", when,
		(unsigned)read_register(D1GRPH_ENABLE), (unsigned)read_register(D1GRPH_CONTROL),
		(unsigned)read_register(D1GRPH_PRIMARY_SURFACE_ADDRESS), (unsigned)read_register(D1GRPH_PITCH),
		(unsigned)read_register(D1GRPH_X_END), (unsigned)read_register(D1GRPH_Y_END));
	xsdk_log_write("swap: %s: D1GRPH update %08X, flip control %08X; viewport size %08X, scaler enable %08X", when,
		(unsigned)read_register(D1GRPH_UPDATE), (unsigned)read_register(D1GRPH_FLIP_CONTROL),
		(unsigned)read_register(AVIVO_D1MODE_VIEWPORT_SIZE), (unsigned)read_register(AVIVO_D1SCL_SCALER_ENABLE));
}

static void log_system_buffer(const char *when)
{
	for (uint32_t i = 0; i < SYSTEM_BUFFER_DWORDS; i++) {
		if (system_buffer[i] && system_buffer[i] != PATTERN)
			xsdk_log_write("swap: %s: system buffer dword %u: %08X", when, (unsigned)i, (unsigned)system_buffer[i]);
	}
}

static void fill(volatile uint32_t *front_buffer, uint32_t pixel_count, uint32_t value)
{
	for (uint32_t i = 0; i < pixel_count; i++)
		front_buffer[i] = value;
	__asm__ volatile("sync" ::: "memory");
}

static void log_dwords(const char *what, volatile const uint32_t *dwords, uint32_t count)
{
	for (uint32_t i = 0; i < count; i += 8) {
		xsdk_log_write("swap: %s %02u: %08X %08X %08X %08X %08X %08X %08X %08X", what, (unsigned)i, (unsigned)dwords[i],
			(unsigned)dwords[i + 1], (unsigned)dwords[i + 2], (unsigned)dwords[i + 3], (unsigned)dwords[i + 4],
			(unsigned)dwords[i + 5], (unsigned)dwords[i + 6], (unsigned)dwords[i + 7]);
	}
}

static void log_display(void)
{
	VIDEO_MODE mode;
	memset(&mode, 0, sizeof mode);
	VdQueryVideoMode(&mode);
	xsdk_log_write("swap: video mode %ux%u, interlaced %u, widescreen %u, hi-def %u, %d.%02d Hz, standard %u",
		(unsigned)mode.display_width, (unsigned)mode.display_height, (unsigned)mode.is_interlaced,
		(unsigned)mode.is_widescreen, (unsigned)mode.is_hi_def, (int)mode.refresh_rate,
		(int)(mode.refresh_rate * 100) % 100, (unsigned)mode.video_standard);
	DISPLAY_INFO info;
	memset(&info, 0, sizeof info);
	VdGetCurrentDisplayInformation(&info);
	xsdk_log_write("swap: front buffer %ux%u, colour format %u, pixel format %u; display %ux%u, actual width %u",
		(unsigned)info.front_buffer_width, (unsigned)info.front_buffer_height, (unsigned)info.front_buffer_color_format,
		(unsigned)info.front_buffer_pixel_format, (unsigned)info.display_width, (unsigned)info.display_height,
		(unsigned)info.actual_display_width);
}

static void log_system_command_buffer(void)
{
	for (uint32_t i = 0; i < SYSTEM_BUFFER_DWORDS; i++)
		system_buffer[i] = PATTERN;
	system_value = PATTERN;
	VdGetSystemCommandBuffer(system_buffer, &system_value);
	uint32_t changed = 0;
	for (uint32_t i = 0; i < SYSTEM_BUFFER_DWORDS; i++) {
		if (system_buffer[i] == PATTERN)
			continue;
		changed++;
		if (system_buffer[i])
			xsdk_log_write("swap: system buffer dword %u: %08X", (unsigned)i, (unsigned)system_buffer[i]);
	}
	xsdk_log_write("swap: VdGetSystemCommandBuffer changed %u of %u buffer dwords; value %08X", (unsigned)changed,
		(unsigned)SYSTEM_BUFFER_DWORDS, (unsigned)system_value);
}

int main(void)
{
	xsdk_log_write("swap probe");
	log_display();
	log_system_command_buffer();
	xsdk_log_write("swap: reading the display registers");
	report_display("at start");

	VIDEO_MODE mode;
	memset(&mode, 0, sizeof mode);
	VdQueryVideoMode(&mode);
	const uint32_t width = mode.display_width;
	const uint32_t height = mode.display_height;
	const uint32_t pixel_count = width * height;
	const uint32_t front_buffer_bytes = (pixel_count * 4 + PAGE_BYTES - 1) & ~(PAGE_BYTES - 1);

	const NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("swap: xsdk_gpu_open returned 0x%08X", (unsigned)status);
		return 1;
	}
	report_display("after xsdk_gpu_open");
	volatile uint32_t *const front_buffer = MmAllocatePhysicalMemoryEx(
		0, front_buffer_bytes, PAGE_READWRITE | PAGE_WRITECOMBINE | MEM_LARGE_PAGES, 0, UINT32_MAX, PAGE_BYTES);
	volatile uint32_t *const page =
		MmAllocatePhysicalMemoryEx(0, PAGE_BYTES, PAGE_READWRITE | PAGE_NOCACHE | MEM_LARGE_PAGES, 0, UINT32_MAX, 0);
	if (!front_buffer || !page) {
		xsdk_log_write("swap: MmAllocatePhysicalMemoryEx failed");
		return 1;
	}
	volatile uint32_t *const system_writeback = &page[SYSTEM_WRITEBACK_OFFSET / 4];
	*system_writeback = PATTERN;
	const uint32_t front_buffer_address = (uint32_t)front_buffer;
	xsdk_log_write("swap: %ux%u front buffer at %08X (physical %08X), system writeback at %p", (unsigned)width,
		(unsigned)height, (unsigned)front_buffer_address, (unsigned)MmGetPhysicalAddress((const void *)front_buffer),
		(void *)system_writeback);
	fill(front_buffer, pixel_count, colours[0].value);

	// A linear k_8_8_8_8 2D texture; the pitch is in pixels >> 5 (xenia:src/xenia/gpu/xenos.h:1211-1222).
	const uint32_t fetch_constant[6] = {
		FETCH_TYPE_TEXTURE | (width >> 5) << 22,
		FORMAT_8_8_8_8 | ENDIAN_8IN32 << 6 | (front_buffer_address >> 12) << 12,
		(width - 1) | (height - 1) << 13,
		SWIZZLE_XYZW << 1,
		0,
		DIMENSION_2D << 9,
	};
	xsdk_log_write("swap: fetch constant %08X %08X %08X %08X %08X %08X", (unsigned)fetch_constant[0],
		(unsigned)fetch_constant[1], (unsigned)fetch_constant[2], (unsigned)fetch_constant[3],
		(unsigned)fetch_constant[4], (unsigned)fetch_constant[5]);

	const uint32_t texture_format = FORMAT_8_8_8_8;
	const uint32_t color_space = 0;
	volatile uint32_t *const ring_space = xsdk_gpu_reserve(SWAP_DWORDS);
	for (uint32_t i = 0; i < SWAP_DWORDS; i++)
		ring_space[i] = PATTERN;
	VdSetSystemCommandBufferGpuIdentifierAddress(system_writeback);
	xsdk_log_write("swap: calling VdSwap with the value %08X", (unsigned)system_value);
	VdSwap(ring_space, fetch_constant, system_writeback, system_buffer, system_value, &front_buffer_address,
		&texture_format, &color_space, &width, &height);
	log_dwords("ring", ring_space, SWAP_DWORDS);
	log_system_buffer("after VdSwap");
	const NTSTATUS waited = xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
	xsdk_log_write(
		"swap: the swap's fence: 0x%08X; system writeback %08X", (unsigned)waited, (unsigned)*system_writeback);
	report_display("after the swap");
	(void)xsdk_thread_sleep(100);
	report_display("100 ms later");
	log_system_buffer("100 ms later");

	for (uint32_t i = 0; i < sizeof colours / sizeof colours[0]; i++) {
		fill(front_buffer, pixel_count, colours[i].value);
		xsdk_log_write(
			"swap: showing %s (%08X) for %u ms", colours[i].name, (unsigned)colours[i].value, (unsigned)COLOUR_MS);
		(void)xsdk_thread_sleep(COLOUR_MS);
	}
	xsdk_log_write("swap: system writeback %08X", (unsigned)*system_writeback);
	report_display("at end");
	return 0;
}
