#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "bar_scene.h"
#include "tests.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/present.h"

enum {
	FRAMES = 120,
	BEGIN_TIMEOUT_MS = 1000,
	FENCE_TIMEOUT_MS = 1000,
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
	const NTSTATUS status = opened ? xsdk_present_open(mode->buffers, mode->swap_interval, &width, &height) : 0;
	check_mode(mode, "scene and present open", opened && NT_SUCCESS(status));
	if (!opened || !NT_SUCCESS(status)) {
		bar_scene_close(&scene);
		return;
	}
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
	// The last frame's buffer is on screen or about to be, and still allocated until the present closes.
	const bool drawn = presented == FRAMES && NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS)) &&
		bar_scene_holds(&scene, xsdk_present_pixels(last_address), FRAMES - 1);
	if (presented == FRAMES && !drawn)
		bar_scene_log_miss(&scene, xsdk_present_pixels(last_address), FRAMES - 1, mode->name);
	xsdk_log_write("pacing: %s: %u reused buffers held the wrong frame", mode->name, (unsigned)wrong);
	xsdk_present_close();
	XsdkPresentStats stats;
	xsdk_present_stats(&stats);
	xsdk_log_write(
		"pacing: %s: %u frames, %u flips over %u vblanks (first %u, last %u), %u missed, %u late; longest "
		"wait %u us",
		mode->name, (unsigned)presented, (unsigned)stats.flips, (unsigned)stats.vblanks,
		(unsigned)stats.first_flip_vblank, (unsigned)stats.last_flip_vblank, (unsigned)stats.missed,
		(unsigned)stats.late, (unsigned)stats.longest_wait_us);
	check_mode(mode, "every frame shown", presented == FRAMES && stats.flips == FRAMES);
	check_mode(mode, "no vblank missed", stats.flips == FRAMES && stats.missed == 0);
	check_mode(mode, "a flip every swap interval",
		stats.flips == FRAMES &&
			stats.last_flip_vblank - stats.first_flip_vblank == (FRAMES - 1) * mode->swap_interval);
	check_mode(mode, "every flip inside vblank", stats.flips == FRAMES && stats.late == 0);
	check_mode(mode, "every frame resolved with its bar", presented == FRAMES && wrong == 0 && drawn);
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
