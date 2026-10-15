#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "bar_scene.h"
#include "tests.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/present.h"
#include "xsdk/thread.h"

enum {
	FRAMES = 30,
	BEGIN_TIMEOUT_MS = 1000,
	FENCE_TIMEOUT_MS = 1000,
	LAST_FLIP_TIMEOUT_MS = 1000,
	MAX_WRONG_LOGGED = 2,
};

typedef struct {
	const char *name;
	uint32_t buffers;
	uint32_t swap_interval;
	// Each mode's bar has its own colour, so a capture shows which mode is running.
	uint32_t argb;
} Mode;

static const Mode modes[] = {
	{ .name = "3 buffers, every vblank", .buffers = 3, .swap_interval = 1, .argb = 0xFFFFFFFF },
	{ .name = "2 buffers, every vblank", .buffers = 2, .swap_interval = 1, .argb = 0xFFFFFF00 },
	{ .name = "3 buffers, every second vblank", .buffers = 3, .swap_interval = 2, .argb = 0xFF00FFFF },
	{ .name = "2 buffers, every second vblank", .buffers = 2, .swap_interval = 2, .argb = 0xFFFF00FF },
};

void test_present_stats_at_last_flip(uint32_t frames, XsdkPresentStats *stats)
{
	for (uint32_t waited_ms = 0;; waited_ms++) {
		xsdk_present_stats(stats);
		if (stats->flips >= frames || waited_ms == LAST_FLIP_TIMEOUT_MS)
			return;
		(void)xsdk_thread_sleep(1);
	}
}

static void check_mode(const Mode *mode, const char *what, bool passed)
{
	char name[112];
	snprintf(name, sizeof name, "pacing: %s: %s", mode->name, what);
	check(name, passed);
}

// Presents the moving bar for FRAMES frames, then checks every frame showed, each on its own vblanks, none torn.
static void run_mode(const Mode *mode)
{
	BarScene scene;
	const bool opened = bar_scene_open(&scene, mode->argb);
	uint32_t width = 0;
	uint32_t height = 0;
	const NTSTATUS status =
		opened ? xsdk_present_open(XSDK_DISPLAY_720P, mode->buffers, mode->swap_interval, &width, &height) : 0;
	check_mode(mode, "scene and present open", opened && NT_SUCCESS(status));
	if (!opened || !NT_SUCCESS(status)) {
		bar_scene_close(&scene);
		return;
	}
	XsdkGpuStats before;
	xsdk_gpu_stats(&before);
	uint32_t presented = 0;
	uint32_t last_address = 0;
	uint32_t wrong = 0;
	for (; presented < FRAMES; presented++) {
		if (!NT_SUCCESS(xsdk_present_begin(&last_address, BEGIN_TIMEOUT_MS)))
			break;
		// The buffer last held the frame mode->buffers before; frame 0's background is whatever eDRAM held.
		if (presented > mode->buffers &&
			!bar_scene_holds(&scene, xsdk_present_pixels(last_address), presented - mode->buffers)) {
			if (wrong < MAX_WRONG_LOGGED)
				bar_scene_log_miss(&scene, xsdk_present_pixels(last_address), presented - mode->buffers, mode->name);
			wrong++;
		}
		bar_scene_draw(&scene, presented, last_address);
		xsdk_present_end();
	}
	XsdkPresentStats stats;
	test_present_stats_at_last_flip(FRAMES, &stats);
	// The last frame's buffer is on screen, and still allocated until the present closes.
	const bool drawn = presented == FRAMES && NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS)) &&
		bar_scene_holds(&scene, xsdk_present_pixels(last_address), FRAMES - 1);
	if (presented == FRAMES && !drawn)
		bar_scene_log_miss(&scene, xsdk_present_pixels(last_address), FRAMES - 1, mode->name);
	xsdk_log_write("pacing: %s: %u reused buffers held the wrong frame", mode->name, (unsigned)wrong);
	XsdkGpuStats after;
	xsdk_gpu_stats(&after);
	xsdk_present_close();
	xsdk_log_write("pacing: %s: %u swaps, fence %08X, landed %08X; XAM drew %u, showed %u", mode->name,
		(unsigned)(after.swaps - before.swaps), (unsigned)after.swap_fence, (unsigned)after.swap_fence_landed,
		(unsigned)(after.system_draws - before.system_draws), (unsigned)stats.system_frames);
	xsdk_log_write(
		"pacing: %s: %u frames, %u flips over %u vblanks (first %u, last %u), %u missed, %u late; longest "
		"wait %u us",
		mode->name, (unsigned)presented, (unsigned)stats.flips, (unsigned)stats.vblanks,
		(unsigned)stats.first_flip_vblank, (unsigned)stats.last_flip_vblank, (unsigned)stats.missed,
		(unsigned)stats.late, (unsigned)stats.longest_wait_us);
	if (stats.late)
		xsdk_log_write("pacing: %s: the first late flip came at vblank %u, line %u", mode->name,
			(unsigned)stats.first_late_vblank, (unsigned)stats.first_late_line);
	check_mode(mode, "every frame shown", presented == FRAMES && stats.flips == FRAMES);
	check_mode(mode, "no vblank missed", stats.flips == FRAMES && stats.missed == 0);
	check_mode(mode, "a flip every swap interval",
		stats.flips == FRAMES &&
			stats.last_flip_vblank - stats.first_flip_vblank == (FRAMES - 1) * mode->swap_interval);
	check_mode(mode, "every flip inside vblank", stats.flips == FRAMES && stats.late == 0);
	check_mode(mode, "every frame resolved with its bar", presented == FRAMES && wrong == 0 && drawn);
	// The kernel's swap is how the Guide draws (docs/HARDWARE.md, "The Guide shows through VdSwap").
	check_mode(mode, "every frame through the kernel's swap, its last fence landed",
		presented == FRAMES && after.swaps - before.swaps == FRAMES && drawn &&
			after.swap_fence_landed == after.swap_fence);
	// So opening the Guide during the test fails it.
	check_mode(mode, "every frame showed its own buffer, the Guide closed",
		after.system_draws == before.system_draws && stats.system_frames == 0);
	bar_scene_close(&scene);
}

void test_pacing(void)
{
	const NTSTATUS status = xsdk_gpu_open();
	check("pacing: GPU open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return;
	for (uint32_t i = 0; i < sizeof modes / sizeof modes[0]; i++)
		run_mode(&modes[i]);
	check("pacing: GPU close", NT_SUCCESS(xsdk_gpu_close()));
}
