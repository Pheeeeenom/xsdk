#include <stdint.h>
#include <string.h>

#include "probe_gpu.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// Swaps to two CPU-filled front buffers with VdSwap, logging the display registers after each (docs/HARDWARE.md).

enum {
	PAGE_BYTES = 0x10000,
	SWAP_DWORDS = 64,
	FENCE_TIMEOUT_MS = 1000,
	HOLD_MS = 3000,
	PATTERN = 0xA5A5A5A5,
	SYSTEM_BUFFER_DWORDS = 64,
	// The writeback slot VdSwap is given, in the probe's uncached page.
	SYSTEM_WRITEBACK_OFFSET = 0x40,
	// As the CPU writes them, 0xAARRGGBB.
	RED = 0xFFFF0000,
	BLUE = 0xFF0000FF,
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
	volatile uint32_t *pixels;
	uint32_t width;
	uint32_t height;
} FrontBuffer;

static uint32_t system_buffer[SYSTEM_BUFFER_DWORDS];
static uint32_t system_value;

static void report_registers(const char *when)
{
	volatile const uint32_t *const registers = probe_gpu_registers;
	xsdk_log_write("swap: %s: D1GRPH enable %08X, control %08X, surface %08X, pitch %08X, end %08X x %08X", when,
		(unsigned)registers[D1GRPH_ENABLE], (unsigned)registers[D1GRPH_CONTROL],
		(unsigned)registers[D1GRPH_PRIMARY_SURFACE_ADDRESS], (unsigned)registers[D1GRPH_PITCH],
		(unsigned)registers[D1GRPH_X_END], (unsigned)registers[D1GRPH_Y_END]);
	xsdk_log_write("swap: %s: D1GRPH update %08X, flip control %08X; viewport size %08X, scaler enable %08X", when,
		(unsigned)registers[D1GRPH_UPDATE], (unsigned)registers[D1GRPH_FLIP_CONTROL],
		(unsigned)registers[AVIVO_D1MODE_VIEWPORT_SIZE], (unsigned)registers[AVIVO_D1SCL_SCALER_ENABLE]);
}

static DISPLAY_INFO report_display_information(const char *when)
{
	DISPLAY_INFO info;
	memset(&info, 0, sizeof info);
	VdGetCurrentDisplayInformation(&info);
	xsdk_log_write("swap: %s: front buffer %ux%u, colour format %u, pixel format %u; display %ux%u", when,
		(unsigned)info.front_buffer_width, (unsigned)info.front_buffer_height, (unsigned)info.front_buffer_color_format,
		(unsigned)info.front_buffer_pixel_format, (unsigned)info.display_width, (unsigned)info.display_height);
	return info;
}

static void get_system_command_buffer(void)
{
	for (uint32_t i = 0; i < SYSTEM_BUFFER_DWORDS; i++)
		system_buffer[i] = PATTERN;
	system_value = PATTERN;
	VdGetSystemCommandBuffer(system_buffer, &system_value);
	xsdk_log_write("swap: VdGetSystemCommandBuffer gave %08X", (unsigned)system_value);
}

static NTSTATUS allocate(FrontBuffer *buffer, uint32_t colour)
{
	const uint32_t pixel_count = buffer->width * buffer->height;
	const uint32_t bytes = (pixel_count * 4 + PAGE_BYTES - 1) & ~(PAGE_BYTES - 1);
	buffer->pixels = MmAllocatePhysicalMemoryEx(
		0, bytes, PAGE_READWRITE | PAGE_WRITECOMBINE | MEM_LARGE_PAGES, 0, UINT32_MAX, PAGE_BYTES);
	if (!buffer->pixels)
		return STATUS_NO_MEMORY;
	for (uint32_t i = 0; i < pixel_count; i++)
		buffer->pixels[i] = colour;
	__asm__ volatile("sync" ::: "memory");
	return STATUS_SUCCESS;
}

static void swap_to(const FrontBuffer *buffer, volatile uint32_t *system_writeback)
{
	const uint32_t address = (uint32_t)buffer->pixels;
	const uint32_t width = buffer->width;
	const uint32_t height = buffer->height;
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
	xsdk_log_write("swap: %s: %ux%u at %08X (physical %08X), fetch constant %08X %08X %08X", buffer->name,
		(unsigned)width, (unsigned)height, (unsigned)address, (unsigned)MmGetPhysicalAddress((const void *)address),
		(unsigned)fetch_constant[0], (unsigned)fetch_constant[1], (unsigned)fetch_constant[2]);

	volatile uint32_t *const ring_space = xsdk_gpu_reserve(SWAP_DWORDS);
	for (uint32_t i = 0; i < SWAP_DWORDS; i++)
		ring_space[i] = PATTERN;
	*system_writeback = PATTERN;
	VdSwap(ring_space, fetch_constant, system_writeback, system_buffer, system_value, &address, &texture_format,
		&color_space, &width, &height);
	xsdk_log_write("swap: %s: ring %08X %08X %08X %08X %08X %08X", buffer->name, (unsigned)ring_space[0],
		(unsigned)ring_space[1], (unsigned)ring_space[2], (unsigned)ring_space[3], (unsigned)ring_space[4],
		(unsigned)ring_space[5]);
	const NTSTATUS waited = xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
	xsdk_log_write(
		"swap: %s: fence 0x%08X, system writeback %08X", buffer->name, (unsigned)waited, (unsigned)*system_writeback);
	(void)xsdk_thread_sleep(100);
	report_registers(buffer->name);
	(void)report_display_information(buffer->name);
	(void)xsdk_thread_sleep(HOLD_MS);
	report_registers(buffer->name);
}

int main(void)
{
	xsdk_log_write("swap probe");
	const DISPLAY_INFO info = report_display_information("at start");
	VIDEO_MODE mode;
	memset(&mode, 0, sizeof mode);
	VdQueryVideoMode(&mode);
	xsdk_log_write("swap: video mode %ux%u, %d.%02d Hz", (unsigned)mode.display_width, (unsigned)mode.display_height,
		(int)mode.refresh_rate, (int)(mode.refresh_rate * 100) % 100);
	get_system_command_buffer();
	xsdk_log_write("swap: reading the display registers");
	report_registers("at start");

	const NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("swap: xsdk_gpu_open returned 0x%08X", (unsigned)status);
		return 1;
	}
	report_registers("after xsdk_gpu_open");
	volatile uint32_t *const page =
		MmAllocatePhysicalMemoryEx(0, PAGE_BYTES, PAGE_READWRITE | PAGE_NOCACHE | MEM_LARGE_PAGES, 0, UINT32_MAX, 0);
	FrontBuffer large = { .name = "red 1280x720", .width = mode.display_width, .height = mode.display_height };
	FrontBuffer small = {
		.name = "blue front-buffer size", .width = info.front_buffer_width, .height = info.front_buffer_height
	};
	if (!page || !NT_SUCCESS(allocate(&large, RED)) || !NT_SUCCESS(allocate(&small, BLUE))) {
		xsdk_log_write("swap: MmAllocatePhysicalMemoryEx failed");
		return 1;
	}
	volatile uint32_t *const system_writeback = &page[SYSTEM_WRITEBACK_OFFSET / 4];
	VdSetSystemCommandBufferGpuIdentifierAddress(system_writeback);

	swap_to(&large, system_writeback);
	swap_to(&small, system_writeback);
	return 0;
}
