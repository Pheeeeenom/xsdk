#include <stdint.h>

#include "probe_gpu.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"

// Resolves as the present test does, then after each register the triangle's draw state adds, logging pixels left out.

enum {
	WIDTH = 640,
	HEIGHT = 480,
	PIXELS = WIDTH * HEIGHT,
	FENCE_TIMEOUT_MS = 1000,
	CLEAR_ARGB = 0xFF202020,
	// The display's magenta as a word in memory; a resolve that writes nothing leaves it.
	MAGENTA = 0xFF00FFFF,
};

// Registers the resolve's coverage may depend on (xenia:src/xenia/gpu/register_table.inc).
enum {
	RB_SURFACE_INFO = 0x2000,
	RB_BLENDCONTROL0 = 0x2201,
	RB_COLORCONTROL = 0x2202,
	PA_CL_CLIP_CNTL = 0x2204,
	PA_SU_SC_MODE_CNTL = 0x2205,
	PA_CL_VTE_CNTL = 0x2206,
	PA_SC_AA_CONFIG = 0x2301,
	PA_SU_VTX_CNTL = 0x2302,
	PA_SC_AA_MASK = 0x2312,
};

static const struct {
	const char *name;
	uint32_t index;
} inherited[] = {
	{ "RB_SURFACE_INFO", RB_SURFACE_INFO },
	{ "RB_BLENDCONTROL0", RB_BLENDCONTROL0 },
	{ "RB_COLORCONTROL", RB_COLORCONTROL },
	{ "PA_CL_CLIP_CNTL", PA_CL_CLIP_CNTL },
	{ "PA_SU_SC_MODE_CNTL", PA_SU_SC_MODE_CNTL },
	{ "PA_CL_VTE_CNTL", PA_CL_VTE_CNTL },
	{ "PA_SC_AA_CONFIG", PA_SC_AA_CONFIG },
	{ "PA_SU_VTX_CNTL", PA_SU_VTX_CNTL },
	{ "PA_SC_AA_MASK", PA_SC_AA_MASK },
};

static void put_register(uint32_t index, uint32_t value)
{
	volatile uint32_t *const packet = xsdk_gpu_reserve(2);
	packet[0] = index;
	packet[1] = value;
}

static void resolve(const char *name, volatile uint32_t *pixels)
{
	for (uint32_t i = 0; i < PIXELS; i++)
		pixels[i] = MAGENTA;
	__asm__ volatile("sync" ::: "memory");
	xsdk_gpu_resolve(xsdk_gpu_address(pixels), WIDTH, HEIGHT, CLEAR_ARGB);
	const NTSTATUS waited = xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
	uint32_t unwritten = 0;
	uint32_t first = PIXELS;
	uint32_t last_column = 0;
	uint32_t last_row = 0;
	for (uint32_t i = 0; i < PIXELS; i++) {
		if (pixels[i] != MAGENTA)
			continue;
		unwritten++;
		if (first == PIXELS)
			first = i;
		last_column += i % WIDTH == WIDTH - 1;
		last_row += i / WIDTH == HEIGHT - 1;
	}
	if (!unwritten) {
		xsdk_log_write("resolve: %s: fence 0x%08X; every pixel written", name, (unsigned)waited);
		return;
	}
	xsdk_log_write(
		"resolve: %s: fence 0x%08X; %u unwritten, the first at (%u, %u); %u in the last column, %u in the last row",
		name, (unsigned)waited, (unsigned)unwritten, (unsigned)(first % WIDTH), (unsigned)(first / WIDTH),
		(unsigned)last_column, (unsigned)last_row);
}

int main(void)
{
	xsdk_log_write("resolve probe");
	// Drawing registers read as XAM left them (docs/HARDWARE.md, "libxenon's set-up makes the resolves work").
	for (uint32_t i = 0; i < sizeof inherited / sizeof inherited[0]; i++)
		xsdk_log_write(
			"resolve: inherited %s %08X", inherited[i].name, (unsigned)probe_gpu_registers[inherited[i].index]);
	const NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("resolve: xsdk_gpu_open returned 0x%08X", (unsigned)status);
		return 1;
	}
	volatile uint32_t *const pixels = xsdk_gpu_alloc(PIXELS * 4);
	if (!pixels)
		return 1;
	resolve("as the present test resolves", pixels);
	resolve("again", pixels);
	put_register(PA_SC_AA_CONFIG, 0);
	resolve("after PA_SC_AA_CONFIG 0", pixels);
	resolve("again", pixels);
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	resolve("after the rest of the triangle's target state", pixels);
	resolve("again", pixels);
	xsdk_gpu_free(pixels);
	xsdk_log_write("resolve: done; the GPU goes back at exit");
	return 0;
}
