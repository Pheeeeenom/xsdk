#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "bar_scene.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/present.h"

// Milestone 9's run: the moving bar for 10 minutes, three buffers and a flip every vblank, logged each minute.

enum {
	RUN_MS = 10 * 60 * 1000,
	MINUTE_MS = 60 * 1000,
	BUFFERS = 3,
	SWAP_INTERVAL = 1,
	BAR_ARGB = 0xFFFFFFFF,
	BEGIN_TIMEOUT_MS = 1000,
	MAX_WRONG_LOGGED = 4,
};

static uint32_t passed_count;
static uint32_t failed_count;

static void check(const char *name, bool passed)
{
	xsdk_log_write("%s: %s", passed ? "PASS" : "FAIL", name);
	passed_count += passed;
	failed_count += !passed;
}

static uint32_t elapsed_ms(const struct timespec *start)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)((now.tv_sec - start->tv_sec) * 1000 + (now.tv_nsec - start->tv_nsec) / 1000000);
}

static void log_stats(const char *when, uint32_t frames, uint32_t wrong)
{
	XsdkPresentStats stats;
	xsdk_present_stats(&stats);
	xsdk_log_write(
		"pacing: %s: %u frames, %u flips over %u vblanks, %u missed, %u late, %u frames wrong; longest wait "
		"%u us",
		when, (unsigned)frames, (unsigned)stats.flips, (unsigned)stats.vblanks, (unsigned)stats.missed,
		(unsigned)stats.late, (unsigned)wrong, (unsigned)stats.longest_wait_us);
}

int main(void)
{
	xsdk_log_write("pacing: 10 minutes, %u buffers, a flip every vblank", (unsigned)BUFFERS);
	const NTSTATUS status = xsdk_gpu_open();
	BarScene scene;
	const bool opened = NT_SUCCESS(status) && bar_scene_open(&scene, BAR_ARGB);
	uint32_t width = 0;
	uint32_t height = 0;
	const bool presenting =
		opened && NT_SUCCESS(xsdk_present_open(XSDK_DISPLAY_720P, BUFFERS, SWAP_INTERVAL, &width, &height));
	xsdk_log_write("pacing: GPU 0x%08X, display %ux%u", (unsigned)status, (unsigned)width, (unsigned)height);
	check("pacing: GPU, scene and present open", presenting);
	if (!presenting)
		return 1;
	struct timespec start;
	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	uint32_t frame = 0;
	uint32_t wrong = 0;
	uint32_t next_minute_ms = MINUTE_MS;
	bool kept_up = true;
	while (elapsed_ms(&start) < RUN_MS) {
		uint32_t address;
		if (!NT_SUCCESS(xsdk_present_begin(&address, BEGIN_TIMEOUT_MS))) {
			kept_up = false;
			break;
		}
		// The buffer last held frame - BUFFERS, shown and off screen now; frame 0's background is whatever eDRAM held.
		if (frame > BUFFERS && !bar_scene_holds(&scene, xsdk_present_pixels(address), frame - BUFFERS)) {
			if (wrong < MAX_WRONG_LOGGED)
				bar_scene_log_miss(&scene, xsdk_present_pixels(address), frame - BUFFERS, "pacing");
			wrong++;
		}
		bar_scene_draw(&scene, frame, address);
		xsdk_present_end();
		frame++;
		if (elapsed_ms(&start) >= next_minute_ms) {
			char when[16];
			snprintf(when, sizeof when, "minute %u", (unsigned)(next_minute_ms / MINUTE_MS));
			log_stats(when, frame, wrong);
			next_minute_ms += MINUTE_MS;
		}
	}
	xsdk_present_close();
	log_stats("done", frame, wrong);
	XsdkPresentStats stats;
	xsdk_present_stats(&stats);
	check("pacing: every frame queued in time for 10 minutes", kept_up);
	check("pacing: every frame shown", stats.flips == frame);
	check("pacing: no vblank missed", stats.missed == 0);
	check("pacing: a flip every vblank", stats.flips && stats.last_flip_vblank - stats.first_flip_vblank == frame - 1);
	check("pacing: every flip inside vblank", stats.late == 0);
	check("pacing: every frame resolved with its bar", wrong == 0);
	bar_scene_close(&scene);
	check("pacing: GPU close", NT_SUCCESS(xsdk_gpu_close()));
	xsdk_log_write("pacing: %u passed, %u failed", (unsigned)passed_count, (unsigned)failed_count);
	return failed_count != 0;
}
